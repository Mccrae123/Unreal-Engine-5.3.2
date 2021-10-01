// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UnrealUSDWrapper.h"

#include "USDStageOptions.h"

#include "AssetExportTask.h"
#include "Engine/EngineTypes.h"
#include "RHIDefinitions.h"

#include "LevelExporterUSDOptions.generated.h"

USTRUCT( BlueprintType )
struct USDEXPORTER_API FLevelExporterUSDOptionsInner
{
	GENERATED_BODY()

	/** Whether to export only the selected actors, and assets used by them */
    UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Export settings" )
    bool bSelectionOnly;

	/** Whether to bake UE materials and add material bindings to the baked assets */
    UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Export settings" )
    bool bBakeMaterials;

	/** Resolution to use when baking materials into textures */
	UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Export settings", meta = ( EditCondition = "bBakeMaterials", ClampMin = "1" ) )
	FIntPoint BakeResolution = FIntPoint( 512, 512 );

	/** Whether to remove the 'unrealMaterial' attribute after binding the corresponding baked material */
	UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Export settings", meta = ( EditCondition = "bBakeMaterials" ) )
	bool bRemoveUnrealMaterials;

	/** If true, the actual static/skeletal mesh data is exported in "payload" files, and referenced via the payload composition arc */
	UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Export settings" )
	bool bUsePayload;

	/** USD format to use for exported payload files */
	UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Export settings", meta = ( EditCondition = "bUsePayload", GetOptions = GetUsdExtensions ) )
	FString PayloadFormat;

	/** Lowest of the LOD indices to export static and skeletal meshes with (use 0 for maximum detail) */
	UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Export settings", meta = ( ClampMin = "0" ) )
	int32 LowestMeshLOD = 0;

	/** Highest of the LOD indices to export static and skeletal meshes with */
	UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Export settings", meta = ( ClampMin = "0" ) )
	int32 HighestMeshLOD = MAX_MESH_LOD_COUNT - 1;

	/** Whether to use UE actor folders as empty prims */
    UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Export settings" )
    bool bExportActorFolders;

	/** Lowest of the LOD indices to export landscapes with (use 0 for full resolution) */
	UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Export settings", meta = ( ClampMin = "0" ) )
	int32 LowestLandscapeLOD;

	/**
	 * Highest of the LOD indices to export landscapes with. Each value above 0 halves resolution.
	 * The max value depends on the number of components and sections per component of each landscape, and may be clamped.
	 */
	UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Export settings", meta = ( ClampMin = "0" ) )
	int32 HighestLandscapeLOD;

	/** Resolution to use when baking landscape materials into textures  */
	UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Export settings", meta = ( ClampMin = "1" ) )
	FIntPoint LandscapeBakeResolution = FIntPoint( 1024, 1024 );

	/** If true, and if we have a level sequence animating the level during export, it will revert any actor or component to its unanimated state before writing to USD */
	UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Export settings" )
	bool bIgnoreSequencerAnimations = false;

	/** If true, will export sub-levels as separate layers (referenced as sublayers). If false, will collapse all sub-levels in a single exported root layer */
    UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Sublayers" )
    bool bExportSublayers;

	/** Names of levels that should be ignored when collecting actors to export (e.g. "Persistent Level", "Level1", "MySubLevel", etc.) */
    UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Sublayers" )
    TSet<FString> LevelsToIgnore;
};

/**
 * Options for exporting levels to USD format.
 */
UCLASS( Config = Editor, Blueprintable, HideCategories=Hidden )
class USDEXPORTER_API ULevelExporterUSDOptions : public UObject
{
	GENERATED_BODY()

public:
	/** Basic options about the stage to export */
	UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Stage options", meta = ( ShowOnlyInnerProperties ) )
	FUsdStageOptions StageOptions;

	/** StartTimeCode to be used for all exported layers */
    UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Stage options" )
    float StartTimeCode;

	/** EndTimeCode to be used for all exported layers */
    UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Stage options" )
    float EndTimeCode;

	/** Inner struct that actually contains most of the export options */
    UPROPERTY( EditAnywhere, config, BlueprintReadWrite, Category = "Export settings", meta = ( ShowOnlyInnerProperties ) )
    FLevelExporterUSDOptionsInner Inner;

public:
	// We temporarily stash our export task here as a way of passing our options down to
	// the Python exporter, that does the actual level exporting
	UPROPERTY( VisibleAnywhere, BlueprintReadOnly, Category = Hidden )
	UAssetExportTask* CurrentTask;

private:
	UFUNCTION()
	static TArray<FString> GetUsdExtensions()
	{
		TArray<FString> Extensions = UnrealUSDWrapper::GetAllSupportedFileFormats();
		Extensions.Remove( TEXT( "usdz" ) );
		return Extensions;
	}
};
