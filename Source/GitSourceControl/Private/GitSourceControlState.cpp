// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlState.h"

#include "GitSourceControlStyle.h"

#if ENGINE_MAJOR_VERSION >= 5
#include "Textures/SlateIcon.h"
#if ENGINE_MINOR_VERSION >= 2
#include "RevisionControlStyle/RevisionControlStyle.h"
#endif
#endif

#define LOCTEXT_NAMESPACE "GitSourceControl.State"

int32 FGitSourceControlState::GetHistorySize() const
{
	return History.Num();
}

TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlState::GetHistoryItem( int32 HistoryIndex ) const
{
	check(History.IsValidIndex(HistoryIndex));
	return History[HistoryIndex];
}

TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlState::FindHistoryRevision(int32 RevisionNumber) const
{
	for (auto Iter(History.CreateConstIterator()); Iter; Iter++)
	{
		if ((*Iter)->GetRevisionNumber() == RevisionNumber)
		{
			return *Iter;
		}
	}

	return nullptr;
}

TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlState::FindHistoryRevision(const FString& InRevision) const
{
	for (const auto& Revision : History)
	{
		if (Revision->GetRevision() == InRevision)
		{
			return Revision;
		}
	}

	return nullptr;
}

#if ENGINE_MAJOR_VERSION < 5 || ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 3
TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlState::GetBaseRevForMerge() const
{
	for(const auto& Revision : History)
	{
		// look for the the SHA1 id of the file, not the commit id (revision)
		if (Revision->FileHash == PendingMergeBaseFileHash)
		{
			return Revision;
		}
	}

	return nullptr;
}
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlState::GetCurrentRevision() const
{
	return nullptr;
}
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
ISourceControlState::FResolveInfo FGitSourceControlState::GetResolveInfo() const
{
	return PendingResolveInfo;
}
#endif

// @todo add Slate icons for git specific states (NotAtHead vs Conflicted...)

#if ENGINE_MAJOR_VERSION < 5
#define GET_ICON_RETURN( NAME ) FName( "ContentBrowser.SCC_" #NAME )
FName FGitSourceControlState::GetIconName() const
{
#else
#if ENGINE_MINOR_VERSION >= 2
#define GET_ICON_RETURN( NAME ) FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl." #NAME )
#else
#define GET_ICON_RETURN( NAME ) FSlateIcon(FAppStyle::GetAppStyleSetName(), "Perforce." #NAME )
#endif
FSlateIcon FGitSourceControlState::GetIcon() const
{
#endif
	switch (GetGitState())
	{
	case EGitState::NotAtHead:
		return GET_ICON_RETURN(NotAtHeadRevision);
	case EGitState::LockedOther:
		return GET_ICON_RETURN(CheckedOutByOtherUser);
	case EGitState::NotLatest:
		return GET_ICON_RETURN(ModifiedOtherBranch);
	case EGitState::Unmerged:
		return GET_ICON_RETURN(Branched);
	case EGitState::Added:
		return GET_ICON_RETURN(OpenForAdd);
	case EGitState::Untracked:
		return GET_ICON_RETURN(NotInDepot);
	case EGitState::Deleted:
		return GET_ICON_RETURN(MarkedForDelete);
	case EGitState::Modified:
		// UE 5.2+ 提供中性的本地修改图标；绿色勾只表示当前用户实际持锁。
		// UE 5.2+ provides a neutral local-modification icon; reserve the green check for a lock actually owned by the current user.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2
		if (State.LockState != ELockState::Locked)
		{
			return GET_ICON_RETURN(ModifiedLocally);
		}
#endif
		return GET_ICON_RETURN(CheckedOut);
	case EGitState::CheckedOut:
		return GET_ICON_RETURN(CheckedOut);
	case EGitState::CommittedUnpushed:
#if ENGINE_MAJOR_VERSION >= 5
		// Plugin-owned blue up-arrow ("commits waiting to be pushed"). Not the engine padlock:
		// that glyph is the same shape the locked-by-other-user badge uses (only the tint
		// differs), which would make the two states near-indistinguishable at badge size.
		return FSlateIcon(FGitSourceControlStyle::GetStyleSetName(), "GitSourceControl.CommittedUnpushed");
#else
		return GET_ICON_RETURN(CheckedOut);
#endif
	case EGitState::Ignored:
		return GET_ICON_RETURN(NotInDepot);
	default:
#if ENGINE_MAJOR_VERSION < 5
	  return NAME_None;
#else
	  return FSlateIcon();
#endif
	}
}

#if ENGINE_MAJOR_VERSION < 5
FName FGitSourceControlState::GetSmallIconName() const
{
	switch (GetGitState()) {
	case EGitState::NotAtHead:
	  return FName("ContentBrowser.SCC_NotAtHeadRevision_Small");
	case EGitState::LockedOther:
	  return FName("ContentBrowser.SCC_CheckedOutByOtherUser_Small");
	case EGitState::NotLatest:
	  return FName("ContentBrowser.SCC_ModifiedOtherBranch_Small");
	case EGitState::Unmerged:
	  return FName("ContentBrowser.SCC_Branched_Small");
	case EGitState::Added:
	  return FName("ContentBrowser.SCC_OpenForAdd_Small");
	case EGitState::Untracked:
	  return FName("ContentBrowser.SCC_NotInDepot_Small");
	case EGitState::Deleted:
	  return FName("ContentBrowser.SCC_MarkedForDelete_Small");
	case EGitState::Modified:
        case EGitState::CheckedOut:
        case EGitState::CommittedUnpushed:
                return FName("ContentBrowser.SCC_CheckedOut_Small");
	case EGitState::Ignored:
	  return FName("ContentBrowser.SCC_NotInDepot_Small");
	default:
	  return NAME_None;
	}
}
#endif

FText FGitSourceControlState::GetDisplayName() const
{
	switch (GetGitState())
	{
	case EGitState::NotAtHead:
		return LOCTEXT("NotCurrent", "Not current");
	case EGitState::LockedOther:
		return FText::Format(LOCTEXT("CheckedOutOther", "Checked out by: {0}"), FText::FromString(State.LockUser));
	case EGitState::NotLatest:
		return FText::Format(LOCTEXT("ModifiedOtherBranch", "Modified in branch: {0}"), FText::FromString(State.HeadBranch));
	case EGitState::Unmerged:
		return LOCTEXT("Conflicted", "Conflicted");
	case EGitState::Added:
		return LOCTEXT("OpenedForAdd", "Opened for add");
	case EGitState::Untracked:
		return LOCTEXT("NotControlled", "Not Under Revision Control");
	case EGitState::Deleted:
		return LOCTEXT("MarkedForDelete", "Marked for delete");
	case EGitState::Modified:
		if (State.LockState == ELockState::Locked)
		{
			return LOCTEXT("CheckedOutModified", "已 Checkout（本地已修改）");
		}
		if (State.LockState == ELockState::NotLocked)
		{
			return LOCTEXT("ModifiedLocallyNotCheckedOut", "本地已修改，未 Checkout");
		}
		return LOCTEXT("ModifiedLocally", "本地已修改");
	case EGitState::CheckedOut:
		return LOCTEXT("CheckedOut", "已 Checkout");
	case EGitState::CommittedUnpushed:
		return LOCTEXT("CommittedUnpushed", "Committed, not pushed");
	case EGitState::Ignored:
		return LOCTEXT("Ignore", "Ignore");
	case EGitState::Lockable:
		return LOCTEXT("ReadOnly", "Read only");
	case EGitState::None:
		return LOCTEXT("Unknown", "Unknown");
	default:
		return FText();
	}
}

FText FGitSourceControlState::GetDisplayTooltip() const
{
	switch (GetGitState())
	{
	case EGitState::NotAtHead:
		return LOCTEXT("NotCurrent_Tooltip", "The file(s) are not at the head revision");
	case EGitState::LockedOther:
		return FText::Format(LOCTEXT("CheckedOutOther_Tooltip", "Checked out by: {0}"), FText::FromString(State.LockUser));
	case EGitState::NotLatest:
		return FText::Format(LOCTEXT("ModifiedOtherBranch_Tooltip", "Modified in branch: {0} CL:{1} ({2})"), FText::FromString(State.HeadBranch), FText::FromString(HeadCommit), FText::FromString(HeadAction));
	case EGitState::Unmerged:
		return LOCTEXT("ContentsConflict_Tooltip", "The contents of the item conflict with updates received from the repository.");
	case EGitState::Added:
		return LOCTEXT("OpenedForAdd_Tooltip", "The file(s) are opened for add");
	case EGitState::Untracked:
		return LOCTEXT("NotControlled_Tooltip", "Item is not under revision control.");
	case EGitState::Deleted:
		return LOCTEXT("MarkedForDelete_Tooltip", "The file(s) are marked for delete");
	case EGitState::Modified:
		if (State.LockState == ELockState::Locked)
		{
			return LOCTEXT(
				"CheckedOutModified_Tooltip",
				"文件已由当前用户 Checkout，并包含尚未提交的本地修改。");
		}
		if (State.LockState == ELockState::NotLocked)
		{
			return LOCTEXT(
				"ModifiedLocallyNotCheckedOut_Tooltip",
				"文件存在本地修改，但当前没有持有 LFS 锁；可执行 Check Out 补取锁。");
		}
		return LOCTEXT(
			"ModifiedLocally_Tooltip",
			"文件包含尚未提交的本地修改。");
	case EGitState::CheckedOut:
		return LOCTEXT("CheckedOut_Tooltip", "文件已由当前用户 Checkout。");
	case EGitState::CommittedUnpushed:
		return LOCTEXT("CommittedUnpushed_Tooltip", "All local changes are committed and waiting to be pushed; the lock is held until the push lands.");
	case EGitState::Ignored:
		return LOCTEXT("Ignored_Tooltip", "Item is being ignored.");
	case EGitState::Lockable:
		return LOCTEXT("ReadOnly_Tooltip", "The file(s) are marked locally as read-only");
	case EGitState::None:
		return LOCTEXT("Unknown_Tooltip", "Unknown revision control state");
	default:
		return FText();
	}
}

const FString& FGitSourceControlState::GetFilename() const
{
	return LocalFilename;
}

const FDateTime& FGitSourceControlState::GetTimeStamp() const
{
	return TimeStamp;
}

// Deleted and Missing assets cannot appear in the Content Browser, but they do in the Submit files to Revision Control window!
bool FGitSourceControlState::CanCheckIn() const
{
	// We can check in if this is new content
	if (IsAdded())
	{
		// 新增状态不能覆盖未知或他人持锁；只有明确允许本地工作的锁状态可提交。
		// Added content cannot override unknown or foreign ownership; submit requires a workable lock state.
		return State.LockState == ELockState::Locked
			|| State.LockState == ELockState::NotLocked
			|| State.LockState == ELockState::Unlockable;
	}

	// Cannot check back in if conflicted or not current
	if (!IsCurrent() || IsConflicted())
	{
		return false;
	}

	// A clean file whose commits only await push has nothing to check in by itself: the
	// per-asset dialog would collect a commit message that gets discarded and list one asset
	// while the push carries every pending commit anyway. That flow is misleading - pushing
	// is repository-wide, so it lives in the global "Push pending local commits" menu action.
	if (State.RemoteState == ERemoteState::AheadUnpushed && !IsModified())
	{
		return false;
	}

	// We can check back in if we're locked.
	if (State.LockState == ELockState::Locked)
	{
		return true;
	}

	// 已修改文件只接受明确可工作的 NotLocked/Unlockable；未知或他人锁失败关闭。
	// Modified files accept explicitly workable NotLocked/Unlockable states; unknown or foreign locks fail closed.
	if ((State.LockState == ELockState::NotLocked
			|| State.LockState == ELockState::Unlockable)
		&& IsModified()
		&& IsSourceControlled())
	{
		return true;
	}

	return false;
}

bool FGitSourceControlState::CanCheckout() const
{
	if (State.LockState == ELockState::Unlockable)
	{
		// Everything is already available for check in (checked out).
		return false;
	}
	else
	{
		// 只有未锁定且当前版本的受控文件才能取得锁，避免修改过期的二进制内容。
		// Only unlocked, current, controlled files may acquire a lock; stale binary edits risk merge conflicts.
		const bool bCanAcquireOrdinaryLock = State.LockState == ELockState::NotLocked;
		return bCanAcquireOrdinaryLock
			&& IsCurrent()
			&& IsSourceControlled();
	}
}

bool FGitSourceControlState::IsCheckedOut() const
{
	// UE 5.7 同一接口既驱动 Checked Out Filter，又是 SettingsHelpers、Wwise 和删除预处理
	// 判断 no-checkout provider 文件“已打开可编辑”的唯一入口。Locked 仍是远端所有权；
	// Unlockable 只是 UE 兼容投影。LFS lockable 的 NotLocked/离线 Modified 不再伪装为 checkout。
	// UE 5.7 uses this one interface for both the Checked Out filter and the only "opened/editable"
	// signal consumed by SettingsHelpers, Wwise, and delete preprocessing for no-checkout providers.
	// Locked remains remote ownership; Unlockable is an explicit UE adapter. LFS NotLocked states do
	// not masquerade as checkout.
	return HasVerifiedOwnLock()
		|| (State.LockState == ELockState::Unlockable && !IsAdded());
}

bool FGitSourceControlState::HasVerifiedOwnLock() const
{
	return State.LockState == ELockState::Locked;
}

bool FGitSourceControlState::IsCheckedOutOther(FString* Who) const
{
	if (Who != nullptr)
	{
		// The packages dialog uses our lock user regardless if it was locked by other or us.
		// But, if there is no lock user, it shows information about modification in other branches, which is important.
		// So, only show our own lock user if it hasn't been modified in another branch.
		// This is a very, very rare state (maybe impossible), but one that should be displayed properly.
		if (State.LockState == ELockState::LockedOther || (State.LockState == ELockState::Locked && !IsModifiedInOtherBranch()))
		{
			*Who = State.LockUser;
		}
	}
	return State.LockState == ELockState::LockedOther;
}

bool FGitSourceControlState::IsCheckedOutInOtherBranch(const FString& CurrentBranch) const
{
	// You can't check out separately per branch
	return false;
}

bool FGitSourceControlState::IsModifiedInOtherBranch(const FString& CurrentBranch) const
{
	return State.RemoteState == ERemoteState::NotLatest;
}

bool FGitSourceControlState::GetOtherBranchHeadModification(FString& HeadBranchOut, FString& ActionOut, int32& HeadChangeListOut) const
{
	if (!IsModifiedInOtherBranch())
	{
		return false;
	}

	HeadBranchOut = State.HeadBranch;
	ActionOut = HeadAction; // TODO: from ERemoteState
	HeadChangeListOut = 0; // TODO: get head commit
	return true;
}

bool FGitSourceControlState::IsCurrent() const
{
	return State.RemoteState != ERemoteState::NotAtHead && State.RemoteState != ERemoteState::NotLatest;
}

bool FGitSourceControlState::IsSourceControlled() const
{
	return State.TreeState != ETreeState::Untracked && State.TreeState != ETreeState::Ignored && State.TreeState != ETreeState::NotInRepo;
}

bool FGitSourceControlState::IsAdded() const
{
	// Added is when a file was untracked and is now added.
	return State.FileState == EFileState::Added;
}

bool FGitSourceControlState::IsDeleted() const
{
	return State.FileState == EFileState::Deleted;
}

bool FGitSourceControlState::IsIgnored() const
{
	return State.TreeState == ETreeState::Ignored;
}

bool FGitSourceControlState::CanEdit() const
{
	// 编辑能力与远端锁所有权分离：普通 Git 中已跟踪文件无需锁；LFS 中新增文件、当前用户
	// 持锁文件，以及离线 Make Writable 后已经产生的未锁本地修改仍可编辑。
	// Editability is separate from remote lock ownership: tracked files need no lock in ordinary Git,
	// while LFS additions, verified own locks, and already-modified unlocked files created through the
	// offline Make Writable path remain editable.
	if (State.LockState == ELockState::Locked)
	{
		return true;
	}
	if (State.LockState == ELockState::Unlockable)
	{
		return IsSourceControlled();
	}
	if (State.LockState == ELockState::NotLocked)
	{
		return IsAdded() || (IsModified() && IsSourceControlled());
	}
	return false;
}

bool FGitSourceControlState::CanDelete() const
{
	// Perforce enforces that a deleted file must be current.
	if (!IsCurrent())
	{
		return false;
	}
	// If someone else hasn't checked it out, we can delete revision controlled files.
	return !IsCheckedOutOther() && IsSourceControlled();
}

bool FGitSourceControlState::IsUnknown() const
{
	return State.FileState == EFileState::Unknown && State.TreeState == ETreeState::NotInRepo;
}

bool FGitSourceControlState::IsModified() const
{
	return State.TreeState == ETreeState::Working ||
		State.TreeState == ETreeState::Staged;
}


bool FGitSourceControlState::CanAdd() const
{
	return State.TreeState == ETreeState::Untracked;
}

bool FGitSourceControlState::IsConflicted() const
{
	return State.FileState == EFileState::Unmerged;
}

bool FGitSourceControlState::CanRevert() const
{
	// A clean file whose commits only await push has nothing a revert could undo (a revert
	// works on the working tree and cannot touch local commits). The only real effect would
	// be silently dropping our lock while it still protects the unpushed content - so keep
	// Revert disabled for that state; the lock is released by the push.
	if (!IsModified() && State.RemoteState == ERemoteState::AheadUnpushed)
	{
		return false;
	}
	// Can revert the file state if we modified, even if it was locked by someone else.
	// Useful for when someone locked a file, and you just wanna play around with it locallly, and then revert it.
	return CanCheckIn() || IsModified();
}

EGitState::Type FGitSourceControlState::GetGitState() const
{
	// No matter what, we must pull from remote, even if we have locked or if we have modified.
	switch (State.RemoteState)
	{
	case ERemoteState::NotAtHead:
		return EGitState::NotAtHead;
	default:
		break;
	}

	/** Someone else locked this file across branches. */
	// We cannot push under any circumstance, if someone else has locked.
	if (State.LockState == ELockState::LockedOther)
	{
		return EGitState::LockedOther;
	}

	// We could theoretically push, but we shouldn't.
	if (State.RemoteState == ERemoteState::NotLatest)
	{
		return EGitState::NotLatest;
	}

	switch (State.FileState)
	{
	case EFileState::Unmerged:
		return EGitState::Unmerged;
	case EFileState::Added:
		return EGitState::Added;
	case EFileState::Deleted:
		return EGitState::Deleted;
	case EFileState::Modified:
		return EGitState::Modified;
	default:
		break;
	}

	if (State.TreeState == ETreeState::Untracked)
	{
		return EGitState::Untracked;
	}

	if (State.LockState == ELockState::Locked)
	{
		// Clean working tree but carrying commits the remote branch doesn't have: the lock is
		// only held until the push lands - distinguish it from an actively-edited checkout.
		if (State.RemoteState == ERemoteState::AheadUnpushed)
		{
			return EGitState::CommittedUnpushed;
		}
		return EGitState::CheckedOut;
	}

	if (IsSourceControlled())
	{
		if (CanCheckout())
		{
			return EGitState::Lockable;
		}
		return EGitState::Unmodified;
	}

	return EGitState::None;
}

#undef LOCTEXT_NAMESPACE
