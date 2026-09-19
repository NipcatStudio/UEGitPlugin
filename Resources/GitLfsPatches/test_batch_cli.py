import http.server
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

if len(sys.argv) != 2:
    raise SystemExit('Usage: python test_batch_cli.py <git-lfs executable>')
BINARY = pathlib.Path(sys.argv[1]).resolve()
ENV = dict(os.environ, GIT_TERMINAL_PROMPT='0', GCM_INTERACTIVE='Never',
           GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM='1')
for key in list(ENV):
    if key.lower().endswith('_proxy'):
        del ENV[key]
ENV['NO_PROXY'] = '*'

# 假服务器仅服务临时仓库；验证实际 CLI，不访问或解锁项目资产。
# Exercise the real CLI against a temporary repository and local fake lock service only.
def run_case(modified=False, blocked=False, invalid=False, by_path=False):
    records = [{'id': 'id-' + str(i), 'path': 'Content/Asset' + str(i) + '.uasset',
                'owner': {'name': 'test'}, 'locked_at': '2026-01-01T00:00:00Z'} for i in range(6)]
    received = []
    active = 0
    peak = 0
    mutex = threading.Lock()
    class Handler(http.server.BaseHTTPRequestHandler):
        """只处理夹具的列表、verify/unlock，记录真实请求并返回可控结果。
        Serve only fixture listing/verification/unlock requests and record their outcomes.
        """
        def log_message(self, *args):
            pass
        def do_GET(self):
            request = urllib.parse.urlsplit(self.path)
            if not request.path.endswith('/locks'):
                self.send_error(404)
                return
            query = urllib.parse.parse_qs(request.query)
            found = [x for x in records if all(x[key] == query[key][0] for key in ('id', 'path') if key in query)]
            payload = json.dumps({'locks': found}).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/vnd.git-lfs+json')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        def do_POST(self):
            nonlocal active, peak
            body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', '0'))))
            assert body['ref']['name'] == 'refs/heads/test'
            if self.path.endswith('/locks/verify'):
                data, code = {'ours': records, 'theirs': []}, 200
            else:
                lock_id = self.path.split('/')[-2]
                assert body['force'] is False
                with mutex:
                    active += 1
                    peak = max(peak, active)
                    received.append(lock_id)
                time.sleep(0.04)
                data, code = ({'message': 'test rejection'}, 409) if blocked and lock_id == 'id-1' else ({'lock': next(x for x in records if x['id'] == lock_id)}, 200)
                with mutex:
                    active -= 1
            payload = json.dumps(data).encode()
            self.send_response(code)
            self.send_header('Content-Type', 'application/vnd.git-lfs+json')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
    with tempfile.TemporaryDirectory(prefix='lfs-batch-cli-') as tmp:
        repo = pathlib.Path(tmp)
        def git(*args):
            return subprocess.run(['git', '-C', str(repo), *args], check=True, env=ENV, capture_output=True)
        git('init', '-b', 'test')
        git('config', 'user.name', 'Test')
        git('config', 'user.email', 'test@example.invalid')
        git('config', 'core.hooksPath', str(repo / 'no-hooks'))
        git('config', 'lfs.storage', str(repo / 'lfs-storage'))
        (repo / 'Content').mkdir()
        for record in records:
            (repo / record['path']).write_text('original\n')
        git('add', 'Content')
        git('commit', '-m', 'fixture')
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            git('config', 'lfs.url', 'http://127.0.0.1:' + str(server.server_port) + '/api')
            git('config', 'lfs.concurrenttransfers', '4')
            p = subprocess.run([str(BINARY), '-C', str(repo), 'locks', '--verify'], capture_output=True, env=ENV, text=True, timeout=10)
            assert p.returncode == 0, p.stderr
            if modified:
                (repo / records[0]['path']).write_text('staged user edit\n')
                git('add', '--', records[0]['path'])
                (repo / records[0]['path']).write_text('new user edit\n')
            before_index = git('show', ':' + records[0]['path']).stdout
            args = [str(BINARY), '-C', str(repo), 'unlock', '--json']
            args += [x['path'] for x in records] if by_path else ['--id=' + x['id'] for x in records] + ['--id=id-5']
            if invalid:
                args.append('--id=')
            start = time.monotonic()
            p = subprocess.run(args, capture_output=True, env=ENV, text=True, timeout=10)
            if invalid:
                assert p.returncode != 0 and not received, (p.returncode, received)
                return {'invalid_empty_id': True, 'exit': p.returncode, 'requests': received}
            output = json.loads(p.stdout)
            assert p.returncode == (2 if blocked else 0), (p.returncode, p.stdout, p.stderr)
            expected = {x['id'] for x in records}
            assert set(received) == expected and len(received) == len(expected), received
            assert 2 <= peak <= 4, peak
            success_ids = {next(r['id'] for r in records if r['path'] == x['path']) if by_path else x['id']
                           for x in output if x['unlocked']}
            assert success_ids == expected - ({'id-1'} if blocked else set()), output
            if modified:
                assert (repo / records[0]['path']).read_text() == 'new user edit\n'
                assert git('show', ':' + records[0]['path']).stdout == before_index
            return {'modified': modified, 'blocked': blocked, 'by_path': by_path, 'exit': p.returncode, 'requests': received,
                    'peak_concurrency': peak, 'seconds': round(time.monotonic() - start, 3)}
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)

if __name__ == '__main__':
    print(json.dumps([run_case(), run_case(modified=True), run_case(modified=True, by_path=True),
                      run_case(blocked=True), run_case(invalid=True)], indent=2))
