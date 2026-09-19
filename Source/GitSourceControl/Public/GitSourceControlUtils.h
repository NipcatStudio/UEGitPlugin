// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "GitSourceControlRevision.h"
#include "GitSourceControlState.h"
#include "Runtime/Launch/Resources/Version.h"
#if ENGINE_MAJOR_VERSION == 5
#include "UObject/ObjectSaveContext.h"
#endif

class FGitSourceControlState;

class FGitSourceControlCommand;

/**
 * UEGitPlugin 对本地文件只读位的显式收敛决策；它不表示远端锁所有权。
 * Explicit convergence decision for a local file's read-only bit; it does not represent remote
 * lock ownership.
 */
enum class EGitLocalReadOnlyPolicy : uint8
{
	Preserve,
	ReadOnly,
	Writable,
	/** 枚举哨兵，仅用于完整遍历测试；不得作为运行时策略。 */
	Count,
};

/**
 * Helper struct for maintaining temporary files for passing to commands
 */
class FGitScopedTempFile
{
public:

	/** Constructor - open & write string to temp file */
	FGitScopedTempFile(const FText& InText);

	/** Destructor - delete temp file */
	~FGitScopedTempFile();

	/** Get the filename of this temp file - empty if it failed to be created */
	const FString& GetFilename() const;

private:
	/** The filename we are writing to */
	FString Filename;
};

struct FGitVersion;

/**
 * 单端 LFS 的临时视图；认证所有权独立于显示用户名，显式解锁同步移除两者。
 * Temporary single-endpoint LFS view; authenticated ownership is separate from display names,
 * and an explicit release removes both facts together.
 */
class FGitLockedFilesCache
{
public:
	static FDateTime LastUpdated;

 static TMap<FString, FString> GetLockedFiles();
 static void SetLockedFiles(const TMap<FString, FString>& newLocks);
 static void AddLockedFile(const FString& filePath, const FString& lockUser);
 static void RemoveLockedFile(const FString& filePath);
 /** 只接收完整认证列表；平滑暂时差异时保留认证身份，不能从显示用户名推导本人所有权。 */
 static TMap<FString, FString> UpdateFromServerListing(const FString& InRepositoryRoot, const TMap<FString, FString>& FreshLocks, const TSet<FString>& AuthenticatedOwnFiles);
	/** 当前缓存是否有服务器认证为本人的锁；显示用户名不授予权限。 */
	static bool IsOwnedByCurrentCredential(const FString& FilePath);

	// A lock transition observed by this cache (our own lock/unlock, or a background listing
	// diff). Queued on whatever thread noticed it and drained on the game thread by
	// FGitSourceControlProvider::Tick, which applies it to the per-file state cache and
	// notifies the UI. Without that propagation, a file state computed while the cache was
	// momentarily wrong (eventually-consistent listings around editor startup) keeps its
	// stale checkout badge until something happens to re-query that specific file.
	struct FLockStateFixup
	{
		FString FilePath;
		FString LockUser;
		bool bLocked = false;
	};
	static TArray<FLockStateFixup> TakePendingStateFixups();

private:
 static void OnFileLockChanged(const FString& filePath, const FString& lockUser, bool locked);
 static void SetLockedFilesInternal(const TMap<FString, FString>& newLocks);
 // update local read/write state when our own lock statuses change
	static TMap<FString, FString> LockedFiles;
	/** 当前凭据通过 verify 认证持有的路径；与 LockedFiles 使用同一互斥锁。 */
	static TSet<FString> AuthenticatedOwnFiles;
	// guards LockedFiles and the streak maps: source control commands run on pooled worker
	// threads, so listings and lock/unlock completions can touch the cache concurrently
	static FCriticalSection LockedFilesMutex;
	// consecutive remote listings a known lock has been missing from / an unknown lock has
	// appeared in; a divergence is only accepted once corroborated (see UpdateFromServerListing)
	static TMap<FString, int32> MissingStreak;
	static TMap<FString, int32> AppearStreak;
	// guarded by LockedFilesMutex; see FLockStateFixup
	static TArray<FLockStateFixup> PendingStateFixups;
};

namespace GitSourceControlUtils
{
	/**
	 * 解析状态命令应采用的 LFS 权限模式；设置代次变化时，只要旧命令或新设置任一启用
	 * LFS，就保持权限收敛，避免旧命令在新锁模式下放开文件。
	 */
	bool IsLfsReadOnlyPolicyActive(
		bool bInCommandUsingGitLfsLocking,
		bool bInSettingsSuperseded,
		bool bInCurrentSettingsUsingGitLfsLocking);

	/** 根据锁模式和类型化状态决定是否保持、设置或清除本地只读位。 */
	EGitLocalReadOnlyPolicy GetLocalReadOnlyPolicy(
		const FGitSourceControlState& InState,
		bool bInUsingGitLfsLocking);

	/**
	 * 把单端 LFS 的增删事实与认证所有权投影到完整状态；显示用户名不参与权限判断。
	 * 解锁干净既有文件会显式恢复只读；普通 status 的 Clean+NotLocked 仍可 Preserve 手工 writable 窗口。
	 * Projects one single-endpoint LFS listing transition into a complete cached state. The resulting
	 * complete state plus transition provenance owns the permission decision.
	 */
	EGitLocalReadOnlyPolicy ApplyLegacyLfsLockStateTransition(
		FGitSourceControlState& InOutState,
		const FString& InLockUser,
		bool bInLocked,
		bool bInOwnedByCurrentCredential);

	/**
	 * 把只读策略应用到磁盘文件；未知策略、文件不存在或属性修改失败时，不改盘、返回 false，
	 * 并始终写入 Output Log 与 Source Control Message Log。
	 * @param OutFailureReason 失败时接收含文件路径与目标权限的中文原因；成功时清空；可为 nullptr。
	 */
	bool ApplyLocalReadOnlyPolicy(
		const FString& InFilename,
		EGitLocalReadOnlyPolicy InPolicy,
		FString* OutFailureReason = nullptr);

	/** 只有启用 LFS 锁工作流时才消费锁缓存及其状态转换。 */
	bool ShouldUseLegacyLfsLockCache(
		bool bInUsingGitLfsLocking);

	/**
		*  Returns an updated repo root if all selected files are in a plugin subfolder, and the plugin subfolder is a git repo
		*  This supports the case where each plugin is a sub module
		*
		* @param AbsoluteFilePaths		The list of files in the SC operation
		* @param PathToRepositoryRoot	The original path to the repository root (used by default)
		*/
	FString ChangeRepositoryRootIfSubmodule(TArray<FString>& AbsoluteFilePaths, const FString& PathToRepositoryRoot);

	/**
		*  Returns an updated repo root if all selected file is in a plugin subfolder, and the plugin subfolder is a git repo
		*  This supports the case where each plugin is a sub module
		*
		* @param AbsoluteFilePath		The file in the SC operation
		* @param PathToRepositoryRoot	The original path to the repository root (used by default)
		*/
	FString ChangeRepositoryRootIfSubmodule(FString & AbsoluteFilePath, const FString& PathToRepositoryRoot);

/**
 * Find the path to the Git binary, looking into a few places (standalone Git install, and other common tools embedding Git)
 * @returns the path to the Git binary if found, or an empty string.
 */
FString FindGitBinaryPath();

/**
 * Run a Git "version" command to check the availability of the binary.
 * @param InPathToGitBinary		The path to the Git binary
 * @param OutGitVersion         If provided, populate with the git version parsed from "version" command
 * @returns true if the command succeeded and returned no errors
 */
bool CheckGitAvailability(const FString& InPathToGitBinary, FGitVersion* OutVersion = nullptr);

/**
 * Parse the output from the "version" command into GitMajorVersion and GitMinorVersion.
 * @param InVersionString       The version string returned by `git --version`
 * @param OutVersion            The FGitVersion to populate
 */
 void ParseGitVersion(const FString& InVersionString, FGitVersion* OutVersion);

	/**
		* Check git for various optional capabilities by various means.
		* @param InPathToGitBinary		The path to the Git binary
		* @param OutGitVersion			If provided, populate with the git version parsed from "version" command
		*/
	void FindGitCapabilities(const FString& InPathToGitBinary, FGitVersion* OutVersion);

	/**
		* Run a Git "lfs" command to check the availability of the "Large File System" extension.
		* @param InPathToGitBinary		The path to the Git binary
		* @param OutGitVersion			If provided, populate with the git version parsed from "version" command
		*/
	void FindGitLfsCapabilities(const FString& InPathToGitBinary, FGitVersion* OutVersion);

/**
 * Find the root of the Git repository, looking from the provided path and upward in its parent directories
 * @param InPath				The path to the Game Directory (or any path or file in any git repository)
 * @param OutRepositoryRoot		The path to the root directory of the Git repository if found, else the path to the ProjectDir
 * @returns true if the command succeeded and returned no errors
 */
bool FindRootDirectory(const FString& InPath, FString& OutRepositoryRoot);

/**
 * Get Git config user.name & user.email
 * @param	InPathToGitBinary	The path to the Git binary
 * @param	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory (can be empty)
 * @param	OutUserName			Name of the Git user configured for this repository (or globaly)
 * @param	OutEmailName		E-mail of the Git user configured for this repository (or globaly)
 */
void GetUserConfig(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutUserName, FString& OutUserEmail);

/**
 * Get Git current checked-out branch
 * @param	InPathToGitBinary	The path to the Git binary
 * @param	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory
 * @param	OutBranchName		Name of the current checked-out branch (if any, ie. not in detached HEAD)
 * @returns true if the command succeeded and returned no errors
 */
bool GetBranchName(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutBranchName);

/**
 * Get Git remote tracking branch
 * @returns false if the branch is not tracking a remote
 */
bool GetRemoteBranchName(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutBranchName);

 /**
 * Get Git remote tracking branches that match wildcard
 * @returns false if no matching branches
 */
 bool GetRemoteBranchesWildcard(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& PatternMatch, TArray<FString>& OutBranchNames);

/**
 * Get Git current commit details
 * @param	InPathToGitBinary	The path to the Git binary
 * @param	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory
 * @param	OutCommitId			Current Commit full SHA1
 * @param	OutCommitSummary	Current Commit description's Summary
 * @returns true if the command succeeded and returned no errors
 */
bool GetCommitInfo(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutCommitId, FString& OutCommitSummary);

/**
 * Get the URL of the "origin" defaut remote server
 * @param	InPathToGitBinary	The path to the Git binary
 * @param	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory
 * @param	OutRemoteUrl		URL of "origin" defaut remote server
 * @returns true if the command succeeded and returned no errors
 */
bool GetRemoteUrl(const FString& InPathToGitBinary, const FString& InRepositoryRoot, FString& OutRemoteUrl);

// A list of asset paths that we will always check locks, status, etc on startup and periodically 
TArray<FString> GetSourceControlledAssetPaths();

/**
 * Run a Git command - output is a string TArray.
 *
 * @param	InCommand			The Git command - e.g. commit
 * @param	InPathToGitBinary	The path to the Git binary
 * @param	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory (can be empty)
 * @param	InParameters		The parameters to the Git command
 * @param	InFiles				The files to be operated on
 * @param	OutResults			The results (from StdOut) as an array per-line
 * @param	OutErrorMessages	Any errors (from StdErr) as an array per-line
 * @returns true if the command succeeded and returned no errors
 */
GITSOURCECONTROL_API  bool RunCommand( const FString & InCommand, const FString & InPathToGitBinary, const FString & InRepositoryRoot, const TArray< FString > & InParameters, const TArray< FString > & InFiles, TArray< FString > & OutResults, TArray< FString > & OutErrorMessages );

/**
 * 运行会写入仓库的 Git 命令；每个实际子进程（包括拆分后的每一批及 index.lock
 * 恢复重试）启动前都调用写边界。边界返回 false 时立即停止，未启动的后续进程不会执行。
 * Runs a repository-mutating Git command and invokes the write boundary immediately before every
 * real subprocess, including every split batch and index-lock retry. A rejected boundary stops all
 * later processes.
 */
GITSOURCECONTROL_API bool RunCommandWithPreWriteBoundary(
	const FString& InCommand,
	const FString& InPathToGitBinary,
	const FString& InRepositoryRoot,
	const TArray<FString>& InParameters,
	const TArray<FString>& InFiles,
	TFunctionRef<bool()> InPreWriteBoundary,
	TArray<FString>& OutResults,
	TArray<FString>& OutErrorMessages);
bool RunCommandInternalRaw(const FString& InCommand, const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles, FString& OutResults, FString& OutErrors, const int32 ExpectedReturnCode = 0);

/**
 * 运行会返回机器解析路径的 Git 命令，并逐次强制 core.quotepath=false。
 * Runs a Git command whose output paths are machine-parsed, forcing core.quotepath=false for every
 * invocation instead of relying on repository or workstation configuration.
 */
GITSOURCECONTROL_API bool RunCommandWithLiteralPaths(
	const FString& InCommand,
	const FString& InPathToGitBinary,
	const FString& InRepositoryRoot,
	const TArray<FString>& InParameters,
	const TArray<FString>& InFiles,
	TArray<FString>& OutResults,
	TArray<FString>& OutErrorMessages);

/**
 * 运行 porcelain status 并强制返回未经 C-style/octal 转义的字面路径。
 * Runs porcelain status with literal, non-C-style/octal-escaped path output.
 */
bool RunStatusWithLiteralPaths(
	const FString& InPathToGitBinary,
	const FString& InRepositoryRoot,
	const TArray<FString>& InParameters,
	const TArray<FString>& InFiles,
	TArray<FString>& OutResults,
	TArray<FString>& OutErrorMessages);

/** 从一行 literal porcelain status 中取得仓库内绝对路径。 */
FString GetFullPathFromGitStatus(
	const FString& InResult,
	const FString& InRepositoryRoot);

/**
 * Unloads packages of specified named files
 */
TArray<class UPackage*> UnlinkPackages(const TArray<FString>& InPackageNames);

/**
 * Reloads packages for these packages
 */
void ReloadPackages(TArray<UPackage*>& InPackagesToReload);

/**
 * Gets all Git tracked files, including within directories, recursively
 */
bool ListFilesInDirectoryRecurse(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InDirectory, TArray<FString>& OutFiles);

/**
 * Run a Git "commit" command by batches.
 *
 * @param	InPathToGitBinary	The path to the Git binary
 * @param	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory
 * @param	InParameter			The parameters to the Git commit command
 * @param	InFiles				The files to be operated on
 * @param	OutErrorMessages	Any errors (from StdErr) as an array per-line
 * @returns true if the command succeeded and returned no errors
 */
bool RunCommit(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InParameters, const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages);

/**
 * @brief Detects how to parse the result of a "status" command to get workspace file states
 *
 *  It is either a command for a whole directory (ie. "Content/", in case of "Submit to Revision Control" menu),
 * or for one or more files all on a same directory (by design, since we group files by directory in RunUpdateStatus())
 *
 * @param[in]	InPathToGitBinary	The path to the Git binary
 * @param[in]	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory (can be empty)
 * @param[in]	InUsingLfsLocking	Tells if using the Git LFS file Locking workflow
 * @param[in]	InSettingsUsingLfsLocking	执行时设置是否仍启用 LFS 锁 / Whether current settings still enable LFS locking
 * @param[in]	InSettingsGeneration	命令捕获的一致设置代次 / Coherent settings generation captured by this command
 * @param[in]	InFiles				List of files in a directory, or the path to the directory itself (never empty).
 * @param[out]	InResults			Results from the "status" command
 * @param[out]	OutStates			States of files for witch the status has been gathered (distinct than InFiles in case of a "directory status")
 * @param[out]	InOutSettingsSuperseded	旧命令快照生成 fail-closed 状态时置位 / Set when an obsolete command snapshot produced fail-closed states
 */
GITSOURCECONTROL_API void ParseStatusResults(
	const FString& InPathToGitBinary,
	const FString& InRepositoryRoot,
	bool InUsingLfsLocking,
	bool InSettingsUsingLfsLocking,
	uint64 InSettingsGeneration,
	const TArray<FString>& InFiles,
	const TMap<FString, FString>& InResults,
	TMap<FString, FGitSourceControlState>& OutStates,
	bool& InOutSettingsSuperseded);

/**
 * Checks remote branches to see file differences.
 *
 * @param	CurrentBranchName The current branch we are on.
 * @param	InPathToGitBinary	The path to the Git binary
 * @param	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory
 * @param	OnePath				The file to be checked
 * @param	OutErrorMessages	Any errors (from StdErr) as an array per-line
 */
void CheckRemote(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& Files,
				 TArray<FString>& OutErrorMessages, TMap<FString, FGitSourceControlState>& OutStates);

/**
 * Run a Git "status" command and parse it.
 *
 * @param	InPathToGitBinary	The path to the Git binary
 * @param	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory (can be empty)
 * @param	InUsingLfsLocking	Tells if using the Git LFS file Locking workflow
 * @param	InFiles				The files to be operated on
 * @param	OutErrorMessages	Any errors (from StdErr) as an array per-line
 * @param   OutStates           The resultant states
 * @returns true if the command succeeded and returned no errors
 */
bool RunUpdateStatus(
	const FString& InPathToGitBinary,
	const FString& InRepositoryRoot,
	bool InUsingLfsLocking,
	bool InSettingsUsingLfsLocking,
	uint64 InSettingsGeneration,
	const TArray<FString>& InFiles,
	TArray<FString>& OutErrorMessages,
	TMap<FString, FGitSourceControlState>& OutStates,
	bool& InOutSettingsSuperseded);

#if ENGINE_MAJOR_VERSION == 5
/**
 * Keep Consistency of being file staged
 *
 * @param	Filename			Saved filename
 * @param	Pkg					Package (for adapting delegate)
 * @param   ObjectSaveContext	Context for save (for adapting delegate)
 */
void UpdateFileStagingOnSaved(const FString& Filename, UPackage* Pkg, FObjectPostSaveContext ObjectSaveContext);
#endif

/**
 * 保存已位于 Staged changelist 的文件后，排队受管的重新暂存命令。
 *
 * @param	Filename			已保存文件的绝对路径。
 * @returns 文件需要重新暂存且命令成功进入队列时返回 true；不等待 Git add 完成。
 */
bool UpdateFileStagingOnSavedInternal(const FString& Filename);

/**
 * 
 *
 * @param	Filename			Saved filename
 * @param	Pkg					Package (for adapting delegate)
 * @param   ObjectSaveContext	Context for save (for adapting delegate)
 */    
void UpdateStateOnAssetRename(const FAssetData& InAssetData, const FString& InOldName);

#if ENGINE_MAJOR_VERSION == 5
/**
 * 
 *
 * @param	Filename			Saved filename
 * @param	Pkg					Package (for adapting delegate)
 * @param   ObjectSaveContext	Context for save (for adapting delegate)
 */
bool UpdateChangelistStateByCommand();
#endif

/**
 * Run a Git "cat-file" command to dump the binary content of a revision into a file.
 *
 * @param	InPathToGitBinary	The path to the Git binary
 * @param	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory
 * @param	InParameter			The parameters to the Git show command (rev:path)
 * @param	InDumpFileName		The temporary file to dump the revision
 * @returns true if the command succeeded and returned no errors
*/
bool RunDumpToFile(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InParameter, const FString& InDumpFileName);

/**
 * Run a Git "log" command and parse it.
 *
 * @param	InPathToGitBinary	The path to the Git binary
 * @param	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory
 * @param	InFile				The file to be operated on
 * @param	bMergeConflict		In case of a merge conflict, we also need to get the tip of the "remote branch" (MERGE_HEAD) before the log of the "current branch" (HEAD)
 * @param	OutErrorMessages	Any errors (from StdErr) as an array per-line
 * @param	OutHistory			The history of the file
 */
bool RunGetHistory(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const FString& InFile, bool bMergeConflict, TArray<FString>& OutErrorMessages, TGitSourceControlHistory& OutHistory);

/**
 * Helper function to convert a filename array to relative paths.
 * @param	InFileNames		The filename array
 * @param	InRelativeTo	Path to the WorkspaceRoot
 * @return an array of filenames, transformed into relative paths
 */
TArray<FString> RelativeFilenames(const TArray<FString>& InFileNames, const FString& InRelativeTo);

/**
 * Helper function to convert a filename array to absolute paths.
 * @param	InFileNames		The filename array (relative paths)
 * @param	InRelativeTo	Path to the WorkspaceRoot
 * @return an array of filenames, transformed into absolute paths
 */
TArray<FString> AbsoluteFilenames(const TArray<FString>& InFileNames, const FString& InRelativeTo);

/**
 * Remove redundant errors (that contain a particular string) and also
 * update the commands success status if all errors were removed.
 */
void RemoveRedundantErrors(FGitSourceControlCommand& InCommand, const FString& InFilter);

	bool RunLFSCommand(const FString& InCommand, const FString& InRepositoryRoot, const FString& GitBinaryFallback, const TArray<FString>& InParameters, const TArray<FString>& InFiles, TArray<FString>& OutResults, TArray<FString>& OutErrorMessages);

	/** 当前选择的内置 LFS 是否支持一次进程提交多个精确 ID；系统 LFS 返回 false。 */
	bool SupportsBatchedLFSUnlock();

	/** 每个实际 LFS 写子进程启动前调用写边界；拒绝后不执行后续批次。 */
	bool RunLFSCommandWithPreWriteBoundary(
		const FString& InCommand,
		const FString& InRepositoryRoot,
		const FString& GitBinaryFallback,
		const TArray<FString>& InParameters,
		const TArray<FString>& InFiles,
		TFunctionRef<bool()> InPreWriteBoundary,
		TArray<FString>& OutResults,
		TArray<FString>& OutErrorMessages);

/**
 * Helper function for various commands to update cached states.
 * @returns true if any states were updated
 */
GITSOURCECONTROL_API bool UpdateCachedStates( const TMap< const FString, FGitState > & InResults );

/**
* Helper function for various commands to collect new states.
* @returns true if any states were updated
*/
GITSOURCECONTROL_API bool CollectNewStates( const TMap< FString, FGitSourceControlState > & InStates, TMap< const FString, FGitState > & OutResults );

/**
 * Helper function for various commands to collect new states.
 * @returns true if any states were updated
 */
bool CollectNewStates(const TArray<FString>& InFiles, TMap<const FString, FGitState>& OutResults, EFileState::Type FileState, ETreeState::Type TreeState = ETreeState::Unset, ELockState::Type LockState = ELockState::Unset, ERemoteState::Type RemoteState = ERemoteState::Unset);

	/**
		 * Run 'git lfs locks" to extract all lock information for all files in the repository
		 *
		 * @param	InRepositoryRoot	The Git repository from where to run the command - usually the Game directory
		 * @param   GitBinaryFallBack   The Git binary fallback path
		 * @param	OutErrorMessages    Any errors (from StdErr) as an array per-line
		 * @param	OutLocks		    The lock results (file, username)
		 * @returns true if the command succeeded and returned no errors
		 */
	/**
	 * OutRawServerLocks (optional): set to the raw, un-damped server listing when a fresh listing
	 * was actually fetched (left unset on cache hits and the offline --cached/--local fallback).
	 * OutLocks is smoothed by flap damping, which keeps a just-released lock alive for a few
	 * cycles - callers verifying "is this lock really gone" must use the raw listing instead.
	 */
	bool GetAllLocks(const FString& InRepositoryRoot, const FString& GitBinaryFallBack, TArray<FString>& OutErrorMessages, TMap<FString, FString>& OutLocks, bool bInvalidateCache = false, TOptional<TMap<FString, FString>>* OutRawServerLocks = nullptr);





/**
 * Gets locks from state cache
 */
void GetLockedFiles(const TArray<FString>& InFiles, TArray<FString>& OutFiles);

/**
 * Checks cache for if this file type is lockable
 */
bool IsFileLFSLockable(const FString& InFile);

/**
 * Gets Git attribute to see if these extensions are lockable
 */
bool CheckLFSLockable(const FString& InPathToGitBinary, const FString& InRepositoryRoot, const TArray<FString>& InFiles, TArray<FString>& OutErrorMessages);

GITSOURCECONTROL_API bool FetchRemote( const FString & InPathToGitBinary, const FString & InPathToRepositoryRoot, bool InUsingGitLfsLocking, TArray< FString > & OutResults, TArray< FString > & OutErrorMessages );

bool PullOrigin(const FString& InPathToGitBinary, const FString& InPathToRepositoryRoot, const TArray<FString>& InFiles, TArray<FString>& OutFiles,
				TArray<FString>& OutResults, TArray<FString>& OutErrorMessages);


GITSOURCECONTROL_API TSharedPtr< class ISourceControlRevision, ESPMode::ThreadSafe > GetOriginRevisionOnBranch( const FString & InPathToGitBinary, const FString & InRepositoryRoot, const FString & InRelativeFileName, TArray< FString > & OutErrorMessages, const FString & BranchName );

}
