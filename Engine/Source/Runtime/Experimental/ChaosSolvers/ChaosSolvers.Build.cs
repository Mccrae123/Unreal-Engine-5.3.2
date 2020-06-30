// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
    public class ChaosSolvers : ModuleRules
    {
        public ChaosSolvers(ReadOnlyTargetRules Target) : base(Target)
        {
            PublicIncludePaths.Add("Runtime/Experimental/ChaosSolvers/Public");

            PublicDependencyModuleNames.AddRange(
                new string[] {
                "Core",
                "CoreUObject",
				"Chaos",
				"FieldSystemCore",
				"GeometryCollectionCore",
                "GeometryCollectionSimulationCore"
                }
            ); 

            PublicDefinitions.Add("COMPILE_WITHOUT_UNREAL_SUPPORT=0");
			PrivateDefinitions.Add("CHAOS_INCLUDE_LEVEL_1=1");
		}
    }
}
