// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlOperations.h"

#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SourceControlOperations.h"
#include "ISourceControlModule.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlCommand.h"
#include "GitMessageLog.h"
#include "GitSourceControlSettings.h"
#include "GitSourceControlUtils.h"
#include "GitLfsUnlock.h"
#include "SourceControlHelpers.h"
#include "Logging/MessageLog.h"
#include "Misc/MessageDialog.h"
#include "HAL/PlatformProcess.h"

#include <thread>

#define LOCTEXT_NAMESPACE "GitSourceControl"

/**
 * 核对命令创建时冻结的完整锁设置；单调 Generation 防止把“改回原值”误认成同一次配置。
 * Validates the complete lock settings frozen when the command was created. The monotonic
 * generation prevents changing a value away and back from being mistaken for the same snapshot.
 */
bool GitSourceControlOperations::DoLockSettingsSnapshotsMatch(
	uint64 InCommandGeneration,
	bool bInCommandUsingLfs,
	const FString& InCommandLfsUser,
	uint64 InCurrentGeneration,
	bool bInCurrentUsingLfs,
	const FString& InCurrentLfsUser)
{
	// 普通 Git 两侧都未启用锁时，LFS 身份/端点等非活动设置不属于该命令的写边界。
	// When neither snapshot enables locking, inactive LFS identity/endpoint changes are outside the
	// write boundary of an ordinary Git command.
	if (!bInCommandUsingLfs
		&& !bInCurrentUsingLfs)
	{
		return true;
	}
	return InCurrentGeneration == InCommandGeneration
		&& bInCurrentUsingLfs == bInCommandUsingLfs
		&& InCurrentLfsUser == InCommandLfsUser;
}

static bool IsLockCommandConfigurationCurrent(
	const FGitSourceControlCommand& InCommand)
{
	const FGitLockSettingsSnapshot Current =
		FGitSourceControlModule::Get()
			.AccessSettings()
			.GetLockSettingsSnapshot();
	return GitSourceControlOperations::DoLockSettingsSnapshotsMatch(
		InCommand.LockSettingsGeneration,
		InCommand.bSettingsUsingGitLfsLocking,
		InCommand.LfsUserName,
		Current.Generation,
		Current.bUsingGitLfsLocking,
		Current.LfsUserName);
}

/**
 * 紧邻不可逆写入核对锁设置；LFS 模式还必须保持命令创建时的明确 symbolic branch。
 * Revalidates lock settings immediately before an irreversible write; LFS also preserves the
 * explicit symbolic branch captured for the command.
 */
static bool EnsureLockCommandWriteBoundary(
	const FGitSourceControlCommand& InCommand,
	const FString& InAction,
	TArray<FString>& OutErrorMessages)
{
	if (!IsLockCommandConfigurationCurrent(InCommand))
	{
		OutErrorMessages.Add(FString::Printf(
			TEXT("%s 被阻止：Git LFS/锁设置或 LFS 用户已在命令执行期间变化。"),
			*InAction));
		return false;
	}

	if (InCommand.bUsingGitLfsLocking)
	{
		FString CurrentBranch;
		if (!GitSourceControlUtils::GetBranchName(
				InCommand.PathToGitBinary,
				InCommand.PathToGitRoot,
				CurrentBranch)
			|| CurrentBranch.StartsWith(TEXT("HEAD detached at "))
			|| InCommand.LockBranch.IsEmpty()
			|| CurrentBranch != InCommand.LockBranch)
		{
			OutErrorMessages.Add(FString::Printf(
				TEXT("%s 被阻止：当前 symbolic branch“%s”与锁命令快照“%s”不一致。"),
				*InAction,
				*CurrentBranch,
				*InCommand.LockBranch));
			return false;
		}
	}
	return true;
}

static bool EnsureCommandLockWriteBoundary(
	const FGitSourceControlCommand& InCommand,
	const FString& InAction,
	TArray<FString>& OutErrorMessages)
{
	return EnsureLockCommandWriteBoundary(InCommand, InAction, OutErrorMessages);
}

/**
 * 所有会改动 Git 索引、工作区或引用的 worker 都从此处紧邻写入复核命令快照。
 * Every worker mutation of the Git index, working tree, or refs revalidates the command snapshot
 * here immediately before the write.
 */
static bool RunMutationAfterCommandLockBoundary(
	FGitSourceControlCommand& InCommand,
	const FString& InAction,
	TFunctionRef<bool()> InMutation,
	bool* bOutBoundaryPassed = nullptr)
{
	const bool bBoundaryPassed = EnsureCommandLockWriteBoundary(
		InCommand,
		InAction,
		InCommand.ResultInfo.ErrorMessages);
	if (bOutBoundaryPassed != nullptr)
	{
		*bOutBoundaryPassed = bBoundaryPassed;
	}
	return bBoundaryPassed && InMutation();
}

/**
 * Git 通用写命令的唯一生产入口；参数与文件列表仍由 owning worker 明确提供。
 * Single production entry for generic mutating Git commands; the owning worker still supplies the
 * exact parameters and file set.
 */
static bool RunGitCommandMutationAfterLockBoundary(
	FGitSourceControlCommand& InCommand,
	const FString& InAction,
	const FString& InSubCommand,
	const TArray<FString>& InParameters,
	const TArray<FString>& InFiles,
	TArray<FString>& OutResults,
	TArray<FString>& OutErrorMessages,
	bool* bOutBoundaryPassed = nullptr)
{
	bool bBoundaryAttempted = false;
	bool bEveryBoundaryPassed = true;
	const bool bResult = GitSourceControlUtils::RunCommandWithPreWriteBoundary(
		InSubCommand,
		InCommand.PathToGitBinary,
		InCommand.PathToRepositoryRoot,
		InParameters,
		InFiles,
		[&]()
		{
			bBoundaryAttempted = true;
			const bool bBoundaryPassed = EnsureCommandLockWriteBoundary(
				InCommand,
				InAction,
				InCommand.ResultInfo.ErrorMessages);
			bEveryBoundaryPassed &= bBoundaryPassed;
			return bBoundaryPassed;
		},
		OutResults,
		OutErrorMessages);
	if (bOutBoundaryPassed != nullptr)
	{
		*bOutBoundaryPassed = bBoundaryAttempted && bEveryBoundaryPassed;
	}
	return bResult;
}

bool GitSourceControlOperations::RunAfterLockWriteBoundary(
	TFunctionRef<bool()> InBoundaryCheck,
	TFunctionRef<bool()> InWrite)
{
	if (!InBoundaryCheck())
	{
		return false;
	}
	return InWrite();
}

bool GitSourceControlOperations::RunFreshStateCommitBeforeLockTransitions(
	TFunctionRef<bool()> InFreshStateCommit,
	bool bInCommandQueueQuiescent,
	TFunctionRef<bool()> InLockTransitionCommit)
{
	const bool bFreshStateUpdated = InFreshStateCommit();
	if (!bInCommandQueueQuiescent)
	{
		return bFreshStateUpdated;
	}
	const bool bLockTransitionUpdated = InLockTransitionCommit();
	return bFreshStateUpdated || bLockTransitionUpdated;
}

bool GitSourceControlOperations::IsStateEligibleForCheckInBoundary(
	const FGitSourceControlState& InState)
{
	if (!InState.CanCheckIn())
	{
		return false;
	}
	return true;
}

ELockState::Type GitSourceControlOperations::GetIndexMutationFallbackLockState(
	bool bUsingGitLfsLocking,
	const FString& InFilename)
{
	return bUsingGitLfsLocking
		&& GitSourceControlUtils::IsFileLFSLockable(InFilename)
			? ELockState::LockedOther
			: ELockState::Unlockable;
}

void GitSourceControlOperations::BuildPostPullPushValidationScope(
	bool bRefreshSucceeded,
	const TArray<FString>& InPrePullFiles,
	const TArray<FString>& InRefreshedFiles,
	TArray<FString>& OutValidationFiles)
{
	// 参数保留 pull 前范围是为了让调用契约显式可测：成功后绝不能意外复用它。
	// Keeping the pre-pull scope in the signature makes the invariant testable: it must never be
	// reused after a successful refresh.
	(void)InPrePullFiles;
	OutValidationFiles = bRefreshSucceeded
		? InRefreshedFiles
		: TArray<FString>();
}

bool GitSourceControlOperations::BuildLfsPushRefs(
	const FString& InLocalBranch,
	const FString& InUpstreamBranch,
	FString& OutLocalBranchRef,
	FString& OutRemoteTrackingRef,
	FString& OutPushRefSpec,
	bool& OutHasRemoteBaseline)
{
	OutLocalBranchRef.Reset();
	OutRemoteTrackingRef.Reset();
	OutPushRefSpec.Reset();
	OutHasRemoteBaseline = false;

	if (InLocalBranch.IsEmpty()
		|| InLocalBranch.StartsWith(TEXT("HEAD detached at ")))
	{
		return false;
	}

	FString OriginBranch = InLocalBranch;
	if (!InUpstreamBranch.IsEmpty())
	{
		const FString OriginPrefix(TEXT("origin/"));
		if (!InUpstreamBranch.StartsWith(OriginPrefix))
		{
			return false;
		}
		OriginBranch = InUpstreamBranch.RightChop(OriginPrefix.Len());
		if (OriginBranch.IsEmpty())
		{
			return false;
		}
		OutHasRemoteBaseline = true;
	}

	OutLocalBranchRef = FString::Printf(
		TEXT("refs/heads/%s"),
		*InLocalBranch);
	OutRemoteTrackingRef = FString::Printf(
		TEXT("refs/remotes/origin/%s"),
		*OriginBranch);
	OutPushRefSpec = FString::Printf(
		TEXT("%s:refs/heads/%s"),
		*OutLocalBranchRef,
		*OriginBranch);
	return true;
}

bool GitSourceControlOperations::RunLiteralPathListCommand(
	const FString& InSubCommand,
	const FString& InPathToGitBinary,
	const FString& InRepositoryRoot,
	const TArray<FString>& InParameters,
	TArray<FString>& OutResults,
	TArray<FString>& OutErrorMessages)
{
	return GitSourceControlUtils::RunCommandWithLiteralPaths(
		InSubCommand,
		InPathToGitBinary,
		InRepositoryRoot,
		InParameters,
		FGitSourceControlModule::GetEmptyStringArray(),
		OutResults,
		OutErrorMessages);
}

/**
 * 将 LFS 锁工作流的比较基线、本地来源和实际 push 目的统一绑定到命令分支。
 * Binds the LFS lock workflow's comparison baseline, local source, and actual push destination to
 * the command branch as one coherent target.
 */
static bool ResolveLockedPushRefs(
	const FGitSourceControlCommand& InCommand,
	FString& OutLocalBranchRef,
	FString& OutRemoteTrackingRef,
	FString& OutPushRefSpec,
	bool& OutHasRemoteBaseline,
	TArray<FString>& OutErrorMessages)
{
	if (!EnsureCommandLockWriteBoundary(
		InCommand,
		TEXT("LFS 锁工作流 push 目标解析"),
		OutErrorMessages))
	{
		return false;
	}

	FString UpstreamBranch;
	GitSourceControlUtils::GetRemoteBranchName(
		InCommand.PathToGitBinary,
		InCommand.PathToRepositoryRoot,
		UpstreamBranch);
	if (!GitSourceControlOperations::BuildLfsPushRefs(
			InCommand.LockBranch,
			UpstreamBranch,
			OutLocalBranchRef,
			OutRemoteTrackingRef,
			OutPushRefSpec,
			OutHasRemoteBaseline))
	{
		OutErrorMessages.Add(FString::Printf(
			TEXT(
				"LFS 锁工作流不支持本地分支“%s”的 upstream“%s”；只支持 origin。"),
			*InCommand.LockBranch,
			*UpstreamBranch));
		return false;
	}

	if (!OutHasRemoteBaseline)
	{
		return true;
	}

	TArray<FString> VerificationResults;
	const TArray<FString> VerificationParameters{
		TEXT("--verify"),
		TEXT("--quiet"),
		FString::Printf(
			TEXT("%s^{commit}"),
			*OutRemoteTrackingRef)};
	if (!GitSourceControlUtils::RunCommand(
		TEXT("rev-parse"),
		InCommand.PathToGitBinary,
		InCommand.PathToRepositoryRoot,
		VerificationParameters,
		FGitSourceControlModule::GetEmptyStringArray(),
		VerificationResults,
		OutErrorMessages))
	{
		OutErrorMessages.Add(FString::Printf(
			TEXT(
				"LFS 锁工作流无法读取 upstream 基线“%s”；请先 fetch 并确认远端分支存在。"),
			*OutRemoteTrackingRef));
		return false;
	}
	return true;
}

/**
 * 用命令的同代锁快照运行状态计算，并把过期结果请求传回 Provider。
 * Runs status calculation with the command's coherent lock snapshot and propagates a superseded
 * result back to the provider.
 */
static bool RunUpdateStatusForCommand(
	FGitSourceControlCommand& InCommand,
	const TArray<FString>& InFiles,
	TArray<FString>& OutErrorMessages,
	TMap<FString, FGitSourceControlState>& OutStates)
{
	const TArray<FString>* KnownChangelistFiles = nullptr;
#if ENGINE_MAJOR_VERSION == 5
	KnownChangelistFiles = &InCommand.KnownChangelistFiles;
#endif
	return GitSourceControlUtils::RunUpdateStatus(
		InCommand.PathToGitBinary,
		InCommand.PathToRepositoryRoot,
		InCommand.bUsingGitLfsLocking,
		InCommand.bSettingsUsingGitLfsLocking,
		InCommand.LockSettingsGeneration,
		InFiles,
		OutErrorMessages,
		OutStates,
		InCommand.bStatusSettingsSuperseded,
		KnownChangelistFiles);
}

static const FGitSourceControlState* FindStateByPath(
	const TMap<FString, FGitSourceControlState>& InStates,
	const FString& InFile)
{
	if (const FGitSourceControlState* ExactState = InStates.Find(InFile))
	{
		return ExactState;
	}
	for (const auto& State : InStates)
	{
		if (FPaths::IsSamePath(State.Key, InFile))
		{
			return &State.Value;
		}
	}
	return nullptr;
}

/**
 * index 已成功变化后立即读取真实 Git/锁状态；只有本地 status 本身失败才投影保守 fallback。
 * Refreshes actual Git/lock state immediately after a successful index mutation. A conservative
 * fallback is projected only when local status itself fails.
 */
static void CollectStatesAfterIndexMutation(
	FGitSourceControlCommand& InCommand,
	const TArray<FString>& InFiles,
	EFileState::Type InFallbackFileState,
	TMap<const FString, FGitState>& OutStates)
{
	TMap<FString, FGitSourceControlState> UpdatedStates;
	if (RunUpdateStatusForCommand(
			InCommand,
			InFiles,
			InCommand.ResultInfo.ErrorMessages,
			UpdatedStates))
	{
		GitSourceControlUtils::CollectNewStates(UpdatedStates, OutStates);
		return;
	}

	const FString Warning = TEXT(
		"Git index 已更新，但无法刷新文件状态；lockable LFS 文件保持失败关闭，下一次 Refresh 将重试。");
	UE_LOG(LogSourceControl, Warning, TEXT("%s"), *Warning);
	FTSMessageLog SourceControlLog("SourceControl");
	SourceControlLog.Warning(FText::FromString(Warning));
	for (const FString& File : InFiles)
	{
		const ELockState::Type FallbackLockState =
			GitSourceControlOperations::GetIndexMutationFallbackLockState(
				InCommand.bUsingGitLfsLocking,
				File);
		GitSourceControlUtils::CollectNewStates(
			TArray<FString>{File},
			OutStates,
			InFallbackFileState,
			ETreeState::Staged,
			FallbackLockState);
		if (FallbackLockState == ELockState::LockedOther)
		{
			OutStates.FindChecked(File).LockUser = TEXT("状态刷新失败");
			if (FPaths::FileExists(File))
			{
				GitSourceControlUtils::ApplyLocalReadOnlyPolicy(
					File,
					EGitLocalReadOnlyPolicy::ReadOnly);
			}
		}
	}
}

/**
 * CheckIn 必须重新读取本地 Git 状态，再进入所属操作的能力门，不能仅凭旧 UI 缓存提交。
 * Refresh local Git state before the operation-owned CheckIn gate instead of trusting the old UI cache.
 */
static bool ValidateStatesBeforeCheckIn(FGitSourceControlCommand& InCommand)
{
	TMap<FString, FGitSourceControlState> UpdatedStates;
	if (!RunUpdateStatusForCommand(
			InCommand,
			InCommand.Files,
			InCommand.ResultInfo.ErrorMessages,
			UpdatedStates))
	{
		InCommand.ResultInfo.ErrorMessages.Add(
			TEXT("提交被阻止：无法刷新所选文件的本地 Git 状态。"));
		return false;
	}

	for (const FString& File : InCommand.Files)
	{
		const FGitSourceControlState* State = FindStateByPath(UpdatedStates, File);
		if (!State)
		{
			InCommand.ResultInfo.ErrorMessages.Add(FString::Printf(
				TEXT("提交被阻止：状态刷新没有返回文件：%s"),
				*File));
			return false;
		}
	}

	for (const FString& File : InCommand.Files)
	{
		const FGitSourceControlState* RefreshedState = FindStateByPath(UpdatedStates, File);
		check(RefreshedState != nullptr);
		FGitSourceControlState BoundaryState = *RefreshedState;

		if (!GitSourceControlOperations::IsStateEligibleForCheckInBoundary(
				BoundaryState))
		{
			InCommand.ResultInfo.ErrorMessages.Add(FString::Printf(
				TEXT("提交被阻止：文件状态不允许 Check In（file=%d tree=%d lock=%d）：%s"),
				static_cast<int32>(BoundaryState.State.FileState),
				static_cast<int32>(BoundaryState.State.TreeState),
				static_cast<int32>(BoundaryState.State.LockState),
				*File));
			return false;
		}
	}
	return true;
}

/**
 * 单端 LFS 允许断网 local commit，但 push 前必须用 raw 新鲜列表拒绝其他用户的锁。
 * Single-endpoint LFS permits an offline local commit, but push must use a fresh raw listing to
 * reject locks owned by another user.
 */
static bool ValidateLegacyLfsLocksBeforePush(
	FGitSourceControlCommand& InCommand,
	const TArray<FString>& InFiles)
{
	if (!InCommand.bUsingGitLfsLocking)
	{
		return true;
	}
	TArray<FString> LockableFiles = InFiles.FilterByPredicate(
		GitSourceControlUtils::IsFileLFSLockable);
	if (LockableFiles.IsEmpty())
	{
		return true;
	}

	TMap<FString, FString> RawLocks;
	if (!GitSourceControlUtils::GetAuthoritativeLfsLocks(
			InCommand.PathToGitRoot,
			InCommand.PathToGitBinary,
			RawLocks,
			InCommand.ResultInfo.ErrorMessages))
	{
		InCommand.ResultInfo.ErrorMessages.Add(
			TEXT("Push 被阻止：无法取得新鲜 Git LFS 锁列表；本地 commit 已保留，可联网后重试 Push。"));
		return false;
	}

	for (const FString& File : LockableFiles)
	{
		const FString* Owner = RawLocks.Find(File);
		if (!Owner)
		{
			for (const auto& Lock : RawLocks)
			{
				if (FPaths::IsSamePath(Lock.Key, File))
				{
					Owner = &Lock.Value;
					break;
				}
			}
		}
		if (Owner && *Owner != InCommand.LfsUserName)
		{
			InCommand.ResultInfo.ErrorMessages.Add(FString::Printf(
				TEXT("Push 被阻止：文件当前由其他 Git LFS 用户“%s”锁定：%s"),
				**Owner,
				*File));
			return false;
		}
	}
	return true;
}

FName FGitConnectWorker::GetName() const
{
	return "Connect";
}

bool FGitConnectWorker::Execute(FGitSourceControlCommand& InCommand)
{
	// The connect worker checks if we are connected to the remote server.
	check(InCommand.Operation->GetName() == GetName());
	TSharedRef<FConnect, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FConnect>(InCommand.Operation);

	// Skip login operations, since Git does not have to login.
	// It's not a big deal for async commands though, so let those go through.
	// More information: this is a heuristic for cases where UE is trying to create
	// a valid Perforce connection as a side effect for the connect worker. For Git,
	// the connect worker has no side effects. It is simply a query to retrieve information
	// to be displayed to the user, like in the revision control settings or on init.
	// Therefore, there is no need for synchronously establishing a connection if not there.
	if (InCommand.Concurrency == EConcurrency::Synchronous)
	{
		InCommand.bCommandSuccessful = true;
		return true;
	}

	// Check Git availability
	// We already know that Git is available if PathToGitBinary is not empty, since it is validated then.
	if (InCommand.PathToGitBinary.IsEmpty())
	{
		const FText& NotFound = LOCTEXT("GitNotFound", "Failed to enable Git revision control. You need to install Git and ensure the plugin has a valid path to the git executable.");
		InCommand.ResultInfo.ErrorMessages.Add(NotFound.ToString());
		Operation->SetErrorText(NotFound);
		InCommand.bCommandSuccessful = false;
		return false;
	}

	// Get default branch: git remote show

	TArray<FString> Parameters {
		TEXT("-h"), // Only limit to branches
		TEXT("-q") // Skip printing out remote URL, we don't use it
	};

	// Check if remote matches our refs.
	// Could be useful in the future, but all we want to know right now is if connection is up.
	// Parameters.Add("--exit-code");
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("ls-remote"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), FGitSourceControlModule::GetEmptyStringArray(), InfoMessages, ErrorMessages);
	if (!InCommand.bCommandSuccessful)
	{
		const FText& NotFound = LOCTEXT("GitRemoteFailed", "Failed Git remote connection. Ensure your repo is initialized, and check your connection to the Git host.");
		InCommand.ResultInfo.ErrorMessages.Add(NotFound.ToString());
		Operation->SetErrorText(NotFound);
	}

	// TODO: always return true, and enter an offline mode if could not connect to remote
	return InCommand.bCommandSuccessful;
}

bool FGitConnectWorker::UpdateStates() const
{
	return false;
}

FName FGitCheckOutWorker::GetName() const
{
	return "CheckOut";
}

bool FGitCheckOutWorker::Execute(FGitSourceControlCommand& InCommand)
{
	// If we have nothing to process, exit immediately
	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	check(InCommand.Operation->GetName() == GetName());

	if (!InCommand.bUsingGitLfsLocking)
	{
		InCommand.bCommandSuccessful = false;
		return InCommand.bCommandSuccessful;
	}

	// lock files: execute the LFS command on relative filenames
	const TArray<FString>& RelativeFiles = GitSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToGitRoot);

	const TArray<FString>& LockableRelativeFiles = RelativeFiles.FilterByPredicate(GitSourceControlUtils::IsFileLFSLockable);

	if (LockableRelativeFiles.Num() < 1)
	{
		InCommand.bCommandSuccessful = true;
		return InCommand.bCommandSuccessful;
	}

	UE_LOG(LogSourceControl, Display, TEXT("LFS 签出上下文：仓库=%s；文件=%s"),
		*InCommand.PathToGitRoot, *FString::Join(LockableRelativeFiles, TEXT("；")));
	const FString& LockUser = InCommand.LfsUserName;
	bool bSuccess = false;
	{
		bool bLockWriteAttempted = false;
		bSuccess = GitSourceControlUtils::RunLFSCommandWithPreWriteBoundary(
			TEXT("lock"),
			InCommand.PathToGitRoot,
			InCommand.PathToGitBinary,
			FGitSourceControlModule::GetEmptyStringArray(),
			LockableRelativeFiles,
			[&]()
			{
				const bool bBoundaryPassed = EnsureLockCommandWriteBoundary(
					InCommand,
					TEXT("Git LFS lock"),
					InCommand.ResultInfo.ErrorMessages);
				bLockWriteAttempted |= bBoundaryPassed;
				return bBoundaryPassed;
			},
			InCommand.ResultInfo.InfoMessages,
			InCommand.ResultInfo.ErrorMessages);
		if (bLockWriteAttempted && !bSuccess)
		{
			// 部分托管服务（已在 git.code.tencent.com 观察到）的 lock POST 可能在服务器
			// 成功后于客户端超时；用新鲜权威列表确认全部路径仍由当前用户持有，避免把
			// 已成功的 checkout 永久表现为失败。
			// The lock endpoint of some hosts (observed on git.code.tencent.com) is flaky: the
			// lock POST can time out client-side while the lock is created server-side, after
			// which every retry fails with "lock already created" and checkout looks permanently
			// broken to the user. Confirm against a fresh listing - if we own the lock for every
			// file we asked for, this checkout has in fact succeeded.
			TArray<GitLfsUnlock::FVerifiedLock> ServerLocks;
			TArray<FString> LockListingErrors;
			TArray<FString> LockLines;
			FString ParseError;
			const bool bListed = GitSourceControlUtils::RunLFSCommand(
				TEXT("locks"), InCommand.PathToGitRoot, InCommand.PathToGitBinary,
				{TEXT("--verify")}, FGitSourceControlModule::GetEmptyStringArray(), LockLines, LockListingErrors);
			if (GitLfsUnlock::ParseVerification(bListed, LockLines, ServerLocks, ParseError))
			{
				bSuccess = true;
				for (const FString& RelativeFile : LockableRelativeFiles)
				{
					const auto* Verified = ServerLocks.FindByPredicate([&](const GitLfsUnlock::FVerifiedLock& Entry)
					{
						return FPaths::IsSamePath(Entry.RelativePath, RelativeFile);
					});
					if (!Verified || !Verified->bOurs)
					{
						bSuccess = false;
						break;
					}
				}
				if (bSuccess)
				{
					UE_LOG(LogSourceControl, Warning, TEXT("CheckOut: 'git lfs lock' reported failure but the server lists us as lock owner of all %d file(s); treating the checkout as successful."), LockableRelativeFiles.Num());
				}
			}
			else
			{
				InCommand.ResultInfo.ErrorMessages.Append(LockListingErrors);
				InCommand.ResultInfo.ErrorMessages.Add(ParseError);
			}
		}
	}
	InCommand.bCommandSuccessful = bSuccess;
	if (bSuccess)
	{
		TArray<FString> AbsoluteFiles;
		for (const auto& RelativeFile : LockableRelativeFiles)
		{
			FString AbsoluteFile = FPaths::Combine(InCommand.PathToGitRoot, RelativeFile);
			FPaths::NormalizeFilename(AbsoluteFile);
			{
				FGitLockedFilesCache::AddLockedFile(AbsoluteFile, LockUser);
			}
			AbsoluteFiles.Add(AbsoluteFile);
		}

		GitSourceControlUtils::CollectNewStates(AbsoluteFiles, States, EFileState::Unset, ETreeState::Unset, ELockState::Locked);
		for (auto& State : States)
		{
			State.Value.LockUser = LockUser;
		}
	}

	return InCommand.bCommandSuccessful;
}

bool FGitCheckOutWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

static FText ParseCommitResults(const TArray<FString>& InResults)
{
	if (InResults.Num() >= 1)
	{
		const FString& FirstLine = InResults[0];
		return FText::Format(LOCTEXT("CommitMessage", "Commited {0}."), FText::FromString(FirstLine));
	}
	return LOCTEXT("CommitMessageUnknown", "Submitted revision.");
}

FName FGitCheckInWorker::GetName() const
{
	return "CheckIn";
}

const FText EmptyCommitMsg;

/**
 * pull 后从最新远端基线重算提交范围，供 LFS push 门重新校验。
 * Recompute the committed scope from the refreshed remote baseline after a pull for LFS push validation.
 */
static bool RefreshPushScope(
	FGitSourceControlCommand& InCommand,
	const FString& InRemoteBranch,
	const FString& InLocalBranch,
	TArray<FString>& OutCommittedFiles,
	TArray<FString>& OutNewlyAddedFiles)
{
	OutCommittedFiles.Reset();
	OutNewlyAddedFiles.Reset();
	const TArray<FString> DiffParameters{
		TEXT("--name-only"),
		FString::Printf(
			TEXT("%s...%s"),
			*InRemoteBranch,
			*InLocalBranch),
		TEXT("--")};
	if (!GitSourceControlOperations::RunLiteralPathListCommand(
			TEXT("diff"),
			InCommand.PathToGitBinary,
			InCommand.PathToRepositoryRoot,
			DiffParameters,
			OutCommittedFiles,
			InCommand.ResultInfo.ErrorMessages))
	{
		return false;
	}

	const TArray<FString> AddedParameters{
		TEXT("--name-only"),
		TEXT("--diff-filter=A"),
		FString::Printf(
			TEXT("%s...%s"),
			*InRemoteBranch,
			*InLocalBranch),
		TEXT("--")};
	if (!GitSourceControlOperations::RunLiteralPathListCommand(
			TEXT("diff"),
			InCommand.PathToGitBinary,
			InCommand.PathToRepositoryRoot,
			AddedParameters,
			OutNewlyAddedFiles,
			InCommand.ResultInfo.ErrorMessages))
	{
		return false;
	}

	OutCommittedFiles = GitSourceControlUtils::AbsoluteFilenames(
		OutCommittedFiles,
		InCommand.PathToRepositoryRoot);
	OutNewlyAddedFiles = GitSourceControlUtils::AbsoluteFilenames(
		OutNewlyAddedFiles,
		InCommand.PathToRepositoryRoot);
	return true;
}

/**
 * 解锁使用完整认证核验、精确 ID 与操作内重试；不把查询失败伪装成无锁，也不重复还原文件。
 * Unlock using complete authenticated verification, exact IDs, and in-operation retries. A failed
 * listing is never an empty authoritative result, and recovery never reverts file contents again.
 */
static bool RunLFSUnlockIdempotent(FGitSourceControlCommand& InCommand, const TArray<FString>& InAbsoluteFiles,
								   TArray<FString>& OutResults, TArray<FString>& OutErrorMessages,
								   bool bKeepLocalChanges = false)
{
	const TArray<FString> RelativeFiles = GitSourceControlUtils::RelativeFilenames(InAbsoluteFiles, InCommand.PathToGitRoot);
	UE_LOG(LogSourceControl, Display, TEXT("LFS 解锁上下文：仓库=%s；文件=%s"),
		*InCommand.PathToGitRoot, *FString::Join(RelativeFiles, TEXT("；")));
	TArray<FString> ReleasedFiles;
	FString ReleaseError;
	const bool bSuccess = GitLfsUnlock::Release(
		RelativeFiles,
		[&](TArray<GitLfsUnlock::FVerifiedLock>& OutLocks, FString& OutError)
		{
			// 3.3.0 的 --json 会吞掉 SearchLocks 错误；文本 verify 保留退出码及认证身份。
			// In 3.3.0 --json swallows SearchLocks errors; text verify preserves errors and identity.
			TArray<FString> Lines;
			TArray<FString> Errors;
			const bool bListed = GitSourceControlUtils::RunLFSCommand(
				TEXT("locks"), InCommand.PathToGitRoot, InCommand.PathToGitBinary,
				{TEXT("--verify")}, FGitSourceControlModule::GetEmptyStringArray(), Lines, Errors);
			const bool bVerified = GitLfsUnlock::ParseVerification(bListed, Lines, OutLocks, OutError);
			if (!bVerified && !Errors.IsEmpty())
			{
				OutError += TEXT(" ") + FString::Join(Errors, TEXT("；"));
			}
			return bVerified;
		},
		[&](const TArray<GitLfsUnlock::FVerifiedLock>& Locks, FString& OutError)
		{
			TArray<FString> Errors;
			bool bUnlocked = true;
			const int32 BatchSize = GitSourceControlUtils::SupportsBatchedLFSUnlock() ? Locks.Num() : 1;
			for (int32 Start = 0; Start < Locks.Num(); Start += BatchSize)
			{
				TArray<FString> IdParameters;
				TArray<FString> Paths;
				TArray<FString> Results;
				for (int32 Index = Start; Index < FMath::Min(Start + BatchSize, Locks.Num()); ++Index)
				{
					IdParameters.Add(FString::Printf(TEXT("--id=\"%s\""), *Locks[Index].Id));
					Paths.Add(Locks[Index].RelativePath);
				}
				// 一个内置 LFS 进程共享连接和缓存；不并行启动多个进程争写 lockcache.db。
				// One bundled LFS process shares connections/cache instead of racing multiple cache writers.
				bUnlocked &= GitSourceControlUtils::RunLFSCommandWithPreWriteBoundary(
					TEXT("unlock"), InCommand.PathToGitRoot, InCommand.PathToGitBinary,
					IdParameters, FGitSourceControlModule::GetEmptyStringArray(),
					[&]()
					{
						if (!EnsureLockCommandWriteBoundary(InCommand, TEXT("Git LFS verified-ID unlock"), Errors))
						{
							return false;
						}
						// 主动释放锁保留内容；只有自动释放才以新出现的本地修改阻止解锁。
						// Explicit release keeps local content; only automatic release is blocked by new local edits.
						if (!bKeepLocalChanges)
						{
							TArray<FString> Status;
							if (!GitSourceControlUtils::RunStatusWithLiteralPaths(
								InCommand.PathToGitBinary, InCommand.PathToGitRoot,
								{TEXT("--porcelain"), TEXT("-uall")}, Paths, Status, Errors))
							{
								return false;
							}
							if (!Status.IsEmpty())
							{
								Errors.Add(FString::Printf(TEXT("自动解锁前出现本地修改，保留锁：%s"), *FString::Join(Paths, TEXT("；"))));
								return false;
							}
						}
						if (!EnsureLockCommandWriteBoundary(InCommand, TEXT("Git LFS batch unlock"), Errors))
						{
							return false;
						}
						UE_LOG(LogSourceControl, Display, TEXT("LFS 批量解锁：%d 个认证 ID；%s"), Paths.Num(), *FString::Join(Paths, TEXT("；")));
						return true;
					}, Results, Errors);
			}
			OutError = FString::Join(Errors, TEXT("；"));
			if (!bUnlocked)
			{
				UE_LOG(LogSourceControl, Warning, TEXT("LFS 批量请求未确认，将统一复核；不会重放已确认完成的文件：%s"), *OutError);
			}
			return bUnlocked;
		},
		[&]()
		{
			return EnsureLockCommandWriteBoundary(
				InCommand, TEXT("Git LFS unlock verification context"), OutErrorMessages);
		},
		ReleasedFiles,
		ReleaseError);
	for (const FString& RelativeFile : ReleasedFiles)
	{
		const FString AbsoluteFile = FPaths::ConvertRelativePathToFull(InCommand.PathToGitRoot, RelativeFile);
		FGitLockedFilesCache::RemoveLockedFile(AbsoluteFile);
		OutResults.Add(FString::Printf(TEXT("LFS 解锁已由远端认证核验确认：%s"), *RelativeFile));
	}
	if (!bSuccess)
	{
		OutErrorMessages.Add(ReleaseError);
	}
	return bSuccess;
}

bool FGitCheckInWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	TSharedRef<FCheckIn, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FCheckIn>(InCommand.Operation);

	// make a temp file to place our commit message in
	bool bDoCommit = InCommand.Files.Num() > 0;
	const FText& CommitMsg = bDoCommit ? Operation->GetDescription() : EmptyCommitMsg;
	FGitScopedTempFile CommitMsgFile(CommitMsg);
	if (CommitMsgFile.GetFilename().Len() > 0)
	{
		FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();

		if (bDoCommit)
		{
			if (!ValidateStatesBeforeCheckIn(InCommand))
			{
				InCommand.bCommandSuccessful = false;
				return false;
			}
			FString ParamCommitMsgFilename = TEXT("--file=\"");
			ParamCommitMsgFilename += FPaths::ConvertRelativePathToFull(CommitMsgFile.GetFilename());
			ParamCommitMsgFilename += TEXT("\"");
			TArray<FString> CommitParameters {ParamCommitMsgFilename};
			const TArray<FString>& FilesToCommit = GitSourceControlUtils::RelativeFilenames(InCommand.Files, InCommand.PathToRepositoryRoot);

			// If no files were committed, this is false, so we treat it as if we never wanted to commit in the first place.
			bool bCommitBoundaryRejected = false;
			bool bCommitStarted = false;
			bDoCommit = RunMutationAfterCommandLockBoundary(
				InCommand,
				TEXT("Git commit"),
				[
					&InCommand,
					&CommitParameters,
					&FilesToCommit,
					&bCommitBoundaryRejected
				]()
				{
					return GitSourceControlUtils::RunCommit(
						InCommand.PathToGitBinary,
						InCommand.PathToRepositoryRoot,
						CommitParameters,
						FilesToCommit,
						[&InCommand, &bCommitBoundaryRejected]()
						{
							const bool bBoundaryPassed =
								EnsureCommandLockWriteBoundary(
								InCommand,
								TEXT("Git commit 子进程"),
								InCommand.ResultInfo.ErrorMessages);
							bCommitBoundaryRejected |= !bBoundaryPassed;
							return bBoundaryPassed;
						},
						InCommand.ResultInfo.InfoMessages,
						InCommand.ResultInfo.ErrorMessages);
				},
				&bCommitStarted);
			if (!bCommitStarted)
			{
				InCommand.bCommandSuccessful = false;
				return false;
			}
			if (bCommitBoundaryRejected)
			{
				// 子进程边界拒绝不是“没有可提交内容”；前一批 add/commit 可能已经成功，必须
				// 保持现场并明确失败，禁止继续 push 或 unlock。
				// A rejected subprocess boundary is not "nothing to commit". An earlier add/commit may
				// already have succeeded, so preserve it and fail explicitly without push or unlock.
				InCommand.ResultInfo.ErrorMessages.Add(
					TEXT(
						"Git commit 子进程写边界被拒绝；此前已完成的暂存或提交保持原样，"
						"请刷新 Source Control 状态后确认。"));
				InCommand.bCommandSuccessful = false;
				return false;
			}
		}

		// If we commit, we can push up the deleted state to gone
		if (bDoCommit)
		{
			// 保留删除项的共享状态，随后由主线程的新鲜 status 移出变更列表；worker 不能
			// 提前移除缓存，否则列表会持有再也收不到更新的旧引用。
			// Keep the shared deleted-file state until the fresh status reaches the game thread.
			// Removing it here would leave changelists holding an orphaned reference.
			Operation->SetSuccessMessage(ParseCommitResults(InCommand.ResultInfo.InfoMessages));
			const FString& Message = (InCommand.ResultInfo.InfoMessages.Num() > 0) ? InCommand.ResultInfo.InfoMessages[0] : TEXT("");
			UE_LOG(LogSourceControl, Log, TEXT("commit successful: %s"), *Message);
			GitSourceControlUtils::GetCommitInfo(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.CommitId, InCommand.CommitSummary);
		}

		// Collect difference between the remote and what we have on top of remote locally. This is to handle unpushed commits other than the one we just did.
		// Doesn't matter that we're not synced. Because our local branch is always based on the remote.
		TArray<FString> CommittedFiles;
		TArray<FString> NewlyAddedCommittedFiles;
		FString BranchName;
		FString LocalBranchName = TEXT("HEAD");
		FString OriginPullBranchName;
		TArray<FString> PushParameters{
			TEXT("-u"),
			TEXT("origin"),
			TEXT("HEAD")};
		bool bDiffSuccess = false;
		bool bHasRemoteBaseline = false;
		bool bLfsPushRefsReady = !InCommand.bUsingGitLfsLocking;
		if (InCommand.bUsingGitLfsLocking)
		{
			FString PushRefSpec;
			bLfsPushRefsReady = ResolveLockedPushRefs(
				InCommand,
				LocalBranchName,
				BranchName,
				PushRefSpec,
				bHasRemoteBaseline,
				InCommand.ResultInfo.ErrorMessages);

			if (bLfsPushRefsReady)
			{
				PushParameters[2] = MoveTemp(PushRefSpec);
				const FString TrackingPrefix(TEXT("refs/remotes/origin/"));
				if (BranchName.StartsWith(TrackingPrefix))
				{
					OriginPullBranchName = BranchName.RightChop(TrackingPrefix.Len());
				}
			}
		}
		else
		{
			bHasRemoteBaseline = GitSourceControlUtils::GetRemoteBranchName(
				InCommand.PathToGitBinary,
				InCommand.PathToRepositoryRoot,
				BranchName);
		}

		if (bHasRemoteBaseline)
		{
			TArray<FString> Parameters{
				TEXT("--name-only"),
				FString::Printf(
					TEXT("%s...%s"),
					*BranchName,
					*LocalBranchName),
				TEXT("--")};
			bDiffSuccess = GitSourceControlOperations::RunLiteralPathListCommand(
				TEXT("diff"),
				InCommand.PathToGitBinary,
				InCommand.PathToRepositoryRoot,
				Parameters,
				CommittedFiles,
				InCommand.ResultInfo.ErrorMessages);

		}
		else if (bLfsPushRefsReady)
		{
			// 首推必须绑定冻结的本地分支，不能用 --branches 把其他本地分支的提交混入锁范围。
			// A first push is bound to the frozen local branch; --branches would mix commits from other
			// local branches into the lock scope.
			TArray<FString> Parameters;
			Parameters.Add(
				InCommand.bUsingGitLfsLocking
					? LocalBranchName
					: TEXT("--branches"));
			Parameters.Append({
				TEXT("--not"),
				TEXT("--remotes"),
				TEXT("--name-only"),
				TEXT("--pretty="),
				TEXT("--")});
			bDiffSuccess = GitSourceControlOperations::RunLiteralPathListCommand(
				TEXT("log"),
				InCommand.PathToGitBinary,
				InCommand.PathToRepositoryRoot,
				Parameters,
				CommittedFiles,
				InCommand.ResultInfo.ErrorMessages);
			// Dedup files list between commits
			CommittedFiles = TSet<FString>{CommittedFiles}.Array();
		}

		bool bUnpushedFiles;
		TSet<FString> FilesToCheckIn {InCommand.Files};
		if (bDiffSuccess)
		{
			// Only push if we have a difference (any commits at all, not just the one we just did)
			bUnpushedFiles = CommittedFiles.Num() > 0;
			CommittedFiles = GitSourceControlUtils::AbsoluteFilenames(CommittedFiles, InCommand.PathToRepositoryRoot);
			NewlyAddedCommittedFiles = GitSourceControlUtils::AbsoluteFilenames(
				NewlyAddedCommittedFiles,
				InCommand.PathToRepositoryRoot);
			FilesToCheckIn.Append(CommittedFiles.FilterByPredicate(GitSourceControlUtils::IsFileLFSLockable));
		}
		else
		{
			// Be cautious, try pushing anyway
			bUnpushedFiles = true;
		}

		TArray<FString> PulledFiles;
		bool bLegacyLfsPushPreflightSucceeded = true;
		if (bUnpushedFiles
			&& InCommand.bUsingGitLfsLocking)
		{
			if (!bLfsPushRefsReady || !bDiffSuccess)
			{
				InCommand.ResultInfo.ErrorMessages.Add(
					TEXT("Push 被阻止：无法确定全部未推送文件的范围；本地 commit 已保留。"));
				bLegacyLfsPushPreflightSucceeded = false;
			}
			else
			{
				bLegacyLfsPushPreflightSucceeded =
					ValidateLegacyLfsLocksBeforePush(InCommand, CommittedFiles);
			}
		}

		// If we have unpushed files, push
		if (bUnpushedFiles
			&& bLegacyLfsPushPreflightSucceeded)
		{
			InCommand.bCommandSuccessful = RunGitCommandMutationAfterLockBoundary(
				InCommand,
				TEXT("Git push"),
				TEXT("push"),
				PushParameters,
				FGitSourceControlModule::GetEmptyStringArray(),
				InCommand.ResultInfo.InfoMessages,
				InCommand.ResultInfo.ErrorMessages);

			if (!InCommand.bCommandSuccessful)
			{
				// if out of date, pull first, then try again
				bool bWasOutOfDate = false;
				for (const auto& PushError : InCommand.ResultInfo.ErrorMessages)
				{
					if ((PushError.Contains(TEXT("[rejected]")) && (PushError.Contains(TEXT("non-fast-forward")) || PushError.Contains(TEXT("fetch first")))) ||
						PushError.Contains(TEXT("cannot lock ref")))
					{
						// Don't do it during iteration, want to append pull results to InCommand.ResultInfo.ErrorMessages
						bWasOutOfDate = true;
						break;
					}
				}
				if (bWasOutOfDate)
				{
					// Get latest
					const bool bFetched = GitSourceControlUtils::FetchRemote(
						InCommand.PathToGitBinary,
						InCommand.PathToRepositoryRoot,
						false,
						[&InCommand]()
						{
							return EnsureCommandLockWriteBoundary(
								InCommand,
								TEXT("Git fetch before push retry"),
								InCommand.ResultInfo.ErrorMessages);
						},
						InCommand.ResultInfo.InfoMessages,
						InCommand.ResultInfo.ErrorMessages);
					if (bFetched)
					{
						// Update local with latest
						const bool bPulled = RunMutationAfterCommandLockBoundary(
							InCommand,
							TEXT("Git pull 重试"),
							[&]()
							{
								return GitSourceControlUtils::PullOrigin(
									InCommand.PathToGitBinary,
									InCommand.PathToRepositoryRoot,
									FGitSourceControlModule::GetEmptyStringArray(),
									PulledFiles,
									InCommand.ResultInfo.InfoMessages,
									InCommand.ResultInfo.ErrorMessages,
									[&InCommand]()
									{
										return EnsureCommandLockWriteBoundary(
											InCommand,
											TEXT("Git pull retry subprocess"),
											InCommand.ResultInfo.ErrorMessages);
									},
									InCommand.bUsingGitLfsLocking
										? OriginPullBranchName
										: FString());
							});
						if (bPulled)
						{
							// pull 会改变远端基线；先重算“哪些文件仍是新增”，再刷新双方服务器。
							// A pull changes the remote baseline. Recompute which files are still newly
							// added before refreshing both lock servers.
							TArray<FString> RetryCommittedFiles;
							TArray<FString> RetryNewlyAddedFiles;
							const bool bNeedsRetryScope =
								InCommand.bUsingGitLfsLocking;
							const bool bRetryScopeReady = !bNeedsRetryScope
								|| RefreshPushScope(
									InCommand,
									BranchName,
									LocalBranchName,
									RetryCommittedFiles,
									RetryNewlyAddedFiles);
							TArray<FString> RetryValidationFiles;
							GitSourceControlOperations::BuildPostPullPushValidationScope(
								bRetryScopeReady,
								CommittedFiles,
								RetryCommittedFiles,
								RetryValidationFiles);
							const bool bRetryLegacyLfsLocksValid = bRetryScopeReady
								&& (!InCommand.bUsingGitLfsLocking
									|| ValidateLegacyLfsLocksBeforePush(
										InCommand,
										RetryValidationFiles));
							const bool bRetryWriteBoundaryValid =
								bRetryLegacyLfsLocksValid;
							if (bRetryWriteBoundaryValid)
							{
								InCommand.bCommandSuccessful =
									RunGitCommandMutationAfterLockBoundary(
										InCommand,
										TEXT("Git push 重试"),
										TEXT("push"),
										PushParameters,
										FGitSourceControlModule::GetEmptyStringArray(),
										InCommand.ResultInfo.InfoMessages,
										InCommand.ResultInfo.ErrorMessages);

							}
						}
					}

					// Our push still wasn't successful
					if (!InCommand.bCommandSuccessful)
					{
						if (!Provider.bPendingRestart)
						{
							// If it fails, just let the user do it
							FText PushFailMessage(LOCTEXT("GitPush_OutOfDate_Msg", "Git Push failed because there are changes you need to pull.\n\n"
																				   "An attempt was made to pull, but failed, because while the Unreal Editor is "
																				   "open, files cannot always be updated.\n\n"
																				   "Please exit the editor, and update the project again."));
							FText PushFailTitle(LOCTEXT("GitPush_OutOfDate_Title", "Git Pull Required"));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
							FMessageDialog::Open(EAppMsgType::Ok, PushFailMessage, PushFailTitle);
#else
							FMessageDialog::Open(EAppMsgType::Ok, PushFailMessage, &PushFailTitle);
#endif
							UE_LOG(LogSourceControl, Log, TEXT("Push failed because we're out of date, prompting user to resolve manually"));
						}
					}
				}
			}
		}
		else if (bUnpushedFiles)
		{
			InCommand.bCommandSuccessful = false;
		}
		else
		{
			InCommand.bCommandSuccessful = true;
		}

		// git-lfs: unlock files
		if (InCommand.bUsingGitLfsLocking)
		{
			// If we successfully pushed (or didn't need to push), unlock the files marked for check in
			if (InCommand.bCommandSuccessful)
			{
				// unlock files: execute the LFS command on relative filenames
				// (unlock only locked files, that is, not Added files)
				TArray<FString> LockedFiles;
				GitSourceControlUtils::GetLockedFiles(FilesToCheckIn.Array(), LockedFiles);
				if (LockedFiles.Num() > 0)
				{
					// Not strictly necessary to succeed, so don't update command success.
					// The idempotent helper purges the lock cache when the server confirms the
					// locks are gone, so a failed POST no longer strands phantom locks there.
					RunLFSUnlockIdempotent(InCommand, LockedFiles, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
				}
			}
		}

		// Collect all the files we touched through the pull update
		if (bUnpushedFiles && PulledFiles.Num())
		{
			FilesToCheckIn.Append(PulledFiles);
		}
		// Before, we added only lockable files from CommittedFiles. But now, we want to update all files, not just lockables.
		FilesToCheckIn.Append(CommittedFiles);

		// now update the status of our files
		TMap<FString, FGitSourceControlState> UpdatedStates;
		bool bSuccess = RunUpdateStatusForCommand(
			InCommand,
			FilesToCheckIn.Array(),
			InCommand.ResultInfo.ErrorMessages,
			UpdatedStates);
		if (bSuccess)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		return InCommand.bCommandSuccessful;
	}

	InCommand.bCommandSuccessful = false;

	return false;
}

bool FGitCheckInWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitMarkForAddWorker::GetName() const
{
	return "MarkForAdd";
}

bool FGitMarkForAddWorker::Execute(FGitSourceControlCommand& InCommand)
{
	// If we have nothing to process, exit immediately
	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	check(InCommand.Operation->GetName() == GetName());

	InCommand.bCommandSuccessful = RunGitCommandMutationAfterLockBoundary(
		InCommand,
		TEXT("Git add"),
		TEXT("add"),
		FGitSourceControlModule::GetEmptyStringArray(),
		InCommand.Files,
		InCommand.ResultInfo.InfoMessages,
		InCommand.ResultInfo.ErrorMessages);

	if (InCommand.bCommandSuccessful)
	{
		CollectStatesAfterIndexMutation(
			InCommand,
			InCommand.Files,
			EFileState::Added,
			States);
	}
	else
	{
		TMap<FString, FGitSourceControlState> UpdatedStates;
		bool bSuccess = RunUpdateStatusForCommand(
			InCommand,
			InCommand.Files,
			InCommand.ResultInfo.ErrorMessages,
			UpdatedStates);
		if (bSuccess)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	}

	return InCommand.bCommandSuccessful;
}

bool FGitMarkForAddWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitDeleteWorker::GetName() const
{
	return "Delete";
}

bool FGitDeleteWorker::Execute(FGitSourceControlCommand& InCommand)
{
	// If we have nothing to process, exit immediately
	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	check(InCommand.Operation->GetName() == GetName());

	InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommand(TEXT("rm"), InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, FGitSourceControlModule::GetEmptyStringArray(), InCommand.Files, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);

	if (InCommand.bCommandSuccessful)
	{
		GitSourceControlUtils::CollectNewStates(InCommand.Files, States, EFileState::Deleted, ETreeState::Staged);
	}
	else
	{
		TMap<FString, FGitSourceControlState> UpdatedStates;
		bool bSuccess = RunUpdateStatusForCommand(InCommand, InCommand.Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		if (bSuccess)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	}

	return InCommand.bCommandSuccessful;
}

bool FGitDeleteWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

// Get lists of Missing files (ie "deleted"), Modified files, and "other than Added" Existing files
void GetMissingVsExistingFiles(const TArray<FString>& InFiles, TArray<FString>& OutMissingFiles, TArray<FString>& OutAllExistingFiles, TArray<FString>& OutOtherThanAddedExistingFiles)
{
	FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();

	const TArray<FString> Files = (InFiles.Num() > 0) ? (InFiles) : (Provider.GetFilesInCache());

	TArray<TSharedRef<ISourceControlState, ESPMode::ThreadSafe>> LocalStates;
	Provider.GetState(Files, LocalStates, EStateCacheUsage::Use);
	for (const auto& State : LocalStates)
	{
		if (FPaths::FileExists(State->GetFilename()))
		{
			if (State->IsAdded())
			{
				OutAllExistingFiles.Add(State->GetFilename());
			}
			else if (State->IsModified())
			{
				OutOtherThanAddedExistingFiles.Add(State->GetFilename());
				OutAllExistingFiles.Add(State->GetFilename());
			}
			else if (State->IsSourceControlled())
			{
				// 显式 Revert 不能因瞬时“无锁”图标被跳过；远端解锁核验由 worker 负责。
				// A transient unlocked badge must not skip an explicit Revert; the worker verifies release.
				OutOtherThanAddedExistingFiles.Add(State->GetFilename());
			}
		}
		else
		{
			// If already queued for deletion, don't try to delete again
			if (State->IsSourceControlled() && !State->IsDeleted())
			{
				OutMissingFiles.Add(State->GetFilename());
			}
		}
	}
}

FName FGitRevertWorker::GetName() const
{
	return "Revert";
}

bool FGitRevertWorker::Execute(FGitSourceControlCommand& InCommand)
{
	InCommand.bCommandSuccessful = true;

	// Soft Revert 表示只释放锁；必须在任何 reset/checkout/取消新增操作之前分流，保留工作区与索引。
	// Soft Revert releases locks only. Branch before any reset/checkout/unadd so both worktree and index remain intact.
	const auto RevertOperation = StaticCastSharedRef<FRevert>(InCommand.Operation);
	if (RevertOperation->IsSoftRevert())
	{
		if (!InCommand.bUsingGitLfsLocking || InCommand.Files.IsEmpty())
		{
			InCommand.ResultInfo.ErrorMessages.Add(TEXT("主动解锁需要启用 Git LFS，并明确指定文件；不会按空范围操作整仓库。"));
			InCommand.bCommandSuccessful = false;
			return false;
		}
		const bool bReleased = RunLFSUnlockIdempotent(InCommand, InCommand.Files,
			InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages, true);
		TMap<FString, FGitSourceControlState> UpdatedStates;
		const bool bRefreshed = RunUpdateStatusForCommand(InCommand, InCommand.Files,
			InCommand.ResultInfo.ErrorMessages, UpdatedStates);
		if (bRefreshed)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
		InCommand.bCommandSuccessful = bReleased && bRefreshed;
		return InCommand.bCommandSuccessful;
	}

	// Filter files by status
	TArray<FString> MissingFiles;
	TArray<FString> AllExistingFiles;
	TArray<FString> OtherThanAddedExistingFiles;
	GetMissingVsExistingFiles(InCommand.Files, MissingFiles, AllExistingFiles, OtherThanAddedExistingFiles);

	const bool bRevertAll = InCommand.Files.Num() < 1;
	if (bRevertAll)
	{
		TArray<FString> Parms;
		Parms.Add(TEXT("--hard"));
		InCommand.bCommandSuccessful &= RunGitCommandMutationAfterLockBoundary(
			InCommand,
			TEXT("Git reset --hard"),
			TEXT("reset"),
			Parms,
			FGitSourceControlModule::GetEmptyStringArray(),
			InCommand.ResultInfo.InfoMessages,
			InCommand.ResultInfo.ErrorMessages);

		Parms.Reset(2);
		Parms.Add(TEXT("-f")); // force
		Parms.Add(TEXT("-d")); // remove directories
		InCommand.bCommandSuccessful &= RunGitCommandMutationAfterLockBoundary(
			InCommand,
			TEXT("Git clean -fd"),
			TEXT("clean"),
			Parms,
			FGitSourceControlModule::GetEmptyStringArray(),
			InCommand.ResultInfo.InfoMessages,
			InCommand.ResultInfo.ErrorMessages);
	}
	else
	{
		if (MissingFiles.Num() > 0)
		{
			// "Added" files that have been deleted needs to be removed from revision control
			InCommand.bCommandSuccessful &= RunGitCommandMutationAfterLockBoundary(
				InCommand,
				TEXT("Git rm during revert"),
				TEXT("rm"),
				FGitSourceControlModule::GetEmptyStringArray(),
				MissingFiles,
				InCommand.ResultInfo.InfoMessages,
				InCommand.ResultInfo.ErrorMessages);
		}
		if (AllExistingFiles.Num() > 0)
		{
			// reset and revert any changes already added to the index
			InCommand.bCommandSuccessful &= RunGitCommandMutationAfterLockBoundary(
				InCommand,
				TEXT("Git reset during revert"),
				TEXT("reset"),
				FGitSourceControlModule::GetEmptyStringArray(),
				AllExistingFiles,
				InCommand.ResultInfo.InfoMessages,
				InCommand.ResultInfo.ErrorMessages);
			InCommand.bCommandSuccessful &= RunGitCommandMutationAfterLockBoundary(
				InCommand,
				TEXT("Git checkout during revert"),
				TEXT("checkout"),
				FGitSourceControlModule::GetEmptyStringArray(),
				AllExistingFiles,
				InCommand.ResultInfo.InfoMessages,
				InCommand.ResultInfo.ErrorMessages);
		}
		if (OtherThanAddedExistingFiles.Num() > 0)
		{
			// revert any changes in working copy (this would fails if the asset was in "Added" state, since after "reset" it is now "untracked")
			// may need to try a few times due to file locks from prior operations
			bool CheckoutSuccess = false;
			int32 Attempts = 10;
			while( Attempts-- > 0 )
			{
				bool bWriteBoundaryPassed = false;
				CheckoutSuccess = RunGitCommandMutationAfterLockBoundary(
					InCommand,
					TEXT("Git checkout retry during revert"),
					TEXT("checkout"),
					FGitSourceControlModule::GetEmptyStringArray(),
					OtherThanAddedExistingFiles,
					InCommand.ResultInfo.InfoMessages,
					InCommand.ResultInfo.ErrorMessages,
					&bWriteBoundaryPassed);
				if (CheckoutSuccess)
				{
					break;
				}
				if (!bWriteBoundaryPassed)
				{
					break;
				}

				FPlatformProcess::Sleep(0.1f);
			}

			InCommand.bCommandSuccessful &= CheckoutSuccess;
		}
	}

	if (InCommand.bUsingGitLfsLocking)
	{
		// unlock files: execute the LFS command on relative filenames
		// (unlock only locked files, that is, not Added files)
		TArray<FString> LockedFiles;
		{
			// 普通 LFS 的解锁与无锁确认不能由可能过期的图标缓存筛掉。
			// A stale badge cache must not skip ordinary LFS release or authoritative absence checks.
			LockedFiles = OtherThanAddedExistingFiles.FilterByPredicate(GitSourceControlUtils::IsFileLFSLockable);
		}
		if (LockedFiles.Num() > 0)
		{
			// A revert works on the working tree and cannot undo local commits: files whose
			// commits still await push keep their lock (releasing it would leave the unpushed
			// content unprotected while looking like a successful revert). Reachable despite
			// CanRevert() vetoing such files because a mixed multi-selection runs the whole
			// batch. The push releases these locks.
			TArray<TSharedRef<ISourceControlState, ESPMode::ThreadSafe>> LockedStates;
			FGitSourceControlModule::Get().GetProvider().GetState(LockedFiles, LockedStates, EStateCacheUsage::Use);
			for (const auto& State : LockedStates)
			{
				const TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe>& GitState = StaticCastSharedRef<FGitSourceControlState>(State);
				if (GitState->State.RemoteState == ERemoteState::AheadUnpushed)
				{
					LockedFiles.Remove(GitState->GetFilename());
				}
			}
		}
		if (LockedFiles.Num() > 0)
		{
			// At this point the git revert itself has already completed; only a genuine
			// still-held lock should fail the operation, not unlocking an already-released
			// (phantom) lock the state cache remembered.
			InCommand.bCommandSuccessful &= RunLFSUnlockIdempotent(InCommand, LockedFiles, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
		}
	}

	// If no files were specified (full revert), refresh all relevant files instead of the specified files (which is an empty list in full revert)
	// This is required so that files that were "Marked for add" have their status updated after a full revert.
	TArray<FString> FilesToUpdate = InCommand.Files;
	if (InCommand.Files.Num() <= 0)
	{
		for (const auto& File : MissingFiles) FilesToUpdate.Add(File);
		for (const auto& File : AllExistingFiles) FilesToUpdate.Add(File);
		for (const auto& File : OtherThanAddedExistingFiles) FilesToUpdate.Add(File);
	}

	// now update the status of our files
	TMap<FString, FGitSourceControlState> UpdatedStates;
	bool bSuccess = RunUpdateStatusForCommand(
		InCommand,
		FilesToUpdate,
		InCommand.ResultInfo.ErrorMessages,
		UpdatedStates);
	if (bSuccess)
	{
		GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
	}
	GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));

	return InCommand.bCommandSuccessful;
}

bool FGitRevertWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitSyncWorker::GetName() const
{
	return "Sync";
}

bool FGitSyncWorker::Execute(FGitSourceControlCommand& InCommand)
{
	const TSharedRef<FSync, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FSync>(InCommand.Operation);
	if (!Operation->GetRevision().IsEmpty() || Operation->IsLastSyncedFlagSet())
	{
		// UE 的 Writable/Uncontrolled Revert 使用强制同步到当前修订；这不是拉取远端或切分支。
		// UE Writable/Uncontrolled Revert requests a forced revision sync, not a pull or branch switch.
		InCommand.bCommandSuccessful = false;
		if (!Operation->IsForced() || Operation->IsHeadRevisionFlagSet()
			|| (Operation->IsLastSyncedFlagSet() && !Operation->GetRevision().IsEmpty())
			|| InCommand.Files.IsEmpty())
		{
			InCommand.ResultInfo.ErrorMessages.Add(TEXT("指定修订的同步需要明确强制覆盖、唯一修订来源和非空文件范围；未修改文件。"));
			return false;
		}
		TArray<FString> Files;
		for (const FString& File : InCommand.Files)
		{
			const FString AbsoluteFile = FPaths::ConvertRelativePathToFull(InCommand.PathToRepositoryRoot, File);
			if (File.IsEmpty() || FPaths::DirectoryExists(AbsoluteFile)
				|| AbsoluteFile.Contains(TEXT("\"")) || AbsoluteFile.Contains(TEXT("\r")) || AbsoluteFile.Contains(TEXT("\n"))
				|| !FPaths::IsUnderDirectory(AbsoluteFile, InCommand.PathToGitRoot))
			{
				InCommand.ResultInfo.ErrorMessages.Add(FString::Printf(TEXT("还原范围必须是当前仓库内的明确文件：%s"), *File));
				return false;
			}
			Files.AddUnique(AbsoluteFile);
		}
		const FString Revision = Operation->IsLastSyncedFlagSet() ? TEXT("HEAD") : Operation->GetRevision();
		if (Revision.Contains(TEXT("\"")) || Revision.Contains(TEXT("\\"))
			|| Revision.Contains(TEXT("\r")) || Revision.Contains(TEXT("\n")) || Revision.Contains(TEXT("\t")))
		{
			InCommand.ResultInfo.ErrorMessages.Add(TEXT("修订参数含不安全字符；未修改文件。"));
			return false;
		}
		TArray<FString> ResolvedRevision;
		bool bResolved = GitSourceControlUtils::RunCommand(TEXT("rev-parse"), InCommand.PathToGitBinary,
			InCommand.PathToGitRoot, {TEXT("--verify"), TEXT("--end-of-options"), FString::Printf(TEXT("\"%s^{commit}\""), *Revision)},
			{}, ResolvedRevision, InCommand.ResultInfo.ErrorMessages);
		bResolved = bResolved && ResolvedRevision.Num() == 1;
		if (bResolved)
		{
			bResolved = ResolvedRevision[0].Len() == 40 || ResolvedRevision[0].Len() == 64;
			for (TCHAR Character : ResolvedRevision[0]) { bResolved &= FChar::IsHexDigit(Character); }
		}
		if (!bResolved)
		{
			InCommand.ResultInfo.ErrorMessages.Add(FString::Printf(TEXT("无法解析本地提交修订：%s；未修改文件。"), *Revision));
			return false;
		}
		InCommand.bCommandSuccessful = GitSourceControlUtils::RunCommandWithPreWriteBoundary(
			TEXT("--literal-pathspecs"), InCommand.PathToGitBinary, InCommand.PathToGitRoot,
			{TEXT("checkout"), ResolvedRevision[0], TEXT("--")}, Files,
			[&InCommand]()
			{
				return EnsureCommandLockWriteBoundary(InCommand, TEXT("Git restore selected revision"), InCommand.ResultInfo.ErrorMessages);
			}, InCommand.ResultInfo.InfoMessages, InCommand.ResultInfo.ErrorMessages);
		TMap<FString, FGitSourceControlState> UpdatedStates;
		if (RunUpdateStatusForCommand(InCommand, Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates))
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
		return InCommand.bCommandSuccessful;
	}

	TArray<FString> Results;
	const bool bFetched = GitSourceControlUtils::FetchRemote(
		InCommand.PathToGitBinary,
		InCommand.PathToRepositoryRoot,
		false,
		[&InCommand]()
		{
			return EnsureCommandLockWriteBoundary(
				InCommand,
				TEXT("Git fetch before pull"),
				InCommand.ResultInfo.ErrorMessages);
		},
		InCommand.ResultInfo.InfoMessages,
		InCommand.ResultInfo.ErrorMessages);
	if (!bFetched)
	{
		return false;
	}

	InCommand.bCommandSuccessful = RunMutationAfterCommandLockBoundary(
		InCommand,
		TEXT("Git pull"),
		[&]()
		{
			return GitSourceControlUtils::PullOrigin(
				InCommand.PathToGitBinary,
				InCommand.PathToRepositoryRoot,
				InCommand.Files,
				InCommand.Files,
				Results,
				InCommand.ResultInfo.ErrorMessages,
				[&InCommand]()
				{
					return EnsureCommandLockWriteBoundary(
						InCommand,
						TEXT("Git pull subprocess"),
						InCommand.ResultInfo.ErrorMessages);
				});
		});

	// now update the status of our files
	TMap<FString, FGitSourceControlState> UpdatedStates;
	const bool bSuccess = RunUpdateStatusForCommand(
		InCommand,
		InCommand.Files,
		InCommand.ResultInfo.ErrorMessages,
		UpdatedStates);
	if (bSuccess)
	{
		GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
	}
	GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	GitSourceControlUtils::GetCommitInfo(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.CommitId, InCommand.CommitSummary);

	return InCommand.bCommandSuccessful;
}

bool FGitSyncWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitFetch::GetName() const
{
	return "Fetch";
}

FText FGitFetch::GetInProgressString() const
{
	// TODO Configure origin
	return LOCTEXT("SourceControl_Push", "Fetching from remote origin...");
}

FName FGitFetchWorker::GetName() const
{
	return "Fetch";
}

bool FGitFetchWorker::Execute(FGitSourceControlCommand& InCommand)
{
	InCommand.bCommandSuccessful = GitSourceControlUtils::FetchRemote(
		InCommand.PathToGitBinary,
		InCommand.PathToRepositoryRoot,
		InCommand.bUsingGitLfsLocking,
		[&InCommand]()
		{
			return EnsureCommandLockWriteBoundary(
				InCommand,
				TEXT("Git fetch"),
				InCommand.ResultInfo.ErrorMessages);
		},
		InCommand.ResultInfo.InfoMessages,
		InCommand.ResultInfo.ErrorMessages);
	if (!InCommand.bCommandSuccessful)
	{
		return false;
	}

	check(InCommand.Operation->GetName() == GetName());
	TSharedRef<FGitFetch, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FGitFetch>(InCommand.Operation);

	if (Operation->bUpdateStatus)
	{

		// Now update the status of all our files
		const TArray<FString> ProjectDirs = GitSourceControlUtils::GetSourceControlledAssetPaths();

		TMap<FString, FGitSourceControlState> UpdatedStates;
		InCommand.bCommandSuccessful = RunUpdateStatusForCommand(
			InCommand,
			ProjectDirs,
			InCommand.ResultInfo.ErrorMessages,
			UpdatedStates);
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (InCommand.bCommandSuccessful)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
	}

	return InCommand.bCommandSuccessful;
}

bool FGitFetchWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitUpdateStatusWorker::GetName() const
{
	return "UpdateStatus";
}

bool FGitUpdateStatusWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FUpdateStatus>(InCommand.Operation);

	// 事务扫描完整权威 LFS 列表，所以单文件状态刷新也不会遗漏此前由 UGit 创建的锁。
	// The transaction scans the full authoritative LFS list, so even a one-file status refresh
	// cannot miss a lock previously created by UGit.

	if(InCommand.Files.Num() > 0)
	{
		TMap<FString, FGitSourceControlState> UpdatedStates;
		InCommand.bCommandSuccessful = RunUpdateStatusForCommand(
			InCommand,
			InCommand.Files,
			InCommand.ResultInfo.ErrorMessages,
			UpdatedStates);
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (InCommand.bCommandSuccessful)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
			if (Operation->ShouldUpdateHistory())
			{
				for (const auto& State : UpdatedStates)
				{
					const FString& File = State.Key;
					TGitSourceControlHistory History;

					// 当前修订始终取本地 HEAD 的首项；对端历史只作为冲突参考，不能覆盖本地基线。
					// Current revision is the first local HEAD entry; conflict-side history is supplemental only.
					const bool bLocalHistorySucceeded = GitSourceControlUtils::RunGetHistory(
						InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, File, false,
						InCommand.ResultInfo.ErrorMessages, History);
					InCommand.bCommandSuccessful &= bLocalHistorySucceeded;
					if (!bLocalHistorySucceeded)
					{
						History.Reset();
					}
					else if (!History.IsEmpty() && State.Value.IsConflicted())
					{
						TGitSourceControlHistory MergeHistory;
						if (GitSourceControlUtils::RunGetHistory(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, File, true,
							InCommand.ResultInfo.ErrorMessages, MergeHistory))
						{
							History.Append(MergeHistory);
							for (int32 Index = 0; Index < History.Num(); ++Index)
							{
								History[Index]->RevisionNumber = History.Num() - Index;
							}
						}
					}
					Histories.Add(*File, History);
				}
			}
		}
	}
	else
	{
		// no path provided: only update the status of assets in Content/ directory and also Config files
		const TArray<FString> ProjectDirs = GitSourceControlUtils::GetSourceControlledAssetPaths();

		TMap<FString, FGitSourceControlState> UpdatedStates;
		InCommand.bCommandSuccessful = RunUpdateStatusForCommand(
			InCommand,
			ProjectDirs,
			InCommand.ResultInfo.ErrorMessages,
			UpdatedStates);
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (InCommand.bCommandSuccessful)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
	}

	GitSourceControlUtils::GetCommitInfo(InCommand.PathToGitBinary, InCommand.PathToRepositoryRoot, InCommand.CommitId, InCommand.CommitSummary);

	// don't use the ShouldUpdateModifiedState() hint here as it is specific to Perforce: the above normal Git status has already told us this information (like Git and Mercurial)

	return InCommand.bCommandSuccessful;
}

bool FGitUpdateStatusWorker::UpdateStates() const
{
	bool bUpdated = GitSourceControlUtils::UpdateCachedStates(States);

	FGitSourceControlModule& GitSourceControl = FModuleManager::GetModuleChecked<FGitSourceControlModule>( "GitSourceControl" );
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
	const bool bUsingGitLfsLocking = Provider.UsesCheckout();

	// TODO without LFS : Workaround a bug with the Source Control Module not updating file state after a simple "Save" with no "Checkout" (when not using File Lock)
	const FDateTime Now = bUsingGitLfsLocking ? FDateTime::Now() : FDateTime::MinValue();

	// add history, if any
	for(const auto& History : Histories)
	{
		TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(History.Key);
		State->History = History.Value;
		State->TimeStamp = Now;
		bUpdated = true;
	}

	return bUpdated;
}

FName FGitCopyWorker::GetName() const
{
	return "Copy";
}

bool FGitCopyWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	// Copy or Move operation on a single file : Git does not need an explicit copy nor move,
	// but after a Move the Editor create a redirector file with the old asset name that points to the new asset.
	// The redirector needs to be committed with the new asset to perform a real rename.
	// => the following is to "MarkForAdd" the redirector, but it still need to be committed by selecting the whole directory and "check-in"
	InCommand.bCommandSuccessful = RunGitCommandMutationAfterLockBoundary(
		InCommand,
		TEXT("Git add copied file"),
		TEXT("add"),
		FGitSourceControlModule::GetEmptyStringArray(),
		InCommand.Files,
		InCommand.ResultInfo.InfoMessages,
		InCommand.ResultInfo.ErrorMessages);

	if (InCommand.bCommandSuccessful)
	{
		CollectStatesAfterIndexMutation(
			InCommand,
			InCommand.Files,
			EFileState::Added,
			States);
	}
	else
	{
		TMap<FString, FGitSourceControlState> UpdatedStates;
		const bool bSuccess = RunUpdateStatusForCommand(
			InCommand,
			InCommand.Files,
			InCommand.ResultInfo.ErrorMessages,
			UpdatedStates);
		GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
		if (bSuccess)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
	}

	return InCommand.bCommandSuccessful;
}

bool FGitCopyWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

FName FGitResolveWorker::GetName() const
{
	return "Resolve";
}

bool FGitResolveWorker::Execute( class FGitSourceControlCommand& InCommand )
{
	check(InCommand.Operation->GetName() == GetName());

	// mark the conflicting files as resolved:
	TArray<FString> Results;
	InCommand.bCommandSuccessful = RunGitCommandMutationAfterLockBoundary(
		InCommand,
		TEXT("Git add resolved file"),
		TEXT("add"),
		FGitSourceControlModule::GetEmptyStringArray(),
		InCommand.Files,
		Results,
		InCommand.ResultInfo.ErrorMessages);

	// now update the status of our files
	TMap<FString, FGitSourceControlState> UpdatedStates;
	const bool bSuccess = RunUpdateStatusForCommand(
		InCommand,
		InCommand.Files,
		InCommand.ResultInfo.ErrorMessages,
		UpdatedStates);
	GitSourceControlUtils::RemoveRedundantErrors(InCommand, TEXT("' is outside repository"));
	if (bSuccess)
	{
		GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
	}

	return InCommand.bCommandSuccessful;
}

bool FGitResolveWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

#if ENGINE_MAJOR_VERSION == 5
FName FGitMoveToChangelistWorker::GetName() const
{
	return "MoveToChangelist";
}

bool FGitMoveToChangelistWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}

bool FGitMoveToChangelistWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());

	FGitSourceControlChangelist DestChangelist = InCommand.Changelist;
	bool bResult = false;
	if(DestChangelist.GetName().Equals(TEXT("Staged")))
	{
		bResult = RunGitCommandMutationAfterLockBoundary(
			InCommand,
			TEXT("Git add to staged changelist"),
			TEXT("add"),
			FGitSourceControlModule::GetEmptyStringArray(),
			InCommand.Files,
			InCommand.ResultInfo.InfoMessages,
			InCommand.ResultInfo.ErrorMessages);
	}
	else if(DestChangelist.GetName().Equals(TEXT("Working")))
	{
		TArray<FString> Parameter;
		Parameter.Add(TEXT("--staged"));
		bResult = RunGitCommandMutationAfterLockBoundary(
			InCommand,
			TEXT("Git restore from staged changelist"),
			TEXT("restore"),
			Parameter,
			InCommand.Files,
			InCommand.ResultInfo.InfoMessages,
			InCommand.ResultInfo.ErrorMessages);
	}

	if (bResult)
	{
		TMap<FString, FGitSourceControlState> UpdatedStates;
		bResult = RunUpdateStatusForCommand(
			InCommand,
			InCommand.Files,
			InCommand.ResultInfo.ErrorMessages,
			UpdatedStates);
		if (bResult)
		{
			GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
		}
	}
	InCommand.bCommandSuccessful = bResult;
	return bResult;
}

FName FGitUpdateStagingWorker::GetName() const
{
	return "UpdateChangelistsStatus";
}

bool FGitUpdateStagingWorker::Execute(FGitSourceControlCommand& InCommand)
{
	check(InCommand.Operation->GetName() == GetName());
	const TArray<FString> Files = InCommand.Files.IsEmpty()
		? GitSourceControlUtils::GetSourceControlledAssetPaths() : InCommand.Files;
	TMap<FString, FGitSourceControlState> UpdatedStates;
	InCommand.bCommandSuccessful = RunUpdateStatusForCommand(
		InCommand, Files, InCommand.ResultInfo.ErrorMessages, UpdatedStates);
	if (InCommand.bCommandSuccessful)
	{
		GitSourceControlUtils::CollectNewStates(UpdatedStates, States);
	}
	return InCommand.bCommandSuccessful;
}

bool FGitUpdateStagingWorker::UpdateStates() const
{
	return GitSourceControlUtils::UpdateCachedStates(States);
}
#endif

#undef LOCTEXT_NAMESPACE
