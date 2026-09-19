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

#endif // WITH_DEV_AUTOMATION_TESTS
