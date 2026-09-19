// Copyright (c) 2014-2023 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlProvider.h"

#include "GitMessageLog.h"
#include "GitSourceControlState.h"
#include "Misc/Paths.h"
#include "Misc/QueuedThreadPool.h"
#include "GitSourceControlCommand.h"
#include "ISourceControlModule.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlOperations.h"
#include "GitSourceControlSettings.h"
#include "GitSourceControlUtils.h"
#include "SGitSourceControlSettings.h"
#include "GitSourceControlChangelistState.h"
#include "Logging/MessageLog.h"
#include "ScopedSourceControlProgress.h"
#include "SourceControlHelpers.h"
#include "SourceControlOperations.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/AsyncWork.h"
#include "Async/TaskGraphInterfaces.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/MessageDialog.h"
#include "Misc/ScopeLock.h"

#include "Runtime/Launch/Resources/Version.h"
#if ENGINE_MAJOR_VERSION == 5
#include "UObject/ObjectSaveContext.h"
#endif

#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

/**
 * 仓库初始化的不可变输入和完整输出；后台任务只持有这些值，不捕获 Provider 或模块回调。
 * Immutable input and complete output for repository initialization. The background task owns only
 * these values and captures neither the provider nor module callbacks.
 */
struct FGitRepositoryInitializationRequest
{
	/** Git 可执行文件。 */
	FString GitBinaryPath;
	/** 仓库根目录。 */
	FString RepositoryRoot;
};

struct FGitRepositoryInitializationResult
{
	/** 仓库身份与分支探测是否成功。 */
	bool bSucceeded = false;
	/** .umap 与 .uasset 是否都由当前仓库声明为 lockable。 */
	bool bLockableAttributesAvailable = false;
	/** Git 用户名。 */
	FString UserName;
	/** Git 邮箱。 */
	FString UserEmail;
	/** 当前分支。 */
	FString BranchName;
	/** 远端跟踪分支。 */
	FString RemoteBranchName;
	/** origin URL。 */
	FString RemoteUrl;
	/** 后台阶段产生的诊断。 */
	TArray<FString> ErrorMessages;
};

/**
 * 线程池中的纯值仓库探测；完成前由 FAsyncTask 持有，绝不投递无法等待的 GT continuation。
 * Value-only repository discovery on the thread pool. FAsyncTask owns it through completion and it
 * never posts an untracked game-thread continuation.
 */
class FGitRepositoryInitializationWork : public FNonAbandonableTask
{
	friend class FAsyncTask<FGitRepositoryInitializationWork>;

public:
	explicit FGitRepositoryInitializationWork(
		FGitRepositoryInitializationRequest&& InRequest)
		: Request(MoveTemp(InRequest))
	{
	}

	/** 仅在任务完成并同步后读取。 */
	FGitRepositoryInitializationResult&& TakeResult()
	{
		return MoveTemp(Result);
	}

private:
	void DoWork()
	{
		GitSourceControlUtils::GetUserConfig(
			Request.GitBinaryPath,
			Request.RepositoryRoot,
			Result.UserName,
			Result.UserEmail);

		if (!GitSourceControlUtils::GetBranchName(
				Request.GitBinaryPath,
				Request.RepositoryRoot,
				Result.BranchName))
		{
			Result.ErrorMessages.Add(
				TEXT("仓库初始化时无法读取当前 Git 分支。"));
			return;
		}

		GitSourceControlUtils::GetRemoteBranchName(
			Request.GitBinaryPath,
			Request.RepositoryRoot,
			Result.RemoteBranchName);
		GitSourceControlUtils::GetRemoteUrl(
			Request.GitBinaryPath,
			Request.RepositoryRoot,
			Result.RemoteUrl);

		const TArray<FString> Files{TEXT("*.uasset"), TEXT("*.umap")};
		TArray<FString> LockableErrorMessages;
		if (!GitSourceControlUtils::CheckLFSLockable(
				Request.GitBinaryPath,
				Request.RepositoryRoot,
				Files,
				LockableErrorMessages))
		{
			Result.ErrorMessages.Append(LockableErrorMessages);
		}
		else
		{
			Result.bLockableAttributesAvailable =
				GitSourceControlUtils::IsFileLFSLockable(TEXT(".umap"))
				&& GitSourceControlUtils::IsFileLFSLockable(TEXT(".uasset"));
		}

		Result.bSucceeded = true;
	}

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(
			FGitRepositoryInitializationWork,
			STATGROUP_ThreadPoolAsyncTasks);
	}

	/** 任务私有输入。 */
	FGitRepositoryInitializationRequest Request;
	/** 任务完成后由 Provider 单次取走。 */
	FGitRepositoryInitializationResult Result;
};

/**
 * 对 FAsyncTask 的非模板 Provider 句柄；IsDone/EnsureCompletion 保证销毁前线程池已完全退出。
 * Non-template provider handle for FAsyncTask. IsDone/EnsureCompletion guarantee that the pool task
 * has fully left before destruction.
 */
class FGitRepositoryInitializationTask
{
public:
	explicit FGitRepositoryInitializationTask(
		FGitRepositoryInitializationRequest&& InRequest)
		: Task(MakeUnique<FAsyncTask<FGitRepositoryInitializationWork>>(
			MoveTemp(InRequest)))
	{
	}

	/** 在当前线程立即运行。 */
	void StartSynchronous()
	{
		Task->StartSynchronousTask();
	}

	/** 在线程池排队；排队对象由本句柄持有，Close 可等待。 */
	void StartBackground()
	{
		Task->StartBackgroundTask();
	}

	/** 单帧非阻塞完成检查；true 时已经同步到任务尾部。 */
	bool IsDone()
	{
		return Task->IsDone();
	}

	/** 等待或收回尚未启动的任务，并保证执行尾部已完成。 */
	void EnsureCompletion()
	{
		Task->EnsureCompletion();
	}

	/** 同步完成后单次取走结果。 */
	FGitRepositoryInitializationResult TakeResult()
	{
		return Task->GetTask().TakeResult();
	}

private:
	/** 完整拥有后台任务，析构前必须已完成。 */
	TUniquePtr<FAsyncTask<FGitRepositoryInitializationWork>> Task;
};

static FName ProviderName("Git LFS 2");

void FGitSourceControlProvider::Init(bool bForceConnection)
{
	bClosing = false;

	// Init() is called multiple times at startup: do not check git each time
	if(!bGitAvailable)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GitSourceControl"));
		if(Plugin.IsValid())
		{
			UE_LOG(LogSourceControl, Log, TEXT("Git plugin '%s'"), *(Plugin->GetDescriptor().VersionName));
		}

		CheckGitAvailability();
	}

#if ENGINE_MAJOR_VERSION == 5
	UPackage::PackageSavedWithContextEvent.AddStatic(&GitSourceControlUtils::UpdateFileStagingOnSaved);
#endif

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	AssetRegistryModule.Get().OnAssetRenamed().AddStatic(&GitSourceControlUtils::UpdateStateOnAssetRename);	

	// bForceConnection: not used anymore
}

void FGitSourceControlProvider::CheckGitAvailability()
{
	FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	PathToGitBinary = GitSourceControl.AccessSettings().GetBinaryPath();
	if(PathToGitBinary.IsEmpty())
	{
		// Try to find Git binary, and update settings accordingly
		PathToGitBinary = GitSourceControlUtils::FindGitBinaryPath();
		if(!PathToGitBinary.IsEmpty())
		{
			GitSourceControl.AccessSettings().SetBinaryPath(PathToGitBinary);
		}
	}

	if(!PathToGitBinary.IsEmpty())
	{
		UE_LOG(LogSourceControl, Log, TEXT("Using '%s'"), *PathToGitBinary);
		bGitAvailable = true;
		CheckRepositoryStatus();
	}
	else
	{
		bGitAvailable = false;
	}
}

void FGitSourceControlProvider::UpdateSettings()
{
	const FGitSourceControlModule& GitSourceControl = FGitSourceControlModule::Get();
	const FGitLockSettingsSnapshot Settings =
		GitSourceControl.AccessSettings().GetLockSettingsSnapshot();
	const bool bPreviousUsingGitLfsLocking = bUsingGitLfsLocking;
	const bool bPreviousLockingConfigurationValid =
		bLockingConfigurationValid;
	const FString PreviousLockUser = LockUser;
	const uint64 PreviousGeneration = LockSettingsGeneration;
	ApplyEffectiveLockingSettings(Settings);
	const bool bLockSettingsChanged =
		PreviousGeneration != LockSettingsGeneration
		|| bPreviousUsingGitLfsLocking != bUsingGitLfsLocking
		|| bPreviousLockingConfigurationValid
			!= bLockingConfigurationValid
		|| PreviousLockUser != LockUser;
	if (bLockSettingsChanged && bGitRepositoryFound)
	{
		bBackgroundRefreshRequested = true;
		NextBackgroundRefreshTimeSeconds = 0.0;
	}
}

void FGitSourceControlProvider::ApplyEffectiveLockingSettings(
	const FGitLockSettingsSnapshot& InSettings)
{
	LockingConfigurationError.Reset();
	if (LockingConfigurationError.IsEmpty()
		&& InSettings.bUsingGitLfsLocking
		&& bLockableAttributesCapabilityKnown
		&& !bLockableAttributesAvailable)
	{
		LockingConfigurationError = TEXT(
			"Git Source Control 锁配置无法生效：已启用 Git LFS 锁，但仓库未为"
			" .uasset/.umap 提供有效 lockable 属性；Provider 已停止操作，"
			"请修复根目录 .gitattributes 或显式关闭 Git LFS 锁。");
	}
	bLockingConfigurationValid =
		LockingConfigurationError.IsEmpty();
	bUsingGitLfsLocking =
		bLockingConfigurationValid
		&& InSettings.bUsingGitLfsLocking
		&& bLockableAttributesAvailable;
	LockUser = InSettings.LfsUserName;
	LockSettingsGeneration = InSettings.Generation;
}

#if WITH_DEV_AUTOMATION_TESTS
void FGitSourceControlProvider::ApplyLockingSettingsForTests(
	const FGitLockSettingsSnapshot& InSettings,
	bool bInLockableAttributesAvailable)
{
	bGitRepositoryFound = true;
	bLockableAttributesCapabilityKnown = true;
	bLockableAttributesAvailable =
		bInLockableAttributesAvailable;
	ApplyEffectiveLockingSettings(InSettings);
}
#endif

void FGitSourceControlProvider::CheckRepositoryStatus()
{
	// Uncooked -game/-server 进程也会加载本模块（UncookedOnly），但并不拥有版本控制 UI 或工作流。
	// 启动期仓库扫描会创建 Git/LFS 子进程，其游戏线程完成任务可能落在初始 LoadMap 中段；实测这会破坏堆并导致启动崩溃。
	// 因此该 Provider 的扫描属于纯 Editor 机制，非 Editor 进程必须完全跳过。
	// Uncooked -game/-server processes also load this module (UncookedOnly), but have no revision control UI or workflow.
	// The boot-time repository scan spawns Git/LFS subprocesses whose game-thread completion may land during the initial LoadMap;
	// this has been observed corrupting the heap and crashing startup, so non-Editor processes must skip this Editor-only scan.
	if (!GIsEditor)
	{
		bGitRepositoryFound = false;
		return;
	}

	GitSourceControlMenu.Register();

	// 每次仓库探测都先撤销旧能力，只有本轮成功结果才能重新授予 checkout。
	// Revoke stale repository capability before every probe. Only this probe's successful result
	// may enable checkout again.
	bGitRepositoryFound = false;
	bLockableAttributesCapabilityKnown = false;
	bLockableAttributesAvailable = false;
	// Make sure our settings our up to date
	UpdateSettings();

	// Find the path to the root Git directory (if any, else uses the ProjectDir)
	const FString PathToProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	PathToRepositoryRoot = PathToProjectDir;
	if (!GitSourceControlUtils::FindRootDirectory(PathToProjectDir, PathToGitRoot))
	{
		UE_LOG(LogSourceControl, Error, TEXT("Failed to find valid Git root directory."));
		bGitRepositoryFound = false;
		return;
	}
	PathToRepositoryRoot = PathToGitRoot;

	if (!GitSourceControlUtils::CheckGitAvailability(PathToGitBinary, &GitVersion))
	{
		UE_LOG(LogSourceControl, Error, TEXT("Failed to find valid Git executable."));
		bGitRepositoryFound = false;
		return;
	}

	// 重复 Init/连接请求复用已经排队的初始化；任务结果只能由 Tick 或同步路径消费。
	// Repeated Init/connect requests reuse an already queued initialization. Only Tick or the
	// synchronous path may consume its result.
	if (RepositoryInitializationTask.IsValid())
	{
		return;
	}

	FGitRepositoryInitializationRequest Request;
	Request.GitBinaryPath = PathToGitBinary;
	Request.RepositoryRoot = PathToRepositoryRoot;
	RepositoryInitializationTask =
		MakeShared<FGitRepositoryInitializationTask, ESPMode::ThreadSafe>(
			MoveTemp(Request));

	if (FApp::IsUnattended() || IsRunningCommandlet())
	{
		RepositoryInitializationTask->StartSynchronous();
		FinalizeRepositoryInitialization(true, true);
	}
	else
	{
		RepositoryInitializationTask->StartBackground();
	}
}

void FGitSourceControlProvider::FinalizeRepositoryInitialization(
	bool bWaitForCompletion,
	bool bApplyResult)
{
	if (!RepositoryInitializationTask.IsValid())
	{
		return;
	}
	if (!bWaitForCompletion && !RepositoryInitializationTask->IsDone())
	{
		return;
	}

	RepositoryInitializationTask->EnsureCompletion();
	FGitRepositoryInitializationResult Result =
		RepositoryInitializationTask->TakeResult();
	RepositoryInitializationTask.Reset();

	if (!bApplyResult || bClosing)
	{
		return;
	}

	UserName = MoveTemp(Result.UserName);
	UserEmail = MoveTemp(Result.UserEmail);
	for (const FString& ErrorMessage : Result.ErrorMessages)
	{
		UE_LOG(LogSourceControl, Error, TEXT("%s"), *ErrorMessage);
	}
	if (!Result.bSucceeded)
	{
		UE_LOG(
			LogSourceControl,
			Error,
			TEXT("Failed to update repo on initialization."));
		bGitRepositoryFound = false;
		return;
	}

	BranchName = MoveTemp(Result.BranchName);
	RemoteBranchName = MoveTemp(Result.RemoteBranchName);
	RemoteUrl = MoveTemp(Result.RemoteUrl);
	const FGitLockSettingsSnapshot CurrentSettings =
		FGitSourceControlModule::Get()
			.AccessSettings()
			.GetLockSettingsSnapshot();
	// Editor 可见设置仍是运行时事实来源；后台结果只提供仓库能力，绝不能恢复旧的开关快照。
	// Editor-visible settings remain the runtime source of truth. The background result contributes
	// only repository capability and can never restore a stale enable/disable snapshot.
	bLockableAttributesAvailable =
		Result.bLockableAttributesAvailable;
	bLockableAttributesCapabilityKnown = true;
	ApplyEffectiveLockingSettings(CurrentSettings);
	if (bUsingGitLfsLocking)
	{
		UE_LOG(LogSourceControl, Log, TEXT("Git LFS Locking is enabled."));
	}
	else if (CurrentSettings.bUsingGitLfsLocking)
	{
		UE_LOG(
			LogSourceControl,
			Error,
			TEXT(
				"Git LFS Locking is disabled. Files .uasset or .umap are not "
				"lockable. Make sure your .gitattributes is setting lockable "
				"attributes for .uasset or .umap at the root of the git repository."));
	}
	bGitRepositoryFound = true;
	NextBackgroundRefreshTimeSeconds =
		FPlatformTime::Seconds() + 30.0;

	// 仓库初始化只负责只读探测；状态刷新进入 Provider 拥有的标准命令队列。
	// Initialization is read-only; refresh state through the provider-owned command queue.
#if ENGINE_MAJOR_VERSION >= 5
	Execute(
		ISourceControlOperation::Create<FUpdateStatus>(),
		FSourceControlChangelistPtr(),
		FGitSourceControlModule::GetEmptyStringArray(),
		EConcurrency::Asynchronous);
#else
	Execute(
		ISourceControlOperation::Create<FUpdateStatus>(),
		FGitSourceControlModule::GetEmptyStringArray(),
		EConcurrency::Asynchronous);
#endif
}

void FGitSourceControlProvider::SetLastErrors(const TArray<FText>& InErrors)
{

	FScopeLock Lock(&LastErrorsCriticalSection);
	LastErrors = InErrors;
}

TArray<FText> FGitSourceControlProvider::GetLastErrors() const
{
	FScopeLock Lock(&LastErrorsCriticalSection);
	TArray<FText> Result = LastErrors;
	return Result;
}

int32 FGitSourceControlProvider::GetNumLastErrors() const
{
	FScopeLock Lock(&LastErrorsCriticalSection);
	return LastErrors.Num();
}

void FGitSourceControlProvider::Close()
{
	bClosing = true;
	// 初始化任务从排队时即由 Provider 持有；这里等待其完整退出并丢弃结果，不依赖 GT
	// continuation，因此 Close 在 GameThread 等待也不会形成互等。
	// The provider owns initialization from enqueue time. Wait for complete exit and discard the
	// result here without a game-thread continuation, so waiting in Close cannot deadlock the GT.
	FinalizeRepositoryInitialization(true, false);

	// 关闭阶段继续 Tick 直到全部已接收命令完成；bClosing 阻止回调再次排队。
	// 模块卸载前必须交付全部状态与 completion，不能留下仍使用本模块的 worker。
	// Tick until all admitted commands complete; bClosing prevents callbacks from enqueueing more.
	// Deliver state and completion before unload so no worker outlives this module.
	while (!CommandQueue.IsEmpty())
	{
		// 部分已接收 Git worker 会在 pull 的包卸载/重载边界同步等待 GT 任务。Close
		// 本身运行在 GT，因此必须泵已排队 TaskGraph 工作再轮询命令，避免双方互等。
		// Some admitted Git workers synchronously await GT package unlink/reload tasks during pull.
		// Close itself runs on the GT, so pump queued TaskGraph work before polling commands to
		// prevent a worker/Close deadlock.
		if (IsInGameThread())
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(
				ENamedThreads::GameThread);
		}
		Tick();
		if (!CommandQueue.IsEmpty())
		{
			FPlatformProcess::Sleep(0.01f);
		}
	}

	// clear the cache
	StateCache.Empty();
#if ENGINE_MAJOR_VERSION == 5
	ChangelistsStateCache.Empty();
#endif
	// Remove all extensions to the "Revision Control" menu in the Editor Toolbar
	GitSourceControlMenu.Unregister();

	bGitAvailable = false;
	bGitRepositoryFound = false;
	bLockableAttributesCapabilityKnown = false;
	bLockableAttributesAvailable = false;
	bUsingGitLfsLocking = false;
	bLockingConfigurationValid = true;
	LockingConfigurationError.Reset();
	LockSettingsGeneration = 0;
	UserName.Empty();
	UserEmail.Empty();
	bBackgroundRefreshInFlight = false;
	bBackgroundRefreshRequested = false;
	NextBackgroundRefreshTimeSeconds = 0.0;
}

TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> FGitSourceControlProvider::GetStateInternal(const FString& Filename)
{
	TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe>* State = StateCache.Find(Filename);
	if (State != NULL)
	{
		// found cached item
		return (*State);
	}
	else
	{
		// cache an unknown state for this item
		TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> NewState = MakeShareable( new FGitSourceControlState(Filename) );
		StateCache.Add(Filename, NewState);
		return NewState;
	}
}

#if ENGINE_MAJOR_VERSION == 5
TSharedRef<FGitSourceControlChangelistState, ESPMode::ThreadSafe> FGitSourceControlProvider::GetStateInternal(const FGitSourceControlChangelist& InChangelist)
{
	TSharedRef<FGitSourceControlChangelistState, ESPMode::ThreadSafe>* State = ChangelistsStateCache.Find(InChangelist);
	if (State != NULL)
	{
		// found cached item
		return (*State);
	}
	else
	{
		// cache an unknown state for this item
		TSharedRef<FGitSourceControlChangelistState, ESPMode::ThreadSafe> NewState = MakeShared<FGitSourceControlChangelistState>(InChangelist);
		ChangelistsStateCache.Add(InChangelist, NewState);
		return NewState;
	}
}

void FGitSourceControlProvider::UpdateChangelistState(
	const TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe>& InState)
{
	check(IsInGameThread());
	if (InState->State.TreeState == ETreeState::Unset)
	{
		return;
	}

	FGitSourceControlChangelist Destination;
	if (InState->State.TreeState == ETreeState::Staged)
	{
		Destination = FGitSourceControlChangelist::StagedChangelist;
	}
	else if (InState->State.TreeState == ETreeState::Working
		|| InState->State.TreeState == ETreeState::Untracked)
	{
		Destination = FGitSourceControlChangelist::WorkingChangelist;
	}
	if (InState->Changelist == Destination)
	{
		return;
	}

	// 变更列表与文件缓存共用已解析的 Git 状态；不再为 UI 另扫目录，也不从 worker 修改数组。
	// Changelists share the parsed Git state with the file cache: no extra scan or worker-side array writes.
	const auto Staged = GetStateInternal(FGitSourceControlChangelist::StagedChangelist);
	const auto Working = GetStateInternal(FGitSourceControlChangelist::WorkingChangelist);
	Staged->Files.Remove(InState);
	Working->Files.Remove(InState);
	InState->Changelist = Destination;
	if (Destination == FGitSourceControlChangelist::StagedChangelist)
	{
		Staged->Files.Add(InState);
	}
	else if (Destination == FGitSourceControlChangelist::WorkingChangelist)
	{
		Working->Files.Add(InState);
	}
	Staged->TimeStamp = Working->TimeStamp = FDateTime::Now();
}

TArray<FString> FGitSourceControlProvider::GetFilesInChangelists() const
{
	check(IsInGameThread());
	TArray<FString> Files;
	for (const auto& Changelist : ChangelistsStateCache)
	{
		for (const FSourceControlStateRef& State : Changelist.Value->Files)
		{
			Files.Add(State->GetFilename());
		}
	}
	return Files;
}
#endif

FText FGitSourceControlProvider::GetStatusText() const
{
	FFormatNamedArguments Args;
	Args.Add(TEXT("IsAvailable"), (IsEnabled() && IsAvailable()) ? LOCTEXT("Yes", "Yes") : LOCTEXT("No", "No"));
	Args.Add( TEXT("RepositoryName"), FText::FromString(PathToRepositoryRoot) );
	Args.Add( TEXT("RemoteUrl"), FText::FromString(RemoteUrl) );
	Args.Add( TEXT("UserName"), FText::FromString(UserName) );
	Args.Add( TEXT("UserEmail"), FText::FromString(UserEmail) );
	Args.Add( TEXT("BranchName"), FText::FromString(BranchName) );
	Args.Add( TEXT("CommitId"), FText::FromString(CommitId.Left(8)) );
	Args.Add( TEXT("CommitSummary"), FText::FromString(CommitSummary) );

	FText FormattedError;
	if (!bLockingConfigurationValid)
	{
		FFormatNamedArguments ErrorArgs;
		ErrorArgs.Add(
			TEXT("ErrorText"),
			FText::FromString(LockingConfigurationError));

		FormattedError = FText::Format(
			LOCTEXT(
				"GitLockingConfigurationErrorStatusText",
				"Error: {ErrorText}\n\n"),
			ErrorArgs);
	}
	else
	{
		const TArray<FText>& RecentErrors = GetLastErrors();
		if (RecentErrors.Num() > 0)
		{
			FFormatNamedArguments ErrorArgs;
			ErrorArgs.Add(TEXT("ErrorText"), RecentErrors[0]);

			FormattedError = FText::Format(
				LOCTEXT(
					"GitErrorStatusText",
					"Error: {ErrorText}\n\n"),
				ErrorArgs);
		}
	}

	Args.Add(TEXT("ErrorText"), FormattedError);

	return FText::Format(
		LOCTEXT(
			"GitStatusText",
			"{ErrorText}Enabled: {IsAvailable}\nLocal repository: {RepositoryName}\n"
			"Remote: {RemoteUrl}\nUser: {UserName}\nE-mail: {UserEmail}\n"
			"[{BranchName} {CommitId}] {CommitSummary}"),
		Args);
}

/** Quick check if revision control is enabled */
bool FGitSourceControlProvider::IsEnabled() const
{
	return bGitRepositoryFound && bLockingConfigurationValid;
}

/** Quick check if revision control is available for use (useful for server-based providers) */
bool FGitSourceControlProvider::IsAvailable() const
{
	return bGitRepositoryFound && bLockingConfigurationValid;
}

const FName& FGitSourceControlProvider::GetName(void) const
{
	return ProviderName;
}

ECommandResult::Type FGitSourceControlProvider::GetState( const TArray<FString>& InFiles, TArray< TSharedRef<ISourceControlState, ESPMode::ThreadSafe> >& OutState, EStateCacheUsage::Type InStateCacheUsage )
{
	if (!IsEnabled())
	{
		return ECommandResult::Failed;
	}

	if (InStateCacheUsage == EStateCacheUsage::ForceUpdate)
	{
		TArray<FString> ForceUpdate;
		for (FString Path : InFiles)
		{
			// Remove the path from the cache, so it's not ignored the next time we force check.
			// If the file isn't in the cache, force update it now.
			if (!RemoveFileFromIgnoreForceCache(Path))
			{
				ForceUpdate.Add(Path);
			}
		}
		if (ForceUpdate.Num() > 0)
		{
			Execute(ISourceControlOperation::Create<FUpdateStatus>(), ForceUpdate);
		}
	}

	const TArray<FString>& AbsoluteFiles = SourceControlHelpers::AbsoluteFilenames(InFiles);

	for (TArray<FString>::TConstIterator It(AbsoluteFiles); It; It++)
	{
		OutState.Add(GetStateInternal(*It));
	}

	return ECommandResult::Succeeded;
}

#if ENGINE_MAJOR_VERSION >= 5
ECommandResult::Type FGitSourceControlProvider::GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage)
{
	if (!IsEnabled())
	{
		return ECommandResult::Failed;
	}

	for (FSourceControlChangelistRef Changelist : InChangelists)
	{
		FGitSourceControlChangelistRef GitChangelist = StaticCastSharedRef<FGitSourceControlChangelist>(Changelist);
		OutState.Add(GetStateInternal(GitChangelist.Get()));
	}
	return ECommandResult::Succeeded;
}
#endif

TArray<FSourceControlStateRef> FGitSourceControlProvider::GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const
{
	TArray<FSourceControlStateRef> Result;
	for (const auto& CacheItem : StateCache)
	{
		const FSourceControlStateRef& State = CacheItem.Value;
		if (Predicate(State))
		{
			Result.Add(State);
		}
	}
	return Result;
}

bool FGitSourceControlProvider::RemoveFileFromCache(const FString& Filename)
{
	return StateCache.Remove(Filename) > 0;
}

bool FGitSourceControlProvider::AddFileToIgnoreForceCache(const FString& Filename)
{
	return IgnoreForceCache.Add(Filename) > 0;
}

bool FGitSourceControlProvider::RemoveFileFromIgnoreForceCache(const FString& Filename)
{
	return IgnoreForceCache.Remove(Filename) > 0;
}

/** Get files in cache */
TArray<FString> FGitSourceControlProvider::GetFilesInCache()
{
	TArray<FString> Files;
	for (const auto& State : StateCache)
	{
		Files.Add(State.Key);
	}
	return Files;
}

FDelegateHandle FGitSourceControlProvider::RegisterSourceControlStateChanged_Handle( const FSourceControlStateChanged::FDelegate& SourceControlStateChanged )
{
	return OnSourceControlStateChanged.Add( SourceControlStateChanged );
}

void FGitSourceControlProvider::UnregisterSourceControlStateChanged_Handle( FDelegateHandle Handle )
{
	OnSourceControlStateChanged.Remove( Handle );
}

#if ENGINE_MAJOR_VERSION < 5
ECommandResult::Type FGitSourceControlProvider::Execute( const FSourceControlOperationRef& InOperation, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency, const FSourceControlOperationComplete& InOperationCompleteDelegate )
#else
ECommandResult::Type FGitSourceControlProvider::Execute( const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr InChangelist, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency, const FSourceControlOperationComplete& InOperationCompleteDelegate )
#endif
{
	if (bClosing)
	{
		InOperationCompleteDelegate.ExecuteIfBound(
			InOperation,
			ECommandResult::Failed);
		return ECommandResult::Failed;
	}
	if(!IsEnabled() && !(InOperation->GetName() == "Connect")) // Only Connect operation allowed while not Enabled (Repository found)
	{
		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	TArray<FString> AbsoluteFiles = SourceControlHelpers::AbsoluteFilenames(InFiles);

	// Query to see if we allow this operation
	TSharedPtr<IGitSourceControlWorker, ESPMode::ThreadSafe> Worker = CreateWorker(InOperation->GetName());
	if(!Worker.IsValid())
	{
		// this operation is unsupported by this revision control provider
		FFormatNamedArguments Arguments;
		Arguments.Add( TEXT("OperationName"), FText::FromName(InOperation->GetName()) );
		Arguments.Add( TEXT("ProviderName"), FText::FromName(GetName()) );
		FText Message(FText::Format(LOCTEXT("UnsupportedOperation", "Operation '{OperationName}' not supported by revision control provider '{ProviderName}'"), Arguments));

		FTSMessageLog("SourceControl").Error(Message);
		InOperation->AddErrorMessge(Message);

		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	// A git process acts on exactly one repository, but the editor freely batches files from
	// the project repo and plugin submodules into a single operation (e.g. saving a mixed set
	// of dirty packages issues one CheckOut for all of them). The old behavior forced such a
	// batch onto the main root ("Selected files belong to different submodules") and the
	// whole operation failed. Partition the files by their owning repository and run one
	// internal command per repository instead; the engine-visible operation completes once,
	// when the last internal command finishes, failed if any of them failed.
	TMap<FString, TArray<FString>> FilesByRepositoryRoot;
	for (const FString& AbsoluteFile : AbsoluteFiles)
	{
		FString FileCopy = AbsoluteFile;
		FString Root = GitSourceControlUtils::ChangeRepositoryRootIfSubmodule(FileCopy, PathToRepositoryRoot);
		FPaths::NormalizeDirectoryName(Root);
		FilesByRepositoryRoot.FindOrAdd(MoveTemp(Root)).Add(AbsoluteFile);
	}

	if (FilesByRepositoryRoot.Num() > 1)
	{
		struct FFanOutState
		{
			int32 RemainingCommands = 0;
			bool bAnyFailed = false;
			FSourceControlOperationComplete OriginalDelegate;
		};
		TSharedRef<FFanOutState, ESPMode::ThreadSafe> FanOut = MakeShared<FFanOutState, ESPMode::ThreadSafe>();
		FanOut->RemainingCommands = FilesByRepositoryRoot.Num();
		FanOut->OriginalDelegate = InOperationCompleteDelegate;

		// Completion delegates always run on the game thread (Tick, or the pump inside
		// ExecuteSynchronousCommand), so plain members are safe here.
		const FSourceControlOperationComplete PerRepositoryDelegate = FSourceControlOperationComplete::CreateLambda(
			[FanOut](const FSourceControlOperationRef& CompletedOperation, ECommandResult::Type InResult)
			{
				FanOut->bAnyFailed |= (InResult != ECommandResult::Succeeded);
				if (--FanOut->RemainingCommands == 0)
				{
					FanOut->OriginalDelegate.ExecuteIfBound(CompletedOperation, FanOut->bAnyFailed ? ECommandResult::Failed : ECommandResult::Succeeded);
				}
			});

		ECommandResult::Type AggregatedResult = ECommandResult::Succeeded;
		for (TPair<FString, TArray<FString>>& FilesForRoot : FilesByRepositoryRoot)
		{
			TSharedPtr<IGitSourceControlWorker, ESPMode::ThreadSafe> RepositoryWorker = CreateWorker(InOperation->GetName());
			FGitSourceControlCommand* RepositoryCommand = new FGitSourceControlCommand(InOperation, RepositoryWorker.ToSharedRef());
			RepositoryCommand->UpdateRepositoryRootIfSubmodule(FilesForRoot.Value);
			RepositoryCommand->Files = FilesForRoot.Value;
			RepositoryCommand->OperationCompleteDelegate = PerRepositoryDelegate;
#if ENGINE_MAJOR_VERSION == 5
			TSharedPtr<FGitSourceControlChangelist, ESPMode::ThreadSafe> RepositoryChangelistPtr = StaticCastSharedPtr<FGitSourceControlChangelist>(InChangelist);
			RepositoryCommand->Changelist = RepositoryChangelistPtr ? RepositoryChangelistPtr.ToSharedRef().Get() : FGitSourceControlChangelist();
#endif
			ECommandResult::Type RepositoryResult;
			if (InConcurrency == EConcurrency::Synchronous)
			{
				RepositoryCommand->bAutoDelete = false;
				RepositoryResult = ExecuteSynchronousCommand(*RepositoryCommand, InOperation->GetInProgressString(), false);
			}
			else
			{
				RepositoryCommand->bAutoDelete = true;
				RepositoryResult = IssueCommand(*RepositoryCommand);
			}
			if (RepositoryResult != ECommandResult::Succeeded)
			{
				AggregatedResult = RepositoryResult;
			}
		}
		return AggregatedResult;
	}

	FGitSourceControlCommand* Command = new FGitSourceControlCommand(InOperation, Worker.ToSharedRef());
	Command->UpdateRepositoryRootIfSubmodule(AbsoluteFiles);
	Command->Files = AbsoluteFiles;
	Command->OperationCompleteDelegate = InOperationCompleteDelegate;

#if ENGINE_MAJOR_VERSION == 5
	TSharedPtr<FGitSourceControlChangelist, ESPMode::ThreadSafe> ChangelistPtr = StaticCastSharedPtr<FGitSourceControlChangelist>(InChangelist);
	Command->Changelist = ChangelistPtr ? ChangelistPtr.ToSharedRef().Get() : FGitSourceControlChangelist();
#endif

	// fire off operation
	if(InConcurrency == EConcurrency::Synchronous)
	{
		Command->bAutoDelete = false;

#if UE_BUILD_DEBUG
		UE_LOG(LogSourceControl, Log, TEXT("ExecuteSynchronousCommand(%s)"), *InOperation->GetName().ToString());
#endif
		return ExecuteSynchronousCommand(*Command, InOperation->GetInProgressString(), false);
	}
	else
	{
		Command->bAutoDelete = true;

#if UE_BUILD_DEBUG
		UE_LOG(LogSourceControl, Log, TEXT("IssueAsynchronousCommand(%s)"), *InOperation->GetName().ToString());
#endif
		return IssueCommand(*Command);
	}
}

#if ENGINE_MAJOR_VERSION < 5
bool FGitSourceControlProvider::CanCancelOperation( const FSourceControlOperationRef& InOperation ) const
#else
bool FGitSourceControlProvider::CanCancelOperation( const FSourceControlOperationRef& InOperation ) const
#endif
{
	// TODO: maybe support cancellation again?
#if 0
	for (int32 CommandIndex = 0; CommandIndex < CommandQueue.Num(); ++CommandIndex)
	{
		const FGitSourceControlCommand& Command = *CommandQueue[CommandIndex];
		if (Command.Operation == InOperation)
		{
			check(Command.bAutoDelete);
			return true;
		}
	}
#endif

	// operation was not in progress!
	return false;
}

#if ENGINE_MAJOR_VERSION < 5
void FGitSourceControlProvider::CancelOperation( const FSourceControlOperationRef& InOperation )
#else
void FGitSourceControlProvider::CancelOperation( const FSourceControlOperationRef& InOperation )
#endif
{
	for (int32 CommandIndex = 0; CommandIndex < CommandQueue.Num(); ++CommandIndex)
	{
		FGitSourceControlCommand& Command = *CommandQueue[CommandIndex];
		if (Command.Operation == InOperation)
		{
			check(Command.bAutoDelete);
			Command.Cancel();
			return;
		}
	}
}

bool FGitSourceControlProvider::UsesLocalReadOnlyState() const
{
	// 只有 LFS 模式把磁盘只读位作为 Provider 受控状态。ordinary Git 由 Unlockable 的
	// IsCheckedOut 兼容投影满足 UE 保存/SettingsHelpers；若这里也返回 true，UE 会把仓库中
	// 所有默认可写文件误收进 Uncontrolled Changelists。
	// Only LFS mode makes the disk bit provider-managed state. Ordinary Git uses the Unlockable
	// IsCheckedOut adapter for UE save/SettingsHelpers consumers. Returning true here as well would
	// put every normally writable repository file into Uncontrolled Changelists.
	return bUsingGitLfsLocking;
}

bool FGitSourceControlProvider::UsesChangelists() const
{
	return true;
}

bool FGitSourceControlProvider::UsesCheckout() const
{
	return bUsingGitLfsLocking; // Git LFS Lock uses read-only state
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
bool FGitSourceControlProvider::UsesFileRevisions() const
{
	return true;
}

#if ENGINE_MINOR_VERSION >= 8
TOptional<bool> FGitSourceControlProvider::HasChangesToSync() const
{
	return TOptional<bool>();
}

TOptional<bool> FGitSourceControlProvider::HasChangesToCheckIn() const
{
	return TOptional<bool>();
}
#else
TOptional<bool> FGitSourceControlProvider::IsAtLatestRevision() const
{
	return TOptional<bool>();
}

TOptional<int> FGitSourceControlProvider::GetNumLocalChanges() const
{
	return TOptional<int>();
}
#endif
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
bool FGitSourceControlProvider::AllowsDiffAgainstDepot() const
{
	return true;
}

bool FGitSourceControlProvider::UsesUncontrolledChangelists() const
{
	return true;
}

bool FGitSourceControlProvider::UsesSnapshots() const
{
	return false;
}
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
bool FGitSourceControlProvider::UsesSoftRevertOnDelete() const
{
	return false;
}
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
bool FGitSourceControlProvider::CanExecuteOperation(const FSourceControlOperationRef& InOperation) const {
	return WorkersMap.Find(InOperation->GetName()) != nullptr;
}

TMap<ISourceControlProvider::EStatus, FString> FGitSourceControlProvider::GetStatus() const
{
	TMap<EStatus, FString> Result;
	Result.Add(EStatus::Enabled, IsEnabled() ? TEXT("Yes") : TEXT("No") );
	Result.Add(EStatus::Connected, (IsEnabled() && IsAvailable()) ? TEXT("Yes") : TEXT("No") );
	Result.Add(EStatus::User, UserName);
	Result.Add(EStatus::Repository, PathToRepositoryRoot);
	Result.Add(EStatus::Remote, RemoteUrl);
	Result.Add(EStatus::Branch, BranchName);
	Result.Add(EStatus::Email, UserEmail);
	return Result;
}
#endif

TSharedPtr<IGitSourceControlWorker, ESPMode::ThreadSafe> FGitSourceControlProvider::CreateWorker(const FName& InOperationName) const
{
	const FGetGitSourceControlWorker* Operation = WorkersMap.Find(InOperationName);
	if(Operation != nullptr)
	{
		return Operation->Execute();
	}

	return nullptr;
}

void FGitSourceControlProvider::RegisterWorker( const FName& InName, const FGetGitSourceControlWorker& InDelegate )
{
	WorkersMap.Add( InName, InDelegate );
}

void FGitSourceControlProvider::OutputCommandMessages(const FGitSourceControlCommand& InCommand) const
{
	FTSMessageLog SourceControlLog("SourceControl");

	for (int32 ErrorIndex = 0; ErrorIndex < InCommand.ResultInfo.ErrorMessages.Num(); ++ErrorIndex)
	{
		SourceControlLog.Error(FText::FromString(InCommand.ResultInfo.ErrorMessages[ErrorIndex]));
	}

	for (int32 InfoIndex = 0; InfoIndex < InCommand.ResultInfo.InfoMessages.Num(); ++InfoIndex)
	{
		SourceControlLog.Info(FText::FromString(InCommand.ResultInfo.InfoMessages[InfoIndex]));
	}
}

void FGitSourceControlProvider::UpdateRepositoryStatus(const class FGitSourceControlCommand& InCommand)
{
	if (InCommand.bStatusSettingsSuperseded)
	{
		// 旧代状态只用于暂时失败关闭；立即排队同代刷新，不能等待常规 30 秒周期。
		// Superseded states only fail closed temporarily. Queue a same-generation refresh
		// immediately instead of waiting for the regular 30-second interval.
		bBackgroundRefreshRequested = true;
		NextBackgroundRefreshTimeSeconds = 0.0;
	}

	// For all operations running UpdateStatus, get Commit information:
	if (!InCommand.CommitId.IsEmpty())
	{
		CommitId = InCommand.CommitId;
		CommitSummary = InCommand.CommitSummary;
	}
}

void FGitSourceControlProvider::StartBackgroundRefreshIfDue()
{
	const double CurrentTimeSeconds = FPlatformTime::Seconds();
	if (bClosing
		|| !IsEnabled()
		|| bBackgroundRefreshInFlight
		|| (!bBackgroundRefreshRequested
			&& (NextBackgroundRefreshTimeSeconds <= 0.0
				|| CurrentTimeSeconds
					< NextBackgroundRefreshTimeSeconds)))
	{
		return;
	}

	TSharedRef<FGitFetch, ESPMode::ThreadSafe> RefreshOperation =
		ISourceControlOperation::Create<FGitFetch>();
	RefreshOperation->bUpdateStatus = true;
	bBackgroundRefreshInFlight = true;
	bBackgroundRefreshRequested = false;
	NextBackgroundRefreshTimeSeconds = 0.0;
#if ENGINE_MAJOR_VERSION >= 5
	const ECommandResult::Type Result = Execute(
		RefreshOperation,
		FSourceControlChangelistPtr(),
		FGitSourceControlModule::GetEmptyStringArray(),
		EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateRaw(
			this,
			&FGitSourceControlProvider::OnBackgroundRefreshComplete));
#else
	const ECommandResult::Type Result = Execute(
		RefreshOperation,
		FGitSourceControlModule::GetEmptyStringArray(),
		EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateRaw(
			this,
			&FGitSourceControlProvider::OnBackgroundRefreshComplete));
#endif
	if (Result != ECommandResult::Succeeded && bBackgroundRefreshInFlight)
	{
		bBackgroundRefreshInFlight = false;
		NextBackgroundRefreshTimeSeconds = FPlatformTime::Seconds() + 30.0;
	}
}

void FGitSourceControlProvider::OnBackgroundRefreshComplete(
	const FSourceControlOperationRef& InOperation,
	ECommandResult::Type InResult)
{
	(void)InOperation;
	(void)InResult;
	bBackgroundRefreshInFlight = false;
	NextBackgroundRefreshTimeSeconds = bBackgroundRefreshRequested
		? FPlatformTime::Seconds()
		: FPlatformTime::Seconds() + 30.0;
}

void FGitSourceControlProvider::Tick()
{
	// 初始化结果和周期刷新都只在 Provider Tick 消费/提交，因此 Close 能完整等待同一
	// FAsyncTask/CommandQueue 所有权链，不存在游离 GT continuation。
	// Initialization results and periodic refreshes are consumed/submitted only from provider Tick,
	// so Close can drain the same FAsyncTask/CommandQueue ownership chain with no detached GT task.
	FinalizeRepositoryInitialization(false, true);
	StartBackgroundRefreshIfDue();

#if ENGINE_MAJOR_VERSION < 5
	bool bStatesUpdated = false;
#else
	bool bStatesUpdated = TicksUntilNextForcedUpdate == 1;
	if( TicksUntilNextForcedUpdate > 0 )
	{
		--TicksUntilNextForcedUpdate;
	}
#endif

	// 锁转换必须等所有已排队 worker 提交新鲜 Git 状态；全局 fixup 可能由另一个并发 command
	// 先产生，若由首个完成 command 提前消费，仍会在旧 Modified+Locked 上错误计算 Writable。
	// Lock transitions must wait until every queued worker has committed fresh Git state. A global
	// fixup may have been produced early by another concurrent command; letting the first completed
	// command drain it would still compute Writable from stale Modified+Locked state.
	auto ApplyPendingLegacyLfsLockStateFixups = [this]() -> bool
	{
		TArray<FGitLockedFilesCache::FLockStateFixup> LockStateFixups =
			FGitLockedFilesCache::TakePendingStateFixups();
		if (!GitSourceControlUtils::ShouldUseLegacyLfsLockCache(
				bUsingGitLfsLocking)
			|| LockStateFixups.IsEmpty())
		{
			return false;
		}
		for (const FGitLockedFilesCache::FLockStateFixup& Fixup : LockStateFixups)
		{
			TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> State = GetStateInternal(Fixup.FilePath);
			const EGitLocalReadOnlyPolicy PermissionPolicy =
				GitSourceControlUtils::ApplyLegacyLfsLockStateTransition(
					*State,
					Fixup.LockUser,
					Fixup.bLocked,
					FGitLockedFilesCache::IsOwnedByCurrentCredential(Fixup.FilePath));
			if (FPaths::FileExists(Fixup.FilePath))
			{
				GitSourceControlUtils::ApplyLocalReadOnlyPolicy(
					Fixup.FilePath,
					PermissionPolicy);
			}
		}
		return true;
	};
	bool bLockTransitionsApplied = false;

	for (int32 CommandIndex = 0; CommandIndex < CommandQueue.Num(); ++CommandIndex)
	{
		FGitSourceControlCommand& Command = *CommandQueue[CommandIndex];

		if (Command.bExecuteProcessed)
		{
			// Remove command from the queue
			CommandQueue.RemoveAt(CommandIndex);

			if (!Command.IsCanceled())
			{
				// Update repository status on UpdateStatus operations
				UpdateRepositoryStatus(Command);
			}

			// 先落当前 worker 的 clean/modified；只有其余 command 也已排空，才消费可能由任一
			// worker 产生的全局 lock/unlock fixup。
			// Commit the current worker's clean/modified facts first. Drain global lock/unlock fixups,
			// which may come from any worker, only after every other command has also left the queue.
			const bool bCommandQueueQuiescent = CommandQueue.IsEmpty();
			bStatesUpdated |=
				GitSourceControlOperations::RunFreshStateCommitBeforeLockTransitions(
					[&Command]()
					{
						return Command.Worker->UpdateStates();
					},
					bCommandQueueQuiescent,
					ApplyPendingLegacyLfsLockStateFixups);
			bLockTransitionsApplied = bCommandQueueQuiescent;

			// dump any messages to output log
			OutputCommandMessages(Command);

			// run the completion delegate callback if we have one bound
			if (!Command.IsCanceled())
			{
				Command.ReturnResults();
			}

			// commands that are left in the array during a tick need to be deleted
			if(Command.bAutoDelete)
			{
				// Only delete commands that are not running 'synchronously'
				delete &Command;
			}

			// only do one command per tick loop, as we dont want concurrent modification
			// of the command queue (which can happen in the completion delegate)
			break;
		}
		else if (Command.bCancelled)
		{
			// If this was a synchronous command, set it free so that it will be deleted automatically
			// when its (still running) thread finally finishes
			Command.bAutoDelete = true;

			Command.ReturnResults();
			break;
		}
	}

	if (!bLockTransitionsApplied && CommandQueue.IsEmpty())
	{
		bStatesUpdated |= ApplyPendingLegacyLfsLockStateFixups();
	}

	if (bStatesUpdated)
	{
		OnSourceControlStateChanged.Broadcast();
	}
}

TArray< TSharedRef<ISourceControlLabel> > FGitSourceControlProvider::GetLabels( const FString& InMatchingSpec ) const
{
	TArray< TSharedRef<ISourceControlLabel> > Tags;

	// NOTE list labels. Called by CrashDebugHelper() (to remote debug Engine crash)
	//					 and by SourceControlHelpers::AnnotateFile() (to add source file to report)
	// Reserved for internal use by Epic Games with Perforce only
	return Tags;
}

#if ENGINE_MAJOR_VERSION >= 5
TArray<FSourceControlChangelistRef> FGitSourceControlProvider::GetChangelists( EStateCacheUsage::Type InStateCacheUsage )
{
	if (!IsEnabled())
	{
		return TArray<FSourceControlChangelistRef>();
	}

	TArray<FSourceControlChangelistRef> Changelists;
	Algo::Transform(ChangelistsStateCache, Changelists, [](const auto& Pair) { return MakeShared<FGitSourceControlChangelist, ESPMode::ThreadSafe>(Pair.Key); });
	return Changelists;
}
#endif

#if SOURCE_CONTROL_WITH_SLATE
TSharedRef<class SWidget> FGitSourceControlProvider::MakeSettingsWidget() const
{
	return SNew(SGitSourceControlSettings);
}
#endif

ECommandResult::Type FGitSourceControlProvider::ExecuteSynchronousCommand(FGitSourceControlCommand& InCommand, const FText& Task, bool bSuppressResponseMsg)
{
	ECommandResult::Type Result = ECommandResult::Failed;

	struct Local
	{
		static void CancelCommand(FGitSourceControlCommand* InControlCommand)
		{
			InControlCommand->Cancel();
		}
	};

	FText TaskText = Task;
	// Display the progress dialog
	if (bSuppressResponseMsg)
	{
		TaskText = FText::GetEmpty();
	}

	int i = 0;

	// Display the progress dialog if a string was provided
	{
		// TODO: support cancellation?
		//FScopedSourceControlProgress Progress(TaskText, FSimpleDelegate::CreateStatic(&Local::CancelCommand, &InCommand));
		FScopedSourceControlProgress Progress(TaskText);

		// Issue the command asynchronously...
		IssueCommand( InCommand );

		// ... then wait for its completion (thus making it synchronous)
		while (!InCommand.IsCanceled() && CommandQueue.Contains(&InCommand))
		{
			// Tick the command queue and update progress.
			Tick();

			if (i >= 20) {
				Progress.Tick();
				i = 0;
			}
			i++;

			// Sleep for a bit so we don't busy-wait so much.
			FPlatformProcess::Sleep(0.01f);
		}

		if (InCommand.bCancelled)
		{
			Result = ECommandResult::Cancelled;
		}
		else if (InCommand.bCommandSuccessful)
		{
			Result = ECommandResult::Succeeded;
		}
		else if (!bSuppressResponseMsg)
		{
			// 失败不是统一的网络故障，也不表示之前步骤已回滚；保留完整明细，非模态通知可直接打开日志。
			// Failure is neither necessarily a network error nor a rollback; retain details in a non-modal, linked notification.
			const FString Detail = InCommand.ResultInfo.ErrorMessages.IsEmpty()
				? TEXT("命令未提供具体原因，请查看版本控制日志。")
				: InCommand.ResultInfo.ErrorMessages[0];
			const FText Summary = FText::Format(
				LOCTEXT("Git_CommandIncomplete", "{0} 未全部完成。{1}\n点击查看具体文件、失败原因及已完成步骤。"),
				FText::FromName(InCommand.Operation->GetName()),
				FText::FromString(Detail.Len() > 180 ? Detail.Left(180) + TEXT("…") : Detail));
			UE_LOG(LogSourceControl, Error, TEXT("Command '%s' failed in %s: %s"),
				*InCommand.Operation->GetName().ToString(), *InCommand.PathToGitRoot,
				*FString::Join(InCommand.ResultInfo.ErrorMessages, TEXT("\n")));
			FMessageLog("SourceControl").Notify(Summary, EMessageSeverity::Error, true);
		}
	}

	// Delete the command now if not marked as auto-delete
	if (!InCommand.bAutoDelete)
	{
		delete &InCommand;
	}

	return Result;
}

ECommandResult::Type FGitSourceControlProvider::IssueCommand(FGitSourceControlCommand& InCommand, const bool bSynchronous)
{
	InCommand.QueuedAtSeconds = FPlatformTime::Seconds();
	if (!bSynchronous && GThreadPool != nullptr)
	{
		// Queue this to our worker thread(s) for resolving.
		// When asynchronous, any callback gets called from Tick().
		GThreadPool->AddQueuedWork(&InCommand);
		CommandQueue.Add(&InCommand);
		return ECommandResult::Succeeded;
	}
	else
	{
		UE_LOG(LogSourceControl, Log, TEXT("There are no threads available to process the revision control command '%s'. Running synchronously."), *InCommand.Operation->GetName().ToString());

		InCommand.bCommandSuccessful = InCommand.DoWork();

		InCommand.Worker->UpdateStates();

		OutputCommandMessages(InCommand);

		// Callback now if present. When asynchronous, this callback gets called from Tick().
		return InCommand.ReturnResults();
	}
}

bool FGitSourceControlProvider::QueryStateBranchConfig(const FString& ConfigSrc, const FString& ConfigDest)
{
	// Check similar preconditions to Perforce (valid src and dest),
	if (ConfigSrc.Len() == 0 || ConfigDest.Len() == 0)
	{
		return false;
	}

	if (!bGitAvailable || !bGitRepositoryFound)
	{
		FTSMessageLog("SourceControl").Error(LOCTEXT("StatusBranchConfigNoConnection", "Unable to retrieve status branch configuration from repo, no connection"));
		return false;
	}

	// Otherwise, we can assume that whatever our user is doing to config state branches is properly synced, so just copy.
	// TODO: maybe don't assume, and use git show instead?
	IFileManager::Get().Copy(*ConfigDest, *ConfigSrc);
	return true;
}

void FGitSourceControlProvider::RegisterStateBranches(const TArray<FString>& BranchNames, const FString& ContentRootIn)
{
	StatusBranchNamePatternsInternal = BranchNames;
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
bool FGitSourceControlProvider::GetStateBranchAtIndex(int32 BranchIndex, FString& OutBranchName) const
{
	auto StatusBranchNames = GetStatusBranchNames();

	if (StatusBranchNames.IsValidIndex(BranchIndex))
	{
		OutBranchName = StatusBranchNames[BranchIndex];
		return true;
	}
	return false;
}
#endif

int32 FGitSourceControlProvider::GetStateBranchIndex(const FString& StateBranchName) const
{
	// How do state branches indices work?
	// Order matters. Lower values are lower in the hierarchy, i.e., changes from higher branches get automatically merged down.
	// The higher branch is, the stabler it is, and has changes manually promoted up.

	// Check if we are checking the index of the current branch
	// UE uses FEngineVersion for the current branch name because of UEGames setup, but we want to handle otherwise for Git repos.
	auto StatusBranchNames = GetStatusBranchNames();
	if (StateBranchName == FEngineVersion::Current().GetBranch())
	{
		const int32 CurrentBranchStatusIndex = StatusBranchNames.IndexOfByKey(BranchName);
		const bool bCurrentBranchInStatusBranches = CurrentBranchStatusIndex != INDEX_NONE;
		// If the user's current branch is tracked as a status branch, give the proper index
		if (bCurrentBranchInStatusBranches)
		{
			return CurrentBranchStatusIndex;
		}
		// If the current branch is not a status branch, make it the highest branch
		// This is semantically correct, since if a branch is not marked as a status branch
		// it merges changes in a similar fashion to the highest status branch, i.e. manually promotes them
		// based on the user merging those changes in. and these changes always get merged from even the highest point
		// of the stream. i.e, promoted/stable changes are always up for consumption by this branch.
		return INT32_MAX;
	}

	// If we're not checking the current branch, then we don't need to do special handling.
	// If it is not a status branch, there is no message
	return StatusBranchNames.IndexOfByKey(StateBranchName);
}

TArray<FString> FGitSourceControlProvider::GetStatusBranchNames() const
{
	TArray<FString> StatusBranches;
	if (PathToGitBinary.IsEmpty() || PathToRepositoryRoot.IsEmpty())
		return StatusBranches;

	for (int i = 0; i < StatusBranchNamePatternsInternal.Num(); i++)
	{
		TArray<FString> Matches;
		bool bResult = GitSourceControlUtils::GetRemoteBranchesWildcard(PathToGitBinary, PathToRepositoryRoot, StatusBranchNamePatternsInternal[i], Matches);
		if (bResult && Matches.Num() > 0)
		{
			for (int j = 0; j < Matches.Num(); j++)
			{
				StatusBranches.Add(Matches[j].TrimStartAndEnd());	
			}
		}
	}

	return StatusBranches;
}

#undef LOCTEXT_NAMESPACE
