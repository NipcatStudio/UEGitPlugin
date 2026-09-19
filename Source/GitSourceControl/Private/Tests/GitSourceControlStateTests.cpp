// Copyright (c) 2026 LunarMaxim
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt).

#if WITH_DEV_AUTOMATION_TESTS

#include "GitSourceControlState.h"
#include "GitSourceControlUtils.h"

#include "GitSourceControlOperations.h"
#include "GitSourceControlCommand.h"
#include "SourceControlOperations.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlSettings.h"

#include "HAL/PlatformMisc.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace
{
/**
 * 一条规范状态及其面向 UE 消费者的预期投影；只列生产中有意义的状态组合。
 * One canonical state and its expected projection to UE consumers; only production-meaningful
 * combinations are listed.
 */
struct FGitStateContractCase
{
	const TCHAR* Label;
	EFileState::Type FileState;
	ETreeState::Type TreeState;
	ELockState::Type LockState;
	ERemoteState::Type RemoteState;
	bool bCheckedOut;
	bool bCheckedOutOther;
	bool bCanEdit;
	bool bCanCheckout;
	bool bCanCheckIn;
	bool bCanRevert;
	bool bPassesCheckedOutFilter;
};

/**
 * 一条本地文件权限策略输入及预期决策。
 * One local file-permission policy input and its expected decision.
 */
struct FGitLocalReadOnlyPolicyCase
{
	const TCHAR* Label;
	EFileState::Type FileState;
	ETreeState::Type TreeState;
	ELockState::Type LockState;
	bool bUsingLfsLocking;
	EGitLocalReadOnlyPolicy ExpectedPolicy;
};

FGitSourceControlState MakeState(
	EFileState::Type FileState,
	ETreeState::Type TreeState,
	ELockState::Type LockState,
	ERemoteState::Type RemoteState = ERemoteState::UpToDate)
{
	FGitSourceControlState State(TEXT("Content/StateContract.uasset"));
	State.State.FileState = FileState;
	State.State.TreeState = TreeState;
	State.State.LockState = LockState;
	State.State.RemoteState = RemoteState;
	if (LockState == ELockState::LockedOther)
	{
		State.State.LockUser = TEXT("AnotherUser");
	}
	return State;
}

/** Provider 尚在异步初始化时，优先从当前进程 PATH 解析自动化测试所需的 Git。 */
FString GetAutomationTestGitBinary()
{
	const FString ConfiguredGitBinary = FGitSourceControlModule::Get()
		.GetProvider()
		.GetGitBinaryPath();
	if (!ConfiguredGitBinary.IsEmpty())
	{
		return ConfiguredGitBinary;
	}

	TArray<FString> SearchDirectories;
	FPlatformMisc::GetEnvironmentVariable(TEXT("PATH")).ParseIntoArray(
		SearchDirectories,
		FPlatformMisc::GetPathVarDelimiter(),
		true);
	IPlatformFile& PlatformFile =
		FPlatformFileManager::Get().GetPlatformFile();
#if PLATFORM_WINDOWS
	const TCHAR* GitExecutable = TEXT("git.exe");
#else
	const TCHAR* GitExecutable = TEXT("git");
#endif
	for (FString SearchDirectory : SearchDirectories)
	{
		SearchDirectory.TrimStartAndEndInline();
		SearchDirectory.TrimQuotesInline();
		const FString Candidate = FPaths::Combine(
			SearchDirectory,
			GitExecutable);
		if (PlatformFile.FileExists(*Candidate))
		{
			return Candidate;
		}
	}

	return GitSourceControlUtils::FindGitBinaryPath();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlWritableRevertTest,
	"GitSourceControl.State.RevertWritable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlWritableRevertTest::RunTest(const FString& Parameters)
{
	const FString GitBinary = GetAutomationTestGitBinary();
	if (!TestFalse(TEXT("还原测试需要 Git"), GitBinary.IsEmpty())) { return false; }
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("Automation"), TEXT("UEGitWritableRevert-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestTrue(TEXT("创建独立还原测试仓库"), PlatformFile.CreateDirectoryTree(*Root))) { return false; }
	ON_SCOPE_EXIT { PlatformFile.DeleteDirectoryRecursively(*Root); };
	TArray<FString> GitOutput;
	auto Git = [&](const TArray<FString>& Args, bool bExpectedSuccess = true)
	{
		TArray<FString> Errors;
		GitOutput.Reset();
		const bool bSuccess = GitSourceControlUtils::RunCommand(Args[0], GitBinary, Root,
			TArray<FString>(Args.GetData() + 1, Args.Num() - 1), {}, GitOutput, Errors);
		return TestEqual(TEXT("夹具 Git 命令结果"), bSuccess, bExpectedSuccess);
	};
	auto Write = [&](const TCHAR* File, const TCHAR* Text)
	{
		return TestTrue(TEXT("写入夹具文件"), FFileHelper::SaveStringToFile(Text, *FPaths::Combine(Root, File)));
	};
	auto Content = [&](const TCHAR* File)
	{
		FString Text;
		TestTrue(TEXT("读取夹具文件"), FFileHelper::LoadFileToString(Text, *FPaths::Combine(Root, File)));
		return Text;
	};
	if (!Git({TEXT("init"), TEXT("-b"), TEXT("test")})
		|| !Git({TEXT("config"), TEXT("user.name"), TEXT("UEGitAutomation")})
		|| !Git({TEXT("config"), TEXT("user.email"), TEXT("uegit@example.invalid")})
		|| !Git({TEXT("config"), TEXT("commit.gpgsign"), TEXT("false")})
		|| !Git({TEXT("config"), TEXT("core.hooksPath"), TEXT("no-hooks")})
		|| !Write(TEXT("Selected.txt"), TEXT("baseline")) || !Write(TEXT("Other.txt"), TEXT("baseline"))
		|| !Write(TEXT("[1].txt"), TEXT("literal baseline")) || !Write(TEXT("1.txt"), TEXT("neighbor baseline"))
		|| !Git({TEXT("add"), TEXT("--"), TEXT(".")}) || !Git({TEXT("commit"), TEXT("-m"), TEXT("baseline")})
		|| !Git({TEXT("rev-parse"), TEXT("HEAD")})) { return false; }
	const FString BaselineCommit = GitOutput[0];
	if (!Write(TEXT("Selected.txt"), TEXT("current local revision"))
		|| !Git({TEXT("commit"), TEXT("-am"), TEXT("local-current")})
		|| !Git({TEXT("rev-parse"), TEXT("HEAD")})) { return false; }
	const FString CurrentCommit = GitOutput[0];
	const FString Selected = FPaths::Combine(Root, TEXT("Selected.txt"));

	// 夹具是无 LFS 的独立 Git 仓库；命令仍保留创建时的设置快照，不改真实 Provider 配置。
	// The fixture is plain Git; retain each command's settings snapshot without changing the real provider.
	auto Configure = [&](FGitSourceControlCommand& Command, const TArray<FString>& Files)
	{
		Command.PathToGitBinary = GitBinary;
		Command.PathToGitRoot = Root;
		Command.PathToRepositoryRoot = Root;
		Command.Files = Files;
		Command.bUsingGitLfsLocking = false;
	};
	auto LoadCurrentRevision = [&]()
	{
		TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Operation = ISourceControlOperation::Create<FUpdateStatus>();
		Operation->SetUpdateHistory(true);
		TSharedRef<FGitUpdateStatusWorker, ESPMode::ThreadSafe> Worker = MakeShared<FGitUpdateStatusWorker, ESPMode::ThreadSafe>();
		FGitSourceControlCommand Command(Operation, Worker);
		Configure(Command, {Selected});
		TestTrue(TEXT("真实 UpdateStatus worker 提供本地历史"), Worker->Execute(Command));
		FGitSourceControlState State(Selected);
		State.State = Worker->States.FindRef(Selected);
		State.History = Worker->Histories.FindRef(Selected);
		return State.GetCurrentRevision();
	};
	auto Sync = [&](const FString& Revision, const TArray<FString>& Files, bool bForce, bool bLastSynced, bool bStale = false)
	{
		TSharedRef<FSync, ESPMode::ThreadSafe> Operation = ISourceControlOperation::Create<FSync>();
		Operation->SetRevision(Revision);
		Operation->SetForce(bForce);
		Operation->SetLastSyncedFlag(bLastSynced);
		TSharedRef<FGitSyncWorker, ESPMode::ThreadSafe> Worker = MakeShared<FGitSyncWorker, ESPMode::ThreadSafe>();
		FGitSourceControlCommand Command(Operation, Worker);
		Configure(Command, Files);
		if (bStale) { Command.bSettingsUsingGitLfsLocking = !Command.bSettingsUsingGitLfsLocking; }
		return Worker->Execute(Command);
	};
	FGitSourceControlState NoHistory(Selected);
	TestFalse(TEXT("没有历史不伪造当前修订"), NoHistory.GetCurrentRevision().IsValid());
	const TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> CurrentRevision = LoadCurrentRevision();
	if (!TestTrue(TEXT("Writable Revert 获得有效当前修订"), CurrentRevision.IsValid())) { return false; }
	TestEqual(TEXT("当前修订来自本地 HEAD 的文件历史"), CurrentRevision->GetRevision(), CurrentCommit.Left(8));
	if (!Write(TEXT("Selected.txt"), TEXT("staged edit")) || !Write(TEXT("Other.txt"), TEXT("unrelated staged"))
		|| !Git({TEXT("add"), TEXT("--"), TEXT("Selected.txt"), TEXT("Other.txt")})
		|| !Write(TEXT("Selected.txt"), TEXT("offline writable edit"))
		|| !Write(TEXT("Other.txt"), TEXT("unrelated working edit"))) { return false; }
	TestTrue(TEXT("UI 的强制指定修订同步真实还原文件"), Sync(CurrentRevision->GetRevision(), {Selected}, true, false));
	TestEqual(TEXT("选中文件恢复为当前修订"), Content(TEXT("Selected.txt")), FString(TEXT("current local revision")));
	Git({TEXT("show"), TEXT(":Selected.txt")});
	TestEqual(TEXT("选中文件的暂存内容同时恢复"), FString::Join(GitOutput, TEXT("\n")), FString(TEXT("current local revision")));
	Git({TEXT("show"), TEXT(":Other.txt")});
	TestEqual(TEXT("保留未选中文件的暂存内容"), FString::Join(GitOutput, TEXT("\n")), FString(TEXT("unrelated staged")));
	TestEqual(TEXT("保留未选中文件的工作区内容"), Content(TEXT("Other.txt")), FString(TEXT("unrelated working edit")));
	TestTrue(TEXT("支持指定历史修订但不切换 HEAD"), Sync(BaselineCommit, {Selected}, true, false));
	TestEqual(TEXT("历史修订被准确还原"), Content(TEXT("Selected.txt")), FString(TEXT("baseline")));
	TestTrue(TEXT("Uncontrolled Revert 的 LastSynced 标志恢复本地 HEAD"), Sync(TEXT(""), {Selected}, true, true));
	TestEqual(TEXT("LastSynced 不取更旧或远端版本"), Content(TEXT("Selected.txt")), FString(TEXT("current local revision")));
	Write(TEXT("[1].txt"), TEXT("literal edit"));
	Write(TEXT("1.txt"), TEXT("neighbor edit"));
	TestTrue(TEXT("路径按字面值还原"), Sync(CurrentCommit, {FPaths::Combine(Root, TEXT("[1].txt"))}, true, false));
	TestEqual(TEXT("只还原带括号的明确文件"), Content(TEXT("[1].txt")), FString(TEXT("literal baseline")));
	TestEqual(TEXT("不把括号当通配符误改相邻文件"), Content(TEXT("1.txt")), FString(TEXT("neighbor edit")));
	Write(TEXT("Selected.txt"), TEXT("protected edit"));
	TestFalse(TEXT("未确认强制覆盖不能丢弃修改"), Sync(CurrentCommit, {Selected}, false, false));
	TestFalse(TEXT("空范围不得变为整仓库操作"), Sync(CurrentCommit, {}, true, false));
	TestFalse(TEXT("目录不得变为整仓库还原"), Sync(CurrentCommit, {Root}, true, false));
	TestFalse(TEXT("引号不能扩张文件参数范围"), Sync(CurrentCommit, {FPaths::Combine(Root, TEXT("\" . \""))}, true, false));
	TestFalse(TEXT("不安全修订不得写文件"), Sync(TEXT("HEAD\" --force"), {Selected}, true, false));
	TestFalse(TEXT("设置快照过期时写边界拒绝还原"), Sync(CurrentCommit, {Selected}, true, false, true));
	TestEqual(TEXT("拒绝后保留工作区修改"), Content(TEXT("Selected.txt")), FString(TEXT("protected edit")));
	Git({TEXT("rev-parse"), TEXT("HEAD")});
	TestEqual(TEXT("全部还原都未移动 HEAD"), GitOutput[0], CurrentCommit);
	TestFalse(TEXT("没有执行 fetch"), PlatformFile.FileExists(*FPaths::Combine(Root, TEXT(".git/FETCH_HEAD"))));

	// 构造真正的合并冲突，验证补充对端历史后仍返回本地修订而不是 MERGE_HEAD。
	// A real merge conflict verifies that supplemental remote history cannot become the current revision.
	if (!Git({TEXT("checkout"), TEXT("HEAD"), TEXT("--"), TEXT(".")})
		|| !Git({TEXT("checkout"), TEXT("-b"), TEXT("other"), BaselineCommit})
		|| !Write(TEXT("Selected.txt"), TEXT("other branch"))
		|| !Git({TEXT("commit"), TEXT("-am"), TEXT("other-change")})
		|| !Git({TEXT("checkout"), TEXT("test")})
		|| !Git({TEXT("merge"), TEXT("--no-ff"), TEXT("other")}, false)) { return false; }
	const TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> ConflictRevision = LoadCurrentRevision();
	if (TestTrue(TEXT("冲突仍有明确本地当前修订"), ConflictRevision.IsValid()))
	{
		TestEqual(TEXT("冲突不能把对端提交当作当前版本"), ConflictRevision->GetRevision(), CurrentCommit.Left(8));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlRemoteScopeTest,
	"GitSourceControl.State.RemoteScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlRemoteScopeTest::RunTest(const FString& Parameters)
{
	const FString GitBinary = GetAutomationTestGitBinary();
	if (!TestFalse(TEXT("远端范围测试需要 Git"), GitBinary.IsEmpty())) { return false; }
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("Automation"), TEXT("UEGitRemoteScope-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestTrue(TEXT("创建独立测试仓库"), PlatformFile.CreateDirectoryTree(*FPaths::Combine(Root, TEXT("Content"))))) { return false; }
	ON_SCOPE_EXIT { PlatformFile.DeleteDirectoryRecursively(*Root); };
	// 临时仓库位于工程下，但独立拥有 HEAD/upstream；不访问网络或运行工作站 hooks。
	// The nested fixture owns its HEAD/upstream independently, without network access or workstation hooks.
	auto Run = [&](const TArray<FString>& Args)
	{
		TArray<FString> Params{
			FString::Printf(TEXT("core.hooksPath=\"%s/no-hooks\""), *Root),
			TEXT("-c"), TEXT("commit.gpgsign=false"),
			TEXT("-c"), TEXT("user.name=UEGitAutomation"),
			TEXT("-c"), TEXT("user.email=uegit@example.invalid")};
		Params.Append(Args);
		TArray<FString> Output, Errors;
		return TestTrue(TEXT("临时 Git 命令成功"), GitSourceControlUtils::RunCommand(
			TEXT("-c"), GitBinary, Root, Params, {}, Output, Errors));
	};
	const FString Selected = FPaths::Combine(Root, TEXT("Content/Selected.txt"));
	const FString Other = FPaths::Combine(Root, TEXT("Content/Other.txt"));
	if (!Run({TEXT("init"), TEXT("-b"), TEXT("scope-test")})
		|| !FFileHelper::SaveStringToFile(TEXT("baseline"), *Selected)
		|| !FFileHelper::SaveStringToFile(TEXT("baseline"), *Other)
		|| !Run({TEXT("add"), TEXT("--"), TEXT("Content")})
		|| !Run({TEXT("commit"), TEXT("-m"), TEXT("baseline")})
		|| !Run({TEXT("config"), TEXT("remote.origin.url"), TEXT(".")})
		|| !Run({TEXT("config"), TEXT("remote.origin.fetch"), TEXT("+refs/heads/*:refs/remotes/origin/*")})
		|| !Run({TEXT("update-ref"), TEXT("refs/remotes/origin/scope-test"), TEXT("HEAD")})
		|| !Run({TEXT("branch"), TEXT("--set-upstream-to=origin/scope-test")})
		|| !FFileHelper::SaveStringToFile(TEXT("local commit"), *Selected)
		|| !FFileHelper::SaveStringToFile(TEXT("local commit"), *Other)
		|| !Run({TEXT("add"), TEXT("--"), TEXT("Content")})
		|| !Run({TEXT("commit"), TEXT("-m"), TEXT("local-change")}))
	{
		return false;
	}
	TMap<FString, FGitSourceControlState> States;
	States.Add(Selected, FGitSourceControlState(Selected));
	States.Add(Other, FGitSourceControlState(Other));
	TArray<FString> Errors;
	const auto OtherBefore = States.FindChecked(Other).State.RemoteState;
	GitSourceControlUtils::CheckRemote(GitBinary, Root, {Selected}, Errors, States);
	TestEqual(TEXT("使用嵌套仓库自己的 upstream，识别目标文件未推送提交"),
		States.FindChecked(Selected).State.RemoteState, ERemoteState::AheadUnpushed);
	TestEqual(TEXT("不扩大到未请求文件"), States.FindChecked(Other).State.RemoteState, OtherBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitSourceControlLiteralPathScopeTest,
	"GitSourceControl.State.LiteralPathScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlLiteralPathScopeTest::RunTest(const FString& Parameters)
{
	const FString GitBinary = GetAutomationTestGitBinary();
	TestTrue(TEXT("路径范围集成测试需要可用 Git"), !GitBinary.IsEmpty());
	if (GitBinary.IsEmpty())
	{
		return false;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	const FString TempRepository = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("Automation"),
			FString::Printf(
				TEXT("UEGitLiteralPath-%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
	TestTrue(
		TEXT("应创建路径范围集成测试仓库"),
		PlatformFile.CreateDirectoryTree(*TempRepository));
	ON_SCOPE_EXIT
	{
		if (PlatformFile.DirectoryExists(*TempRepository))
		{
			PlatformFile.DeleteDirectoryRecursively(*TempRepository);
		}
	};
	if (!PlatformFile.DirectoryExists(*TempRepository))
	{
		return false;
	}

	TArray<FString> Results;
	TArray<FString> Errors;
	TestTrue(
		TEXT("应初始化路径范围集成测试仓库"),
		GitSourceControlUtils::RunCommand(
			TEXT("init"),
			GitBinary,
			TempRepository,
			FGitSourceControlModule::GetEmptyStringArray(),
			FGitSourceControlModule::GetEmptyStringArray(),
			Results,
			Errors));
	Results.Reset();
	Errors.Reset();
	TestTrue(
		TEXT("测试仓库应显式模拟 Git 默认路径转义"),
		GitSourceControlUtils::RunCommand(
			TEXT("config"),
			GitBinary,
			TempRepository,
			TArray<FString>{TEXT("core.quotepath"), TEXT("true")},
			FGitSourceControlModule::GetEmptyStringArray(),
			Results,
			Errors));
	Results.Reset();
	Errors.Reset();
	TestTrue(
		TEXT("测试仓库应设置本地提交身份"),
		GitSourceControlUtils::RunCommand(
			TEXT("config"),
			GitBinary,
			TempRepository,
			TArray<FString>{TEXT("user.name"), TEXT("UEGit Automation")},
			FGitSourceControlModule::GetEmptyStringArray(),
			Results,
			Errors));
	Results.Reset();
	Errors.Reset();
	TestTrue(
		TEXT("测试仓库应设置本地提交邮箱"),
		GitSourceControlUtils::RunCommand(
			TEXT("config"),
			GitBinary,
			TempRepository,
			TArray<FString>{TEXT("user.email"), TEXT("uegit-automation@example.invalid")},
			FGitSourceControlModule::GetEmptyStringArray(),
			Results,
			Errors));
	TestTrue(
		TEXT("测试仓库应显式声明 .uasset 为 lockable"),
		FFileHelper::SaveStringToFile(
			TEXT("*.uasset lockable\n*.umap lockable\n"),
			*FPaths::Combine(TempRepository, TEXT(".gitattributes"))));
	Errors.Reset();
	TestTrue(
		TEXT("Unicode 路径测试应初始化自己的 LFS lockable 规则"),
		GitSourceControlUtils::CheckLFSLockable(
			GitBinary,
			TempRepository,
			{TEXT("*.uasset"), TEXT("*.umap")},
			Errors));

	const FString RelativeTrackedPath = TEXT("Content/中文 已跟踪.uasset");
	const FString AbsoluteTrackedPath = FPaths::Combine(
		TempRepository,
		RelativeTrackedPath);
	TestTrue(
		TEXT("应创建中文已跟踪资产目录"),
		PlatformFile.CreateDirectoryTree(*FPaths::GetPath(AbsoluteTrackedPath)));
	TestTrue(
		TEXT("应创建中文已跟踪资产基线"),
		FFileHelper::SaveStringToFile(
			TEXT("tracked baseline"),
			*AbsoluteTrackedPath));
	Results.Reset();
	Errors.Reset();
	TestTrue(
		TEXT("应把中文已跟踪资产加入基线 index"),
		GitSourceControlUtils::RunCommand(
			TEXT("add"),
			GitBinary,
			TempRepository,
			FGitSourceControlModule::GetEmptyStringArray(),
			TArray<FString>{RelativeTrackedPath},
			Results,
			Errors));
	Results.Reset();
	Errors.Reset();
	TestTrue(
		TEXT("应提交中文已跟踪资产基线"),
		GitSourceControlUtils::RunCommand(
			TEXT("commit"),
			GitBinary,
			TempRepository,
			TArray<FString>{TEXT("-m"), TEXT("literal-path-baseline")},
			FGitSourceControlModule::GetEmptyStringArray(),
			Results,
			Errors));
	TestTrue(
		TEXT("应修改中文已跟踪资产"),
		FFileHelper::SaveStringToFile(
			TEXT("tracked modified"),
			*AbsoluteTrackedPath));

	const FString RelativeAssetPath = TEXT("Content/中文 资产.uasset");
	const FString AbsoluteAssetPath = FPaths::Combine(
		TempRepository,
		RelativeAssetPath);
	TestTrue(
		TEXT("应创建中文和空格路径目录"),
		PlatformFile.CreateDirectoryTree(*FPaths::GetPath(AbsoluteAssetPath)));
	TestTrue(
		TEXT("应创建中文和空格路径资产"),
		FFileHelper::SaveStringToFile(
			TEXT("literal path contract"),
			*AbsoluteAssetPath));
	Results.Reset();
	Errors.Reset();
	TestTrue(
		TEXT("应把中文路径资产加入临时 index"),
		GitSourceControlUtils::RunCommand(
			TEXT("add"),
			GitBinary,
			TempRepository,
			FGitSourceControlModule::GetEmptyStringArray(),
			TArray<FString>{RelativeAssetPath},
			Results,
			Errors));

	const FString ContentDirectory = FPaths::Combine(
		TempRepository,
		TEXT("Content"));
	TArray<FString> DirectoryFiles;
	TestTrue(
		TEXT("production directory expansion 应返回字面中文路径"),
		GitSourceControlUtils::ListFilesInDirectoryRecurse(
			GitBinary,
			TempRepository,
			ContentDirectory,
			DirectoryFiles));
	TestTrue(
		TEXT("directory expansion 应包含中文 Added 资产"),
		DirectoryFiles.ContainsByPredicate(
			[&AbsoluteAssetPath](const FString& File)
			{
				return FPaths::IsSamePath(File, AbsoluteAssetPath);
			}));
	TestTrue(
		TEXT("directory expansion 应包含中文 Modified 资产"),
		DirectoryFiles.ContainsByPredicate(
			[&AbsoluteTrackedPath](const FString& File)
			{
				return FPaths::IsSamePath(File, AbsoluteTrackedPath);
			}));

	Results.Reset();
	Errors.Reset();
	TestTrue(
		TEXT("逐行路径查询应覆盖仓库的 core.quotepath=true"),
		GitSourceControlOperations::RunLiteralPathListCommand(
			TEXT("diff"),
			GitBinary,
			TempRepository,
			TArray<FString>{TEXT("--cached"), TEXT("--name-only"), TEXT("--")},
			Results,
			Errors));
	TestEqual(TEXT("中文路径查询应只返回一个文件"), Results.Num(), 1);
	if (Results.Num() == 1)
	{
		TestEqual(
			TEXT("中文和空格路径必须按字面值返回"),
			Results[0],
			RelativeAssetPath);
		TestTrue(
			TEXT("字面路径必须继续被识别为 lockable 资产"),
			GitSourceControlUtils::IsFileLFSLockable(Results[0]));
	}

	Results.Reset();
	Errors.Reset();
	TestTrue(
		TEXT("production porcelain status 应返回字面中文路径"),
		GitSourceControlUtils::RunStatusWithLiteralPaths(
			GitBinary,
			TempRepository,
			TArray<FString>{TEXT("--porcelain"), TEXT("-uall")},
			TArray<FString>{ContentDirectory},
			Results,
			Errors));
	TestEqual(TEXT("directory production status 应返回 Added 和 Modified"), Results.Num(), 2);
	if (Results.Num() == 2)
	{
		const FGitLockSettingsSnapshot CurrentSettings =
			FGitSourceControlModule::Get()
				.AccessSettings()
				.GetLockSettingsSnapshot();
		TMap<FString, FString> StatusResults;
		for (const FString& StatusLine : Results)
		{
			const FString ParsedAbsolutePath =
				GitSourceControlUtils::GetFullPathFromGitStatus(
					StatusLine,
					TempRepository);
			TestTrue(
				TEXT("production status parser 应恢复目录中的中文绝对路径"),
				FPaths::IsSamePath(ParsedAbsolutePath, AbsoluteAssetPath)
					|| FPaths::IsSamePath(ParsedAbsolutePath, AbsoluteTrackedPath));
			StatusResults.Add(ParsedAbsolutePath, StatusLine);
		}
		TMap<FString, FGitSourceControlState> ParsedStates;
		bool bSettingsSuperseded = false;
		GitSourceControlUtils::ParseStatusResults(
			GitBinary,
			TempRepository,
			false,
			CurrentSettings.bUsingGitLfsLocking,
			CurrentSettings.Generation,
			TArray<FString>{ContentDirectory},
			StatusResults,
			ParsedStates,
			bSettingsSuperseded);
		const FGitSourceControlState* ParsedAddedState = ParsedStates.Find(AbsoluteAssetPath);
		TestNotNull(
			TEXT("directory status parser 应返回中文 Added 资产状态"),
			ParsedAddedState);
		if (ParsedAddedState)
		{
			TestTrue(
				TEXT("中文暂存资产必须保持 Added 分类"),
				ParsedAddedState->IsAdded());
		}
		const FGitSourceControlState* ParsedModifiedState = ParsedStates.Find(AbsoluteTrackedPath);
		TestNotNull(
			TEXT("directory status parser 应返回中文 Modified 资产状态"),
			ParsedModifiedState);
		if (ParsedModifiedState)
		{
			TestTrue(
				TEXT("中文已跟踪资产必须保持 Modified 分类"),
				ParsedModifiedState->IsModified());
		}
	}

	Results.Reset();
	Errors.Reset();
	TestTrue(
		TEXT("Pull 预检 diff 应返回未经转义的中文路径"),
		GitSourceControlUtils::RunCommandWithLiteralPaths(
			TEXT("diff"),
			GitBinary,
			TempRepository,
			TArray<FString>{TEXT("--name-only"), TEXT("HEAD"), TEXT("--")},
			FGitSourceControlModule::GetEmptyStringArray(),
			Results,
			Errors));
	TestTrue(
		TEXT("Pull 预检路径集应包含中文 Added 资产"),
		Results.Contains(RelativeAssetPath));
	TestTrue(
		TEXT("Pull 预检路径集应包含中文 Modified 资产"),
		Results.Contains(RelativeTrackedPath));

	Results.Reset();
	Errors.Reset();
	TestTrue(
		TEXT("remote-state log 应返回未经转义的中文路径"),
		GitSourceControlUtils::RunCommandWithLiteralPaths(
			TEXT("log"),
			GitBinary,
			TempRepository,
			TArray<FString>{TEXT("--pretty="), TEXT("--name-only"), TEXT("HEAD"), TEXT("--")},
			FGitSourceControlModule::GetEmptyStringArray(),
			Results,
			Errors));
	TestTrue(
		TEXT("remote-state 路径集应包含中文已跟踪资产"),
		Results.Contains(RelativeTrackedPath));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitSourceControlAuthoritativeLfsJsonTest,
	"GitSourceControl.State.AuthoritativeLfsJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlAuthoritativeLfsJsonTest::RunTest(const FString& Parameters)
{
	const FString RepositoryRoot = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir(),
		TEXT("Automation/UEGitStrictLfsJson"));
	TMap<FString, FString> Locks;
	FString Error;
	const FString ValidJson =
		TEXT("[{\"id\":\"lock-1\",\"path\":\"Content/中文 资产.uasset\",\"owner\":{\"name\":\"artist\"},\"locked_at\":\"2026-08-06T00:00:00Z\"}]");
	TestTrue(
		TEXT("strict LFS JSON parser accepts a complete canonical listing"),
		GitSourceControlUtils::ParseAuthoritativeLfsLockJson(
			RepositoryRoot,
			ValidJson,
			Locks,
			Error));
	TestEqual(TEXT("strict LFS JSON listing returns exactly one lock"), Locks.Num(), 1);
	const FString ExpectedPath = FPaths::ConvertRelativePathToFull(
		RepositoryRoot,
		TEXT("Content/中文 资产.uasset"));
	const FString* ParsedOwner = Locks.Find(ExpectedPath);
	TestNotNull(TEXT("strict LFS JSON listing preserves the literal asset path"), ParsedOwner);
	if (ParsedOwner)
	{
		TestEqual(TEXT("strict LFS JSON listing preserves explicit owner"), *ParsedOwner, FString(TEXT("artist")));
	}

	auto ExpectRejected = [this, &RepositoryRoot](
		const TCHAR* Label,
		const FString& Json)
	{
		TMap<FString, FString> RejectedLocks;
		FString Rejection;
		TestFalse(
			Label,
			GitSourceControlUtils::ParseAuthoritativeLfsLockJson(
				RepositoryRoot,
				Json,
				RejectedLocks,
				Rejection));
		TestTrue(
			*FString::Printf(TEXT("%s returns no partial authorization"), Label),
			RejectedLocks.IsEmpty());
		TestTrue(
			*FString::Printf(TEXT("%s remains diagnosable"), Label),
			!Rejection.IsEmpty());
	};
	ExpectRejected(
		TEXT("truncated LFS JSON listing is rejected"),
		TEXT("[{\"id\":\"lock-1\""));
	ExpectRejected(
		TEXT("non-array LFS JSON listing is rejected"),
		TEXT("{\"locks\":[]}"));
	ExpectRejected(
		TEXT("missing owner object is rejected"),
		TEXT("[{\"id\":\"lock-1\",\"path\":\"Content/A.uasset\"}]"));
	ExpectRejected(
		TEXT("empty owner is rejected instead of inferred as current user"),
		TEXT("[{\"id\":\"lock-1\",\"path\":\"Content/A.uasset\",\"owner\":{\"name\":\"\"}}]"));
	ExpectRejected(
		TEXT("unsafe repository-relative path is rejected"),
		TEXT("[{\"id\":\"lock-1\",\"path\":\"../Outside.uasset\",\"owner\":{\"name\":\"artist\"}}]"));
	ExpectRejected(
		TEXT("duplicate lock path rejects the whole listing"),
		TEXT("[{\"id\":\"lock-1\",\"path\":\"Content/A.uasset\",\"owner\":{\"name\":\"artist\"}},{\"id\":\"lock-2\",\"path\":\"Content/A.uasset\",\"owner\":{\"name\":\"other\"}}]"));
	ExpectRejected(
		TEXT("duplicate lock id rejects the whole listing"),
		TEXT("[{\"id\":\"lock-1\",\"path\":\"Content/A.uasset\",\"owner\":{\"name\":\"artist\"}},{\"id\":\"lock-1\",\"path\":\"Content/B.uasset\",\"owner\":{\"name\":\"artist\"}}]"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitSourceControlModifiedPresentationTest,
	"GitSourceControl.State.ModifiedPresentation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlModifiedPresentationTest::RunTest(const FString& Parameters)
{
	FGitSourceControlState State(TEXT("Content/ModifiedPresentation.uasset"));
	State.State.FileState = EFileState::Modified;
	State.State.TreeState = ETreeState::Working;
	State.State.RemoteState = ERemoteState::UpToDate;

	State.State.LockState = ELockState::NotLocked;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
	TestEqual(
		TEXT("未持锁的本地修改应使用 ModifiedLocally 图标"),
		State.GetIcon().GetStyleName(),
		FName(TEXT("RevisionControl.ModifiedLocally")));
#endif
	TestEqual(
		TEXT("未持锁的本地修改应明确显示未 Checkout"),
		State.GetDisplayName().ToString(),
		FString(TEXT("本地已修改，未 Checkout")));
	TestTrue(
		TEXT("未持锁的本地修改应提示补取 LFS 锁"),
		State.GetDisplayTooltip().ToString().Contains(TEXT("没有持有 LFS 锁"))
			&& State.GetDisplayTooltip().ToString().Contains(TEXT("Check Out")));
	TestTrue(TEXT("未持锁的本地修改仍应提供 Checkout 入口"), State.CanCheckout());
	TestFalse(TEXT("未持锁的本地修改不应报告为 Checkout"), State.IsCheckedOut());
	TestTrue(TEXT("未持锁的既有本地修改仍应允许继续编辑"), State.CanEdit());
	TestFalse(
		TEXT("未持锁的本地修改不应进入 Checked Out Filter"),
		State.IsCheckedOut() || State.IsAdded());

	State.State.LockState = ELockState::Locked;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
	TestEqual(
		TEXT("当前用户持锁的本地修改应保留绿色 CheckedOut 图标"),
		State.GetIcon().GetStyleName(),
		FName(TEXT("RevisionControl.CheckedOut")));
#endif
	TestEqual(
		TEXT("当前用户持锁的本地修改应显示已 Checkout"),
		State.GetDisplayName().ToString(),
		FString(TEXT("已 Checkout（本地已修改）")));
	TestFalse(TEXT("当前用户持锁后不应重复提供 Checkout"), State.CanCheckout());
	TestTrue(TEXT("当前用户持锁时应报告为 Checkout"), State.IsCheckedOut());
	TestTrue(
		TEXT("当前用户持锁时应进入 Checked Out Filter"),
		State.IsCheckedOut() || State.IsAdded());

	State.State.LockState = ELockState::Unlockable;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
	TestEqual(
		TEXT("非锁模式的本地修改也应使用 ModifiedLocally 图标"),
		State.GetIcon().GetStyleName(),
		FName(TEXT("RevisionControl.ModifiedLocally")));
#endif
	TestEqual(
		TEXT("非锁模式不应声称缺少 Checkout"),
		State.GetDisplayName().ToString(),
		FString(TEXT("本地已修改")));
	TestTrue(
		TEXT("非锁模式以 Unlockable 兼容投影满足 UE no-checkout consumers"),
		State.IsCheckedOut());
	TestFalse(
		TEXT("Unlockable 兼容投影不得伪造远端锁所有权"),
		State.HasVerifiedOwnLock());
	TestTrue(
		TEXT("UE Checked Out Filter 对 no-checkout provider 保留 Unlockable 兼容例外"),
		State.IsCheckedOut() || State.IsAdded());
	TestTrue(TEXT("非锁模式的已跟踪文件仍应允许编辑"), State.CanEdit());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitSourceControlMutationBatchBoundaryTest,
	"GitSourceControl.State.MutationBatchBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlMutationBatchBoundaryTest::RunTest(const FString& Parameters)
{
	const FString GitBinary = GetAutomationTestGitBinary();
	TestTrue(TEXT("写边界集成测试需要可用 Git"), !GitBinary.IsEmpty());
	if (GitBinary.IsEmpty())
	{
		return false;
	}

	IPlatformFile& PlatformFile =
		FPlatformFileManager::Get().GetPlatformFile();
	const FString TempRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("Automation"),
			FString::Printf(
				TEXT("UEGitMutationBoundary-%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
	TestTrue(
		TEXT("应创建写边界集成测试目录"),
		PlatformFile.CreateDirectoryTree(*TempRoot));
	ON_SCOPE_EXIT
	{
		if (PlatformFile.DirectoryExists(*TempRoot))
		{
			PlatformFile.DeleteDirectoryRecursively(*TempRoot);
		}
	};
	if (!PlatformFile.DirectoryExists(*TempRoot))
	{
		return false;
	}

	auto InitializeRepository = [
		this,
		&GitBinary
	](const FString& InRepositoryRoot)
	{
		TArray<FString> Results;
		TArray<FString> Errors;
		if (!GitSourceControlUtils::RunCommand(
				TEXT("init"),
				GitBinary,
				InRepositoryRoot,
				FGitSourceControlModule::GetEmptyStringArray(),
				FGitSourceControlModule::GetEmptyStringArray(),
				Results,
				Errors))
		{
			AddError(TEXT("无法初始化写边界测试仓库：") + FString::Join(Errors, TEXT(" | ")));
			return false;
		}
		Results.Reset();
		Errors.Reset();
		if (!GitSourceControlUtils::RunCommand(
				TEXT("config"),
				GitBinary,
				InRepositoryRoot,
				{TEXT("user.name"), TEXT("UEGitBoundaryAutomation")},
				FGitSourceControlModule::GetEmptyStringArray(),
				Results,
				Errors))
		{
			AddError(TEXT("无法设置写边界测试提交用户：") + FString::Join(Errors, TEXT(" | ")));
			return false;
		}
		Results.Reset();
		Errors.Reset();
		if (!GitSourceControlUtils::RunCommand(
				TEXT("config"),
				GitBinary,
				InRepositoryRoot,
				{TEXT("user.email"), TEXT("uegit-boundary@example.invalid")},
				FGitSourceControlModule::GetEmptyStringArray(),
				Results,
				Errors))
		{
			AddError(TEXT("无法设置写边界测试提交邮箱：") + FString::Join(Errors, TEXT(" | ")));
			return false;
		}
		return true;
	};

	// 51 个文件强制进入 50 + 1 两批。第二次边界拒绝后，最后一批不能启动。
	// Fifty-one files force a 50 + 1 split. Rejecting the second boundary must prevent the final
	// batch from starting.
	const FString BatchRepository = FPaths::Combine(TempRoot, TEXT("Batch"));
	TestTrue(
		TEXT("应创建批处理测试仓库"),
		PlatformFile.CreateDirectoryTree(*BatchRepository));
	if (!InitializeRepository(BatchRepository))
	{
		return false;
	}
	TArray<FString> BatchFiles;
	for (int32 FileIndex = 0; FileIndex < 51; ++FileIndex)
	{
		const FString RelativeFile = FString::Printf(
			TEXT("Content/Batch/Item_%02d.txt"),
			FileIndex);
		const FString AbsoluteFile = FPaths::Combine(
			BatchRepository,
			RelativeFile);
		TestTrue(
			TEXT("应创建批处理测试文件目录"),
			PlatformFile.CreateDirectoryTree(*FPaths::GetPath(AbsoluteFile)));
		TestTrue(
			TEXT("应创建批处理测试文件"),
			FFileHelper::SaveStringToFile(
				FString::FromInt(FileIndex),
				*AbsoluteFile));
		BatchFiles.Add(RelativeFile);
	}

	TArray<FString> Results;
	TArray<FString> Errors;
	int32 BatchBoundaryChecks = 0;
	const bool bBatchMutationSucceeded =
		GitSourceControlUtils::RunCommandWithPreWriteBoundary(
			TEXT("add"),
			GitBinary,
			BatchRepository,
			FGitSourceControlModule::GetEmptyStringArray(),
			BatchFiles,
			[&BatchBoundaryChecks]()
			{
				++BatchBoundaryChecks;
				return BatchBoundaryChecks == 1;
			},
			Results,
			Errors);
	TestFalse(TEXT("第二批写边界拒绝后 add 应返回失败"), bBatchMutationSucceeded);
	TestEqual(TEXT("每一批前都应重新调用写边界"), BatchBoundaryChecks, 2);

	Results.Reset();
	Errors.Reset();
	TestTrue(
		TEXT("应查询边界拒绝后的 index"),
		GitSourceControlUtils::RunCommandWithLiteralPaths(
			TEXT("diff"),
			GitBinary,
			BatchRepository,
			{TEXT("--cached"), TEXT("--name-only"), TEXT("--")},
			FGitSourceControlModule::GetEmptyStringArray(),
			Results,
			Errors));
	TestEqual(TEXT("只有第一批 50 个文件可进入 index"), Results.Num(), 50);
	for (int32 FileIndex = 0; FileIndex < 50; ++FileIndex)
	{
		TestTrue(
			TEXT("第一批文件应已暂存"),
			Results.Contains(BatchFiles[FileIndex]));
	}
	TestFalse(
		TEXT("被拒绝的第二批文件不得进入 index"),
		Results.Contains(BatchFiles.Last()));

	// 年轻 index.lock 会让同一批内部重启 Git；第二次尝试也必须重新通过边界。
	// A young index.lock makes the same batch restart Git internally. The second attempt must pass
	// the boundary again as well.
	const FString RetryRepository = FPaths::Combine(TempRoot, TEXT("Retry"));
	TestTrue(
		TEXT("应创建 index.lock 重试测试仓库"),
		PlatformFile.CreateDirectoryTree(*RetryRepository));
	if (!InitializeRepository(RetryRepository))
	{
		return false;
	}
	const FString RetryRelativeFile = TEXT("Content/Retry.txt");
	const FString RetryAbsoluteFile = FPaths::Combine(
		RetryRepository,
		RetryRelativeFile);
	TestTrue(
		TEXT("应创建 index.lock 重试文件目录"),
		PlatformFile.CreateDirectoryTree(*FPaths::GetPath(RetryAbsoluteFile)));
	TestTrue(
		TEXT("应创建 index.lock 重试文件"),
		FFileHelper::SaveStringToFile(TEXT("retry"), *RetryAbsoluteFile));
	const FString YoungIndexLock = FPaths::Combine(
		RetryRepository,
		TEXT(".git/index.lock"));
	TestTrue(
		TEXT("应创建年轻 index.lock 竞争夹具"),
		FFileHelper::SaveStringToFile(TEXT("live contention"), *YoungIndexLock));

	Results.Reset();
	Errors.Reset();
	int32 RetryBoundaryChecks = 0;
	const bool bRetryMutationSucceeded =
		GitSourceControlUtils::RunCommandWithPreWriteBoundary(
			TEXT("add"),
			GitBinary,
			RetryRepository,
			FGitSourceControlModule::GetEmptyStringArray(),
			{RetryRelativeFile},
			[&RetryBoundaryChecks]()
			{
				++RetryBoundaryChecks;
				return RetryBoundaryChecks == 1;
			},
			Results,
			Errors);
	TestFalse(
		TEXT("index.lock 后的第二次写边界拒绝应停止重试"),
		bRetryMutationSucceeded);
	TestEqual(
		TEXT("同一批的 index.lock 重试也应再次调用写边界"),
		RetryBoundaryChecks,
		2);
	TestFalse(
		TEXT("被拒绝的 index.lock 重试不得创建 Git index"),
		PlatformFile.FileExists(
			*FPaths::Combine(RetryRepository, TEXT(".git/index"))));

	// Commit helper 自己含有 add 与 commit 两个写子进程；第二次拒绝必须留下 staged 文件且无 HEAD。
	// RunCommit owns an add and a commit subprocess. Rejecting the second boundary must leave the
	// file staged without creating HEAD.
	const FString CommitRepository = FPaths::Combine(TempRoot, TEXT("Commit"));
	TestTrue(
		TEXT("应创建 commit 写边界测试仓库"),
		PlatformFile.CreateDirectoryTree(*CommitRepository));
	if (!InitializeRepository(CommitRepository))
	{
		return false;
	}
	const FString CommitRelativeFile = TEXT("Content/Commit.txt");
	const FString CommitAbsoluteFile = FPaths::Combine(
		CommitRepository,
		CommitRelativeFile);
	TestTrue(
		TEXT("应创建 commit 测试文件目录"),
		PlatformFile.CreateDirectoryTree(*FPaths::GetPath(CommitAbsoluteFile)));
	TestTrue(
		TEXT("应创建 commit 测试文件"),
		FFileHelper::SaveStringToFile(TEXT("boundary"), *CommitAbsoluteFile));

	Results.Reset();
	Errors.Reset();
	int32 CommitBoundaryChecks = 0;
	const bool bCommitSucceeded = GitSourceControlUtils::RunCommit(
		GitBinary,
		CommitRepository,
		{TEXT("-m"), TEXT("must-not-commit")},
		{CommitRelativeFile},
		[&CommitBoundaryChecks]()
		{
			++CommitBoundaryChecks;
			return CommitBoundaryChecks == 1;
		},
		Results,
		Errors);
	TestFalse(TEXT("commit 子进程边界拒绝后 RunCommit 应失败"), bCommitSucceeded);
	TestEqual(TEXT("RunCommit 应在 add 与 commit 前分别复核"), CommitBoundaryChecks, 2);

	Results.Reset();
	Errors.Reset();
	TestTrue(
		TEXT("应查询 commit 拒绝后的 index"),
		GitSourceControlUtils::RunCommandWithLiteralPaths(
			TEXT("diff"),
			GitBinary,
			CommitRepository,
			{TEXT("--cached"), TEXT("--name-only"), TEXT("--")},
			FGitSourceControlModule::GetEmptyStringArray(),
			Results,
			Errors));
	TestEqual(TEXT("commit 被拒绝前 add 已执行一次"), Results.Num(), 1);
	if (Results.Num() == 1)
	{
		TestEqual(TEXT("暂存文件应是 commit fixture"), Results[0], CommitRelativeFile);
	}

	FString HeadContents;
	TestTrue(
		TEXT("应读取未提交仓库 HEAD symbolic ref"),
		FFileHelper::LoadFileToString(
			HeadContents,
			*FPaths::Combine(CommitRepository, TEXT(".git/HEAD"))));
	HeadContents.TrimStartAndEndInline();
	const FString HeadPrefix = TEXT("ref: ");
	TestTrue(TEXT("新仓库 HEAD 应为 symbolic ref"), HeadContents.StartsWith(HeadPrefix));
	if (HeadContents.StartsWith(HeadPrefix))
	{
		const FString HeadRef = HeadContents.RightChop(HeadPrefix.Len());
		TestFalse(
			TEXT("commit 边界拒绝后不得创建分支 HEAD"),
			PlatformFile.FileExists(
				*FPaths::Combine(CommitRepository, TEXT(".git"), HeadRef)));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
