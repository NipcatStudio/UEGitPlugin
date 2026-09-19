// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "HAL/CriticalSection.h"

/**
 * LFS 操作一次性读取的同代配置快照；Generation 在任一相关设置变化时单调推进。
 * Same-generation settings snapshot for one LFS operation. Generation advances
 * monotonically whenever any relevant setting changes.
 */
struct GITSOURCECONTROL_API FGitLockSettingsSnapshot
{
	/** 配置代次；0 仅表示尚未完成初始加载。 */
	uint64 Generation = 0;
	/** 是否启用 Git LFS 锁。 */
	bool bUsingGitLfsLocking = false;
	/** 配置的 Git LFS owner 名称。 */
	FString LfsUserName;
};

class GITSOURCECONTROL_API FGitSourceControlSettings
{
public:
	/** 获取 Git 可执行文件路径的线程安全副本。 */
	FString GetBinaryPath() const;

	/** Set the Git Binary Path */
	bool SetBinaryPath(const FString& InString);

	/** Tell if using the Git LFS file Locking workflow */
	bool IsUsingGitLfsLocking() const;

	/** Configure the usage of Git LFS file Locking workflow */
	bool SetUsingGitLfsLocking(const bool InUsingGitLfsLocking);

	/** Get the username used by the Git LFS 2 File Locks server */
	const FString GetLfsUserName() const;

	/** Set the username used by the Git LFS 2 File Locks server */
	bool SetLfsUserName(const FString& InString);

	/** 原子读取 LFS 锁设置及其代次。 */
	FGitLockSettingsSnapshot GetLockSettingsSnapshot() const;

	/** Load settings from ini file */
	void LoadSettings();

	/** Save settings to ini file */
	void SaveSettings() const;

private:
	/** A critical section for settings access */
	mutable FCriticalSection CriticalSection;

	/** Git binary path */
	FString BinaryPath;

	/** Tells if using the Git LFS file Locking workflow */
	bool bUsingGitLfsLocking = true;

	/** Username used by the Git LFS 2 File Locks server */
	FString LfsUserName;

	/** LFS 锁配置的单调代次；任一相关字段变化都会推进，回改原值也不会复用。 */
	uint64 LockSettingsGeneration = 0;

	/** 在已持有 CriticalSection 时推进非零配置代次。 */
	void AdvanceLockSettingsGenerationLocked();
};
