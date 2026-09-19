// Copyright (c) 2014-2023 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "GitSourceControlChangelist.h"
#include "ISourceControlProvider.h"
#include "Misc/IQueuedWork.h"
#include "Runtime/Launch/Resources/Version.h"

/** Accumulated error and info messages for a revision control operation.  */
struct FGitSourceControlResultInfo
{
	/** Append any messages from another FSourceControlResultInfo, ensuring to keep any already accumulated info. */
	void Append(const FGitSourceControlResultInfo& InResultInfo)
	{
		InfoMessages.Append(InResultInfo.InfoMessages);
		ErrorMessages.Append(InResultInfo.ErrorMessages);
	}

	/** Info and/or warning message storage */
	TArray<FString> InfoMessages;

	/** Potential error message storage */
	TArray<FString> ErrorMessages;
};


/**
 * Used to execute Git commands multi-threaded.
 */
class FGitSourceControlCommand : public IQueuedWork
{
public:

	FGitSourceControlCommand(const TSharedRef<class ISourceControlOperation, ESPMode::ThreadSafe>& InOperation, const TSharedRef<class IGitSourceControlWorker, ESPMode::ThreadSafe>& InWorker, const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete());

	/**
	 *  Modify the repo root if all selected files are in a plugin subfolder, and the plugin subfolder is a git repo
	 *  This supports the case where each plugin is a sub module
	 */
	void UpdateRepositoryRootIfSubmodule(TArray<FString>& AbsoluteFilePaths);

	/**
	 * This is where the real thread work is done. All work that is done for
	 * this queued object should be done from within the call to this function.
	 */
	bool DoWork();

	/**
	 * Tells the queued work that it is being abandoned so that it can do
	 * per object clean up as needed. This will only be called if it is being
	 * abandoned before completion. NOTE: This requires the object to delete
	 * itself using whatever heap it was allocated in.
	 */
	virtual void Abandon() override;

	/**
	 * This method is also used to tell the object to cleanup but not before
	 * the object has finished it's work.
	 */
	virtual void DoThreadedWork() override;

	/** Attempt to cancel the operation */
	void Cancel();

	/** Is the operation canceled? */
	bool IsCanceled() const;

	/** Save any results and call any registered callbacks. */
	ECommandResult::Type ReturnResults();

public:
	/** Path to the Git binary */
	FString PathToGitBinary;

	/** Path to the root of the Unreal revision control repository: usually the ProjectDir */
	FString PathToRepositoryRoot;

	/** Path to the root of the Git repository: can be the ProjectDir itself, or any parent directory (found by the "Connect" operation) */
	FString PathToGitRoot;

	/** Tell if using the Git LFS file Locking workflow */
	bool bUsingGitLfsLocking;

	/** 命令创建时与配置代次一起冻结的 Git LFS owner 名称。 */
	FString LfsUserName;

	/** 命令创建时 Editor 设置中是否开启 Git LFS；与仓库能力计算后的运行模式分离。 */
	bool bSettingsUsingGitLfsLocking = false;

	/** 命令创建时冻结的锁配置代次；任一相关设置变化后不再允许远端写入。 */
	uint64 LockSettingsGeneration = 0;

	/** 状态计算期间配置或分支已换代；结果按只读失败关闭，并请求 Provider 立即重新刷新。 */
	bool bStatusSettingsSuperseded = false;

	/** 命令创建时锁工作流的明确分支；游离 HEAD 会在写操作前显式失败。 */
	FString LockBranch;

	/** Operation we want to perform - contains outward-facing parameters & results */
	TSharedRef<class ISourceControlOperation, ESPMode::ThreadSafe> Operation;

	/** The object that will actually do the work */
	TSharedRef<class IGitSourceControlWorker, ESPMode::ThreadSafe> Worker;

	/** Delegate to notify when this operation completes */
	FSourceControlOperationComplete OperationCompleteDelegate;

	/**If true, this command has been processed by the revision control thread*/
	volatile int32 bExecuteProcessed;

	/**If true, this command has been cancelled*/
	volatile int32 bCancelled;

	/**If true, the revision control command succeeded*/
	bool bCommandSuccessful;

	/** Current Commit full SHA1 */
	FString CommitId;

	/** Current Commit description's Summary */
	FString CommitSummary;

	/** If true, this command will be automatically cleaned up in Tick() */
	bool bAutoDelete;

	/** Whether we are running multi-treaded or not*/
	EConcurrency::Type Concurrency;

	/** 命令创建、入队、开始与结束时间（单调时钟秒），用于区分准备、排队及执行耗时。 */
	double CreatedAtSeconds = 0.0;
	double QueuedAtSeconds = 0.0;
	double WorkStartedAtSeconds = 0.0;
	double WorkFinishedAtSeconds = 0.0;

	/** 执行 Git 操作的线程编号，用于关联同一线程的子进程耗时日志。 */
	uint32 WorkerThreadId = 0;

	/** Files to perform this operation on */
	TArray<FString> Files;

#if ENGINE_MAJOR_VERSION == 5
    /** Changelist to perform this operation on */
    FGitSourceControlChangelist Changelist;

	/** 主线程冻结的变更列表路径；仅用于已查询目录内消失文件的状态对账。 */
	TArray<FString> KnownChangelistFiles;
#endif

	/** Potential error, warning and info message storage */
	FGitSourceControlResultInfo ResultInfo;

	/** Branch names for status queries */
	TArray< FString > StatusBranchNames;
};
