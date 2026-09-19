// Copyright (c) 2026 LunarMaxim
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt).

#include "Runtime/Launch/Resources/Version.h"

#if WITH_DEV_AUTOMATION_TESTS && ENGINE_MAJOR_VERSION == 5

#include "GitSourceControlChangelistState.h"
#include "GitSourceControlCommand.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlOperations.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlState.h"
#include "GitSourceControlUtils.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SourceControlOperations.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitIncrementalChangelistTest,
	"GitSourceControl.Performance.IncrementalChangelists",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitIncrementalChangelistTest::RunTest(const FString& Parameters)
{
	FGitSourceControlProvider Provider;
	const auto Staged = Provider.GetStateInternal(FGitSourceControlChangelist::StagedChangelist);
	const auto Working = Provider.GetStateInternal(FGitSourceControlChangelist::WorkingChangelist);
	const auto Asset = Provider.GetStateInternal(TEXT("Content/Animation.uasset"));
	const auto PluginAsset = Provider.GetStateInternal(TEXT("Plugins/Gameplay/Content/Animation.uasset"));
	const auto SubmoduleAsset = Provider.GetStateInternal(TEXT("Plugins/Submodule/Content/Other.uasset"));
	PluginAsset->State.TreeState = ETreeState::Staged;
	SubmoduleAsset->State.TreeState = ETreeState::Working;
	Provider.UpdateChangelistState(PluginAsset);
	Provider.UpdateChangelistState(SubmoduleAsset);

	Asset->State.FileState = EFileState::Added;
	Asset->State.TreeState = ETreeState::Staged;
	Asset->State.LockState = ELockState::LockedOther;
	Asset->State.RemoteState = ERemoteState::NotAtHead;
	Provider.UpdateChangelistState(Asset);
	Provider.UpdateChangelistState(Asset);
	TestEqual(TEXT("重复刷新不得重复加入暂存列表"), Staged->Files.Num(), 2);
	TestTrue(TEXT("插件内容也应记录暂存归属，供保存后重新暂存"),
		PluginAsset->Changelist == FGitSourceControlChangelist::StagedChangelist);
	TestTrue(TEXT("更新列表不能修改锁状态"), Asset->State.LockState == ELockState::LockedOther);
	TestTrue(TEXT("更新列表不能修改远端状态"), Asset->State.RemoteState == ERemoteState::NotAtHead);

	Asset->State.TreeState = ETreeState::Working;
	Provider.UpdateChangelistState(Asset);
	TestEqual(TEXT("取消暂存后应移出 Staged"), Staged->Files.Num(), 1);
	TestEqual(TEXT("取消暂存后应进入 Working"), Working->Files.Num(), 2);
	Asset->State.TreeState = ETreeState::Unset;
	Provider.UpdateChangelistState(Asset);
	TestTrue(TEXT("无新鲜本地状态时不得清空原归属"), Working->Files.Contains(Asset));

	for (const ETreeState::Type TreeState : {ETreeState::Unmodified, ETreeState::Ignored, ETreeState::NotInRepo})
	{
		Asset->State.TreeState = ETreeState::Staged;
		Provider.UpdateChangelistState(Asset);
		Asset->State.TreeState = TreeState;
		Provider.UpdateChangelistState(Asset);
		TestFalse(TEXT("干净、忽略或已消失的文件应移出暂存列表"), Staged->Files.Contains(Asset));
		TestFalse(TEXT("干净、忽略或已消失的文件应移出工作列表"), Working->Files.Contains(Asset));
		TestTrue(TEXT("旧归属必须清除，避免后续保存错误地自动暂存"), Asset->Changelist.GetName().IsEmpty());
	}
	Asset->State.TreeState = ETreeState::Untracked;
	Provider.UpdateChangelistState(Asset);
	TestTrue(TEXT("未跟踪文件应进入 Working，不能因 ?? 被误分到 Staged"), Working->Files.Contains(Asset));
	Asset->State.FileState = EFileState::Deleted;
	Asset->State.TreeState = ETreeState::Staged;
	Provider.UpdateChangelistState(Asset);
	TestTrue(TEXT("已暂存删除应留在 Staged"), Staged->Files.Contains(Asset));
	Asset->State.FileState = EFileState::Unmerged;
	Asset->State.TreeState = ETreeState::Working;
	Provider.UpdateChangelistState(Asset);
	TestTrue(TEXT("冲突应保留在 Working"), Working->Files.Contains(Asset));
	TestTrue(TEXT("局部刷新不应清掉插件文件"), Staged->Files.Contains(PluginAsset));
	TestTrue(TEXT("局部刷新不应清掉子模块文件"), Working->Files.Contains(SubmoduleAsset));
	TestEqual(TEXT("主线程快照应覆盖两个列表且无重复"), Provider.GetFilesInChangelists().Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitScopedRefreshTest,
	"GitSourceControl.Performance.ScopedRefresh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitScopedRefreshTest::RunTest(const FString& Parameters)
{
	FString GitBinary = FGitSourceControlModule::Get().GetProvider().GetGitBinaryPath();
	if (GitBinary.IsEmpty())
	{
		GitBinary = GitSourceControlUtils::FindGitBinaryPath();
	}
	if (!TestFalse(TEXT("集成测试需要可用 Git"), GitBinary.IsEmpty()))
	{
		return false;
	}
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("Automation"), TEXT("GitScopedRefresh-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestTrue(TEXT("创建独立临时仓库"), PlatformFile.CreateDirectoryTree(*Root)))
	{
		return false;
	}
	ON_SCOPE_EXIT { PlatformFile.DeleteDirectoryRecursively(*Root); };

	auto Git = [&](const FString& Command, const TArray<FString>& Args)
	{
		TArray<FString> Results, Errors;
		return TestTrue(*FString::Printf(TEXT("临时仓库 Git %s"), *Command),
			GitSourceControlUtils::RunCommand(Command, GitBinary, Root, Args, {}, Results, Errors));
	};
	auto Write = [&](const FString& RelativePath, const FString& Text)
	{
		const FString File = FPaths::Combine(Root, RelativePath);
		return TestTrue(TEXT("创建测试文件"), PlatformFile.CreateDirectoryTree(*FPaths::GetPath(File))
			&& FFileHelper::SaveStringToFile(Text, *File));
	};
	const FString Scope = FPaths::Combine(Root, TEXT("Content/Inside"));
	const FString Modified = FPaths::Combine(Scope, TEXT("Modified.txt"));
	const FString Added = FPaths::Combine(Scope, TEXT("Added.txt"));
	const FString Deleted = FPaths::Combine(Scope, TEXT("Deleted.txt"));
	const FString Gone = FPaths::Combine(Scope, TEXT("Gone.txt"));
	const FString Ignored = FPaths::Combine(Scope, TEXT("Ignored.txt"));
	const FString Outside = FPaths::Combine(Root, TEXT("Content/Outside/Other.txt"));
	const FString PluginFile = FPaths::Combine(Root, TEXT("Plugins/Gameplay/Content/New.txt"));
	const FString NestedRoot = FPaths::Combine(Root, TEXT("Plugins/Nested"));
	const FString NestedFile = FPaths::Combine(NestedRoot, TEXT("Content/Nested.txt"));
	if (!Git(TEXT("init"), {})
		|| !Git(TEXT("config"), {TEXT("user.name"), TEXT("UEGitAutomation")})
		|| !Git(TEXT("config"), {TEXT("user.email"), TEXT("uegit-automation@example.invalid")})
		|| !Git(TEXT("config"), {TEXT("commit.gpgsign"), TEXT("false")})
		|| !Git(TEXT("config"), {TEXT("core.hooksPath"), TEXT(".git/no-hooks")})
		|| !Write(TEXT("Content/Inside/Modified.txt"), TEXT("baseline"))
		|| !Write(TEXT("Content/Inside/Deleted.txt"), TEXT("baseline"))
		|| !Git(TEXT("add"), {TEXT("Content")})
		|| !Git(TEXT("commit"), {TEXT("-m"), TEXT("baseline")}))
	{
		return false;
	}
	if (!Write(TEXT("Content/Inside/Modified.txt"), TEXT("staged modification"))
		|| !Write(TEXT("Content/Inside/Added.txt"), TEXT("staged addition"))
		|| !Write(TEXT("Content/Outside/Other.txt"), TEXT("outside scope"))
		|| !Write(TEXT("Plugins/Gameplay/Content/New.txt"), TEXT("plugin content"))
		|| !Git(TEXT("add"), {TEXT("Content"), TEXT("Plugins")})
		|| !Write(TEXT("Content/Inside/Modified.txt"), TEXT("MM working copy"))
		|| !Write(TEXT("Content/Inside/Added.txt"), TEXT("AM working copy"))
		|| !Write(TEXT("Content/Inside/Ignored.txt"), TEXT("previously untracked"))
		|| !Write(TEXT(".gitignore"), TEXT("Content/Inside/Ignored.txt\n"))
		|| !Git(TEXT("rm"), {TEXT("Content/Inside/Deleted.txt")})
		|| !Write(TEXT("Plugins/Nested/Content/Nested.txt"), TEXT("separate repository"))
		|| !Git(TEXT("init"), {TEXT("Plugins/Nested")}))
	{
		return false;
	}

	const TArray<FString> KnownFiles{Modified, Added, Deleted, Gone, Ignored, Outside, PluginFile, NestedFile};
	auto Configure = [&](FGitSourceControlCommand& Command, const TArray<FString>& Files)
	{
		Command.PathToGitBinary = GitBinary;
		Command.PathToRepositoryRoot = Root;
		Command.PathToGitRoot = Root;
		// 临时文本仓库只验证本地状态；不访问用户锁服务，也不改真实 Provider 的设置。
		// The temporary text repo tests local status only, without remote locks or provider setting changes.
		Command.bUsingGitLfsLocking = false;
		Command.KnownChangelistFiles = KnownFiles;
		Command.Files = Files;
	};
	auto Refresh = [&](const TArray<FString>& Files, TMap<const FString, FGitState>& States)
	{
		const auto Worker = MakeShared<FGitUpdateStatusWorker, ESPMode::ThreadSafe>();
		FGitSourceControlCommand Command(ISourceControlOperation::Create<FUpdateStatus>(), Worker);
		Configure(Command, Files);
		const bool bSucceeded = Worker->Execute(Command);
		States = Worker->States;
		return bSucceeded;
	};
	TMap<const FString, FGitState> States;
	auto ExpectTree = [&](const TCHAR* Label, const FString& File, ETreeState::Type Expected)
	{
		const FGitState* State = States.Find(File);
		return TestNotNull(Label, State) && TestTrue(Label, State->TreeState == Expected);
	};
	if (!TestTrue(TEXT("定向状态刷新成功"), Refresh({Added}, States)))
	{
		return false;
	}
	TestEqual(TEXT("单文件查询不能扩大为整个 Content 或已知列表"), States.Num(), 1);
	ExpectTree(TEXT("AM 仍属于 Staged"), Added, ETreeState::Staged);

	if (!TestTrue(TEXT("目录刷新成功"), Refresh({Scope}, States)))
	{
		return false;
	}
	ExpectTree(TEXT("MM 仍属于 Staged"), Modified, ETreeState::Staged);
	const FGitState* DeletedState = States.Find(Deleted);
	if (TestNotNull(TEXT("已暂存删除必须返回状态"), DeletedState))
	{
		TestTrue(TEXT("已暂存删除必须保留"), DeletedState->FileState == EFileState::Deleted);
	}
	ExpectTree(TEXT("已经消失的旧列表项必须产生清理状态"), Gone, ETreeState::NotInRepo);
	ExpectTree(TEXT("被忽略的旧列表项不能冒充已跟踪 clean"), Ignored, ETreeState::Ignored);
	TestFalse(TEXT("不能触及相邻目录"), States.Contains(Outside));
	TestFalse(TEXT("不能触及范围外插件内容"), States.Contains(PluginFile));

	if (!TestTrue(TEXT("插件目录刷新成功"), Refresh({FPaths::Combine(Root, TEXT("Plugins"))}, States)))
	{
		return false;
	}
	TestTrue(TEXT("非子模块插件内容必须可见"), States.Contains(PluginFile));
	TestFalse(TEXT("主仓目录刷新不能用主仓结果清理子模块内部路径"), States.Contains(NestedFile));

	const auto ListWorker = MakeShared<FGitUpdateStagingWorker, ESPMode::ThreadSafe>();
	FGitSourceControlCommand ListCommand(ISourceControlOperation::Create<FUpdatePendingChangelistsStatus>(), ListWorker);
	Configure(ListCommand, {PluginFile});
	TestTrue(TEXT("显式变更列表刷新使用相同定向查询"), ListWorker->Execute(ListCommand));
	TestEqual(TEXT("显式刷新只发布请求的文件"), ListWorker->States.Num(), 1);

	const auto FailedWorker = MakeShared<FGitUpdateStatusWorker, ESPMode::ThreadSafe>();
	FGitSourceControlCommand FailedCommand(ISourceControlOperation::Create<FUpdateStatus>(), FailedWorker);
	Configure(FailedCommand, {Scope});
	FailedCommand.PathToGitBinary = FPaths::Combine(Root, TEXT("missing-git-executable"));
	TestFalse(TEXT("查询失败必须返回失败"), FailedWorker->Execute(FailedCommand));
	TestTrue(TEXT("失败不能发布空白或部分状态来清除旧列表"), FailedWorker->States.IsEmpty());
	return true;
}

#endif
