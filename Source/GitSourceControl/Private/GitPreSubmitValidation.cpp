#if UEGIT_WITH_PRE_SUBMIT_VALIDATION

#include "GitPreSubmitValidation.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "DataValidationChangelist.h"
#include "Editor.h"
#include "EditorValidatorBase.h"
#include "EditorValidatorSubsystem.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlUtils.h"
#include "ISourceControlModule.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DataValidation.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "SourceControlHelpers.h"
#include "Subsystems/EditorSubsystemBlueprintLibrary.h"
#include "UObject/Stack.h"
#include "UObject/StrongObjectPtr.h"

#define LOCTEXT_NAMESPACE "GitPreSubmitValidation"

namespace
{
FDelegateHandle PostEngineInitHandle;
FSourceControlPreSubmitFilesValidationDelegate PreviousFilesValidation;
FSourceControlPreSubmitChangelistValidationDelegate PreviousChangelistValidation;
FDelegateHandle FilesValidationHandle;
FDelegateHandle ChangelistValidationHandle;

bool ReadCommittedDependencies(const FString& GitBinary, const FString& Repository,
	const TArray<FString>& Files, TMap<FString, FGitSourceControlState>& OutCommitted, TArray<FString>& Errors)
{
	TArray<FString> Pathspecs;
	const FString Anchor = FPaths::Combine(Repository, TEXT(".git"));
	for (const FString& File : Files)
	{
		FString Relative = File;
		if (!FPaths::MakePathRelativeTo(Relative, *Anchor))
		{
			return false;
		}
		Pathspecs.Add(TEXT(":(literal)") + Relative);
	}

	TArray<FString> IndexEntries;
	TArray<FString> StatusEntries;
	if (!GitSourceControlUtils::RunCommandWithLiteralPaths(TEXT("ls-files"), GitBinary, Repository,
		{TEXT("-v"), TEXT("--")}, Pathspecs, IndexEntries, Errors)
		|| !GitSourceControlUtils::RunStatusWithLiteralPaths(GitBinary, Repository,
			{TEXT("--porcelain=v1"), TEXT("--no-renames"), TEXT("--untracked-files=all"), TEXT("--")},
			Pathspecs, StatusEntries, Errors))
	{
		return false;
	}

	TSet<FString> ChangedFiles;
	for (const FString& Entry : StatusEntries)
	{
		ChangedFiles.Add(FPaths::ConvertRelativePathToFull(Repository, Entry.Mid(3).TrimQuotes()));
	}

	// 只接受普通跟踪项；不能把被 assume-unchanged 或 skip-worktree 隐藏的改动当成干净。
	// Only ordinary tracked entries qualify; assume-unchanged and skip-worktree can hide changes.
	for (const FString& Entry : IndexEntries)
	{
		if (Entry.StartsWith(TEXT("H "), ESearchCase::CaseSensitive))
		{
			const FString File = FPaths::ConvertRelativePathToFull(Repository, Entry.Mid(2).TrimQuotes());
			if (!ChangedFiles.Contains(File) && FPaths::FileExists(File))
			{
				OutCommitted.Emplace(File, FGitSourceControlState(File));
			}
		}
	}
	return true;
}

EDataValidationResult ValidateGitDependencies(const UDataValidationChangelist& Changelist,
	const FGitSourceControlProvider& Provider, FDataValidationContext& Context)
{
	if (GIsBuildMachine)
	{
		return EDataValidationResult::Valid;
	}

	TSet<TPair<FName, FName>> References;
	for (FName Package : Changelist.ModifiedPackageNames)
	{
		UDataValidationChangelist::GatherDependencies(Package, References);
	}

	TMap<FName, FString> ExternalFiles;
	TMap<FString, TArray<FString>> FilesByRepository;
	bool bInvalid = false;
	for (const TPair<FName, FName>& Reference : References)
	{
		const FName Dependency = Reference.Value;
		if (Changelist.ModifiedPackageNames.Contains(Dependency) || FPackageName::IsVersePackage(Dependency.ToString()))
		{
			continue;
		}
		if (Changelist.DeletedPackageNames.Contains(Dependency))
		{
			Context.AddError(FText::Format(LOCTEXT("DeletedDependency", "{0} is referenced by {1} and cannot be deleted in this submit."),
				FText::FromName(Dependency), FText::FromName(Reference.Key)));
			bInvalid = true;
			continue;
		}
		if (ExternalFiles.Contains(Dependency))
		{
			continue;
		}
		const FString File = USourceControlHelpers::PackageFilename(Dependency.ToString());
		if (FPaths::IsUnderDirectory(File, Provider.GetPathToGitRoot()))
		{
			ExternalFiles.Add(Dependency, File);
			FString ScopedFile = File;
			FString Repository = GitSourceControlUtils::ChangeRepositoryRootIfSubmodule(ScopedFile, Provider.GetPathToGitRoot());
			FilesByRepository.FindOrAdd(MoveTemp(Repository)).AddUnique(File);
		}
	}

	bool bIgnoreOutOfDate = false;
	GConfig->GetBool(TEXT("DataValidationChangelistSettings"), TEXT("bIgnoreOutOfDateDependencies"), bIgnoreOutOfDate, GEditorIni);
	TMap<FString, FGitSourceControlState> Committed;
	for (const TPair<FString, TArray<FString>>& Scope : FilesByRepository)
	{
		TMap<FString, FGitSourceControlState> LocalCommitted;
		TArray<FString> Errors;
		if (!ReadCommittedDependencies(Provider.GetGitBinaryPath(), Scope.Key, Scope.Value, LocalCommitted, Errors))
		{
			Context.AddError(FText::Format(LOCTEXT("GitStatusFailed", "Could not verify Git dependencies in {0}. {1}"),
				FText::FromString(Scope.Key), FText::FromString(FString::Join(Errors, TEXT("\n")))));
			return EDataValidationResult::Invalid;
		}
		if (!bIgnoreOutOfDate && !LocalCommitted.IsEmpty())
		{
			GitSourceControlUtils::CheckRemote(Provider.GetGitBinaryPath(), Scope.Key, Scope.Value, Errors, LocalCommitted);
		}
		Committed.Append(MoveTemp(LocalCommitted));
	}

	for (const TPair<FName, FName>& Reference : References)
	{
		const FString* File = ExternalFiles.Find(Reference.Value);
		if (!File)
		{
			continue;
		}
		if (const FGitSourceControlState* State = Committed.Find(*File))
		{
			if (!bIgnoreOutOfDate && !State->IsCurrent())
			{
				Context.AddWarning(FText::Format(LOCTEXT("NotLatest", "{0} is not at the latest revision (referenced by {1})."),
					FText::FromName(Reference.Value), FText::FromName(Reference.Key)));
			}
		}
		else
		{
			Context.AddError(FText::Format(LOCTEXT("UncommittedDependency",
				"{0} is referenced by {1} but is not a clean, committed file. Include its pending changes or resolve its Git status before submitting."),
				FText::FromName(Reference.Value), FText::FromName(Reference.Key)));
			bInvalid = true;
		}
	}
	return bInvalid ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}

UEditorValidatorSubsystem* GetGitSubmitValidator()
{
	return GEditor ? Cast<UEditorValidatorSubsystem>(UEditorSubsystemBlueprintLibrary::GetEditorSubsystem(
		UEditorValidatorSubsystem::StaticClass())) : nullptr;
}

void ValidateGitSubmit(UDataValidationChangelist* Changelist, const FGitSourceControlProvider& Provider,
	const UEditorValidatorSubsystem& Validator, EDataValidationResult& OutResult, TArray<FText>& Errors, TArray<FText>& Warnings)
{
	if (IAssetRegistry::GetChecked().IsLoadingAssets())
	{
		OutResult = EDataValidationResult::Invalid;
		Errors.Add(LOCTEXT("AssetRegistryScanning", "Asset discovery is in progress. Retry the submit after it finishes."));
		return;
	}

	FDataValidationContext Context(false, EDataValidationUsecase::PreSubmit, {});
	OutResult = ValidateGitDependencies(*Changelist, Provider, Context);
	if (OutResult != EDataValidationResult::Invalid)
	{
		// 引擎的未保存文件和 World Partition 校验只接受原始 Changelist 类型。
		// The engine's dirty-file and World Partition validators require the exact changelist class.
		const FAssetData ChangelistAsset(Changelist);
		Validator.ForEachEnabledValidator([&](UEditorValidatorBase* Entry)
		{
			GInitRunaway();
			OutResult = CombineDataValidationResults(OutResult, Entry->ValidateLoadedAsset(ChangelistAsset, Changelist, Context));
			return true;
		});
	}

	FValidateAssetsSettings Settings;
	Settings.ValidationUsecase = EDataValidationUsecase::PreSubmit;
	Settings.bLoadAssetsForValidation = GetDefault<UDataValidationSettings>()->bLoadAssetsWhenValidatingChangelists;
	Settings.MessageLogPageTitle = LOCTEXT("SubmitValidation", "Git Submit Validation");
	Settings.ShowMessageLogSeverity = { EMessageSeverity::Warning };
	Settings.bCollectPerAssetDetails = true;
	Settings.bShowIfNoFailures = false;
	FValidateAssetsResults Results;
	TSet<FAssetData> Assets;
	Validator.GatherAssetsToValidateFromChangelist(Changelist, Settings, Assets, Context, Results);
	TArray<FAssetData> EligibleAssets;
	for (const FAssetData& Asset : Assets)
	{
		if (Validator.ShouldValidateAsset(Asset, Settings, Context))
		{
			EligibleAssets.Add(Asset);
		}
	}
	Validator.ValidateAssetsWithSettings(EligibleAssets, Settings, Results);
	Context.SplitIssues(Warnings, Errors);
	for (const TPair<FString, FValidateAssetsDetails>& Pair : Results.AssetsDetails)
	{
		Warnings.Append(Pair.Value.ValidationWarnings);
		Errors.Append(Pair.Value.ValidationErrors);
	}
	if (!Errors.IsEmpty() || Results.NumInvalid > 0)
	{
		OutResult = EDataValidationResult::Invalid;
	}
}

const FGitSourceControlProvider* GetActiveGitProvider()
{
	const FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
	return &ISourceControlModule::Get().GetProvider() == &Provider ? &Provider : nullptr;
}

void InstallSubmitValidation()
{
	ISourceControlModule& SourceControl = ISourceControlModule::Get();
	PreviousFilesValidation = SourceControl.GetRegisteredPreSubmitFilesValidation();
	PreviousChangelistValidation = SourceControl.GetRegisteredPreSubmitChangelistValidation();

	const auto FilesValidation = FSourceControlPreSubmitFilesValidationDelegate::CreateLambda(
		[](TArray<FSourceControlStateRef>& States, EDataValidationResult& Result, TArray<FText>& Errors, TArray<FText>& Warnings)
		{
			const FGitSourceControlProvider* Provider = GetActiveGitProvider();
			if (const UEditorValidatorSubsystem* Validator = Provider ? GetGitSubmitValidator() : nullptr)
			{
				TStrongObjectPtr<UDataValidationChangelist> Changelist(NewObject<UDataValidationChangelist>());
				Changelist->Initialize(States);
				ValidateGitSubmit(Changelist.Get(), *Provider, *Validator, Result, Errors, Warnings);
			}
			else
			{
				PreviousFilesValidation.ExecuteIfBound(States, Result, Errors, Warnings);
			}
		});
	const auto ChangelistValidation = FSourceControlPreSubmitChangelistValidationDelegate::CreateLambda(
		[](FSourceControlChangelistPtr Selected, EDataValidationResult& Result, TArray<FText>& Errors, TArray<FText>& Warnings)
		{
			const FGitSourceControlProvider* Provider = GetActiveGitProvider();
			if (const UEditorValidatorSubsystem* Validator = Provider && Selected.IsValid() ? GetGitSubmitValidator() : nullptr)
			{
				TStrongObjectPtr<UDataValidationChangelist> Changelist(NewObject<UDataValidationChangelist>());
				Changelist->Initialize(Selected);
				ValidateGitSubmit(Changelist.Get(), *Provider, *Validator, Result, Errors, Warnings);
			}
			else
			{
				PreviousChangelistValidation.ExecuteIfBound(Selected, Result, Errors, Warnings);
			}
		});
	FilesValidationHandle = FilesValidation.GetHandle();
	ChangelistValidationHandle = ChangelistValidation.GetHandle();
	SourceControl.RegisterPreSubmitFilesValidation(FilesValidation);
	SourceControl.RegisterPreSubmitChangelistValidation(ChangelistValidation);
}
}

void GitPreSubmitValidation::Startup()
{
	if (GetGitSubmitValidator())
	{
		InstallSubmitValidation();
	}
	else if (!IsRunningCommandlet())
	{
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddStatic(&InstallSubmitValidation);
	}
}

void GitPreSubmitValidation::Shutdown()
{
	FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
	if (FModuleManager::Get().IsModuleLoaded(TEXT("SourceControl")))
	{
		ISourceControlModule& SourceControl = ISourceControlModule::Get();
		if (FilesValidationHandle.IsValid() && SourceControl.GetRegisteredPreSubmitFilesValidation().GetHandle() == FilesValidationHandle)
		{
			SourceControl.RegisterPreSubmitFilesValidation(PreviousFilesValidation);
		}
		if (ChangelistValidationHandle.IsValid()
			&& SourceControl.GetRegisteredPreSubmitChangelistValidation().GetHandle() == ChangelistValidationHandle)
		{
			SourceControl.RegisterPreSubmitChangelistValidation(PreviousChangelistValidation);
		}
	}
	PreviousFilesValidation.Unbind();
	PreviousChangelistValidation.Unbind();
}

#undef LOCTEXT_NAMESPACE

#endif
