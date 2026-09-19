// Copyright (c) 2026 LunarMaxim
// Distributed under the MIT License (MIT).

#include "GitLfsUnlock.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

namespace GitLfsUnlock
{
bool ParseVerification(
	bool bCommandSucceeded,
	const TArray<FString>& InLines,
	TArray<FVerifiedLock>& OutLocks,
	FString& OutError)
{
	OutLocks.Reset();
	OutError.Reset();
	if (!bCommandSucceeded)
	{
		OutError = TEXT("Git LFS 身份核验失败；不得将部分结果或空输出当作无锁。");
		return false;
	}

	TArray<FVerifiedLock> Parsed;
	TSet<FString> Ids;
	TSet<FString> Paths;
	for (FString Line : InLines)
	{
		Line.RemoveFromEnd(TEXT("\r"));
		FVerifiedLock Lock;
		Lock.bOurs = Line.StartsWith(TEXT("O "), ESearchCase::CaseSensitive);
		if (!Lock.bOurs && !Line.StartsWith(TEXT("  "), ESearchCase::CaseSensitive))
		{
			OutError = TEXT("Git LFS verify 响应缺少认证身份标记。");
			return false;
		}
		Line.RightChopInline(2);
		const int32 OwnerDelimiter = Line.Find(TEXT("\t"));
		const int32 IdDelimiter = Line.Find(TEXT("\tID:"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (OwnerDelimiter <= 0 || IdDelimiter <= OwnerDelimiter)
		{
			OutError = TEXT("Git LFS verify 响应缺少路径、持有人或锁 ID。");
			return false;
		}
		Lock.RelativePath = Line.Left(OwnerDelimiter).TrimEnd();
		Lock.Id = Line.Mid(IdDelimiter + 4).TrimStartAndEnd();
		const FString Owner = Line.Mid(OwnerDelimiter + 1, IdDelimiter - OwnerDelimiter - 1).TrimStartAndEnd();
		Lock.RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));
		FPaths::NormalizeFilename(Lock.RelativePath);
		const bool bUnsafeId = Lock.Id.IsEmpty() || Lock.Id.Contains(TEXT("\""))
			|| Lock.Id.Contains(TEXT("\\")) || Lock.Id.Contains(TEXT("\t"))
			|| Lock.Id.Contains(TEXT("\r")) || Lock.Id.Contains(TEXT("\n"));
		const bool bUnsafePath = Lock.RelativePath.IsEmpty() || !FPaths::IsRelative(Lock.RelativePath)
			|| Lock.RelativePath == TEXT("..") || Lock.RelativePath.StartsWith(TEXT("../"))
			|| Lock.RelativePath.Contains(TEXT("/../")) || Lock.RelativePath.EndsWith(TEXT("/.."))
			|| Lock.RelativePath.Contains(TEXT("\t")) || Lock.RelativePath.Contains(TEXT("\r"))
			|| Lock.RelativePath.Contains(TEXT("\n"));
		const FString PathKey = Lock.RelativePath.ToLower();
		if (bUnsafeId || bUnsafePath || Owner.IsEmpty() || Owner.Contains(TEXT("\t"))
			|| Ids.Contains(Lock.Id) || Paths.Contains(PathKey))
		{
			OutError = TEXT("Git LFS verify 响应含不安全、重复或歧义的锁记录。");
			return false;
		}
		Ids.Add(Lock.Id);
		Paths.Add(PathKey);
		Parsed.Add(MoveTemp(Lock));
	}
	OutLocks = MoveTemp(Parsed);
	return true;
}

bool Release(
	const TArray<FString>& InRelativePaths,
	TFunctionRef<bool(TArray<FVerifiedLock>&, FString&)> InQuery,
	TFunctionRef<bool(const TArray<FVerifiedLock>&, FString&)> InUnlockBatch,
	TFunctionRef<bool()> InIsContextCurrent,
	TArray<FString>& OutReleasedPaths,
	FString& OutError)
{
	OutReleasedPaths.Reset();
	OutError.Reset();
	TArray<FString> Pending;
	for (const FString& Path : InRelativePaths)
	{
		Pending.AddUnique(Path);
	}
	TMap<FString, FString> CapturedIds;
	TArray<FVerifiedLock> Snapshot;
	bool bHaveVerifiedSnapshot = false;
	FString LastError;

	/**
	 * 重试同一次用户操作，不重复 Revert 文件；无完整读回绝不伪造成功或清除未知锁。
	 * Retry this user operation without reverting files again; missing readback never means success.
	 */
	constexpr int32 MaxAttempts = 3;
	for (int32 Attempt = 0; Attempt < MaxAttempts && !Pending.IsEmpty(); ++Attempt)
	{
		if (Attempt > 0)
		{
			FPlatformProcess::Sleep(0.25f * Attempt);
		}
		if (!InIsContextCurrent())
		{
			OutError = TEXT("解锁上下文已变化；保留未完成的锁，禁止向新上下文发送删除。");
			return false;
		}
		// 完整读回既是上一轮结果，也是下一轮输入；不在两者之间重复整仓库查询。
		// A complete readback also supplies the next attempt, avoiding a duplicate repository query.
		if (!bHaveVerifiedSnapshot)
		{
			Snapshot.Reset();
			if (!InQuery(Snapshot, LastError))
			{
				continue;
			}
		}
		bHaveVerifiedSnapshot = false;
		if (!InIsContextCurrent())
		{
			OutError = TEXT("查询期间解锁上下文已变化；不消费结果或清除缓存。");
			return false;
		}
		TArray<FVerifiedLock> Batch;
		auto SubmitBatch = [&]()
		{
			if (Batch.IsEmpty()) { return true; }
			if (!InIsContextCurrent())
			{
				OutError = TEXT("发送解锁前上下文已变化；未发送本批删除。");
				return false;
			}
			// 请求结果不是终态；超时也要通过统一读回核对，不重放已确认完成的文件。
			// Request success is not final: shared readback also reconciles timeouts without replaying confirmed files.
			InUnlockBatch(Batch, LastError);
			Batch.Reset();
			return true;
		};
		for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
		{
			const FString& Path = Pending[Index];
			const FVerifiedLock* Lock = Snapshot.FindByPredicate([&Path](const FVerifiedLock& Entry)
			{
				return FPaths::IsSamePath(Entry.RelativePath, Path);
			});
			if (!Lock)
			{
				OutReleasedPaths.Add(Path);
				Pending.RemoveAt(Index);
				continue;
			}
			const FString* OriginalId = CapturedIds.Find(Path);
			if (!Lock->bOurs && !OriginalId)
			{
				// Revert 可丢弃本地修改，但他人锁既不删除，也不计入已释放路径。
				// Revert may discard local edits; foreign locks are neither deleted nor reported released.
				Pending.RemoveAt(Index);
				continue;
			}
			if (!Lock->bOurs || (OriginalId && *OriginalId != Lock->Id))
			{
				OutError = FString::Printf(TEXT("文件锁身份或 ID 已变化，不继续删除：%s"), *Path);
				return false;
			}
			CapturedIds.Add(Path, Lock->Id);
			Batch.Add(*Lock);
			if (Batch.Num() == 4 && !SubmitBatch())
			{
				return false;
			}
		}
		if (!SubmitBatch())
		{
			return false;
		}
		if (Pending.IsEmpty())
		{
			break;
		}
		Snapshot.Reset();
		if (!InIsContextCurrent() || !InQuery(Snapshot, LastError))
		{
			continue;
		}
		if (!InIsContextCurrent())
		{
			OutError = TEXT("解锁读回期间上下文已变化；不清除缓存或报告成功。");
			return false;
		}
		bHaveVerifiedSnapshot = true;
		for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
		{
			const FString& Path = Pending[Index];
			const FVerifiedLock* Lock = Snapshot.FindByPredicate([&Path](const FVerifiedLock& Entry)
			{
				return FPaths::IsSamePath(Entry.RelativePath, Path);
			});
			if (!Lock)
			{
				OutReleasedPaths.Add(Path);
				Pending.RemoveAt(Index);
			}
			else if (!Lock->bOurs || CapturedIds.FindRef(Path) != Lock->Id)
			{
				OutError = FString::Printf(TEXT("解锁后出现不同 ID 或身份的锁，不继续删除：%s"), *Path);
				return false;
			}
		}
	}
	if (!Pending.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Git LFS 解锁经自动重试仍未完成远端确认：%s。%s"), *FString::Join(Pending, TEXT("；")), *LastError);
		return false;
	}
	return true;
}
}
