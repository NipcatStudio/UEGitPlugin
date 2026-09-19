"""验证内置 LFS 的跨进程缓存写入、完整快照及保存失败退出码；不连接真实远端。
Exercise the bundled CLI against temporary repositories and a loopback-only lock service.
"""
import concurrent.futures
import http.server
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading

if len(sys.argv) != 2:
    raise SystemExit('Usage: python test_cache_cli.py <bundled-git-lfs-executable>')
BINARY = pathlib.Path(sys.argv[1]).resolve()
ENV = {k: v for k, v in os.environ.items()
       if not k.upper().startswith(('GIT_', 'GCM_')) and not k.lower().endswith('_proxy')}
ENV.update(GIT_TERMINAL_PROMPT='0', GCM_INTERACTIVE='Never',
           GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM='1', NO_PROXY='*')
RECORDS = [{'id': 'fixture-' + str(i),
            'path': 'Content/CacheFixture/Asset_' + str(i) + '_' + 'x' * 120 + '.uasset',
            'owner': {'name': 'Fixture'}, 'locked_at': '2026-01-01T00:00:00Z'}
           for i in range(675)]
PAYLOAD = json.dumps({'ours': RECORDS[:44], 'theirs': RECORDS[44:]}).encode()
control = {'barrier': None, 'break_cache_path': None, 'break_snapshot_path': None,
           'http_failure': False, 'requests': 0, 'broken_barriers': 0}
mutex = threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    """只返回固定的 verify 快照，不实现加锁、解锁或其它远端写入。
    Return a fixed verify snapshot; lock, unlock and other mutations are unsupported.
    """
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', '0'))))
        if not self.path.endswith('/locks/verify') or body.get('ref', {}).get('name') != 'refs/heads/fixture':
            self.send_error(400)
            return
        with mutex:
            control['requests'] += 1
            barrier = control['barrier']
            break_cache_path = control['break_cache_path']
            break_snapshot_path = control['break_snapshot_path']
            http_failure = control['http_failure']
        if barrier:
            try:
                barrier.wait(timeout=10)
            except threading.BrokenBarrierError:
                with mutex:
                    control['broken_barriers'] += 1
        if break_cache_path:
            break_cache_path.mkdir()
        if break_snapshot_path:
            break_snapshot_path.mkdir(parents=True)
        payload = b'{"message":"fixture query failure"}' if http_failure else PAYLOAD
        self.send_response(500 if http_failure else 200)
        self.send_header('Content-Type', 'application/vnd.git-lfs+json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def invoke(repo, *args):
    """显式使用插件的 -C 契约，最多等待二十秒。"""
    return subprocess.run([str(BINARY), '-C', str(repo), *args], env=ENV,
                          capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=20)


def checked_count(result):
    """只有命令成功且完整返回全部锁，才接受本轮核验。"""
    assert result.returncode == 0, (result.returncode, result.stderr)
    assert len(result.stdout.splitlines()) == len(RECORDS), len(result.stdout.splitlines())


with tempfile.TemporaryDirectory(prefix='lfs-cache-cli-') as temporary:
    root = pathlib.Path(temporary)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    def fixture(name):
        """每个场景拥有独立工作区、索引和缓存，禁止继承当前工程的远端及凭据。"""
        repo = root / name
        repo.mkdir()
        def git(*args):
            subprocess.run(['git', '-C', str(repo), *args], env=ENV, capture_output=True,
                           check=True, timeout=15)
        git('init', '-b', 'fixture')
        git('config', 'user.name', 'Fixture')
        git('config', 'user.email', 'fixture@example.invalid')
        git('config', 'core.hooksPath', str(repo / 'disabled-hooks'))
        git('config', 'credential.helper', '')
        git('config', 'lfs.storage', str(repo / 'isolated-lfs'))
        git('config', 'lfs.url', 'http://127.0.0.1:' + str(server.server_port) + '/api')
        git('remote', 'add', 'origin', 'http://127.0.0.1:' + str(server.server_port) + '/fixture.git')
        git('commit', '--allow-empty', '-m', 'Isolated cache fixture')
        return repo

    results = {}
    try:
        repo = fixture('concurrent')
        checked_count(invoke(repo, 'locks', '--verify'))
        for round_number in range(5):
            control['barrier'] = threading.Barrier(8)
            with concurrent.futures.ThreadPoolExecutor(max_workers=9) as pool:
                writers = [pool.submit(invoke, repo, 'locks', '--verify') for _ in range(8)]
                reader = pool.submit(invoke, repo, 'locks', '--verify', '--cached', '--json')
                for writer in writers:
                    checked_count(writer.result())
                snapshot = reader.result()
                assert snapshot.returncode == 0, snapshot.stderr
                data = json.loads(snapshot.stdout)
                assert data == json.loads(PAYLOAD), 'Cached JSON was partial or changed'
            control['barrier'] = None
            checked_count(invoke(repo, 'locks', '--local'))
        assert control['broken_barriers'] == 0, control
        residue = list((repo / 'isolated-lfs').rglob('*.tmp-*'))
        assert not residue, residue
        results['concurrent_verify'] = {'rounds': 5, 'writers_per_round': 8, 'locks': 675,
                                        'readback': 'all valid', 'temporary_file_residue': 0}

        # 在缓存初始化后制造保存错误，验证 defer Close 的错误不会被丢弃。
        # Fail persistence after initialization to verify errors from deferred Close reach the exit code.
        for json_mode in [False, True]:
            repo = fixture('save-failure-' + str(json_mode))
            control['break_cache_path'] = repo / 'isolated-lfs/lockcache.db'
            output = invoke(repo, 'locks', '--verify', *(['--json'] if json_mode else []))
            control['break_cache_path'] = None
            assert output.returncode != 0, 'A failed cache save was reported as success'
            assert 'Unable to save lock cache' in output.stderr, output.stderr
            results['save_failure_json_' + str(json_mode)] = output.returncode

        # JSON 快照发布失败也必须返回失败；不能只检查最后的 KV 保存结果。
        # Snapshot publication has its own error path, separate from the final KV save.
        repo = fixture('snapshot-failure')
        snapshot_path = repo / 'isolated-lfs/cache/locks/refs/heads/fixture/verifiable'
        control['break_snapshot_path'] = snapshot_path
        output = invoke(repo, 'locks', '--verify', '--json')
        control['break_snapshot_path'] = None
        assert output.returncode != 0 and 'Error while retrieving locks' in output.stderr, output.stderr
        assert snapshot_path.is_dir() and output.stdout == ''
        results['snapshot_failure_json'] = output.returncode

        repo = fixture('query-failure')
        control['http_failure'] = True
        output = invoke(repo, 'locks', '--verify', '--json')
        control['http_failure'] = False
        assert output.returncode != 0 and 'fixture query failure' in output.stderr, output.stderr
        assert output.stdout == ''
        results['http_failure_json'] = output.returncode

        repo = fixture('corrupt')
        cache = repo / 'isolated-lfs'
        cache.mkdir()
        database = cache / 'lockcache.db'
        corrupt = b'preserved corrupt cache evidence'
        database.write_bytes(corrupt)
        requests = control['requests']
        output = invoke(repo, 'locks', '--verify')
        assert output.returncode != 0 and 'lock cache initialization' in output.stderr, output.stderr
        assert database.read_bytes() == corrupt
        assert control['requests'] == requests, 'Corruption must not trigger a misleading empty remote result'
        results['corrupt_cache'] = 'explicit failure; original bytes preserved'
        print(json.dumps(results, indent=2))
    finally:
        control['barrier'] = None
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)
