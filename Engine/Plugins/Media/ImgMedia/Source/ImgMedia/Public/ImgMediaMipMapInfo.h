// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IMediaOptions.h"
#include "Tickable.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

class AActor;
class UMediaTexture;

/**
 * Holds info on a camera which we can use for mipmap calculations.
 */
struct FImgMediaMipMapCameraInfo
{
	FImgMediaMipMapCameraInfo(const FVector& InLocation,
		const FMatrix& InViewMatrix,
		float InScreenSize, float InDistAdjust)
		: Location(InLocation)
		, ViewMatrix(InViewMatrix)
		, ScreenSize(InScreenSize)
		, DistAdjust(InDistAdjust)
	{
	}

	/** Position of camera. */
	FVector Location;
	/** View projection matrix of camera. */
	FMatrix ViewMatrix;

	/** Size of screen. */
	float ScreenSize;
	/** Adjustment needed to mip level distance calculations due to camera FOV, etc. */
	float DistAdjust;
};

/** 
 * Describes a single object which is using our img sequence.
 */
struct FImgMediaMipMapObjectInfo
{
	/** Actor that is using our img sequence. */
	TWeakObjectPtr<class AActor> Object;
	/** Width of this object. */
	float Width;
	/** Height of this object. */
	float Height;
	/** LOD bias for the mipmap level. */
	float LODBias;
	/** Multiplier to apply to the distance to account for this object (e.g. its size). */
	float DistAdjust;
};

/**
 * Describes which tiles are visible.
 */
struct FImgMediaTileSelection
{
	/** X position of the most top left visibile tile. */
	uint16 TopLeftX;
	/** Y position of the most top left visible tile. */
	uint16 TopLeftY;
	/**
	 * X position of the most bottom right visible tile + 1.
	 * If this is equal to TopLeftX, then this implies that the tile is not visible.
	 */
	uint16 BottomRightX;
	/** Y position of the most bottom right visible tile + 1. */
	uint16 BottomRightY;

	FImgMediaTileSelection()
	{
		SetAllVisible();
	}

	/**
	 * Marks all tiles as visible.
	 */
	void SetAllVisible()
	{
		TopLeftX = 0;
		TopLeftY = 0;
		BottomRightX = 0xffff;
		BottomRightY = 0xffff;
	}

	/**
	 * Marks all tiles as not visible.
	 */
	void SetAllNotVisible()
	{
		TopLeftX = 0xffff;
		TopLeftY = 0xffff;
		BottomRightX = 0;
		BottomRightY = 0;
	}

	/**
	* See if this selection is visible.
	*
	* @return True if so.
	*/
	bool IsVisible() const { return (TopLeftX < BottomRightX); };


	/**
	 * Include a given tile coordinate to the current section region.
	 *
	 * @param TileCoordX Horizontal tile coordinate.
	 * @param TileCoordY Vertical tile coordinate.
	 */
	void Include(uint16 TileCoordX, uint16 TileCoordY)
	{
		TopLeftX = FMath::Min(TopLeftX, TileCoordX);
		TopLeftY = FMath::Min(TopLeftY, TileCoordY);
		BottomRightX = FMath::Max(BottomRightX, uint16(TileCoordX + 1u));
		BottomRightY = FMath::Max(BottomRightY, uint16(TileCoordY + 1u));
	}

	/**
	 * Check if the current selection contains a tile.
	 *
	 * @param TileCoordX Horizontal tile coordinate.
	 * @param TileCoordY Vertical tile coordinate.
	 * @return True if the coordinate is contained withing bounds, false otherwise.
	 */
	bool Contains(uint16 TileCoordX, uint16 TileCoordY) const
	{
		return TopLeftX <= TileCoordX &&
			TopLeftY <= TileCoordY &&
			BottomRightX > TileCoordX &&
			BottomRightY > TileCoordY;
	}

	/**
	 * Check if the current selection contains another selection within its bounds.
	 *
	 * @param Other Selection to compare.
	 * @return True if the other selection is contained withing bounds, false otherwise.
	 */
	bool Contains(const FImgMediaTileSelection& Other) const
	{
		return TopLeftX <= Other.TopLeftX &&
			TopLeftY <= Other.TopLeftY &&
			BottomRightX >= Other.BottomRightX &&
			BottomRightY >= Other.BottomRightY;
	}
};

/**
 * Contains information for working with mip maps.
 */
class FImgMediaMipMapInfo : public IMediaOptions::FDataContainer, public FTickableGameObject
{
public:
	FImgMediaMipMapInfo();
	virtual ~FImgMediaMipMapInfo();

	/**
	 * This object is using our img sequence.
	 *
	 * @param InActor Object using our img sequence.
	 * @param Width Width of the object. If < 0, then get the width automatically.
	 */
	void AddObject(AActor* InActor, float Width, float LODBias);

	/**
	 * This object is no longer using our img sequence.
	 *
	 * @param InActor Object no longer using our img sequence.
	 */
	void RemoveObject(AActor* InActor);

	/**
	 * All the objets that are using this media texture will be used in our mip map calculations.
	 *
	 * @param InMediaTexture Media texture to get objects from.
	 */
	void AddObjectsUsingThisMediaTexture(UMediaTexture* InMediaTexture);

	/**
	 * Remove all objects from consideration.
	 */
	void ClearAllObjects();

	/**
	 * Get our mip level distances.
	 * @return Distances
	 */
	const TArray<float>& GetMipLevelDistances() const { return MipLevelDistances; }

	/**
	 * Manually set when mip level 0 should appear.
	 *
	 * @param Distance Furthest distance from the camera when mip level 0 should be at 100%.
	 */
	void SetMipLevelDistance(float Distance);

	/**
	 * Provide information on the texture needed for our image sequence.
	 *
	 * @param InSequenceName Name of this sequence.
	 * @param NumMipMaps Number of mipmaps in our image sequence.
	 * @param Dim Dimensions of the textures in our image sequence.
	 */
	void SetTextureInfo(FName InSequenceName, int32 NumMipMaps, int32 InNumTilesX, int32 InNumTilesY,
		const FIntPoint& Dim);

	/**
	 * Get what mipmap level should be used.
	 * Returns the lowest level (highest resolution) mipmap.
	 * Assumes all higher levels will also be used.
	 *
	 * @param TileSelection Will be set with which tiles are visible.
	 * @return Mipmap level.
	 */
	int32 GetDesiredMipLevel(FImgMediaTileSelection& InTileSelection);

	/**
	 * Calculate object distance to camera. 
	 * 
	 * @param InCameraLocation Postion of camera.
	 * @param InObjectLocation Position of object.
	 * @return Distance.
	 */
	IMGMEDIA_API static float GetObjectDistToCamera(const FVector& InCameraLocation, const FVector& InObjectLocation);
	
	/**
	 * Determine which mip level to use for a given distance.
	 *
	 * @param InDistance Distance to use.
	 * @param InMipLevelDistances Distances for each mip level.
	 * @return Mip level.
	 */
	IMGMEDIA_API static int GetMipLevelForDistance(float InDistance, const TArray<float>& InMipLevelDistances);

	/**
	 * Determine the size of an object.
	 *
	 * @param InActor Object to get the size of.
	 * @param Width		Will be set to the width of the object.
	 * @param Height	Will be set to the height of the object.
	 */
	void GetObjectSize(const AActor* InActor, float& Width, float& Height);
	
	/**
	 * Get information on all our cameras.
	 *
	 * @return Array of cameras.
	 */
	const TArray<FImgMediaMipMapCameraInfo>& GetCameraInfo() const { return CameraInfos; }

	/**
	 * Get adjustment needed for distance to take the viewport size into account compared
	 * to the reference viewport
	 *
	 * @return Adjustment factor.
	 */
	float GetViewportDistAdjust() const { return ViewportDistAdjust; }

	/**
	 * Get information on objects that are using our textures.
	 * 
	 * @return Array of objects.
	 */
	const TArray<FImgMediaMipMapObjectInfo*>& GetObjects() const { return Objects; }

	//~ FTickableGameObject interface
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(FImgMediaMipMapInfo, STATGROUP_Tickables); }
	virtual bool IsTickableInEditor() const override { return true; }

protected:
	/**
	 * Performs mipmap calculations and caches the data.
	 */
	void UpdateMipLevelCache();

	/** Decide which tiles are visible and update CachedTileSelection.
	 *
	 * @param CameraInfo				View info to compute visibility from.
	 * @param ObjectLocation			Position of object.
	 * @param NextTileXVector			Vector to go from one tile to the next in the X axis.
	 * @param NextTileYVector			Vector to go from one tile to the next in the Y axis.
	 * @param TileRadiusInWorldSpace	Radius of a bounding sphere for a tile.
	 */
	void CalculateTileVisibility(const FImgMediaMipMapCameraInfo& CameraInfo,
		const FVector& ObjectLocation, const FVector& NextTileXVector,
		const FVector& NextTileYVector, float TileRadiusInWorldSpace);


	/**
	 * Updates the MipLevelDistances based on current information.
	 */
	void UpdateMipLevelDistances();

	/** Name of this sequence. */
	FName SequenceName;
	/** Number of tiles in the X direction. */
	int32 NumTilesX;
	/** Number of tiles in the Y direction. */
	int32 NumTilesY;

	/** Ideal distance for mip level 0. */
	float MipLevel0Distance;
	/** True if MipLevel0Distance has been set manually. */
	bool bIsMipLevel0DistanceSetManually;

	/** Ideal distances for all mip maps. */
	TArray<float> MipLevelDistances;

	/** Array of objects that are using our img sequence. */
	TArray<FImgMediaMipMapObjectInfo*> Objects;

	/** Adjustment for current size of viewport, used in mipmap calculations. */
	float ViewportDistAdjust;
	/** Info for each camera, used in mipmap calculations. */
	TArray<FImgMediaMipMapCameraInfo> CameraInfos;
	
	/** Protects info variables. */
	FCriticalSection InfoCriticalSection;

	/** Desired mipmap level at this current time. */
	int32 CachedMipLevel;
	/** Desired tiles at this current tmie. */
	FImgMediaTileSelection CachedTileSelection;
	/** True if the cached mipmap data has been calculated this frame. */
	bool bIsCachedMipLevelValid;
};
