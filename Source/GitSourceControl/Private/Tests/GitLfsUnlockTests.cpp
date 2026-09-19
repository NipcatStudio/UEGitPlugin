// Copyright (c) 2026 LunarMaxim
// Distributed under the MIT License (MIT).

#if WITH_DEV_AUTOMATION_TESTS

#include "GitLfsUnlock.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLfsUnlockVerificationTest,
	"GitSourceControl.LfsUnlock.Verification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLfsUnlockVerificationTest::RunTest(const FString& Parameters)
{
	using namespace GitLfsUnlock;
	TArray<FVerifiedLock> Locks;
	FString Error;
	const TArray<FString> Valid{
		TEXT("O Content/测试 资产.uasset  \tSameName \tID:own-id"),
		TEXT("  Content/Other.uasset     \tSameName \tID:other-id")};
	TestTrue(TEXT("完整文本保留 Unicode、空格路径及认证身份"), ParseVerification(true, Valid, Locks, Error));
	if (TestEqual(TEXT("返回两个锁"), Locks.Num(), 2))
	{
		TestTrue(TEXT("O 标记才表示本人持有"), Locks[0].bOurs);
		TestFalse(TEXT("同名他人不获得解锁权限"), Locks[1].bOurs);
		TestEqual(TEXT("去除表格填充，不破坏路径中空格"), Locks[0].RelativePath, FString(TEXT("Content/测试 资产.uasset")));
	}
	TestTrue(TEXT("保留服务器路径的前导空格"), ParseVerification(true,
		{TEXT("O  Content/A.uasset \tUser\tID:leading-space")}, Locks, Error));
	if (TestEqual(TEXT("返回原始路径的锁"), Locks.Num(), 1))
	{
		TestEqual(TEXT("不得把另一个路径重新映射成待解锁资产"), Locks[0].RelativePath, FString(TEXT(" Content/A.uasset")));
	}
	TestFalse(TEXT("非零退出的部分列表不能授权"), ParseVerification(false, Valid, Locks, Error));
	TestEqual(TEXT("失败不泄漏可用于授权的部分记录"), Locks.Num(), 0);
	TestFalse(TEXT("非零退出的空列表不等于无锁"), ParseVerification(false, {}, Locks, Error));
	TestTrue(TEXT("完整成功的空列表可表示无锁"), ParseVerification(true, {}, Locks, Error));
	TestFalse(TEXT("无身份标记不能授权"), ParseVerification(true, {TEXT("Content/A.uasset\tUser\tID:1")}, Locks, Error));
	TestFalse(TEXT("拒绝重复锁 ID"), ParseVerification(true,
		{TEXT("O Content/A.uasset\tUser\tID:1"), TEXT("O Content/B.uasset\tUser\tID:1")}, Locks, Error));
	TestFalse(TEXT("拒绝同一路径的歧义代次"), ParseVerification(true,
		{TEXT("O Content/A.uasset\tUser\tID:1"), TEXT("  Content/A.uasset\tUser\tID:2")}, Locks, Error));
	TestFalse(TEXT("拒绝越出仓库的路径"), ParseVerification(true, {TEXT("O ../A.uasset\tUser\tID:1")}, Locks, Error));
	TestFalse(TEXT("拒绝可破坏命令参数边界的 ID"), ParseVerification(true, {TEXT("O Content/A.uasset\tUser\tID:a\" --force")}, Locks, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLfsUnlockRecoveryTest,
	"GitSourceControl.LfsUnlock.Recovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLfsUnlockRecoveryTest::RunTest(const FString& Parameters)
{
	using namespace GitLfsUnlock;
	// 单文件恢复用例保留逐 ID 断言，批次调度另由 Batch 用例验证。
	// Preserve per-ID assertions here; the Batch test separately covers batch dispatch.
	auto Release = [](const TArray<FString>& Paths,
		TFunctionRef<bool(TArray<FVerifiedLock>&, FString&)> Query,
		TFunctionRef<bool(const FVerifiedLock&, FString&)> Unlock,
		TFunctionRef<bool()> IsCurrent, TArray<FString>& ReleasedPaths, FString& Detail)
	{
		return GitLfsUnlock::Release(Paths, Query,
			[&](const TArray<FVerifiedLock>& Batch, FString& Error)
			{
				check(Batch.Num() == 1);
				return Unlock(Batch[0], Error);
			}, IsCurrent, ReleasedPaths, Detail);
	};
	const FString Path = TEXT("Content/A.uasset");
	const FVerifiedLock Original{TEXT("original-id"), Path, true};
	TArray<FString> Released;
	FString Error;
	int32 Queries = 0;
	int32 Writes = 0;
	bool bExists = true;
	TestTrue(TEXT("一次查询错误和一次删除错误后，在同一操作内实际恢复成功"), Release({Path},
		[&](TArray<FVerifiedLock>& Out, FString& Detail)
		{
			Out.Reset();
			if (++Queries == 1) { Detail = TEXT("temporary query failure"); return false; }
			if (bExists) { Out.Add(Original); }
			return true;
		},
		[&](const FVerifiedLock& Lock, FString& Detail)
		{
			TestEqual(TEXT("重试锁定首次认证 ID"), Lock.Id, Original.Id);
			if (++Writes == 1) { Detail = TEXT("temporary unlock failure"); return false; }
			bExists = false;
			return true;
		}, []() { return true; }, Released, Error));
	TestEqual(TEXT("完成真正的第二次删除尝试"), Writes, 2);
	TestTrue(TEXT("只有确认释放的路径可以清缓存"), Released.Contains(Path));

	Writes = 0;
	bExists = true;
	TestTrue(TEXT("请求超时但服务器已释放时，通过读回完成而不重复删除"), Release({Path},
		[&](TArray<FVerifiedLock>& Out, FString&) { Out.Reset(); if (bExists) { Out.Add(Original); } return true; },
		[&](const FVerifiedLock&, FString&) { ++Writes; bExists = false; return false; },
		[]() { return true; }, Released, Error));
	TestEqual(TEXT("不重复已经完成的删除"), Writes, 1);

	Writes = 0;
	TestFalse(TEXT("一直无法核验不允许伪造成功"), Release({Path},
		[](TArray<FVerifiedLock>& Out, FString&) { Out.Reset(); return false; },
		[&](const FVerifiedLock&, FString&) { ++Writes; return true; },
		[]() { return true; }, Released, Error));
	TestEqual(TEXT("无身份核验不删除任何锁"), Writes, 0);
	TestEqual(TEXT("失败的空输出不清缓存"), Released.Num(), 0);

	Writes = 0;
	TestFalse(TEXT("命令声称成功但服务器仍持锁时，不接受假成功"), Release({Path},
		[&](TArray<FVerifiedLock>& Out, FString&) { Out = {Original}; return true; },
		[&](const FVerifiedLock&, FString&) { ++Writes; return true; },
		[]() { return true; }, Released, Error));
	TestEqual(TEXT("自动恢复有界，不无限阻塞 Editor"), Writes, 3);
	TestEqual(TEXT("仍被服务器报告的锁不清缓存"), Released.Num(), 0);

	Writes = 0;
	TestTrue(TEXT("本地 Revert 保留同名但认证为他人的锁"), Release({Path},
		[&](TArray<FVerifiedLock>& Out, FString&) { Out = {{TEXT("foreign-id"), Path, false}}; return true; },
		[&](const FVerifiedLock&, FString&) { ++Writes; return true; },
		[]() { return true; }, Released, Error));
	TestEqual(TEXT("不强制解他人的锁"), Writes, 0);
	TestEqual(TEXT("不把他人锁伪报为已释放"), Released.Num(), 0);

	Writes = 0;
	TestFalse(TEXT("同路径出现不同 ID 时，不继续追逐删除"), Release({Path},
		[&](TArray<FVerifiedLock>& Out, FString&) { Out = {{Writes == 0 ? TEXT("original-id") : TEXT("new-id"), Path, true}}; return true; },
		[&](const FVerifiedLock&, FString&) { ++Writes; return true; },
		[]() { return true; }, Released, Error));
	TestEqual(TEXT("不得删除不同 ID"), Writes, 1);

	Writes = 0;
	int32 Checks = 0;
	TestFalse(TEXT("查询后配置切换，发送删除前必须再次拒绝"), Release({Path},
		[&](TArray<FVerifiedLock>& Out, FString&) { Out = {Original}; return true; },
		[&](const FVerifiedLock&, FString&) { ++Writes; return true; },
		[&]() { return ++Checks == 1; }, Released, Error));
	TestEqual(TEXT("过期上下文不产生删除"), Writes, 0);

	bool bCurrent = true;
	TestFalse(TEXT("首次空查询期间上下文失效，不得伪造已解锁"), Release({Path},
		[&](TArray<FVerifiedLock>& Out, FString&) { Out.Reset(); bCurrent = false; return true; },
		[&](const FVerifiedLock&, FString&) { ++Writes; return true; },
		[&]() { return bCurrent; }, Released, Error));
	TestEqual(TEXT("失效查询不得清缓存"), Released.Num(), 0);

	bCurrent = true;
	bExists = true;
	TestFalse(TEXT("删除后的空查询期间上下文失效，也不得确认终态"), Release({Path},
		[&](TArray<FVerifiedLock>& Out, FString&)
		{
			Out.Reset();
			if (bExists) { Out.Add(Original); } else { bCurrent = false; }
			return true;
		},
		[&](const FVerifiedLock&, FString&) { bExists = false; return true; },
		[&]() { return bCurrent; }, Released, Error));
	TestEqual(TEXT("失效读回不得清缓存"), Released.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLfsUnlockBatchTest,
	"GitSourceControl.LfsUnlock.Batch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitLfsUnlockBatchTest::RunTest(const FString& Parameters)
{
	using namespace GitLfsUnlock;
	TArray<FString> Paths;
	TArray<FVerifiedLock> Original;
	for (int32 Index = 0; Index < 6; ++Index)
	{
		Paths.Add(FString::Printf(TEXT("Content/Asset%d.uasset"), Index));
		Original.Add({FString::Printf(TEXT("id-%d"), Index), Paths.Last(), true});
	}
	TArray<FVerifiedLock> Remaining = Original;
	TArray<FString> Released;
	TArray<int32> BatchSizes;
	FString Error;
	int32 Queries = 0;
	auto Query = [&](TArray<FVerifiedLock>& Out, FString&)
	{
		++Queries;
		Out = Remaining;
		return true;
	};
	TestTrue(TEXT("六个文件共享前后核验，分为四个和两个 ID 的请求批次"), Release(Paths, Query,
		[&](const TArray<FVerifiedLock>& Batch, FString&)
		{
			BatchSizes.Add(Batch.Num());
			for (const FVerifiedLock& Lock : Batch)
			{
				Remaining.RemoveAll([&](const FVerifiedLock& Entry) { return Entry.Id == Lock.Id; });
			}
			return true;
		}, []() { return true; }, Released, Error));
	TestEqual(TEXT("六个资产只需两次核验"), Queries, 2);
	TestTrue(TEXT("请求进程数由六个降为两个"), BatchSizes == TArray<int32>{4, 2});
	TestEqual(TEXT("六个文件均经读回确认"), Released.Num(), 6);

	Remaining = Original;
	Queries = 0;
	TMap<FString, int32> Writes;
	TestFalse(TEXT("部分失败只重试未完成 ID，不重放已释放文件"), Release(Paths, Query,
		[&](const TArray<FVerifiedLock>& Batch, FString&)
		{
			for (const FVerifiedLock& Lock : Batch)
			{
				++Writes.FindOrAdd(Lock.Id);
				if (Lock.Id != TEXT("id-0"))
				{
					Remaining.RemoveAll([&](const FVerifiedLock& Entry) { return Entry.Id == Lock.Id; });
				}
			}
			return true;
		}, []() { return true; }, Released, Error));
	TestEqual(TEXT("重试复用读回，不重复查询整仓库"), Queries, 4);
	TestEqual(TEXT("只剩一个未确认文件"), Released.Num(), 5);
	TestTrue(TEXT("失败明细包含未确认文件"), Error.Contains(Paths[0]));
	for (int32 Index = 0; Index < 6; ++Index)
	{
		TestEqual(TEXT("只有未完成 ID 被重试"), Writes.FindRef(Original[Index].Id), Index == 0 ? 3 : 1);
	}

	Remaining = Original;
	int32 Batches = 0;
	bool bCurrent = true;
	TestFalse(TEXT("批次间上下文变化必须阻止后续请求"), Release(Paths, Query,
		[&](const TArray<FVerifiedLock>&, FString&) { ++Batches; bCurrent = false; return true; },
		[&]() { return bCurrent; }, Released, Error));
	TestEqual(TEXT("上下文失效后未发出第二批"), Batches, 1);
	TestEqual(TEXT("未读回的请求不算已确认释放"), Released.Num(), 0);
	return true;
}

#endif
