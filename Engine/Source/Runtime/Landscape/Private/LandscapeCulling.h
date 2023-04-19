// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FLandscapeSharedBuffers;

/**
* Landscape GPU culling implementation
* Each landscape section is split to a smaller tiles (4x4 quads)
* Tiles are frustum culled in compute
* Culling is done for each view, including shadow views
* Only active for LOD0 atm
*/
namespace UE {
	namespace Landscape {
		namespace Culling {

			struct FArguments
			{
				FRHIBuffer* IndirectArgsBuffer;
				FRHIBuffer* TileDataVertexBuffer;
				uint32 IndirectArgsOffset;
				uint32 TileDataOffset;
			};

			bool UseCulling(EShaderPlatform ShaderPlatform);
			
			FVertexFactoryType* GetTileVertexFactoryType();

			void InitSharedBuffers(FLandscapeSharedBuffers& SharedBuffers, ERHIFeatureLevel::Type FeatureLevel);
			void DeleteSharedBuffers(FLandscapeSharedBuffers& SharedBuffers);
			void SetupMeshBatch(const FLandscapeSharedBuffers& SharedBuffers, FMeshBatch& MeshBatch);
			void RegisterLandscape(uint32 LandscapeKey, int32 SubsectionSizeVerts, int32 NumSubsections);
			void UnregisterLandscape(uint32 LandscapeKey);

			void PreRenderViewFamily(FSceneViewFamily& InViewFamily);
			void InitMainViews(FRDGBuilder& GraphBuilder, TArrayView<const FSceneView*> Views);
			void InitShadowViews(FRDGBuilder& GraphBuilder, TArrayView<const FSceneView*> ShadowDepthViews, TArrayView<FViewMatrices> ShadowViewMatrices);
			bool GetViewArguments(const FSceneView& View, uint32 LandscapeKey, FIntPoint SectionBase, int32 LODIndex, FArguments& Args);
		}
	}
};
