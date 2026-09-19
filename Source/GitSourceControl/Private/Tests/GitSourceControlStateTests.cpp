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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitSourceControlStateContractMatrixTest,
	"GitSourceControl.State.ContractMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlStateContractMatrixTest::RunTest(const FString& Parameters)
{
	const TArray<FGitStateContractCase> Cases = {
		{ TEXT("OrdinaryGitTrackedClean"), EFileState::Unknown, ETreeState::Unmodified, ELockState::Unlockable, ERemoteState::UpToDate, true, false, true, false, false, false, true },
		{ TEXT("LfsCleanNotLocked"), EFileState::Unknown, ETreeState::Unmodified, ELockState::NotLocked, ERemoteState::UpToDate, false, false, false, true, false, false, false },
		{ TEXT("OfflineModifiedNotLocked"), EFileState::Modified, ETreeState::Working, ELockState::NotLocked, ERemoteState::UpToDate, false, false, true, true, true, true, false },
		{ TEXT("OwnLockClean"), EFileState::Unknown, ETreeState::Unmodified, ELockState::Locked, ERemoteState::UpToDate, true, false, true, false, true, true, true },
		{ TEXT("OwnLockModified"), EFileState::Modified, ETreeState::Working, ELockState::Locked, ERemoteState::UpToDate, true, false, true, false, true, true, true },
		{ TEXT("OtherLockClean"), EFileState::Unknown, ETreeState::Unmodified, ELockState::LockedOther, ERemoteState::UpToDate, false, true, false, false, false, false, false },
		{ TEXT("OtherLockModified"), EFileState::Modified, ETreeState::Working, ELockState::LockedOther, ERemoteState::UpToDate, false, true, false, false, false, true, false },
		{ TEXT("AddedNotLocked"), EFileState::Added, ETreeState::Staged, ELockState::NotLocked, ERemoteState::UpToDate, false, false, true, true, true, true, true },
		{ TEXT("AddedUnlockable"), EFileState::Added, ETreeState::Staged, ELockState::Unlockable, ERemoteState::UpToDate, false, false, true, false, true, true, true },
		{ TEXT("AddedLockedOther"), EFileState::Added, ETreeState::Staged, ELockState::LockedOther, ERemoteState::UpToDate, false, true, false, false, false, true, true },
		{ TEXT("AddedUnknownLock"), EFileState::Added, ETreeState::Staged, ELockState::Unknown, ERemoteState::UpToDate, false, false, false, false, false, true, true },
		{ TEXT("UnknownOutsideRepo"), EFileState::Unknown, ETreeState::NotInRepo, ELockState::Unknown, ERemoteState::UpToDate, false, false, false, false, false, false, false },
		{ TEXT("Untracked"), EFileState::Unknown, ETreeState::Untracked, ELockState::Unknown, ERemoteState::UpToDate, false, false, false, false, false, false, false },
		{ TEXT("Ignored"), EFileState::Unknown, ETreeState::Ignored, ELockState::Unknown, ERemoteState::UpToDate, false, false, false, false, false, false, false },
		{ TEXT("NotCurrentClean"), EFileState::Unknown, ETreeState::Unmodified, ELockState::NotLocked, ERemoteState::NotAtHead, false, false, false, false, false, false, false },
		{ TEXT("NotCurrentModified"), EFileState::Modified, ETreeState::Working, ELockState::NotLocked, ERemoteState::NotAtHead, false, false, true, false, false, true, false },
		{ TEXT("ConflictedModified"), EFileState::Unmerged, ETreeState::Working, ELockState::NotLocked, ERemoteState::UpToDate, false, false, true, true, false, true, false },
		{ TEXT("CommittedUnpushedOwnLock"), EFileState::Unknown, ETreeState::Unmodified, ELockState::Locked, ERemoteState::AheadUnpushed, true, false, true, false, false, false, true },
		{ TEXT("TrackedUnknownLock"), EFileState::Unknown, ETreeState::Unmodified, ELockState::Unknown, ERemoteState::UpToDate, false, false, false, false, false, false, false },
		{ TEXT("ModifiedUnknownLock"), EFileState::Modified, ETreeState::Working, ELockState::Unknown, ERemoteState::UpToDate, false, false, false, false, false, true, false },
	};

	for (const FGitStateContractCase& Case : Cases)
	{
		const FGitSourceControlState State = MakeState(
			Case.FileState,
			Case.TreeState,
			Case.LockState,
			Case.RemoteState);
		const FString Prefix = FString::Printf(TEXT("%s: "), Case.Label);
		TestEqual(*(Prefix + TEXT("IsCheckedOut")), State.IsCheckedOut(), Case.bCheckedOut);
		TestEqual(
			*(Prefix + TEXT("verified remote ownership")),
			State.HasVerifiedOwnLock(),
			Case.LockState == ELockState::Locked);
		TestEqual(*(Prefix + TEXT("IsCheckedOutOther")), State.IsCheckedOutOther(), Case.bCheckedOutOther);
		TestEqual(*(Prefix + TEXT("CanEdit")), State.CanEdit(), Case.bCanEdit);
		TestEqual(*(Prefix + TEXT("CanCheckout")), State.CanCheckout(), Case.bCanCheckout);
		TestEqual(*(Prefix + TEXT("CanCheckIn")), State.CanCheckIn(), Case.bCanCheckIn);
		TestEqual(*(Prefix + TEXT("CanRevert")), State.CanRevert(), Case.bCanRevert);
		TestEqual(
			*(Prefix + TEXT("Checked Out Filter projection")),
			State.IsCheckedOut() || State.IsAdded(),
			Case.bPassesCheckedOutFilter);
	}

	// Count 哨兵让新增锁状态自动进入三组矩阵；若没有明确加入白名单，新状态会按失败关闭断言。
	// The Count sentinel puts every future lock state into all three matrices automatically. A new
	// state fails closed unless its capability is deliberately added to the allowlist.
	for (int32 LockValue = static_cast<int32>(ELockState::Unset);
		LockValue < static_cast<int32>(ELockState::Count);
		++LockValue)
	{
		const ELockState::Type LockState = static_cast<ELockState::Type>(LockValue);
		const bool bExpectedOwnLock = LockState == ELockState::Locked;
		const bool bExpectedNoCheckoutAdapter = LockState == ELockState::Unlockable;
		const bool bExpectedCheckedOutProjection = bExpectedOwnLock
			|| bExpectedNoCheckoutAdapter;
		const bool bExpectedOtherLock = LockState == ELockState::LockedOther;
		const bool bExpectedLocalCapability = bExpectedOwnLock
			|| LockState == ELockState::NotLocked
			|| LockState == ELockState::Unlockable;
		const FString Prefix = FString::Printf(TEXT("Lock enum %d: "), LockValue);

		FGitSourceControlState ModifiedState = MakeState(
			EFileState::Modified,
			ETreeState::Working,
			LockState);
		const bool bExpectedModifiedCheckout = LockState == ELockState::NotLocked;
		TestEqual(
			*(Prefix + TEXT("modified UE checkout projection")),
			ModifiedState.IsCheckedOut(),
			bExpectedCheckedOutProjection);
		TestEqual(*(Prefix + TEXT("modified verified ownership")), ModifiedState.HasVerifiedOwnLock(), bExpectedOwnLock);
		TestEqual(*(Prefix + TEXT("modified strict other ownership")), ModifiedState.IsCheckedOutOther(), bExpectedOtherLock);
		TestEqual(*(Prefix + TEXT("modified edit allowlist")), ModifiedState.CanEdit(), bExpectedLocalCapability);
		TestEqual(*(Prefix + TEXT("modified submit allowlist")), ModifiedState.CanCheckIn(), bExpectedLocalCapability);
		TestEqual(*(Prefix + TEXT("modified delete uses original cached eligibility")), ModifiedState.CanDelete(), !bExpectedOtherLock);
		TestEqual(*(Prefix + TEXT("modified checkout allowlist")), ModifiedState.CanCheckout(), bExpectedModifiedCheckout);
		TestEqual(*(Prefix + TEXT("modified Checked Out filter")), ModifiedState.IsCheckedOut() || ModifiedState.IsAdded(), bExpectedCheckedOutProjection);
		TestEqual(
			*(Prefix + TEXT("modified generic CheckIn worker gate")),
			GitSourceControlOperations::IsStateEligibleForCheckInBoundary(
				ModifiedState),
			ModifiedState.CanCheckIn());

		FGitSourceControlState AddedState = MakeState(
			EFileState::Added,
			ETreeState::Staged,
			LockState);
		const bool bExpectedAddedCheckout = LockState == ELockState::NotLocked;
		TestEqual(*(Prefix + TEXT("added strict checkout ownership")), AddedState.IsCheckedOut(), bExpectedOwnLock);
		TestEqual(*(Prefix + TEXT("added verified ownership")), AddedState.HasVerifiedOwnLock(), bExpectedOwnLock);
		TestEqual(*(Prefix + TEXT("added strict other ownership")), AddedState.IsCheckedOutOther(), bExpectedOtherLock);
		TestEqual(*(Prefix + TEXT("added edit allowlist")), AddedState.CanEdit(), bExpectedLocalCapability);
		TestEqual(*(Prefix + TEXT("added submit allowlist")), AddedState.CanCheckIn(), bExpectedLocalCapability);
		TestEqual(*(Prefix + TEXT("added delete uses original cached eligibility")), AddedState.CanDelete(), !bExpectedOtherLock);
		TestEqual(*(Prefix + TEXT("added checkout allowlist")), AddedState.CanCheckout(), bExpectedAddedCheckout);
		TestTrue(*(Prefix + TEXT("added Checked Out filter is UE pending-add behavior")), AddedState.IsCheckedOut() || AddedState.IsAdded());

		FGitSourceControlState CleanState = MakeState(
			EFileState::Unknown,
			ETreeState::Unmodified,
			LockState);
		const bool bExpectedCleanEdit = bExpectedOwnLock
			|| LockState == ELockState::Unlockable;
		TestEqual(*(Prefix + TEXT("clean UE checkout projection")), CleanState.IsCheckedOut(), bExpectedCheckedOutProjection);
		TestEqual(*(Prefix + TEXT("clean verified ownership")), CleanState.HasVerifiedOwnLock(), bExpectedOwnLock);
		TestEqual(*(Prefix + TEXT("clean edit allowlist")), CleanState.CanEdit(), bExpectedCleanEdit);
		TestEqual(*(Prefix + TEXT("clean submit allowlist")), CleanState.CanCheckIn(), bExpectedOwnLock);
		TestEqual(*(Prefix + TEXT("clean delete uses original cached eligibility")), CleanState.CanDelete(), !bExpectedOtherLock);
		TestEqual(*(Prefix + TEXT("clean checkout allowlist")), CleanState.CanCheckout(), LockState == ELockState::NotLocked);

		FGitSourceControlState DeletedState = MakeState(
			EFileState::Deleted,
			ETreeState::Staged,
			LockState);
		TestEqual(*(Prefix + TEXT("deleted uses original cached eligibility")), DeletedState.CanDelete(), !bExpectedOtherLock);

		TestEqual(
			*(Prefix + TEXT("deleted generic CheckIn worker gate")),
			GitSourceControlOperations::IsStateEligibleForCheckInBoundary(
				DeletedState),
			DeletedState.CanCheckIn());

		FGitSourceControlState NotCurrentCleanState = CleanState;
		NotCurrentCleanState.State.RemoteState = ERemoteState::NotAtHead;
		TestFalse(
			*(Prefix + TEXT("恢复原有过期文件不可删除判断")),
			NotCurrentCleanState.CanDelete());

	}

	FGitSourceControlProvider ProviderContract;
	TestFalse(
		TEXT("ordinary Git must not pollute Uncontrolled Changelists with every writable file"),
		ProviderContract.UsesLocalReadOnlyState());
	FGitSourceControlSettings LfsSettings;
	ProviderContract.ApplyLockingSettingsForTests(
		LfsSettings.GetLockSettingsSnapshot(),
		true);
	TestTrue(
		TEXT("LFS mode must expose provider-managed local read-only state"),
		ProviderContract.UsesLocalReadOnlyState());

	const TArray<FString> PrePullScope{TEXT("Content/BeforePull.uasset")};
	const TArray<FString> RefreshedScope{TEXT("Content/AfterPull.uasset")};
	TArray<FString> SelectedScope;
	GitSourceControlOperations::BuildPostPullPushValidationScope(
		true,
		PrePullScope,
		RefreshedScope,
		SelectedScope);
	TestTrue(
		TEXT("post-pull push gate must use the recomputed scope"),
		SelectedScope == RefreshedScope);
	GitSourceControlOperations::BuildPostPullPushValidationScope(
		false,
		PrePullScope,
		RefreshedScope,
		SelectedScope);
	TestTrue(
		TEXT("failed post-pull scope refresh must not reuse the pre-pull scope"),
		SelectedScope.IsEmpty());

	TestTrue(
		TEXT("identical lock settings snapshots remain current"),
		GitSourceControlOperations::DoLockSettingsSnapshotsMatch(
			7,
			false,
			TEXT("artist"),
			7,
			false,
			TEXT("artist")));
	TestFalse(
		TEXT("ordinary-to-LFS toggle invalidates the command snapshot"),
		GitSourceControlOperations::DoLockSettingsSnapshotsMatch(
			7,
			false,
			TEXT("artist"),
			8,
			true,
			TEXT("artist")));
	TestFalse(
		TEXT("LFS identity change invalidates the command snapshot"),
		GitSourceControlOperations::DoLockSettingsSnapshotsMatch(
			7,
			true,
			TEXT("artist"),
			8,
			true,
			TEXT("other")));
	TestTrue(
		TEXT("inactive lock setting generation changes do not block ordinary Git"),
		GitSourceControlOperations::DoLockSettingsSnapshotsMatch(
			7,
			false,
			TEXT("artist"),
			8,
			false,
			TEXT("other")));

	bool bWriteCalled = false;
	TestFalse(
		TEXT("settings or branch boundary failure must suppress commit/push"),
		GitSourceControlOperations::RunAfterLockWriteBoundary(
			[]()
			{
				return false;
			},
			[&bWriteCalled]()
			{
				bWriteCalled = true;
				return true;
			}));
	TestFalse(
		TEXT("failed branch boundary must not invoke the write lambda"),
		bWriteCalled);
	bool bLockWriteCalled = false;
	bool bUnlockWriteCalled = false;
	const bool bChangedLfsSettingsMatch =
		GitSourceControlOperations::DoLockSettingsSnapshotsMatch(
			7,
			true,
			TEXT("artist"),
			8,
			true,
			TEXT("other"));
	TestFalse(
		TEXT("changed LFS settings suppress the remote lock write"),
		GitSourceControlOperations::RunAfterLockWriteBoundary(
			[&bChangedLfsSettingsMatch]()
			{
				return bChangedLfsSettingsMatch;
			},
			[&bLockWriteCalled]()
			{
				bLockWriteCalled = true;
				return true;
			}));
	TestFalse(TEXT("stale LFS lock command never reaches its write lambda"), bLockWriteCalled);
	TestFalse(
		TEXT("changed LFS settings suppress the remote unlock write"),
		GitSourceControlOperations::RunAfterLockWriteBoundary(
			[&bChangedLfsSettingsMatch]()
			{
				return bChangedLfsSettingsMatch;
			},
			[&bUnlockWriteCalled]()
			{
				bUnlockWriteCalled = true;
				return true;
			}));
	TestFalse(TEXT("stale LFS unlock command never reaches its write lambda"), bUnlockWriteCalled);

	FString LocalRef;
	FString RemoteRef;
	FString PushRefSpec;
	bool bHasRemoteBaseline = true;
	TestTrue(
		TEXT("single LFS first push builds an explicit same-name destination"),
		GitSourceControlOperations::BuildLfsPushRefs(
			TEXT("feature/art"),
			FString(),
			LocalRef,
			RemoteRef,
			PushRefSpec,
			bHasRemoteBaseline));
	TestEqual(TEXT("first push local source is frozen"), LocalRef, FString(TEXT("refs/heads/feature/art")));
	TestEqual(TEXT("first push expected tracking ref"), RemoteRef, FString(TEXT("refs/remotes/origin/feature/art")));
	TestEqual(TEXT("first push explicit refspec"), PushRefSpec, FString(TEXT("refs/heads/feature/art:refs/heads/feature/art")));
	TestFalse(TEXT("first push has no remote baseline"), bHasRemoteBaseline);

	TestTrue(
		TEXT("single LFS supports a non-same-name origin upstream"),
		GitSourceControlOperations::BuildLfsPushRefs(
			TEXT("feature/art"),
			TEXT("origin/review/art"),
			LocalRef,
			RemoteRef,
			PushRefSpec,
			bHasRemoteBaseline));
	TestEqual(TEXT("mapped upstream tracking ref"), RemoteRef, FString(TEXT("refs/remotes/origin/review/art")));
	TestEqual(TEXT("mapped upstream push refspec"), PushRefSpec, FString(TEXT("refs/heads/feature/art:refs/heads/review/art")));
	TestTrue(TEXT("mapped upstream provides a baseline"), bHasRemoteBaseline);

	TestFalse(
		TEXT("single LFS rejects unsupported non-origin upstreams"),
		GitSourceControlOperations::BuildLfsPushRefs(
			TEXT("main"),
			TEXT("upstream/main"),
			LocalRef,
			RemoteRef,
			PushRefSpec,
			bHasRemoteBaseline));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSourceControlSoftRevertSafetyTest,
	"GitSourceControl.State.SoftRevertSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlSoftRevertSafetyTest::RunTest(const FString& Parameters)
{
	const FString GitBinary = GetAutomationTestGitBinary();
	if (!TestFalse(TEXT("仅解锁安全测试需要 Git"), GitBinary.IsEmpty())) { return false; }
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("Automation"), TEXT("UEGitSoftRevertSafety-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestTrue(TEXT("创建独立仅解锁测试仓库"), PlatformFile.CreateDirectoryTree(*Root))) { return false; }
	ON_SCOPE_EXIT { PlatformFile.DeleteDirectoryRecursively(*Root); };
	TArray<FString> Output;
	auto Git = [&](const TArray<FString>& Args)
	{
		TArray<FString> Errors;
		Output.Reset();
		return TestTrue(TEXT("夹具 Git 命令成功"), GitSourceControlUtils::RunCommand(Args[0], GitBinary, Root,
			TArray<FString>(Args.GetData() + 1, Args.Num() - 1), {}, Output, Errors));
	};
	auto Write = [&](const TCHAR* Name, const TCHAR* Text)
	{
		return TestTrue(TEXT("写入临时夹具"), FFileHelper::SaveStringToFile(Text, *FPaths::Combine(Root, Name)));
	};
	if (!Git({TEXT("init"), TEXT("-b"), TEXT("test")})
		|| !Git({TEXT("config"), TEXT("user.name"), TEXT("UEGitAutomation")})
		|| !Git({TEXT("config"), TEXT("user.email"), TEXT("uegit@example.invalid")})
		|| !Git({TEXT("config"), TEXT("core.hooksPath"), TEXT("no-hooks")})
		|| !Git({TEXT("config"), TEXT("commit.gpgsign"), TEXT("false")})
		|| !Write(TEXT("[1].txt"), TEXT("baseline")) || !Write(TEXT("1.txt"), TEXT("neighbor"))
		|| !Git({TEXT("add"), TEXT("--"), TEXT(".")}) || !Git({TEXT("commit"), TEXT("-m"), TEXT("baseline")})
		|| !Write(TEXT("[1].txt"), TEXT("staged edit")) || !Git({TEXT("add"), TEXT("--"), TEXT("[1].txt")})
		|| !Write(TEXT("[1].txt"), TEXT("working edit")) || !Write(TEXT("1.txt"), TEXT("keep this edit"))) { return false; }

	const FString Selected = FPaths::Combine(Root, TEXT("[1].txt"));
	// 错误的仅解锁请求必须失败，不能落入普通 Revert 或空范围整仓还原。
	// Invalid unlock-only requests must fail without falling through to ordinary or repository-wide Revert.
	const auto SoftRevert = ISourceControlOperation::Create<FRevert>();
	SoftRevert->SetSoftRevert(true);
	const auto RevertWorker = MakeShared<FGitRevertWorker, ESPMode::ThreadSafe>();
	FGitSourceControlCommand RevertCommand(SoftRevert, RevertWorker);
	RevertCommand.PathToGitBinary = GitBinary;
	RevertCommand.PathToGitRoot = Root;
	RevertCommand.PathToRepositoryRoot = Root;
	RevertCommand.bUsingGitLfsLocking = false;
	RevertCommand.Files = {Selected};
	TestFalse(TEXT("未启用 LFS 时仅解锁不会转成还原"), RevertWorker->Execute(RevertCommand));
	RevertCommand.bUsingGitLfsLocking = true;
	RevertCommand.Files.Reset();
	TestFalse(TEXT("空范围仅解锁不会转成整仓还原"), RevertWorker->Execute(RevertCommand));
	FString LocalEdit;
	TestTrue(TEXT("仅解锁失败后文件仍存在"), FFileHelper::LoadFileToString(LocalEdit, *Selected));
	TestEqual(TEXT("仅解锁失败保留工作区修改"), LocalEdit, FString(TEXT("working edit")));
	Git({TEXT("show"), TEXT(":[1].txt")});
	TestEqual(TEXT("仅解锁失败保留暂存内容"), FString::Join(Output, TEXT("\n")), FString(TEXT("staged edit")));

	return true;
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
	FGitSourceControlLocalReadOnlyPolicyTest,
	"GitSourceControl.State.LocalReadOnlyPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlLocalReadOnlyPolicyTest::RunTest(const FString& Parameters)
{
	for (int32 bCommandUsingLfs = 0; bCommandUsingLfs <= 1; ++bCommandUsingLfs)
	{
		for (int32 bSettingsSuperseded = 0; bSettingsSuperseded <= 1; ++bSettingsSuperseded)
		{
			for (int32 bCurrentSettingsUsingLfs = 0; bCurrentSettingsUsingLfs <= 1; ++bCurrentSettingsUsingLfs)
			{
				const bool bExpected = bCommandUsingLfs != 0
					|| (bSettingsSuperseded != 0 && bCurrentSettingsUsingLfs != 0);
				const FString Label = FString::Printf(
					TEXT("Effective LFS mode command=%d superseded=%d current=%d"),
					bCommandUsingLfs,
					bSettingsSuperseded,
					bCurrentSettingsUsingLfs);
				TestEqual(
					*Label,
					GitSourceControlUtils::IsLfsReadOnlyPolicyActive(
						bCommandUsingLfs != 0,
						bSettingsSuperseded != 0,
						bCurrentSettingsUsingLfs != 0),
					bExpected);
			}
		}
	}

	const TArray<FGitLocalReadOnlyPolicyCase> Cases = {
		{ TEXT("NoLfsTracked"), EFileState::Unknown, ETreeState::Unmodified, ELockState::NotLocked, false, EGitLocalReadOnlyPolicy::Preserve },
		{ TEXT("NoLfsOwnLock"), EFileState::Unknown, ETreeState::Unmodified, ELockState::Locked, false, EGitLocalReadOnlyPolicy::Preserve },
		{ TEXT("LfsAdded"), EFileState::Added, ETreeState::Staged, ELockState::NotLocked, true, EGitLocalReadOnlyPolicy::Writable },
		{ TEXT("LfsAddedUnlockable"), EFileState::Added, ETreeState::Staged, ELockState::Unlockable, true, EGitLocalReadOnlyPolicy::Writable },
		{ TEXT("LfsAddedOtherLock"), EFileState::Added, ETreeState::Staged, ELockState::LockedOther, true, EGitLocalReadOnlyPolicy::ReadOnly },
		{ TEXT("LfsAddedUnknownLock"), EFileState::Added, ETreeState::Staged, ELockState::Unknown, true, EGitLocalReadOnlyPolicy::ReadOnly },
		{ TEXT("LfsAddedUnsetLock"), EFileState::Added, ETreeState::Staged, ELockState::Unset, true, EGitLocalReadOnlyPolicy::ReadOnly },
		{ TEXT("LfsOwnLock"), EFileState::Unknown, ETreeState::Unmodified, ELockState::Locked, true, EGitLocalReadOnlyPolicy::Writable },
		{ TEXT("LfsOtherLock"), EFileState::Unknown, ETreeState::Unmodified, ELockState::LockedOther, true, EGitLocalReadOnlyPolicy::ReadOnly },
		{ TEXT("LfsOfflineModified"), EFileState::Modified, ETreeState::Working, ELockState::NotLocked, true, EGitLocalReadOnlyPolicy::Writable },
		{ TEXT("LfsCleanNotLocked"), EFileState::Unknown, ETreeState::Unmodified, ELockState::NotLocked, true, EGitLocalReadOnlyPolicy::Preserve },
		{ TEXT("LfsModifiedUnknownLock"), EFileState::Modified, ETreeState::Working, ELockState::Unknown, true, EGitLocalReadOnlyPolicy::ReadOnly },
		{ TEXT("LfsCleanUnknownLock"), EFileState::Unknown, ETreeState::Unmodified, ELockState::Unknown, true, EGitLocalReadOnlyPolicy::ReadOnly },
		{ TEXT("LfsCleanUnsetLock"), EFileState::Unknown, ETreeState::Unmodified, ELockState::Unset, true, EGitLocalReadOnlyPolicy::ReadOnly },
		{ TEXT("LfsUntracked"), EFileState::Unknown, ETreeState::Untracked, ELockState::NotLocked, true, EGitLocalReadOnlyPolicy::Preserve },
	};

	for (const FGitLocalReadOnlyPolicyCase& Case : Cases)
	{
		const FGitSourceControlState State = MakeState(Case.FileState, Case.TreeState, Case.LockState);
		const EGitLocalReadOnlyPolicy ActualPolicy = GitSourceControlUtils::GetLocalReadOnlyPolicy(
			State,
			Case.bUsingLfsLocking);
		TestEqual(
			Case.Label,
			static_cast<uint8>(ActualPolicy),
			static_cast<uint8>(Case.ExpectedPolicy));
	}

	// 遍历普通 Git 与 LFS 的全部锁状态，固定 Added、Modified、Clean 的权限优先级。
	// Cover every ordinary-Git/LFS lock state and the permission precedence of Added, Modified and Clean.
	for (int32 LockValue = static_cast<int32>(ELockState::Unset);
		LockValue < static_cast<int32>(ELockState::Count);
		++LockValue)
	{
		const ELockState::Type LockState = static_cast<ELockState::Type>(LockValue);
		const bool bOwnLock = LockState == ELockState::Locked;
		const bool bForeignOrInvalidLock = LockState == ELockState::LockedOther
			|| LockState == ELockState::Unknown
			|| LockState == ELockState::Unset;
		const bool bAddedWorkable = bOwnLock
			|| LockState == ELockState::NotLocked
			|| LockState == ELockState::Unlockable;

		const FGitSourceControlState AddedState = MakeState(
			EFileState::Added,
			ETreeState::Staged,
			LockState);
		const FGitSourceControlState ModifiedState = MakeState(
			EFileState::Modified,
			ETreeState::Working,
			LockState);
		const FGitSourceControlState CleanState = MakeState(
			EFileState::Unknown,
			ETreeState::Unmodified,
			LockState);
		const EGitLocalReadOnlyPolicy ExpectedAddedLfs = bAddedWorkable
			? EGitLocalReadOnlyPolicy::Writable
			: EGitLocalReadOnlyPolicy::ReadOnly;
		const EGitLocalReadOnlyPolicy ExpectedModifiedLfs =
			bOwnLock || LockState == ELockState::NotLocked
				? EGitLocalReadOnlyPolicy::Writable
				: bForeignOrInvalidLock
					? EGitLocalReadOnlyPolicy::ReadOnly
					: EGitLocalReadOnlyPolicy::Preserve;
		const EGitLocalReadOnlyPolicy ExpectedCleanLfs = bOwnLock
			? EGitLocalReadOnlyPolicy::Writable
			: bForeignOrInvalidLock
				? EGitLocalReadOnlyPolicy::ReadOnly
				: EGitLocalReadOnlyPolicy::Preserve;
		const FString Prefix = FString::Printf(TEXT("Permission lock enum %d: "), LockValue);

		TestEqual(
			*(Prefix + TEXT("ordinary Git preserves Added")),
			static_cast<uint8>(GitSourceControlUtils::GetLocalReadOnlyPolicy(AddedState, false)),
			static_cast<uint8>(EGitLocalReadOnlyPolicy::Preserve));
		TestEqual(
			*(Prefix + TEXT("ordinary Git preserves Modified")),
			static_cast<uint8>(GitSourceControlUtils::GetLocalReadOnlyPolicy(ModifiedState, false)),
			static_cast<uint8>(EGitLocalReadOnlyPolicy::Preserve));
		TestEqual(
			*(Prefix + TEXT("ordinary Git preserves Clean")),
			static_cast<uint8>(GitSourceControlUtils::GetLocalReadOnlyPolicy(CleanState, false)),
			static_cast<uint8>(EGitLocalReadOnlyPolicy::Preserve));
		TestEqual(
			*(Prefix + TEXT("single LFS Added")),
			static_cast<uint8>(GitSourceControlUtils::GetLocalReadOnlyPolicy(AddedState, true)),
			static_cast<uint8>(ExpectedAddedLfs));

		TestEqual(
			*(Prefix + TEXT("single LFS Modified")),
			static_cast<uint8>(GitSourceControlUtils::GetLocalReadOnlyPolicy(ModifiedState, true)),
			static_cast<uint8>(ExpectedModifiedLfs));

		TestEqual(
			*(Prefix + TEXT("single LFS Clean")),
			static_cast<uint8>(GitSourceControlUtils::GetLocalReadOnlyPolicy(CleanState, true)),
			static_cast<uint8>(ExpectedCleanLfs));

	}

	for (int32 bModified = 0; bModified <= 1; ++bModified)
	{
		const EFileState::Type FileState = bModified
			? EFileState::Modified
			: EFileState::Unknown;
		const ETreeState::Type TreeState = bModified
			? ETreeState::Working
			: ETreeState::Unmodified;
		const EGitLocalReadOnlyPolicy ExpectedUnlockPolicy = bModified
			? EGitLocalReadOnlyPolicy::Writable
			: EGitLocalReadOnlyPolicy::ReadOnly;
		const FString Prefix = FString::Printf(
			TEXT("Legacy transition modified=%d: "),
			bModified);

		FGitSourceControlState OwnAdd = MakeState(
			FileState,
			TreeState,
			ELockState::NotLocked);
		TestEqual(
			*(Prefix + TEXT("own add makes writable")),
			static_cast<uint8>(GitSourceControlUtils::ApplyLegacyLfsLockStateTransition(
				OwnAdd,
				TEXT("me"),
				true,
				true)),
			static_cast<uint8>(EGitLocalReadOnlyPolicy::Writable));
		TestEqual(*(Prefix + TEXT("own add projects Locked")), OwnAdd.State.LockState, ELockState::Locked);
		TestEqual(
			*(Prefix + TEXT("own remove transition policy")),
			static_cast<uint8>(GitSourceControlUtils::ApplyLegacyLfsLockStateTransition(
				OwnAdd,
				TEXT("me"),
				false,
				false)),
			static_cast<uint8>(ExpectedUnlockPolicy));
		TestEqual(*(Prefix + TEXT("own remove projects NotLocked")), OwnAdd.State.LockState, ELockState::NotLocked);

		FGitSourceControlState OtherAdd = MakeState(
			FileState,
			TreeState,
			ELockState::NotLocked);
		TestEqual(
			*(Prefix + TEXT("other add makes read-only")),
			static_cast<uint8>(GitSourceControlUtils::ApplyLegacyLfsLockStateTransition(
				OtherAdd,
				TEXT("me"),
				true,
				false)),
			static_cast<uint8>(EGitLocalReadOnlyPolicy::ReadOnly));
		TestEqual(*(Prefix + TEXT("other add projects LockedOther")), OtherAdd.State.LockState, ELockState::LockedOther);
		TestEqual(
			*(Prefix + TEXT("other remove transition policy")),
			static_cast<uint8>(GitSourceControlUtils::ApplyLegacyLfsLockStateTransition(
				OtherAdd,
				TEXT("me"),
				false,
				false)),
			static_cast<uint8>(ExpectedUnlockPolicy));
	}

	FGitSourceControlState CachedCleanAfterWorker = MakeState(
		EFileState::Modified,
		ETreeState::Working,
		ELockState::Locked);
	const FGitSourceControlState FreshCleanAfterRevert = MakeState(
		EFileState::Unknown,
		ETreeState::Unmodified,
		ELockState::NotLocked);
	TArray<FString> TransitionOrder;
	EGitLocalReadOnlyPolicy CleanUnlockPolicy = EGitLocalReadOnlyPolicy::Writable;
	TestTrue(
		TEXT("same-tick Revert commits fresh clean state before unlock transition"),
		GitSourceControlOperations::RunFreshStateCommitBeforeLockTransitions(
			[&]()
			{
				TransitionOrder.Add(TEXT("fresh"));
				CachedCleanAfterWorker = FreshCleanAfterRevert;
				return true;
			},
			true,
			[&]()
			{
				TransitionOrder.Add(TEXT("unlock"));
				CleanUnlockPolicy =
					GitSourceControlUtils::ApplyLegacyLfsLockStateTransition(
						CachedCleanAfterWorker,
						TEXT("me"),
						false,
						false);
				return true;
			}));
	TestTrue(
		TEXT("fresh-state and unlock phases execute in owning order"),
		TransitionOrder == TArray<FString>{TEXT("fresh"), TEXT("unlock")});
	TestEqual(
		TEXT("clean file becomes read-only after Revert/check-in unlock"),
		static_cast<uint8>(CleanUnlockPolicy),
		static_cast<uint8>(EGitLocalReadOnlyPolicy::ReadOnly));

	FGitSourceControlState CachedModifiedAfterWorker = MakeState(
		EFileState::Unknown,
		ETreeState::Unmodified,
		ELockState::Locked);
	const FGitSourceControlState FreshModifiedAfterRemoteLoss = MakeState(
		EFileState::Modified,
		ETreeState::Working,
		ELockState::NotLocked);
	EGitLocalReadOnlyPolicy ModifiedUnlockPolicy = EGitLocalReadOnlyPolicy::ReadOnly;
	GitSourceControlOperations::RunFreshStateCommitBeforeLockTransitions(
		[&]()
		{
			CachedModifiedAfterWorker = FreshModifiedAfterRemoteLoss;
			return true;
		},
		true,
		[&]()
		{
			ModifiedUnlockPolicy =
				GitSourceControlUtils::ApplyLegacyLfsLockStateTransition(
					CachedModifiedAfterWorker,
					TEXT("me"),
					false,
					false);
			return true;
		});
	TestEqual(
		TEXT("remote lock loss preserves a still-modified offline file as writable"),
		static_cast<uint8>(ModifiedUnlockPolicy),
		static_cast<uint8>(EGitLocalReadOnlyPolicy::Writable));

	TArray<FString> InterleavedCommandOrder;
	int32 InterleavedTransitionCalls = 0;
	TestTrue(
		TEXT("first completed command still commits its fresh state while another command is pending"),
		GitSourceControlOperations::RunFreshStateCommitBeforeLockTransitions(
			[&]()
			{
				InterleavedCommandOrder.Add(TEXT("first-fresh"));
				return true;
			},
			false,
			[&]()
			{
				++InterleavedTransitionCalls;
				InterleavedCommandOrder.Add(TEXT("transition"));
				return true;
			}));
	TestEqual(
		TEXT("first completed command must not consume another worker's global lock fixup"),
		InterleavedTransitionCalls,
		0);
	TestTrue(
		TEXT("last completed command drains fixups after its own fresh state"),
		GitSourceControlOperations::RunFreshStateCommitBeforeLockTransitions(
			[&]()
			{
				InterleavedCommandOrder.Add(TEXT("last-fresh"));
				return true;
			},
			true,
			[&]()
			{
				++InterleavedTransitionCalls;
				InterleavedCommandOrder.Add(TEXT("transition"));
				return true;
			}));
	TestEqual(
		TEXT("quiescent queue drains each deferred transition once"),
		InterleavedTransitionCalls,
		1);
	TestEqual(
		TEXT("interleaved command ordering"),
		FString::Join(InterleavedCommandOrder, TEXT(",")),
		FString(TEXT("first-fresh,last-fresh,transition")));

	const FString GitBinary = GetAutomationTestGitBinary();
	const FString RepositoryRoot = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir());
	TArray<FString> LockableAttributeErrors;
	TestTrue(
		TEXT("索引失败回退测试应先从当前仓库初始化 LFS lockable 规则"),
		GitSourceControlUtils::CheckLFSLockable(
			GitBinary,
			RepositoryRoot,
			{TEXT("*.uasset"), TEXT("*.umap")},
			LockableAttributeErrors));
	TestTrue(
		TEXT("当前仓库应把 .uasset 声明为 LFS lockable"),
		GitSourceControlUtils::IsFileLFSLockable(
			TEXT("Content/Fallback.uasset")));

	TestEqual(
		TEXT("LFS lockable index fallback fails closed"),
		GitSourceControlOperations::GetIndexMutationFallbackLockState(
			true,
			TEXT("Content/Fallback.uasset")),
		ELockState::LockedOther);
	TestEqual(
		TEXT("non-lockable index fallback remains ordinary Git"),
		GitSourceControlOperations::GetIndexMutationFallbackLockState(
			true,
			TEXT("Config/Fallback.ini")),
		ELockState::Unlockable);
	TestEqual(
		TEXT("no-LFS index fallback remains ordinary Git"),
		GitSourceControlOperations::GetIndexMutationFallbackLockState(
			false,
			TEXT("Content/Fallback.uasset")),
		ELockState::Unlockable);

	const FString TempFile = FPaths::CreateTempFilename(
		*FPaths::ProjectSavedDir(),
		TEXT("UEGitReadOnlyPolicy-"),
		TEXT(".tmp"));
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	ON_SCOPE_EXIT
	{
		if (PlatformFile.FileExists(*TempFile))
		{
			PlatformFile.SetReadOnly(*TempFile, false);
			PlatformFile.DeleteFile(*TempFile);
		}
	};

	TestTrue(
		TEXT("应创建本地权限集成测试临时文件"),
		FFileHelper::SaveStringToFile(TEXT("UEGit local permission contract"), *TempFile));
	if (!PlatformFile.FileExists(*TempFile))
	{
		return false;
	}

	FString FailureReason(TEXT("stale"));
	TestTrue(
		TEXT("ReadOnly 策略应成功应用"),
		GitSourceControlUtils::ApplyLocalReadOnlyPolicy(
			TempFile,
			EGitLocalReadOnlyPolicy::ReadOnly,
			&FailureReason));
	TestTrue(TEXT("成功应用权限时应清空旧失败原因"), FailureReason.IsEmpty());
	TestTrue(TEXT("ReadOnly 策略应设置磁盘只读位"), PlatformFile.IsReadOnly(*TempFile));
	TestTrue(
		TEXT("Preserve 策略应成功且不改动只读文件"),
		GitSourceControlUtils::ApplyLocalReadOnlyPolicy(
			TempFile,
			EGitLocalReadOnlyPolicy::Preserve));
	TestTrue(TEXT("Preserve 后只读位应保持"), PlatformFile.IsReadOnly(*TempFile));
	TestTrue(
		TEXT("Writable 策略应成功应用"),
		GitSourceControlUtils::ApplyLocalReadOnlyPolicy(
			TempFile,
			EGitLocalReadOnlyPolicy::Writable));
	TestFalse(TEXT("Writable 策略应清除磁盘只读位"), PlatformFile.IsReadOnly(*TempFile));
	TestTrue(
		TEXT("Preserve 策略应成功且不改动可写文件"),
		GitSourceControlUtils::ApplyLocalReadOnlyPolicy(
			TempFile,
			EGitLocalReadOnlyPolicy::Preserve));
	TestFalse(TEXT("Preserve 后可写位应保持"), PlatformFile.IsReadOnly(*TempFile));

	FGitSourceControlState FailedAddedState = MakeState(
		EFileState::Added,
		ETreeState::Staged,
		GitSourceControlOperations::GetIndexMutationFallbackLockState(
			true,
			TempFile + TEXT(".uasset")));
	const EGitLocalReadOnlyPolicy FailedAddedPolicy =
		GitSourceControlUtils::GetLocalReadOnlyPolicy(
			FailedAddedState,
			true);
	TestEqual(
		TEXT("failed lockable add status requires read-only convergence"),
		static_cast<uint8>(FailedAddedPolicy),
		static_cast<uint8>(EGitLocalReadOnlyPolicy::ReadOnly));
	TestTrue(
		TEXT("failed lockable add status applies the physical read-only bit"),
		GitSourceControlUtils::ApplyLocalReadOnlyPolicy(
			TempFile,
			FailedAddedPolicy));
	TestTrue(
		TEXT("failed lockable add no longer appears in Writable filter"),
		PlatformFile.IsReadOnly(*TempFile));

	TestTrue(
		TEXT("unknown-lock save-routing setup makes the file writable first"),
		GitSourceControlUtils::ApplyLocalReadOnlyPolicy(
			TempFile,
			EGitLocalReadOnlyPolicy::Writable));
	FGitSourceControlState UnknownTrackedState = MakeState(
		EFileState::Modified,
		ETreeState::Working,
		ELockState::Unknown);
	TestFalse(
		TEXT("unknown tracked LFS state does not grant semantic edit capability"),
		UnknownTrackedState.CanEdit());
	const EGitLocalReadOnlyPolicy UnknownTrackedPolicy =
		GitSourceControlUtils::GetLocalReadOnlyPolicy(
			UnknownTrackedState,
			true);
	TestEqual(
		TEXT("unknown tracked LFS state fails closed at UE physical save routing"),
		static_cast<uint8>(UnknownTrackedPolicy),
		static_cast<uint8>(EGitLocalReadOnlyPolicy::ReadOnly));
	TestTrue(
		TEXT("unknown tracked LFS state replaces a stale writable bit with read-only"),
		GitSourceControlUtils::ApplyLocalReadOnlyPolicy(
			TempFile,
			UnknownTrackedPolicy));
	TestTrue(
		TEXT("unknown tracked LFS asset cannot remain visible to Writable filter/save bypass"),
		PlatformFile.IsReadOnly(*TempFile));

	FailureReason.Reset();
	AddExpectedError(
		TEXT("目标=未知策略"),
		EAutomationExpectedErrorFlags::Contains,
		2);
	TestFalse(
		TEXT("unknown permission policy must fail without changing disk"),
		GitSourceControlUtils::ApplyLocalReadOnlyPolicy(
			TempFile,
			static_cast<EGitLocalReadOnlyPolicy>(255),
			&FailureReason));
	TestTrue(
		TEXT("unknown permission policy preserves the prior read-only bit"),
		PlatformFile.IsReadOnly(*TempFile));
	TestTrue(
		TEXT("unknown permission policy is diagnosable"),
		FailureReason.Contains(TEXT("未知策略"))
			&& FailureReason.Contains(TempFile));

	FailureReason.Reset();
	AddExpectedError(
		TEXT("文件不存在"),
		EAutomationExpectedErrorFlags::Contains,
		2);
	TestFalse(
		TEXT("不存在文件的权限应用应显式失败"),
		GitSourceControlUtils::ApplyLocalReadOnlyPolicy(
			TempFile + TEXT(".missing"),
			EGitLocalReadOnlyPolicy::ReadOnly,
			&FailureReason));
	TestTrue(
		TEXT("权限应用失败原因应包含文件、目标权限和可诊断说明"),
		FailureReason.Contains(TEXT("文件不存在"))
			&& FailureReason.Contains(TEXT("目标=只读"))
			&& FailureReason.Contains(TempFile + TEXT(".missing")));

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGitSourceControlMutationBoundaryWiringTest,
	"GitSourceControl.State.MutationBoundaryWiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitSourceControlMutationBoundaryWiringTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("GitSourceControl"));
	TestTrue(TEXT("GitSourceControl plugin must be discoverable"), Plugin.IsValid());
	if (!Plugin.IsValid())
	{
		return false;
	}

	FString OperationsSource;
	FString ProviderSource;
	FString UtilsSource;
	const FString PrivateSourceDir = FPaths::Combine(
		Plugin->GetBaseDir(),
		TEXT("Source/GitSourceControl/Private"));
	TestTrue(
		TEXT("load production operations source"),
		FFileHelper::LoadFileToString(
			OperationsSource,
			*FPaths::Combine(
				PrivateSourceDir,
				TEXT("GitSourceControlOperations.cpp"))));
	TestTrue(
		TEXT("load production provider source"),
		FFileHelper::LoadFileToString(
			ProviderSource,
			*FPaths::Combine(
				PrivateSourceDir,
				TEXT("GitSourceControlProvider.cpp"))));
	TestTrue(
		TEXT("load production utils source"),
		FFileHelper::LoadFileToString(
			UtilsSource,
			*FPaths::Combine(
				PrivateSourceDir,
				TEXT("GitSourceControlUtils.cpp"))));

	// 规范化空白后检查生产接线，避免格式换行让直接 mutation 绕过测试。
	// Normalize whitespace before inspecting production wiring so formatting cannot hide a direct
	// mutation bypass from the contract test.
	auto CompactSource = [](FString Source)
	{
		Source.ReplaceInline(TEXT("\r"), TEXT(""));
		Source.ReplaceInline(TEXT("\n"), TEXT(""));
		Source.ReplaceInline(TEXT("\t"), TEXT(""));
		Source.ReplaceInline(TEXT(" "), TEXT(""));
		return Source;
	};
	const FString CompactOperations = CompactSource(MoveTemp(OperationsSource));
	const int32 DeleteStart = CompactOperations.Find(TEXT("boolFGitDeleteWorker::Execute("));
	const int32 DeleteEnd = CompactOperations.Find(TEXT("boolFGitDeleteWorker::UpdateStates("));
	if (!TestTrue(TEXT("找到原版删除实现的独立边界"), DeleteStart != INDEX_NONE && DeleteEnd > DeleteStart)) { return false; }
	const FString DeleteBody = CompactOperations.Mid(DeleteStart, DeleteEnd - DeleteStart);
	const FString GuardedOperations = CompactOperations.Left(DeleteStart) + CompactOperations.Mid(DeleteEnd);
	TestTrue(TEXT("删除恢复普通 git rm"), DeleteBody.Contains(TEXT("GitSourceControlUtils::RunCommand(TEXT(\"rm\")")));
	TestTrue(TEXT("删除成功后仅更新本地缓存"), DeleteBody.Contains(TEXT("CollectNewStates(InCommand.Files,States,EFileState::Deleted,ETreeState::Staged)")));
	TestFalse(TEXT("删除不使用新增的前置校验"), DeleteBody.Contains(TEXT("ValidateLocalStatesBeforeDelete")));
	TestFalse(TEXT("删除不强制覆盖本地修改"), DeleteBody.Contains(TEXT("--force")));
	TestFalse(TEXT("删除不使用新增的成功后完整刷新"), DeleteBody.Contains(TEXT("CollectStatesAfterIndexMutation")));

	const FString CompactProvider = CompactSource(MoveTemp(ProviderSource));
	const FString CompactUtils = CompactSource(MoveTemp(UtilsSource));

	const TArray<FString> ForbiddenDirectWorkerMutations{
		TEXT("GitSourceControlUtils::RunCommand(TEXT(\"add\")"),
		TEXT("GitSourceControlUtils::RunCommand(TEXT(\"rm\")"),
		TEXT("GitSourceControlUtils::RunCommand(TEXT(\"reset\")"),
		TEXT("GitSourceControlUtils::RunCommand(TEXT(\"clean\")"),
		TEXT("GitSourceControlUtils::RunCommand(TEXT(\"checkout\")"),
		TEXT("GitSourceControlUtils::RunCommand(TEXT(\"restore\")"),
		TEXT("GitSourceControlUtils::RunCommand(TEXT(\"push\")")};
	for (const FString& Forbidden : ForbiddenDirectWorkerMutations)
	{
		TestFalse(
			*(TEXT("production worker has no raw mutation bypass: ") + Forbidden),
			GuardedOperations.Contains(Forbidden));
	}

	const TArray<FString> RequiredGuardedMutations{
		TEXT("RunMutationAfterCommandLockBoundary(InCommand,TEXT(\"Gitcommit\")"),
		TEXT("RunGitCommandMutationAfterLockBoundary(InCommand,TEXT(\"Gitpush\"),TEXT(\"push\")"),
		TEXT("RunMutationAfterCommandLockBoundary(InCommand,TEXT(\"Gitpull重试\")"),
		TEXT("RunGitCommandMutationAfterLockBoundary(InCommand,TEXT(\"Gitpush重试\"),TEXT(\"push\")"),
		TEXT("RunGitCommandMutationAfterLockBoundary(InCommand,TEXT(\"Gitadd\"),TEXT(\"add\")"),

		TEXT("RunGitCommandMutationAfterLockBoundary(InCommand,TEXT(\"Gitreset--hard\"),TEXT(\"reset\")"),
		TEXT("RunGitCommandMutationAfterLockBoundary(InCommand,TEXT(\"Gitclean-fd\"),TEXT(\"clean\")"),
		TEXT("RunGitCommandMutationAfterLockBoundary(InCommand,TEXT(\"Gitrmduringrevert\"),TEXT(\"rm\")"),
		TEXT("RunGitCommandMutationAfterLockBoundary(InCommand,TEXT(\"Gitresetduringrevert\"),TEXT(\"reset\")"),
		TEXT("RunGitCommandMutationAfterLockBoundary(InCommand,TEXT(\"Gitcheckoutduringrevert\"),TEXT(\"checkout\")"),
		TEXT("RunGitCommandMutationAfterLockBoundary(InCommand,TEXT(\"Gitcheckoutretryduringrevert\"),TEXT(\"checkout\")"),
		TEXT("RunMutationAfterCommandLockBoundary(InCommand,TEXT(\"Gitpull\")"),
		TEXT("RunGitCommandMutationAfterLockBoundary(InCommand,TEXT(\"Gitaddcopiedfile\"),TEXT(\"add\")"),
		TEXT("RunGitCommandMutationAfterLockBoundary(InCommand,TEXT(\"Gitaddresolvedfile\"),TEXT(\"add\")"),
		TEXT("RunGitCommandMutationAfterLockBoundary(InCommand,TEXT(\"Gitaddtostagedchangelist\"),TEXT(\"add\")"),
		TEXT("RunGitCommandMutationAfterLockBoundary(InCommand,TEXT(\"Gitrestorefromstagedchangelist\"),TEXT(\"restore\")"),
		TEXT("RunLFSCommandWithPreWriteBoundary(TEXT(\"lock\")"),
		TEXT("RunLFSCommandWithPreWriteBoundary(TEXT(\"unlock\")"),
		TEXT("TEXT(\"Gitcommit子进程\")"),
		TEXT("TEXT(\"Gitfetchbeforepushretry\")"),
		TEXT("TEXT(\"Gitfetchbeforepull\")"),
		TEXT("TEXT(\"Gitfetch\")"),
		TEXT("TEXT(\"Gitpullretrysubprocess\")"),
		TEXT("TEXT(\"Gitpullsubprocess\")")};
	for (const FString& Required : RequiredGuardedMutations)
	{
		TestTrue(
			*(TEXT("production mutation uses the guarded boundary: ") + Required),
			CompactOperations.Contains(Required));
	}

	TestTrue(
		TEXT("provider computes queue quiescence before draining global lock fixups"),
		CompactProvider.Contains(
			TEXT("constboolbCommandQueueQuiescent=CommandQueue.IsEmpty();"))
			&& CompactProvider.Contains(
				TEXT("bCommandQueueQuiescent,ApplyPendingLegacyLfsLockStateFixups")));
	TestTrue(
		TEXT("provider also drains deferred fixups when a tick observes an already-empty queue"),
		CompactProvider.Contains(
			TEXT("if(!bLockTransitionsApplied&&CommandQueue.IsEmpty())")));

	TestFalse(
		TEXT("changelist status scanning must never restage a porcelain status line"),
		CompactUtils.Contains(TEXT("UpdateFileStagingOnSavedInternal(Result)")));
	TestFalse(
		TEXT("save callback must not invoke raw Git add"),
		CompactUtils.Contains(TEXT("RunCommand(TEXT(\"add\")")));
	TestTrue(
		TEXT("save callback restaging enters the guarded MoveToChangelist worker asynchronously"),
		CompactUtils.Contains(
			TEXT("ISourceControlOperation::Create<FMoveToChangelist>()"))
			&& CompactUtils.Contains(TEXT("EConcurrency::Asynchronous")));

	TestTrue(
		TEXT("generic mutating Git runner guards every real batch subprocess"),
		CompactOperations.Contains(
			TEXT("GitSourceControlUtils::RunCommandWithPreWriteBoundary(")));
	TestTrue(
		TEXT("LFS writes enter the per-subprocess boundary runner"),
		CompactUtils.Contains(
			TEXT("returnGitSourceControlUtils::RunCommandWithPreWriteBoundary(Command,LFSLockBinary,InRepositoryRoot,InParameters,InFiles,InPreWriteBoundary"))
			&& CompactOperations.Contains(
				TEXT("RunLFSCommandWithPreWriteBoundary(")));
	TestTrue(
		TEXT("RunCommit routes every owned subprocess and retry through the boundary-aware internal runner"),
		CompactUtils.Contains(
			TEXT("returnRunCommandInternalWithPreSubprocessBoundary(InCommand,InPathToGitBinary,InRepositoryRoot,InCommandParameters,InCommandFiles,InPreWriteBoundary")));
	const int32 RetryBoundaryGate = CompactUtils.Find(
		TEXT("if(!InPreSubprocessBoundary())"));
	const int32 RawGitSubprocess = CompactUtils.Find(
		TEXT("bResult=RunCommandInternalRaw("),
		ESearchCase::CaseSensitive,
		ESearchDir::FromStart,
		RetryBoundaryGate);
	TestTrue(
		TEXT("index.lock retries recheck immediately before every raw Git subprocess"),
		RetryBoundaryGate != INDEX_NONE
			&& RawGitSubprocess > RetryBoundaryGate);
	TestTrue(
		TEXT("CheckIn must propagate a rejected commit subprocess boundary as failure"),
		CompactOperations.Contains(
			TEXT("bCommitBoundaryRejected|=!bBoundaryPassed;"))
			&& CompactOperations.Contains(
				TEXT("if(bCommitBoundaryRejected){"))
			&& CompactOperations.Contains(
				TEXT("InCommand.bCommandSuccessful=false;returnfalse;")));
	TestTrue(
		TEXT("Pull rechecks after package unlink and immediately before the real pull subprocess"),
		CompactUtils.Contains(
			TEXT("boolbSuccess=RunCommandWithPreWriteBoundary(TEXT(\"pull\")")));
	TestTrue(
		TEXT("Fetch rechecks after lock refresh and immediately before the real fetch subprocess"),
		CompactUtils.Contains(
			TEXT("returnRunCommandWithPreWriteBoundary(TEXT(\"fetch\")")));
	const int32 PullFunctionStart = CompactUtils.Find(TEXT("boolPullOrigin("));
	const int32 PullPackageUnlinkComplete = CompactUtils.Find(
		TEXT("PackagesToReload=PackagesToReloadResult.Get();"),
		ESearchCase::CaseSensitive,
		ESearchDir::FromStart,
		PullFunctionStart);
	const int32 GuardedPullSubprocess = CompactUtils.Find(
		TEXT("boolbSuccess=RunCommandWithPreWriteBoundary(TEXT(\"pull\")"),
		ESearchCase::CaseSensitive,
		ESearchDir::FromStart,
		PullFunctionStart);
	TestTrue(
		TEXT("Pull write gate must remain below the package-unlink wait"),
		PullFunctionStart != INDEX_NONE
			&& PullPackageUnlinkComplete > PullFunctionStart
			&& GuardedPullSubprocess > PullPackageUnlinkComplete);
	const int32 FetchFunctionStart = CompactUtils.Find(TEXT("boolFetchRemote("));
	const int32 FetchLockRefresh = CompactUtils.Find(
		TEXT("GetAllLocks("),
		ESearchCase::CaseSensitive,
		ESearchDir::FromStart,
		FetchFunctionStart);
	const int32 GuardedFetchSubprocess = CompactUtils.Find(
		TEXT("returnRunCommandWithPreWriteBoundary(TEXT(\"fetch\")"),
		ESearchCase::CaseSensitive,
		ESearchDir::FromStart,
		FetchFunctionStart);
	TestTrue(
		TEXT("Fetch write gate must remain below the optional lock refresh"),
		FetchFunctionStart != INDEX_NONE
			&& FetchLockRefresh > FetchFunctionStart
			&& GuardedFetchSubprocess > FetchLockRefresh);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
