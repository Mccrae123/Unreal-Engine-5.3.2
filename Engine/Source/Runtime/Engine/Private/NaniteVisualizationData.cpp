// Copyright Epic Games, Inc. All Rights Reserved.

#include "NaniteVisualizationData.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Misc/ConfigCacheIni.h"

#define LOCTEXT_NAMESPACE "FNaniteVisualizationData"

static FNaniteVisualizationData GNaniteVisualizationData;

// Nanite Visualization Modes (must match NaniteDataDecode.ush)
#define VISUALIZE_OVERVIEW							0
#define VISUALIZE_TRIANGLES							1
#define VISUALIZE_CLUSTERS							2
#define VISUALIZE_PRIMITIVES						3
#define VISUALIZE_INSTANCES							4
#define VISUALIZE_GROUPS							5
#define VISUALIZE_PAGES								6
#define VISUALIZE_OVERDRAW							7
#define VISUALIZE_RASTER_MODE						8
#define VISUALIZE_SCENE_Z_MIN						9
#define VISUALIZE_SCENE_Z_MAX						10
#define VISUALIZE_SCENE_Z_DELTA						11
#define VISUALIZE_MATERIAL_Z_MIN					12
#define VISUALIZE_MATERIAL_Z_MAX					13
#define VISUALIZE_MATERIAL_Z_DELTA					14
#define VISUALIZE_MATERIAL_MODE						15
#define VISUALIZE_MATERIAL_INDEX					16
#define VISUALIZE_MATERIAL_DEPTH					17
#define VISUALIZE_HIT_PROXY_DEPTH					18
#define VISUALIZE_NANITE_MASK						19
#define VISUALIZE_LIGHTMAP_UVS						20
#define VISUALIZE_LIGHTMAP_UV_INDEX					21
#define VISUALIZE_LIGHTMAP_DATA_INDEX				22
#define VISUALIZE_HIERARCHY_OFFSET					23

void FNaniteVisualizationData::Initialize()
{
	if (!bIsInitialized)
	{
		AddVisualizationMode(TEXT("Overview"), LOCTEXT("Overview", "Overview"), FModeType::Overview, VISUALIZE_OVERVIEW);

		AddVisualizationMode(TEXT("Mask"), LOCTEXT("Mask", "Mask"), FModeType::Standard, VISUALIZE_NANITE_MASK);
		AddVisualizationMode(TEXT("Triangles"), LOCTEXT("Triangles", "Triangles"), FModeType::Standard, VISUALIZE_TRIANGLES);
		AddVisualizationMode(TEXT("Clusters"), LOCTEXT("Clusters", "Clusters"), FModeType::Standard, VISUALIZE_CLUSTERS);
		AddVisualizationMode(TEXT("Primitives"), LOCTEXT("Primitives", "Primitives"), FModeType::Standard, VISUALIZE_PRIMITIVES);
		AddVisualizationMode(TEXT("Instances"), LOCTEXT("Instances", "Instances"), FModeType::Standard, VISUALIZE_INSTANCES);
		AddVisualizationMode(TEXT("Overdraw"), LOCTEXT("Overdraw", "Overdraw"), FModeType::Standard, VISUALIZE_OVERDRAW);
		AddVisualizationMode(TEXT("LightmapUV"), LOCTEXT("LightmapUV", "Lightmap UV"), FModeType::Standard, VISUALIZE_LIGHTMAP_UVS);

		AddVisualizationMode(TEXT("Groups"), LOCTEXT("Groups", "Groups"), FModeType::Advanced, VISUALIZE_GROUPS);
		AddVisualizationMode(TEXT("Pages"), LOCTEXT("Pages", "Pages"), FModeType::Advanced, VISUALIZE_PAGES);
		AddVisualizationMode(TEXT("Hierarchy"), LOCTEXT("Hierarchy", "Hierarchy"), FModeType::Advanced, VISUALIZE_HIERARCHY_OFFSET);
		AddVisualizationMode(TEXT("RasterMode"), LOCTEXT("RasterMode", "Raster Mode"), FModeType::Advanced, VISUALIZE_RASTER_MODE);
		AddVisualizationMode(TEXT("SceneZMin"), LOCTEXT("SceneZMin", "Scene Z Min"), FModeType::Advanced, VISUALIZE_SCENE_Z_MIN);
		AddVisualizationMode(TEXT("SceneZMax"), LOCTEXT("SceneZMax", "Scene Z Max"), FModeType::Advanced, VISUALIZE_SCENE_Z_MAX);
		AddVisualizationMode(TEXT("SceneZDelta"), LOCTEXT("SceneZDelta", "Scene Z Delta"), FModeType::Advanced, VISUALIZE_SCENE_Z_DELTA);
		AddVisualizationMode(TEXT("MaterialZMin"), LOCTEXT("MaterialZMin", "Material Z Min"), FModeType::Advanced, VISUALIZE_MATERIAL_Z_MIN);
		AddVisualizationMode(TEXT("MaterialZMax"), LOCTEXT("MaterialZMax", "Material Z Max"), FModeType::Advanced, VISUALIZE_MATERIAL_Z_MAX);
		AddVisualizationMode(TEXT("MaterialZDelta"), LOCTEXT("MaterialZDelta", "Material Z Delta"), FModeType::Advanced, VISUALIZE_MATERIAL_Z_DELTA);
		AddVisualizationMode(TEXT("MaterialMode"), LOCTEXT("MaterialMode", "Material Mode"), FModeType::Advanced, VISUALIZE_MATERIAL_MODE);
		AddVisualizationMode(TEXT("MaterialIndex"), LOCTEXT("MaterialIndex", "Material Index"), FModeType::Advanced, VISUALIZE_MATERIAL_INDEX);
		AddVisualizationMode(TEXT("MaterialDepth"), LOCTEXT("MaterialDepth", "Material Depth"), FModeType::Advanced, VISUALIZE_MATERIAL_DEPTH);
		AddVisualizationMode(TEXT("HitProxyDepth"), LOCTEXT("HitProxyDepth", "Hit Proxy Depth"), FModeType::Advanced, VISUALIZE_HIT_PROXY_DEPTH);
		AddVisualizationMode(TEXT("LightmapUVIndex"), LOCTEXT("LightmapUVIndex", "Lightmap UV Index"), FModeType::Advanced, VISUALIZE_LIGHTMAP_UV_INDEX);
		AddVisualizationMode(TEXT("LightmapDataIndex"), LOCTEXT("LightmapDataIndex", "Lightmap Data Index"), FModeType::Advanced, VISUALIZE_LIGHTMAP_DATA_INDEX);

		ConfigureConsoleCommand();

		bIsInitialized = true;
	}
}

void FNaniteVisualizationData::ConfigureConsoleCommand()
{
	FString AvailableVisualizationModes;
	for (TModeMap::TConstIterator It = ModeMap.CreateConstIterator(); It; ++It)
	{
		const FModeRecord& Record = It.Value();
		AvailableVisualizationModes += FString(TEXT("\n  "));
		AvailableVisualizationModes += Record.ModeString;
	}

	ConsoleDocumentationVisualizationMode = TEXT("When the viewport view-mode is set to 'Nanite Visualization', this command specifies which of the various channels to display. Values entered other than the allowed values shown below will be ignored.");
	ConsoleDocumentationVisualizationMode += AvailableVisualizationModes;

	IConsoleManager::Get().RegisterConsoleVariable(
		GetVisualizeConsoleCommandName(),
		TEXT(""),
		*ConsoleDocumentationVisualizationMode,
		ECVF_Cheat);

	ConsoleDocumentationOverviewTargets = TEXT("Specify the list of modes that can be used in the Nanite visualization overview. Put nothing between the commas to leave a gap.\n\n\tChoose from:\n");
	ConsoleDocumentationOverviewTargets += AvailableVisualizationModes;

	IConsoleManager::Get().RegisterConsoleVariable(
		GetOverviewConsoleCommandName(),
		TEXT("Triangles,Clusters,Primitives,Instances,Mask,Overdraw"),
		*ConsoleDocumentationOverviewTargets,
		ECVF_Default
	);
}

void FNaniteVisualizationData::AddVisualizationMode(
	const TCHAR* ModeString,
	const FText& ModeText,
	const FModeType ModeType,
	uint32 ModeID
)
{
	const FName ModeName = FName(ModeString);

	FModeRecord& Record	= ModeMap.Emplace(ModeName);
	Record.ModeString	= FString(ModeString);
	Record.ModeName		= ModeName;
	Record.ModeText		= ModeText;
	Record.ModeType		= ModeType;
	Record.ModeID		= ModeID;
}

bool FNaniteVisualizationData::IsActive() const
{
	return IsInitialized() && ActiveVisualizationModes.Num() > 0;
}

bool FNaniteVisualizationData::Update(const FName& InViewMode)
{
	if (IsInitialized())
	{
		ActiveVisualizationModes.Reset();

		// First check if overview has a configured mode list
		static IConsoleVariable* ICVarOverview = IConsoleManager::Get().FindConsoleVariable(GetOverviewConsoleCommandName());
		if (ICVarOverview)
		{
			FString OverviewModeList = ICVarOverview->GetString();
			if (IsDifferentToCurrentOverviewModeList(OverviewModeList))
			{
				FString Left, Right;

				// Update our record of the list of modes we've been asked to display
				SetCurrentOverviewModeList(OverviewModeList);
				CurrentOverviewModeNames.Reset();

				// Extract each mode name from the comma separated string
				while (OverviewModeList.Len())
				{
					// Detect last entry in the list
					if (!OverviewModeList.Split(TEXT(","), &Left, &Right))
					{
						Left = OverviewModeList;
						Right = FString();
					}

					// Look up the mode ID for this name
					Left.TrimStartInline();

					const FName ModeName = FName(*Left);
					const int32 ModeID = GetModeID(ModeName);

					if (ModeID == INDEX_NONE)
					{
						UE_LOG(LogNaniteVisualization, Warning, TEXT("Unknown Nanite visualization mode '%s'"), *Left);
					}
					else
					{
						CurrentOverviewModeNames.Emplace(ModeName);
						ActiveVisualizationModes.Add(ModeID);

					}

					OverviewModeList = Right;
				}
			}
		}

	#if 1 // NANITE_VIEW_MODES // TODO: Overview support
		ActiveVisualizationModes.Reset();
	#endif

		// Next check if the console command is set (overrides the editor)
		if (ActiveVisualizationModes.Num() == 0)
		{
			static IConsoleVariable* ICVarVisualize = IConsoleManager::Get().FindConsoleVariable(GetVisualizeConsoleCommandName());
			if (ICVarVisualize)
			{
				const FString ConsoleVisualizationMode = ICVarVisualize->GetString();
				if (!ConsoleVisualizationMode.IsEmpty())
				{
					const FName  ActiveVisualizationName = FName(*ConsoleVisualizationMode);
					const int32 ActiveVisualizationMode = GetModeID(ActiveVisualizationName);
					if (ActiveVisualizationMode == INDEX_NONE)
					{
						UE_LOG(LogNaniteVisualization, Warning, TEXT("Unknown Nanite visualization mode '%s'"), *ConsoleVisualizationMode);
					}
					else
					{
						ActiveVisualizationModes.Add(ActiveVisualizationMode);
					}
				}
			}
		}

		// Finally check the view mode state
		if (ActiveVisualizationModes.Num() == 0 && InViewMode != NAME_None)
		{
			const int32 ActiveVisualizationMode = GetModeID(InViewMode);
			if (ensure(ActiveVisualizationMode != INDEX_NONE))
			{
				ActiveVisualizationModes.Add(ActiveVisualizationMode);
			}
		}
	}

	return IsActive();
}

FText FNaniteVisualizationData::GetModeDisplayName(FName InModeName) const
{
	if (const FModeRecord* Record = ModeMap.Find(InModeName))
	{
		return Record->ModeText;
	}
	else
	{
		return FText::GetEmpty();
	}
}

int32 FNaniteVisualizationData::GetModeID(FName InModeName) const
{
	if (const FModeRecord* Record = ModeMap.Find(InModeName))
	{
		return Record->ModeID;
	}
	else
	{
		return INDEX_NONE;
	}
}

void FNaniteVisualizationData::SetCurrentOverviewModeList(const FString& InNameList)
{
	CurrentOverviewModeList = InNameList;
}

bool FNaniteVisualizationData::IsDifferentToCurrentOverviewModeList(const FString& InNameList)
{
	return InNameList != CurrentOverviewModeList;
}

FNaniteVisualizationData& GetNaniteVisualizationData()
{
	if (!GNaniteVisualizationData.IsInitialized())
	{
		GNaniteVisualizationData.Initialize();
	}

	return GNaniteVisualizationData;
}

#undef LOCTEXT_NAMESPACE
