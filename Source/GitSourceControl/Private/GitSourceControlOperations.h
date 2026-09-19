// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "IGitSourceControlWorker.h"
#include "GitSourceControlState.h"

#include "ISourceControlOperation.h"

namespace GitSourceControlOperations
{
	/** commit 拥有层的最终状态门：新鲜文件状态必须允许 CanCheckIn。 */
	bool IsStateEligibleForCheckInBoundary(
		const FGitSourceControlState& InState);

	/** 已明确请求删除时允许丢弃本地修改，仍拒绝过期版本、冲突或不允许的锁状态。 */
	bool IsStateEligibleForDeleteBoundary(const FGitSourceControlState& InState);

	/** index mutation 的状态刷新失败时：仅 lockable LFS 文件失败关闭，普通文件保持 Unlockable。 */
	ELockState::Type GetIndexMutationFallbackLockState(
		bool bUsingGitLfsLocking,
		const FString& InFilename);

	/** pull 后的 push 校验只能使用重算范围；重算失败时返回空范围并由调用方阻止 push。 */
	void BuildPostPullPushValidationScope(
		bool bRefreshSucceeded,
		const TArray<FString>& InPrePullFiles,
		const TArray<FString>& InRefreshedFiles,
		TArray<FString>& OutValidationFiles);

	/**
	 * 运行返回逐行路径的 Git 子命令，并强制关闭非 ASCII 路径转义，确保锁分类看到真实扩展名。
	 * Runs a line-oriented Git path query with non-ASCII quoting disabled so lock classification sees
	 * the real filename extension.
	 */
	bool RunLiteralPathListCommand(
		const FString& InSubCommand,
		const FString& InPathToGitBinary,
		const FString& InRepositoryRoot,
		const TArray<FString>& InParameters,
		TArray<FString>& OutResults,
		TArray<FString>& OutErrorMessages);

	/** 比较命令冻结的锁设置与当前设置；任一代次、模式或 LFS 身份变化都视为过期。 */
	bool DoLockSettingsSnapshotsMatch(
		uint64 InCommandGeneration,
		bool bInCommandUsingLfs,
		const FString& InCommandLfsUser,
		uint64 InCurrentGeneration,
		bool bInCurrentUsingLfs,
		const FString& InCurrentLfsUser);

	/**
	 * 从冻结的本地分支与可选 upstream 构造显式 LFS push refs；无 upstream 时生成同名首推目标。
	 * Builds explicit LFS push refs from the frozen local branch and optional upstream; an absent
	 * upstream produces a same-name first-push target.
	 */
	bool BuildLfsPushRefs(
		const FString& InLocalBranch,
		const FString& InUpstreamBranch,
		FString& OutLocalBranchRef,
		FString& OutRemoteTrackingRef,
		FString& OutPushRefSpec,
		bool& OutHasRemoteBaseline);

	/**
	 * 先提交 worker 的新鲜状态；只有 command queue 已静止时才消费全局单端 LFS 锁转换。
	 * Commits fresh worker state first and drains global legacy LFS lock transitions only after the
	 * command queue is quiescent, so one completed command cannot consume another worker's fixup.
	 */
	bool RunFreshStateCommitBeforeLockTransitions(
		TFunctionRef<bool()> InFreshStateCommit,
		bool bInCommandQueueQuiescent,
		TFunctionRef<bool()> InLockTransitionCommit);

	/**
	 * 任一锁工作流写入都必须先通过紧邻边界；失败时不得调用 commit/push lambda。
	 * Any lock-workflow write must pass its adjacent boundary before the commit/push lambda runs.
	 *
	 * @param	InBoundaryCheck	设置代次与适用的 live branch 边界检查。
	 * @param	InWrite	仅在边界允许后才调用的不可逆写入。
	 * @returns 边界允许且写入成功时返回 true。
	 */
	bool RunAfterLockWriteBoundary(
		TFunctionRef<bool()> InBoundaryCheck,
		TFunctionRef<bool()> InWrite);

	/** Delete 必须先通过新鲜本地状态门；失败时不得修改索引。 */
	bool RunDeleteAfterLockBoundary(
		TFunctionRef<bool()> InLocalStateGate,
		TFunctionRef<bool()> InIndexMutation);


}

/**
 * Internal operation used to fetch from remote
 */
class FGitFetch : public ISourceControlOperation
{
public:
	// ISourceControlOperation interface
	virtual FName GetName() const override;

	virtual FText GetInProgressString() const override;

	bool bUpdateStatus = false;
};

/** Called when first activated on a project, and then at project load time.
 *  Look for the root directory of the git repository (where the ".git/" subdirectory is located). */
class FGitConnectWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitConnectWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** Lock (check-out) a set of files using Git LFS 2. */
class FGitCheckOutWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitCheckOutWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** Commit (check-in) a set of files to the local depot. */
class FGitCheckInWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitCheckInWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** Add an untracked file to revision control (so only a subset of the git add command). */
class FGitMarkForAddWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitMarkForAddWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** Delete a file and remove it from revision control. */
class FGitDeleteWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitDeleteWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** Revert any change to a file to its state on the local depot. */
class FGitRevertWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitRevertWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** 普通同步更新远端分支；强制修订同步只还原明确文件，不移动 HEAD。
 * Normal sync updates from the remote; forced revision sync restores explicit files without moving HEAD.
 */
class FGitSyncWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitSyncWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** Get revision control status of files on local working copy. */
class FGitUpdateStatusWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitUpdateStatusWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

public:
	/** Temporary states for results */
	TMap<const FString, FGitState> States;

	/** Map of filenames to history */
	TMap<FString, TGitSourceControlHistory> Histories;
};

/** Copy or Move operation on a single file */
class FGitCopyWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitCopyWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** git add to mark a conflict as resolved */
class FGitResolveWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitResolveWorker() {}
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

/** Git push to publish branch for its configured remote */
class FGitFetchWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitFetchWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

#if ENGINE_MAJOR_VERSION == 5
class FGitMoveToChangelistWorker : public IGitSourceControlWorker
{
public:
	virtual ~FGitMoveToChangelistWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};

class FGitUpdateStagingWorker: public IGitSourceControlWorker
{
public:
	virtual ~FGitUpdateStagingWorker() {}
	// IGitSourceControlWorker interface
	virtual FName GetName() const override;
	virtual bool Execute(class FGitSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

	/** Temporary states for results */
	TMap<const FString, FGitState> States;
};
#endif
