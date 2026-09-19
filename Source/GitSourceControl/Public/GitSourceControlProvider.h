// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "GitSourceControlChangelist.h"
#include "ISourceControlProvider.h"
#include "IGitSourceControlWorker.h"
#include "GitSourceControlMenu.h"
#include "Runtime/Launch/Resources/Version.h"

class FGitSourceControlChangelistState;
class FGitSourceControlState;
class FGitRepositoryInitializationTask;
struct FGitLockSettingsSnapshot;

class FGitSourceControlCommand;

DECLARE_DELEGATE_RetVal(FGitSourceControlWorkerRef, FGetGitSourceControlWorker)

/// Git version and capabilites extracted from the string "git version 2.11.0.windows.3"
struct FGitVersion
{
	// Git version extracted from the string "git version 2.11.0.windows.3" (Windows), "git version 2.11.0" (Linux/Mac/Cygwin/WSL) or "git version 2.31.1.vfs.0.3" (Microsoft)
	int Major;   // 2	Major version number
	int Minor;   // 31	Minor version number
	int Patch;   // 1	Patch/bugfix number
	bool bIsFork;
	FString Fork; // "vfs"
	int ForkMajor; // 0	Fork specific revision number
	int ForkMinor; // 3 
	int ForkPatch; // ?

	FGitVersion() 
		: Major(0)
		, Minor(0)
		, Patch(0)
		, bIsFork(false)
		, ForkMajor(0)
		, ForkMinor(0)
		, ForkPatch(0)
	{
	}
};

class GITSOURCECONTROL_API FGitSourceControlProvider final : public ISourceControlProvider
{
public:
	/* ISourceControlProvider implementation */
	virtual void Init(bool bForceConnection = true) override;
	virtual void Close() override;
	virtual FText GetStatusText() const override;
	virtual bool IsEnabled() const override;
	virtual bool IsAvailable() const override;
	virtual const FName& GetName(void) const override;
	virtual bool QueryStateBranchConfig(const FString& ConfigSrc, const FString& ConfigDest) override;
	virtual void RegisterStateBranches(const TArray<FString>& BranchNames, const FString& ContentRootIn) override;
	virtual int32 GetStateBranchIndex(const FString& BranchName) const override;
	virtual ECommandResult::Type GetState( const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage ) override;
#if ENGINE_MAJOR_VERSION >= 5
    virtual ECommandResult::Type GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage) override;
#endif
	virtual TArray<FSourceControlStateRef> GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const override;
	virtual FDelegateHandle RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged) override;
	virtual void UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle) override;
#if ENGINE_MAJOR_VERSION < 5
	virtual ECommandResult::Type Execute( const FSourceControlOperationRef& InOperation, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency = EConcurrency::Synchronous, const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete()) override;
	virtual bool CanCancelOperation( const FSourceControlOperationRef& InOperation ) const override;
	virtual void CancelOperation( const FSourceControlOperationRef& InOperation ) override;
#else
	virtual ECommandResult::Type Execute(const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr InChangelist, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency = EConcurrency::Synchronous, const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete()) override;
	virtual bool CanCancelOperation( const FSourceControlOperationRef& InOperation ) const override;
	virtual void CancelOperation( const FSourceControlOperationRef& InOperation ) override;
#endif
	virtual bool UsesLocalReadOnlyState() const override;
	virtual bool UsesChangelists() const override;
	virtual bool UsesCheckout() const override;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
	virtual bool UsesFileRevisions() const override;
#if ENGINE_MINOR_VERSION >= 8
	virtual TOptional<bool> HasChangesToSync() const override;
	virtual TOptional<bool> HasChangesToCheckIn() const override;
#else
	virtual TOptional<bool> IsAtLatestRevision() const override;
	virtual TOptional<int> GetNumLocalChanges() const override;
#endif
#endif
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
	virtual bool AllowsDiffAgainstDepot() const override;
	virtual bool UsesUncontrolledChangelists() const override;
	virtual bool UsesSnapshots() const override;
#endif
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	virtual bool UsesSoftRevertOnDelete() const override;
#endif
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
	virtual bool CanExecuteOperation( const FSourceControlOperationRef& InOperation ) const override;
	virtual TMap<EStatus, FString> GetStatus() const override;
#endif
	virtual void Tick() override;
	virtual TArray< TSharedRef<class ISourceControlLabel> > GetLabels( const FString& InMatchingSpec ) const override;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	virtual bool GetStateBranchAtIndex(int32 BranchIndex, FString& OutBranchName) const override;
#endif	

#if ENGINE_MAJOR_VERSION >= 5
	virtual TArray<FSourceControlChangelistRef> GetChangelists( EStateCacheUsage::Type InStateCacheUsage ) override;
#endif

#if SOURCE_CONTROL_WITH_SLATE
	virtual TSharedRef<class SWidget> MakeSettingsWidget() const override;
#endif

	using ISourceControlProvider::Execute;

	/**
	 * Check configuration, else standard paths, and run a Git "version" command to check the availability of the binary.
	 */
	void CheckGitAvailability();

	/** Refresh Git settings from revision control settings */
	void UpdateSettings();

	/**
	 * Find the .git/ repository and check its status.
	 */
	void CheckRepositoryStatus();

	/** Is git binary found and working. */
	inline bool IsGitAvailable() const
	{
		return bGitAvailable;
	}

	/** Git version for feature checking */
	inline const FGitVersion& GetGitVersion() const
	{
		return GitVersion;
	}

	/** Path to the root of the Unreal revision control repository: usually the ProjectDir */
	inline const FString& GetPathToRepositoryRoot() const
	{
		return PathToRepositoryRoot;
	}

	/** Path to the root of the Git repository: can be the ProjectDir itself, or any parent directory (found by the "Connect" operation) */
	inline const FString& GetPathToGitRoot() const
	{
		return PathToGitRoot;
	}

	/** Gets the path to the Git binary */
	inline const FString& GetGitBinaryPath() const
	{
		return PathToGitBinary;
	}

	/** Git config user.name */
	inline const FString& GetUserName() const
	{
		return UserName;
	}

	/** Git config user.email */
	inline const FString& GetUserEmail() const
	{
		return UserEmail;
	}

	/** Git remote origin url */
	inline const FString& GetRemoteUrl() const
	{
		return RemoteUrl;
	}

	inline const FString& GetLockUser() const
	{
		return LockUser;
	}

#if WITH_DEV_AUTOMATION_TESTS
	/** 用指定仓库能力应用一次设置快照，供回归测试验证能力不会被后续设置刷新绕过。 */
	void ApplyLockingSettingsForTests(
		const FGitLockSettingsSnapshot& InSettings,
		bool bInLockableAttributesAvailable);
#endif

	/** Helper function used to update state cache */
	TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> GetStateInternal(const FString& Filename);

#if ENGINE_MAJOR_VERSION == 5	
	/** Helper function used to update changelists state cache */
	TSharedRef<FGitSourceControlChangelistState, ESPMode::ThreadSafe> GetStateInternal(const FGitSourceControlChangelist& InChangelist);
#endif

	/**
	 * Register a worker with the provider.
	 * This is used internally so the provider can maintain a map of all available operations.
	 */
	void RegisterWorker( const FName& InName, const FGetGitSourceControlWorker& InDelegate );

	/** Set list of error messages that occurred after last perforce command */
	void SetLastErrors(const TArray<FText>& InErrors);

	/** Get list of error messages that occurred after last perforce command */
	TArray<FText> GetLastErrors() const;

	/** Get number of error messages seen after running last perforce command */
	int32 GetNumLastErrors() const;

	/** Remove a named file from the state cache */
	bool RemoveFileFromCache(const FString& Filename);

	/** Get files in cache */
	TArray<FString> GetFilesInCache();

	bool AddFileToIgnoreForceCache(const FString& Filename);

	bool RemoveFileFromIgnoreForceCache(const FString& Filename);

	const FString& GetBranchName() const
	{
		return BranchName;
	}

	const FString& GetRemoteBranchName() const { return RemoteBranchName; }

	TArray<FString> GetStatusBranchNames() const;

	/** Indicates editor binaries are to be updated upon next sync */
	bool bPendingRestart;

#if ENGINE_MAJOR_VERSION >= 5
	uint32 TicksUntilNextForcedUpdate = 0;
#endif

private:
	/** Provider 正在关闭；关闭阶段拒绝再接收命令。 */
	bool bClosing = false;

	/** Is git binary found and working. */
	bool bGitAvailable = false;

	/** Is git repository found. */
	bool bGitRepositoryFound = false;

	/** 当前仓库是否已验证存在所需的 lockable 属性；所有设置刷新都必须保留此能力门。 */
	bool bLockableAttributesAvailable = false;

	/** 当前仓库的 lockable 能力探测是否已完成；重新探测期间 Provider 保守不可用。 */
	bool bLockableAttributesCapabilityKnown = false;

	/** Is LFS locking enabled? */
	bool bUsingGitLfsLocking = false;

	/** 持久锁开关及仓库能力是否能满足用户要求；无效时整个 Provider 失败关闭。 */
	bool bLockingConfigurationValid = true;

	/** 非法持久开关或仓库能力不足的可见错误。 */
	FString LockingConfigurationError;

	/** 最近应用到 Provider 的 Editor 锁设置代次。 */
	uint64 LockSettingsGeneration = 0;

	/** 周期刷新是否已有受管命令在途。 */
	bool bBackgroundRefreshInFlight = false;

	/** 旧配置或旧分支命令已失败关闭；下一 Tick 必须优先提交新鲜刷新。 */
	bool bBackgroundRefreshRequested = false;

	/** 下一次周期刷新使用的单调时钟秒数。 */
	double NextBackgroundRefreshTimeSeconds = 0.0;

	FString PathToGitBinary;

	FString LockUser;

	/** Critical section for thread safety of error messages that occurred after last perforce command */
	mutable FCriticalSection LastErrorsCriticalSection;

	/** List of error messages that occurred after last perforce command */
	TArray<FText> LastErrors;

	/** Helper function for Execute() */
	TSharedPtr<class IGitSourceControlWorker, ESPMode::ThreadSafe> CreateWorker(const FName& InOperationName) const;

	/** Helper function for running command synchronously. */
	ECommandResult::Type ExecuteSynchronousCommand(class FGitSourceControlCommand& InCommand, const FText& Task, bool bSuppressResponseMsg);
	/** Issue a command asynchronously if possible. */
	ECommandResult::Type IssueCommand(class FGitSourceControlCommand& InCommand, const bool bSynchronous = false );

	/** Output any messages this command holds */
	void OutputCommandMessages(const class FGitSourceControlCommand& InCommand) const;

	/** Update repository status on Connect and UpdateStatus operations */
	void UpdateRepositoryStatus(const class FGitSourceControlCommand& InCommand);

	/** 到期时提交一个受 CommandQueue 管理的周期刷新；关闭阶段不会再提交。 */
	void StartBackgroundRefreshIfDue();

	/** 周期刷新完成回调；命令队列在 Close 返回前保证交付。 */
	void OnBackgroundRefreshComplete(
		const FSourceControlOperationRef& InOperation,
		ECommandResult::Type InResult);

	/**
	 * 将 Editor 设置与仓库 lockable 能力合成为唯一有效锁模式；设置刷新不能绕过仓库能力。
	 * Combine editor settings with the repository lockable capability into the sole effective
	 * locking mode. A later settings refresh must never bypass repository capability.
	 */
	void ApplyEffectiveLockingSettings(
		const FGitLockSettingsSnapshot& InSettings);

	/**
	 * 完成或轮询 Provider 自有的仓库初始化任务；仅在调用线程消费结果，Close 可等待后丢弃。
	 * Completes or polls the provider-owned repository initialization task. Results are consumed only
	 * on the calling thread, while Close can wait and discard them.
	 */
	void FinalizeRepositoryInitialization(bool bWaitForCompletion, bool bApplyResult);

	/** Path to the root of the Unreal revision control repository: usually the ProjectDir */
	FString PathToRepositoryRoot;

	/** Path to the root of the Git repository: can be the ProjectDir itself, or any parent directory (found by the "Connect" operation) */
	FString PathToGitRoot;

	/** Git config user.name (from local repository, else globally) */
	FString UserName;

	/** Git config user.email (from local repository, else globally) */
	FString UserEmail;

	/** Name of the current branch */
	FString BranchName;

	/** Name of the current remote branch */
	FString RemoteBranchName;

	/** URL of the "origin" default remote server */
	FString RemoteUrl;

	/** Current Commit full SHA1 */
	FString CommitId;

	/** Current Commit description's Summary */
	FString CommitSummary;

	/** State cache */
	TMap<FString, TSharedRef<class FGitSourceControlState, ESPMode::ThreadSafe> > StateCache;
#if ENGINE_MAJOR_VERSION == 5
	TMap<FGitSourceControlChangelist, TSharedRef<class FGitSourceControlChangelistState, ESPMode::ThreadSafe> > ChangelistsStateCache;
#endif

	/** The currently registered revision control operations */
	TMap<FName, FGetGitSourceControlWorker> WorkersMap;

	/** Queue for commands given by the main thread */
	TArray < FGitSourceControlCommand* > CommandQueue;

	/** For notifying when the revision control states in the cache have changed */
	FSourceControlStateChanged OnSourceControlStateChanged;

	/** Git version for feature checking */
	FGitVersion GitVersion;

	/** Revision Control Menu Extension */
	FGitSourceControlMenu GitSourceControlMenu;

	/**
		Ignore these files when forcing status updates. We add to this list when we've just updated the status already.
		UE's SourceControl has a habit of performing a double status update, immediately after an operation.
	*/
	TArray<FString> IgnoreForceCache;

	/** Array of branch name patterns for status queries */
	TArray<FString> StatusBranchNamePatternsInternal;

	/**
	 * Provider 自有、可等待的仓库初始化任务；任务不捕获 Provider，结果由 Tick 消费。
	 * Provider-owned, waitable repository initialization task. It never captures the provider and
	 * Tick consumes its result.
	 */
	TSharedPtr<FGitRepositoryInitializationTask, ESPMode::ThreadSafe> RepositoryInitializationTask;
};
