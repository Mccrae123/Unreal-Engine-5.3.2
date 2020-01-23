// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.IO;

namespace UnrealBuildTool.Rules
{
	public class USDStage : ModuleRules
	{
		public USDStage(ReadOnlyTargetRules Target) : base(Target)
		{
			bUseRTTI = true;

			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"CinematicCamera",
					"Core",
					"CoreUObject",
					"EditorStyle",
					"Engine",
					"GeometryCache",
					"InputCore",
					"JsonUtilities",
					"LevelSequence",
					"MaterialEditor",
					"MeshDescription",
					"MeshUtilities",
					"MessageLog",
					"MovieScene",
					"MovieSceneTracks",
					"PropertyEditor",
					"PythonScriptPlugin",
					"RHI",
					"RenderCore",
					"Slate",
					"SlateCore",
					"StaticMeshDescription",
					"USDImporter",
					"USDSchemas",
					"USDUtilities",
					"UnrealEd",
				}
				);

			if (Target.Platform == UnrealTargetPlatform.Win64)
			{
				PrivateDependencyModuleNames.Add("UnrealUSDWrapper");
			}
		}
	}
}
