// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.IO;

namespace UnrealBuildTool.Rules
{
	public class USDSchemas : ModuleRules
	{
		public USDSchemas(ReadOnlyTargetRules Target) : base(Target)
		{
			bUseRTTI = true;

			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"Boost",
					"CinematicCamera",
					"Core",
					"CoreUObject",
					"Engine",
					"GeometryCache",
					"LiveLinkAnimationCore",
					"LiveLinkComponents",
					"LiveLinkInterface",
					"MeshDescription",
					"RenderCore",
					"RHI", // For FMaterialUpdateContext and the right way of updating material instance constants
					"Slate",
					"SlateCore",
					"StaticMeshDescription",
					"UnrealUSDWrapper",
					"USDClasses",
					"USDUtilities",
				}
			);

			if (Target.bBuildEditor)
			{
				PrivateDependencyModuleNames.AddRange(
					new string[]
					{
						"BlueprintGraph",
						"GeometryCacheUSD",
						"Kismet",
						"LiveLinkGraphNode",
						"MaterialEditor",
						"MDLImporter",
						"MeshUtilities",
						"PropertyEditor",
						"UnrealEd",
					}
				);
			}
		}
	}
}
