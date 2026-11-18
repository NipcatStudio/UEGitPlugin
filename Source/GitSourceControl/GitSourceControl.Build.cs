// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

using UnrealBuildTool;

public class GitSourceControl : ModuleRules
{
	public GitSourceControl(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"Core",
				"CoreUObject",
				"Slate",
				"SlateCore",
				"InputCore",
				"DesktopWidgets",
				"EditorStyle",
				"Json",
				"UnrealEd",
				"SourceControl",
				"SourceControlWindows",
				"Projects"
			}
		);

		if (Target.Version.MajorVersion == 5)
		{
			PrivateDependencyModuleNames.Add("ToolMenus");
		}

		bool bUsePreSubmitValidation = Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 7;
		PrivateDefinitions.Add($"UEGIT_WITH_PRE_SUBMIT_VALIDATION={(bUsePreSubmitValidation ? 1 : 0)}");
		if (bUsePreSubmitValidation)
		{
			PrivateDependencyModuleNames.AddRange(new string[] { "AssetRegistry", "DataValidation" });
		}

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			RuntimeDependencies.Add("$(PluginDir)/git-lfs.exe");
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			RuntimeDependencies.Add("$(PluginDir)/git-lfs-mac-amd64");
			RuntimeDependencies.Add("$(PluginDir)/git-lfs-mac-arm64");			
		}
		else if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			RuntimeDependencies.Add("$(PluginDir)/git-lfs");
		}
	}
}
