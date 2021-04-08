// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.IO;

namespace UnrealBuildTool.Rules
{
	public class USDExporter : ModuleRules
	{
		public USDExporter(ReadOnlyTargetRules Target) : base(Target)
		{
			PublicDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
					"CoreUObject",
					"Engine",
					"UnrealUSDWrapper"
				}
			);

			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"EditorFramework",
					"EditorStyle",
					"GeometryCache",
					"PropertyEditor",
					"RawMesh",
					"RenderCore",
					"RHI",
					"InputCore",
					"JsonUtilities",
					"LevelSequence",
					"MaterialBaking", // So that we can use some of the export option properties
					"MeshDescription",
					"MeshUtilities",
					"MessageLog",
					"PythonScriptPlugin",
					"Slate",
					"SlateCore",
					"UnrealEd",
					"USDClasses",
					"USDStageImporter", // For USDOptionsWindow
					"USDUtilities",
                }
			);
		}
	}
}
