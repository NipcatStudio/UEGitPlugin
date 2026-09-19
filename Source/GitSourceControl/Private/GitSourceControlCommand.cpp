// Copyright (c) 2014-2023 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlCommand.h"

#include "Modules/ModuleManager.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlSettings.h"
#include "GitSourceControlUtils.h"

FGitSourceControlCommand::FGitSourceControlCommand(const TSharedRef<class ISourceControlOperation, ESPMode::ThreadSafe>& InOperation, const TSharedRef<class IGitSourceControlWorker, ESPMode::ThreadSafe>& InWorker, const FSourceControlOperationComplete& InOperationCompleteDelegate)
	: Operation(InOperation)
	, Worker(InWorker)
	, OperationCompleteDelegate(InOperationCompleteDelegate)
	, bExecuteProcessed(0)
	, bCancelled(0)
	, bCommandSuccessful(false)
	, bAutoDelete(true)
	, Concurrency(EConcurrency::Synchronous)
{
	// cache the providers settings here
	const FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	const FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
	const FGitLockSettingsSnapshot LockSettings =
		GitSourceControl.AccessSettings().GetLockSettingsSnapshot();
	PathToGitBinary = Provider.GetGitBinaryPath();
	PathToRepositoryRoot = Provider.GetPathToRepositoryRoot();
	PathToGitRoot = Provider.GetPathToGitRoot();
	bUsingGitLfsLocking = Provider.UsesCheckout();
	LfsUserName = LockSettings.LfsUserName;
	bSettingsUsingGitLfsLocking =
		LockSettings.bUsingGitLfsLocking;
	LockSettingsGeneration = LockSettings.Generation;
	if (bUsingGitLfsLocking)
	{
		// 任一 LFS 锁工作流都必须在命令创建时直接冻结 live branch；外部 UGit 切分支后
		// 不能复用 Editor 启动时的 Provider 显示缓存。
		// Every LFS lock workflow freezes the live branch when the command is created. An external
		// UGit branch switch must not reuse the provider's editor-startup display cache.
		GitSourceControlUtils::GetBranchName(
			PathToGitBinary,
			PathToGitRoot,
			LockBranch);
	}
	else
	{
		LockBranch = Provider.GetBranchName();
	}
}

void FGitSourceControlCommand::UpdateRepositoryRootIfSubmodule(TArray<FString>& AbsoluteFilePaths)
{
	const FString NewRepositoryRoot = GitSourceControlUtils::ChangeRepositoryRootIfSubmodule(AbsoluteFilePaths, PathToRepositoryRoot);

	// 当所选文件位于子模块时，本命令的全部 Git/LFS 操作都必须改用子模块根。只有实际检测到
	// 子模块才覆盖 PathToGitRoot，避免破坏“工程嵌套在更大主仓”这一合法情况。
	// When selected files live in a submodule, every Git/LFS operation must use that submodule root.
	// Override PathToGitRoot only for an actual submodule so a project nested in a larger repository
	// keeps its legitimate parent Git root.
	if (NewRepositoryRoot != PathToRepositoryRoot)
	{
		PathToGitRoot = NewRepositoryRoot;
	}

	PathToRepositoryRoot = NewRepositoryRoot;
	if (bUsingGitLfsLocking)
	{
		// 构造函数先看到主 Provider 根；切换为子模块后必须重抓该仓的 live branch，后续
		// commit/push 边界与显式 refspec 才不会拿主仓分支校验子模块。
		// Construction initially sees the provider root. Refresh the repo-scoped live branch after a
		// submodule switch so commit/push boundaries and explicit refspecs target the same repository.
		LockBranch.Reset();
		GitSourceControlUtils::GetBranchName(
			PathToGitBinary,
			PathToGitRoot,
			LockBranch);
	}
}

bool FGitSourceControlCommand::DoWork()
{
	bCommandSuccessful = Worker->Execute(*this);
	FPlatformAtomics::InterlockedExchange(&bExecuteProcessed, 1);

	return bCommandSuccessful;
}

void FGitSourceControlCommand::Abandon()
{
	FPlatformAtomics::InterlockedExchange(&bExecuteProcessed, 1);
}

void FGitSourceControlCommand::DoThreadedWork()
{
	Concurrency = EConcurrency::Asynchronous;
	DoWork();
}

void FGitSourceControlCommand::Cancel()
{
	FPlatformAtomics::InterlockedExchange(&bCancelled, 1);
}

bool FGitSourceControlCommand::IsCanceled() const
{
	return bCancelled != 0;
}

ECommandResult::Type FGitSourceControlCommand::ReturnResults()
{
	// Save any messages that have accumulated
	for (const auto& String : ResultInfo.InfoMessages)
	{
		Operation->AddInfoMessge(FText::FromString(String));
	}
	for (const auto& String : ResultInfo.ErrorMessages)
	{
		Operation->AddErrorMessge(FText::FromString(String));
	}

	// run the completion delegate if we have one bound
	ECommandResult::Type Result = bCancelled ? ECommandResult::Cancelled : (bCommandSuccessful ? ECommandResult::Succeeded : ECommandResult::Failed);
	OperationCompleteDelegate.ExecuteIfBound(Operation, Result);

	return Result;
}
