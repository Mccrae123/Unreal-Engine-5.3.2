// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MLDeformer.h"
#include "MLDeformerAsset.h"

#include "Math/Vector.h"
#include "Math/Transform.h"
#include "Containers/Array.h"
#include "UObject/ObjectPtr.h"
#include "UObject/NameTypes.h"

class UDebugSkelMeshComponent;
class UGeometryCacheComponent;
class UAnimSequence;
class USkeletalMesh;
class UGeometryCache;
class UWorld;
class AActor;
class FMLDeformerSampler;
class FMLDeformerFrameCache;

/**
 * A sampler data object, which is basically a set of data that is used to generate training data for a given frame.
 */
class FMLDeformerSamplerData
{
public:
	struct FInitSettings
	{
		FMLDeformerSampler* Sampler = nullptr;
		UDebugSkelMeshComponent* SkeletalMeshComponent = nullptr;
		UGeometryCacheComponent* GeometryCacheComponent = nullptr;
		int32 NumImportedVertices = -1;
	};

	void Init(const FInitSettings& InitSettings);
	void Update(int32 AnimFrameIndex);

	const FMLDeformerSampler& GetSampler() const { return *Sampler; }

	UDebugSkelMeshComponent* GetSkeletalMeshComponent() const { return SkeletalMeshComponent; }
	UGeometryCacheComponent* GetGeometryCacheComponent() const { return GeometryCacheComponent; }

	const TArray<FVector3f>& GetSkinnedVertexPositions() const { return SkinnedVertexPositions; }
	const TArray<FVector3f>& GetGeometryCachePositions() const { return GeomCacheVertexPositions; }

	const TArray<float>& GetVertexDeltas() const { return VertexDeltas; }
	const TArray<float>& GetBoneRotations() const { return BoneRotations; }
	const TArray<float>& GetCurveValues() const { return CurveValues; }

	int32 GetNumImportedVertices() const { return NumImportedVertices; }
	int32 GetNumBones() const;

	SIZE_T CalcMemUsageInBytes() const;

protected:
	void ExtractSkinnedPositions(int32 LODIndex, TArray<FMatrix44f>& InBoneMatrices, TArray<FVector3f>& TempPositions, TArray<FVector3f>& OutPositions) const;
	void ExtractGeomCachePositions(int32 LODIndex, TArray<FVector3f>& TempPositions, TArray<FVector3f>& OutPositions) const;
	void CalculateVertexDeltas(const TArray<FVector3f>& SkinnedPositions, const TArray<FVector3f>& GeomCachePositions, float DeltaCutoffLength, TArray<float>& OutVertexDeltas);

protected:
	FMLDeformerSampler* Sampler = nullptr;
	TObjectPtr<UDebugSkelMeshComponent>	SkeletalMeshComponent = nullptr;
	TObjectPtr<UGeometryCacheComponent> GeometryCacheComponent = nullptr;
	TArray<FVector3f> SkinnedVertexPositions;
	TArray<FVector3f> TempVertexPositions;
	TArray<FVector3f> GeomCacheVertexPositions;
	TArray<FMatrix44f> BoneMatrices;
	TArray<float> VertexDeltas;	// (NumImportedVerts * 3) -> xyz
	TArray<float> BoneRotations; // (NumBones * 4) -> quat xyzw
	TArray<float> CurveValues;
	int32 NumImportedVertices = 0;
	int32 AnimFrameIndex = -1;
};

/**
 * The training data sampler, which is used to generate training data.
 */
class FMLDeformerSampler
{
public:
	struct FInitSettings
	{
		UWorld* World = nullptr;
		TObjectPtr<UMLDeformerAsset> DeformerAsset = nullptr;
	};

	FMLDeformerSampler() = default;
	FMLDeformerSampler(const FMLDeformerSampler&) = delete;
	FMLDeformerSampler(FMLDeformerSampler&&) = delete;
	~FMLDeformerSampler();

	FMLDeformerSampler& operator = (const FMLDeformerSampler&) = delete;
	FMLDeformerSampler& operator = (FMLDeformerSampler&&) = delete;

	void Init(const FInitSettings& InInitSetings);

	FMLDeformerSamplerData& GetSamplerData() { return SamplerData; }
	const FMLDeformerSamplerData& GetSamplerData() const { return SamplerData; }

	int32 GetNumVertices() const;
	int32 GetNumBones() const;
	int32 GetNumCurves() const;
	int32 GetNumFrames() const;

	const FInitSettings& GetInitSettings() const { return InitSettings; }
	const UMLDeformerAsset& GetDeformerAsset() const { return *InitSettings.DeformerAsset.Get(); }
	UMLDeformerAsset& GetDeformerAsset() { return *InitSettings.DeformerAsset.Get(); }

	SIZE_T CalcMemUsageInBytes() const;

protected:
	AActor* CreateActor(UWorld* InWorld, const FName& Name) const;
	int32 ExtractNumImportedSkinnedVertices(USkeletalMesh* SkeletalMesh) const;
	int32 ExtractNumImportedGeomCacheVertices(UGeometryCache* GeomCache) const;

protected:
	TObjectPtr<AActor> SkelMeshActor = nullptr;
	TObjectPtr<AActor> GeomCacheActor = nullptr;
	FMLDeformerSamplerData SamplerData;
	FInitSettings InitSettings;
};


/**
 * The training data for a given frame.
 * Unlike the sampler frame, this contains only the data used during training and not all temp buffers used to generate this data.
 * This data is already prepared to be passed directly to Python.
 */
class FMLDeformerTrainingFrame
{
public:
	void Clear();
	void InitFromSamplerItem(int32 InAnimFrameIndex, const FMLDeformerSamplerData& InSamplerData);

	const TArray<float>& GetVertexDeltas() const { return VertexDeltas; }
	const TArray<float>& GetBoneRotations() const { return BoneRotations; }
	const TArray<float>& GetCurveValues() const { return CurveValues; }
	int32 GetAnimFrameIndex() const { return AnimFrameIndex; }
	int32 GetNumVertices() const { return VertexDeltas.Num() / 3; }

	SIZE_T CalcMemUsageInBytes() const;

protected:
	TArray<float> VertexDeltas;	// NumVertices * 3, representing the vector x, y, z.
	TArray<float> BoneRotations; // NumBones * 4, representing the quaternion x, y, z, w.
	TArray<float> CurveValues;
	int32 AnimFrameIndex = -1;
};


/**
 * The training frame cache, which contains a subset of all training frames.
 * Frames that are not inside the cache and are requested, will be generated on the fly.
 * This is basically a FIFO cache of training data for given animation frames.
 */
class FMLDeformerFrameCache
{
public:
	struct FInitSettings
	{
		SIZE_T CacheSizeInBytes = 1024 * 1024 * 1024 * 2ull;	// 2 gigabyte on default.
		TObjectPtr<UMLDeformerAsset> DeformerAsset = nullptr;
		UWorld* World = nullptr;
		bool bLogCacheStats = true;
	};

	void Init(const FInitSettings& InitSettings);
	void Prefetch(int32 StartFrameIndex, int32 EndFrameIndex);	// Use this to prefetch a given set of frames. The end frame specified is included as well.
	void Clear(); // Clear the cached data, forcing everything to be regenerated.
	const FMLDeformerTrainingFrame& GetTrainingFrameForAnimFrame(int32 AnimFrameIndex); // Get the training data for a given frame. This automatically generates on the fly it if needed.
	bool IsValid() const;

	int32 GetNumVertices() const;
	int32 GetNumBones() const;
	int32 GetNumCurves() const;

	const UMLDeformerAsset& GetDeformerAsset() const { return *DeformerAsset.Get(); }
	UMLDeformerAsset& GetDeformerAsset() { return *DeformerAsset.Get(); }
	const FMLDeformerSampler& GetSampler() const { return Sampler; }

	SIZE_T CalcMemUsageInBytes() const;

protected:
	int32 GenerateFrame(int32 AnimFrameIndex);	// Returns the new cached frame index
	int32 GetCachedTrainingFrameIndex(int32 AnimFrameIndex) const;	// Returns -1 if not inside the cache.
	int32 GetNextCacheFrameIndex(); // Gets the next cache spot to use when generating a new frame. This might point to already used cache spots. Implemented as FIFO.
	void UpdateFrameMap();
	void ResetFrameMap();

protected:
	FMLDeformerSampler Sampler;
	TObjectPtr<UMLDeformerAsset> DeformerAsset = nullptr;
	TArray<FMLDeformerTrainingFrame> CachedTrainingFrames;
	TArray<int32> FrameMap;	// Map the global frame number to one in the CachedTrainingFrames array, or -1 when it's not inside the cache.
	int32 NextFreeCacheIndex = 0;
};
