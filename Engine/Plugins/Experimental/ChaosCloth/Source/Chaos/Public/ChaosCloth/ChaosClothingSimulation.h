// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "ClothingSimulation.h"
#include "ClothingAsset.h"
#include "Chaos/ArrayCollectionArray.h"
#include "Chaos/PBDEvolution.h"
#include "Chaos/Transform.h"
#include "Chaos/TriangleMesh.h"
#include "Chaos/ChaosDebugDrawDeclares.h"
#include "ChaosCloth/ChaosClothConfig.h"
#include "Components/SkeletalMeshComponent.h"

namespace Chaos
{
	typedef FClothingSimulationContextCommon ClothingSimulationContext;
	template<class T, int d> class TPBDLongRangeConstraintsBase;

	class ClothingSimulation : public FClothingSimulationCommon
#if WITH_EDITOR
		, public FGCObject  // Add garbage collection for debug cloth material
#endif  // #if WITH_EDITOR
	{
	public:
		ClothingSimulation();
		virtual ~ClothingSimulation() override;

		// Set the animation drive stiffness for all actors
		void SetAnimDriveSpringStiffness(float InStiffness);
		void SetGravityOverride(const FVector& InGravityOverride);
		void DisableGravityOverride();

		// Function to be called if any of the assets' configuration parameters have changed
		void RefreshClothConfig();
		// Function to be called if any of the assets' physics assets changes (colliders)
		// This seems to only happen when UPhysicsAsset::RefreshPhysicsAssetChange is called with
		// bFullClothRefresh set to false during changes created using the viewport manipulators.
		void RefreshPhysicsAsset();

#if WITH_EDITOR
		// FGCObject interface
		virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
		// End of FGCObject interface

		// Editor only debug draw function
		CHAOSCLOTH_API void DebugDrawPhysMeshShaded(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const;
#endif  // #if WITH_EDITOR

#if WITH_EDITOR || CHAOS_DEBUG_DRAW
		// Editor & runtime debug draw functions
		CHAOSCLOTH_API void DebugDrawPhysMeshWired(USkeletalMeshComponent* OwnerComponent = nullptr, FPrimitiveDrawInterface* PDI = nullptr) const;
		CHAOSCLOTH_API void DebugDrawPointNormals(USkeletalMeshComponent* OwnerComponent = nullptr, FPrimitiveDrawInterface* PDI = nullptr) const;
		CHAOSCLOTH_API void DebugDrawInversedPointNormals(USkeletalMeshComponent* OwnerComponent = nullptr, FPrimitiveDrawInterface* PDI = nullptr) const;
		CHAOSCLOTH_API void DebugDrawFaceNormals(USkeletalMeshComponent* OwnerComponent = nullptr, FPrimitiveDrawInterface* PDI = nullptr) const;
		CHAOSCLOTH_API void DebugDrawInversedFaceNormals(USkeletalMeshComponent* OwnerComponent = nullptr, FPrimitiveDrawInterface* PDI = nullptr) const;
		CHAOSCLOTH_API void DebugDrawCollision(USkeletalMeshComponent* OwnerComponent = nullptr, FPrimitiveDrawInterface* PDI = nullptr) const;
		CHAOSCLOTH_API void DebugDrawBackstops(USkeletalMeshComponent* OwnerComponent = nullptr, FPrimitiveDrawInterface* PDI = nullptr) const;
		CHAOSCLOTH_API void DebugDrawMaxDistances(USkeletalMeshComponent* OwnerComponent = nullptr, FPrimitiveDrawInterface* PDI = nullptr) const;
		CHAOSCLOTH_API void DebugDrawAnimDrive(USkeletalMeshComponent* OwnerComponent = nullptr, FPrimitiveDrawInterface* PDI = nullptr) const;
		CHAOSCLOTH_API void DebugDrawLongRangeConstraint(USkeletalMeshComponent* OwnerComponent = nullptr, FPrimitiveDrawInterface* PDI = nullptr) const;
		CHAOSCLOTH_API void DebugDrawWindDragForces(USkeletalMeshComponent* OwnerComponent = nullptr, FPrimitiveDrawInterface* PDI = nullptr) const;
#endif  // #if WITH_EDITOR || CHAOS_DEBUG_DRAW

	protected:
		// IClothingSimulation interface
		virtual void Initialize() override;
		virtual void CreateActor(USkeletalMeshComponent* InOwnerComponent, UClothingAssetBase* InAsset, int32 SimDataIndex) override;
		virtual void PostActorCreationInitialize() override;
		virtual IClothingSimulationContext* CreateContext() override { return new ClothingSimulationContext(); }
		virtual void Shutdown() override;
		virtual bool ShouldSimulate() const override { return true; }
		virtual void Simulate(IClothingSimulationContext* InContext) override;
		virtual void DestroyActors() override;
		virtual void DestroyContext(IClothingSimulationContext* InContext) override { delete InContext; }
		virtual void GetSimulationData(TMap<int32, FClothSimulData>& OutData, USkeletalMeshComponent* InOwnerComponent, USkinnedMeshComponent* InOverrideComponent) const override;

		virtual FBoxSphereBounds GetBounds(const USkeletalMeshComponent* InOwnerComponent) const override
		{ return FBoxSphereBounds(Evolution->Particles().X().GetData(), Evolution->Particles().Size()); }

		virtual void AddExternalCollisions(const FClothCollisionData& InData) override;
		virtual void ClearExternalCollisions() override;
		virtual void GetCollisions(FClothCollisionData& OutCollisions, bool bIncludeExternal = true) const override;
		// End of IClothingSimulation interface

	private:
		void UpdateSimulationFromSharedSimConfig();
		void BuildMesh(const FClothPhysicalMeshData& PhysMesh, int32 InSimDataIndex);

		// Reset particle positions to animation positions, masses and velocities are cleared
		void ResetParticles(int32 InSimDataIndex);

		void SetParticleMasses(const UChaosClothConfig* ChaosClothSimConfig, const FClothPhysicalMeshData& PhysMesh, int32 InSimDataIndex);

		void AddConstraints(const UChaosClothConfig* ChaosClothSimConfig, const FClothPhysicalMeshData& PhysMesh, int32 InSimDataIndex);

		void AddSelfCollisions(int32 InSimDataIndex);

		// Extract all collisions (from the physics asset and from the legacy apex collisions)
		void ExtractCollisions(const UClothingAssetCommon* Asset);
		// Extract the collisions from the physics asset referenced by the specified clothing asset
		void ExtractPhysicsAssetCollisions(const UClothingAssetCommon* Asset);
		// Extract the legacy apex collisions from the specified clothing asset 
		void ExtractLegacyAssetCollisions(const UClothingAssetCommon* Asset);
		// Add collisions from a ClothCollisionData structure
		void AddCollisions(const FClothCollisionData& ClothCollisionData, const TArray<int32>& UsedBoneIndices);
		// Update the collision transforms using the specified context, also update old collision if bReinit is true
		void UpdateCollisionTransforms(const ClothingSimulationContext& Context, bool bReinit);
		// Return the correct bone index based on the asset used bone index array
		FORCEINLINE int32 GetMappedBoneIndex(const TArray<int32>& UsedBoneIndices, int32 BoneIndex)
		{
			return UsedBoneIndices.IsValidIndex(BoneIndex) ? UsedBoneIndices[BoneIndex] : INDEX_NONE;
		}

	private:
		// Assets
		TArray<UClothingAssetCommon*> Assets;
		UChaosClothSharedSimConfig* ClothSharedSimConfig;

		// Cloth Interaction Parameters
		// These simulation parameters can be changed through blueprints
		// They will only be updated when the simulation is not running (so are safe to use on any cloth thread)
		TArray<float> AnimDriveSpringStiffness; // One for every Asset

		// Collision Data
		FClothCollisionData ExternalCollisions;  // External collisions
		TArray<Chaos::TRigidTransform<float, 3>> OldCollisionTransforms;  // Used for the kinematic collision transform update
		TArray<Chaos::TRigidTransform<float, 3>> CollisionTransforms;  // Used for the kinematic collision transform update
		Chaos::TArrayCollectionArray<int32> BoneIndices;
		Chaos::TArrayCollectionArray<Chaos::TRigidTransform<float, 3>> BaseTransforms;

		// Animation Data
		TArray<Chaos::TVector<float, 3>> OldAnimationPositions;
		TArray<Chaos::TVector<float, 3>> AnimationPositions;
		TArray<Chaos::TVector<float, 3>> AnimationNormals;

		// Sim Data
		TArray<Chaos::TVector<uint32, 2>> IndexToRangeMap;
		TArray<FTransform> RootBoneWorldTransforms;  // Used for teleportation

		TArray<TUniquePtr<Chaos::TTriangleMesh<float>>> Meshes;
		mutable TArray<TArray<Chaos::TVector<float, 3>>> FaceNormals;
		mutable TArray<TArray<Chaos::TVector<float, 3>>> PointNormals;

		TUniquePtr<Chaos::TPBDEvolution<float, 3>> Evolution;

		uint32 ExternalCollisionsOffset;  // External collisions first particle index

		float Time;
		float DeltaTime;

		bool bOverrideGravity;
		FVector Gravity;
		FVector WindVelocity;

		TArray<TSharedPtr<TPBDLongRangeConstraintsBase<float, 3>>> LongRangeConstraints;

		// This is used to translate between world space and simulation space. Add this to simulation space coordinates to get world space coordinates
		// The function of this is to help with floating point precision errors if the character is far away form the world origin
		// and to decouple the simulation from character acceleration and velocities if it is desired
		bool	bLocalSimSpaceEnabled;
		FVector LocalSimSpaceOffset;  
		FVector PrevLocalSimSpaceOffset;
		FVector LocalSimSpaceVelocity;
		FVector LocalSimSpaceCappedVelocity;
		FVector PrevLocalSimSpaceVelocity;
		FVector ComponentLinearAccScale;
		FVector ComponentLinearAccClamp;

#if WITH_EDITOR
		// Visualization material
		UMaterial* DebugClothMaterial;
		UMaterial* DebugClothMaterialVertex;
#endif  // #if WITH_EDITOR
	};
} // namespace Chaos