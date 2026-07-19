// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlStyle.h"

#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleRegistry.h"
#include "Brushes/SlateImageBrush.h"

TSharedPtr<FSlateStyleSet> FGitSourceControlStyle::StyleInstance = nullptr;

FName FGitSourceControlStyle::GetStyleSetName()
{
	static FName StyleSetName(TEXT("GitSourceControlStyle"));
	return StyleSetName;
}

void FGitSourceControlStyle::Initialize()
{
	if (StyleInstance.IsValid())
	{
		return;
	}

	StyleInstance = MakeShared<FSlateStyleSet>(GetStyleSetName());
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GitSourceControl"));
	if (Plugin.IsValid())
	{
		StyleInstance->SetContentRoot(Plugin->GetBaseDir() / TEXT("Resources"));
	}

	// Blue up-arrow into a bar: local commits waiting to reach the remote - the vertical
	// mirror of the engine's yellow down-arrow NotAtHeadRevision ("newer version on server")
	// badge, in the same pending-blue as its OpenForAdd badge.
	StyleInstance->Set(TEXT("GitSourceControl.CommittedUnpushed"),
		new FSlateVectorImageBrush(StyleInstance->RootToContentDir(TEXT("CommittedUnpushed_16"), TEXT(".svg")),
			FVector2D(16.0f, 16.0f), FLinearColor::FromSRGBColor(FColor::FromHex("#0070E0"))));

	FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
}

void FGitSourceControlStyle::Shutdown()
{
	if (StyleInstance.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
		StyleInstance.Reset();
	}
}
