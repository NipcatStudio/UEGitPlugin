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

#endif // WITH_DEV_AUTOMATION_TESTS
