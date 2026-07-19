// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"

/**
 * Plugin-owned slate brushes for states the engine's RevisionControl style set has no
 * suitable icon for (e.g. "committed, not pushed" - every engine padlock brush is already
 * taken by the locked-by-other-user badge).
 */
class FGitSourceControlStyle
{
public:
	static void Initialize();
	static void Shutdown();

	static FName GetStyleSetName();

private:
	static TSharedPtr<class FSlateStyleSet> StyleInstance;
};
