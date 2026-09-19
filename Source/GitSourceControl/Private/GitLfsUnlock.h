// Copyright (c) 2026 LunarMaxim
// Distributed under the MIT License (MIT).

#pragma once

#include "CoreMinimal.h"

/**
 * 普通 LFS 解锁只消费服务器认证的身份与精确锁 ID；显示用户名不授予删除权限。
 * Ordinary LFS unlocks consume authenticated ownership and exact server lock IDs; display names
 * never authorize deletion.
 */
namespace GitLfsUnlock
{
	/**
	 * 一次完整 verify 查询中的锁记录；所有权来自协议前缀，而非用户名比较。
	 * A lock from a complete verify response; ownership comes from the protocol marker, not names.
	 */
	struct FVerifiedLock
	{
		/** 服务器返回的精确锁 ID。 */
		FString Id;
		/** 已规范化的仓库相对文件路径。 */
		FString RelativePath;
		/** 当前 Git 凭据是否被服务器认证为此锁的持有人。 */
		bool bOurs = false;
	};

	/** 非零退出、部分响应、歧义或格式错误均拒绝；失败时不返回可用于授权的记录。 */
	bool ParseVerification(
		bool bCommandSucceeded,
		const TArray<FString>& InLines,
		TArray<FVerifiedLock>& OutLocks,
		FString& OutError);

	/**
	 * 在一次操作内复核并重试暂时性错误；锁 ID 或认证身份变化时停止，不追逐不同 ID。
	 * 锁 ID 是服务端不透明标识，不承诺服务端不会为同一路径重复使用它。
	 * 每个请求批次最多 4 个已认证 ID，共享前后核验；批次提交前仍复核上下文。
	 * ReleasedPaths 只包含已由成功查询确认释放的文件，批次部分成功时也可精确更新缓存。
	 */
	bool Release(
		const TArray<FString>& InRelativePaths,
		TFunctionRef<bool(TArray<FVerifiedLock>&, FString&)> InQuery,
		TFunctionRef<bool(const TArray<FVerifiedLock>&, FString&)> InUnlockBatch,
		TFunctionRef<bool()> InIsContextCurrent,
		TArray<FString>& OutReleasedPaths,
		FString& OutError);
}
