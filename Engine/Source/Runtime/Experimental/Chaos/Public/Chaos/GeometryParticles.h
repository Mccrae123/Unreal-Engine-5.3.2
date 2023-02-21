// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Chaos/ArrayCollectionArray.h"
#include "Chaos/ImplicitObject.h"
#include "Chaos/Particles.h"
#include "Chaos/Rotation.h"
#include "Chaos/ParticleHandleFwd.h"
#include "Chaos/GeometryParticlesfwd.h"
#include "Chaos/CollisionFilterData.h"
#include "Chaos/Collision/ParticleCollisions.h"
#include "Chaos/Box.h"
#include "Chaos/PhysicalMaterials.h"
#include "UObject/PhysicsObjectVersion.h"
#include "UObject/ExternalPhysicsCustomObjectVersion.h"
#include "UObject/ExternalPhysicsMaterialCustomObjectVersion.h"
#include "Chaos/Properties.h"
#include "Chaos/Framework/PhysicsProxyBase.h"
#include "Chaos/Framework/PhysicsSolverBase.h"
#include "Chaos/ShapeInstance.h"

#ifndef CHAOS_DETERMINISTIC
#define CHAOS_DETERMINISTIC 1
#endif


namespace Chaos
{
	class FConstraintHandle;
	class FParticleCollisions;

	using FConstraintHandleArray = TArray<FConstraintHandle*>;

	namespace CVars
	{
		CHAOS_API extern int32 CCDAxisThresholdMode;
		CHAOS_API extern bool bCCDAxisThresholdUsesProbeShapes;
	}


	/**
	* Union between shape and shapes array pointers, used for passing around shapes with
	* implicit that could be single implicit or union.
	*/
	class FShapeOrShapesArray
	{
	public:

		// Store particle's shape array if particle has union geometry, otherwise individual shape.
		FShapeOrShapesArray(const FGeometryParticleHandle* Particle);

		FShapeOrShapesArray()
			: Shape(nullptr)
			, bIsSingleShape(true)
		{}

		FShapeOrShapesArray(const FPerShapeData* InShape)
			: Shape(InShape)
			, bIsSingleShape(true)
		{}

		FShapeOrShapesArray(const FShapesArray* InShapeArray)
			: ShapeArray(InShapeArray)
			, bIsSingleShape(false)
		{}

		bool IsSingleShape() const { return bIsSingleShape; }

		bool IsValid() const { return Shape != nullptr; }

		// Do not call without checking IsSingleShape().
		const FPerShapeData* GetShape() const
		{
			check(bIsSingleShape);
			return Shape;
		}

		// Do not call without checking IsSingleShape().
		const FShapesArray* GetShapesArray() const
		{
			check(!bIsSingleShape);
			return ShapeArray;
		}

	private:
		union
		{
			const FPerShapeData* Shape;
			const FShapesArray* ShapeArray;
		};

		bool bIsSingleShape;
	};

	FORCEINLINE uint32 GetTypeHash(const FParticleID& Unique)
	{
		return ::GetTypeHash(Unique.GlobalID);
	}

	//Holds the data for getting back at the real handle if it's still valid
	//Systems should not use this unless clean-up of direct handle is slow, this uses thread safe shared ptr which is not cheap
	class FWeakParticleHandle
	{
	public:

		FWeakParticleHandle() = default;
		FWeakParticleHandle(TGeometryParticleHandle<FReal,3>* InHandle) : SharedData(MakeShared<FData, ESPMode::ThreadSafe>(FData{InHandle})){}

		//Assumes the weak particle handle has been initialized so SharedData must exist
		TGeometryParticleHandle<FReal,3>* GetHandleUnsafe() const
		{
			return SharedData->Handle;
		}

		TGeometryParticleHandle<FReal,3>* GetHandle() const { return SharedData ? SharedData->Handle : nullptr; }
		void ResetHandle()
		{
			if(SharedData)
			{
				SharedData->Handle = nullptr;
			}
		}

		bool IsInitialized()
		{
			return SharedData != nullptr;
		}

	private:

		struct FData
		{
			TGeometryParticleHandle<FReal,3>* Handle;
		};
		TSharedPtr<FData,ESPMode::ThreadSafe> SharedData;
	};

	enum class EResimType : uint8
	{
		FullResim = 0,	//fully re-run simulation and keep results (any forces must be applied again)
		//ResimWithPrevForces, //use previous forces and keep results (UNIMPLEMENTED)
		ResimAsSlave UE_DEPRECATED(5.1, "EResimType::ResimAsSlave is deprecated, please use EResimType::ResimAsFollower") = 1,
		ResimAsFollower = 1 //use previous forces and snap to previous results regardless of variation - used to push other objects away
		//ResimAsKinematic //treat as kinematic (UNIMPLEMENTED)
	};
	
	template<class T, int d, EGeometryParticlesSimType SimType>
	class TGeometryParticlesImp : public TParticles<T, d>
	{
	public:

		using TArrayCollection::Size;
		using TParticles<T,d>::X;

		CHAOS_API static TGeometryParticlesImp<T, d, SimType>* SerializationFactory(FChaosArchive& Ar, TGeometryParticlesImp < T, d, SimType>* Particles);
		
		CHAOS_API TGeometryParticlesImp()
		    : TParticles<T, d>()
		{
			MParticleType = EParticleType::Static;
			TArrayCollection::AddArray(&MUniqueIdx);
			TArrayCollection::AddArray(&MR);
			TArrayCollection::AddArray(&MGeometry);
			TArrayCollection::AddArray(&MSharedGeometry);
			TArrayCollection::AddArray(&MDynamicGeometry);
#if CHAOS_DETERMINISTIC
			TArrayCollection::AddArray(&MParticleIDs);
#endif
			TArrayCollection::AddArray(&MHasCollision);
			TArrayCollection::AddArray(&MShapesArray);
			TArrayCollection::AddArray(&MLocalBounds);
			TArrayCollection::AddArray(&MCCDAxisThreshold);
			TArrayCollection::AddArray(&MWorldSpaceInflatedBounds);
			TArrayCollection::AddArray(&MHasBounds);
			TArrayCollection::AddArray(&MSpatialIdx);
			TArrayCollection::AddArray(&MSyncState);
			TArrayCollection::AddArray(&MWeakParticleHandle);
			TArrayCollection::AddArray(&MParticleConstraints);
			TArrayCollection::AddArray(&MParticleCollisions);
			TArrayCollection::AddArray(&MGraphIndex);
			TArrayCollection::AddArray(&MResimType);
			TArrayCollection::AddArray(&MEnabledDuringResim);
			TArrayCollection::AddArray(&MLightWeightDisabled);


#if CHAOS_DEBUG_NAME
			TArrayCollection::AddArray(&MDebugName);
#endif

			if (IsRigidBodySim())
			{
				TArrayCollection::AddArray(&MGeometryParticleHandle);
				TArrayCollection::AddArray(&MGeometryParticle);
				TArrayCollection::AddArray(&MPhysicsProxy);
			}

		}
		TGeometryParticlesImp(const TGeometryParticlesImp<T, d, SimType>& Other) = delete;
		CHAOS_API TGeometryParticlesImp(TGeometryParticlesImp<T, d, SimType>&& Other)
		    : TParticles<T, d>(MoveTemp(Other))
			, MUniqueIdx(MoveTemp(Other.MUniqueIdx))
			, MR(MoveTemp(Other.MR))
			, MGeometry(MoveTemp(Other.MGeometry))
			, MSharedGeometry(MoveTemp(Other.MSharedGeometry))
			, MDynamicGeometry(MoveTemp(Other.MDynamicGeometry))
			, MGeometryParticleHandle(MoveTemp(Other.MGeometryParticleHandle))
			, MGeometryParticle(MoveTemp(Other.MGeometryParticle))
			, MPhysicsProxy(MoveTemp(Other.MPhysicsProxy))
			, MHasCollision(MoveTemp(Other.MHasCollision))
			, MShapesArray(MoveTemp(Other.MShapesArray))
			, MLocalBounds(MoveTemp(Other.MLocalBounds))
			, MCCDAxisThreshold(MoveTemp(Other.MCCDAxisThreshold))
			, MWorldSpaceInflatedBounds(MoveTemp(Other.MWorldSpaceInflatedBounds))
			, MHasBounds(MoveTemp(Other.MHasBounds))
			, MSpatialIdx(MoveTemp(Other.MSpatialIdx))
			, MSyncState(MoveTemp(Other.MSyncState))
			, MWeakParticleHandle(MoveTemp(Other.MWeakParticleHandle))
			, MParticleConstraints(MoveTemp(Other.MParticleConstraints))
			, MParticleCollisions(MoveTemp(Other.MParticleCollisions))
			, MGraphIndex(MoveTemp(Other.MGraphIndex))
			, MResimType(MoveTemp(Other.MResimType))
			, MEnabledDuringResim(MoveTemp(Other.MEnabledDuringResim))
			, MLightWeightDisabled(MoveTemp(Other.MLightWeightDisabled))
#if CHAOS_DETERMINISTIC
			, MParticleIDs(MoveTemp(Other.MParticleIDs))
#endif
#if CHAOS_DEBUG_NAME
			, MDebugName(MoveTemp(Other.MDebugName))
#endif

		{
			MParticleType = EParticleType::Static;
			TArrayCollection::AddArray(&MUniqueIdx);
			TArrayCollection::AddArray(&MR);
			TArrayCollection::AddArray(&MGeometry);
			TArrayCollection::AddArray(&MSharedGeometry);
			TArrayCollection::AddArray(&MDynamicGeometry);
#if CHAOS_DETERMINISTIC
			TArrayCollection::AddArray(&MParticleIDs);
#endif
			TArrayCollection::AddArray(&MHasCollision);
			TArrayCollection::AddArray(&MShapesArray);
			TArrayCollection::AddArray(&MLocalBounds);
			TArrayCollection::AddArray(&MCCDAxisThreshold);
			TArrayCollection::AddArray(&MWorldSpaceInflatedBounds);
			TArrayCollection::AddArray(&MHasBounds);
			TArrayCollection::AddArray(&MSpatialIdx);
			TArrayCollection::AddArray(&MSyncState);
			TArrayCollection::AddArray(&MWeakParticleHandle);
			TArrayCollection::AddArray(&MParticleConstraints);
			TArrayCollection::AddArray(&MParticleCollisions);
			TArrayCollection::AddArray(&MGraphIndex);
			TArrayCollection::AddArray(&MResimType);
			TArrayCollection::AddArray(&MEnabledDuringResim);
			TArrayCollection::AddArray(&MLightWeightDisabled);

#if CHAOS_DEBUG_NAME
			TArrayCollection::AddArray(&MDebugName);
#endif

			if (IsRigidBodySim())
			{
				TArrayCollection::AddArray(&MGeometryParticleHandle);
				TArrayCollection::AddArray(&MGeometryParticle);
				TArrayCollection::AddArray(&MPhysicsProxy);
			}
		}

		static constexpr bool IsRigidBodySim() { return SimType == EGeometryParticlesSimType::RigidBodySim; }

		CHAOS_API TGeometryParticlesImp(TParticles<T, d>&& Other)
		    : TParticles<T, d>(MoveTemp(Other))
		{
			MParticleType = EParticleType::Static;
			TArrayCollection::AddArray(&MUniqueIdx);
			TArrayCollection::AddArray(&MR);
			TArrayCollection::AddArray(&MGeometry);
			TArrayCollection::AddArray(&MSharedGeometry);
			TArrayCollection::AddArray(&MDynamicGeometry);
#if CHAOS_DETERMINISTIC
			TArrayCollection::AddArray(&MParticleIDs);
#endif
			TArrayCollection::AddArray(&MHasCollision);
			TArrayCollection::AddArray(&MShapesArray);
			TArrayCollection::AddArray(&MLocalBounds);
			TArrayCollection::AddArray(&MCCDAxisThreshold);
			TArrayCollection::AddArray(&MWorldSpaceInflatedBounds);
			TArrayCollection::AddArray(&MHasBounds);
			TArrayCollection::AddArray(&MSpatialIdx);
			TArrayCollection::AddArray(&MSyncState);
			TArrayCollection::AddArray(&MWeakParticleHandle);
			TArrayCollection::AddArray(&MParticleConstraints);
			TArrayCollection::AddArray(&MParticleCollisions);
			TArrayCollection::AddArray(&MGraphIndex);
			TArrayCollection::AddArray(&MResimType);
			TArrayCollection::AddArray(&MEnabledDuringResim);
			TArrayCollection::AddArray(&MLightWeightDisabled);


#if CHAOS_DEBUG_NAME
			TArrayCollection::AddArray(&MDebugName);
#endif

			if (IsRigidBodySim())
			{
				TArrayCollection::AddArray(&MGeometryParticleHandle);
				TArrayCollection::AddArray(&MGeometryParticle);
				TArrayCollection::AddArray(&MPhysicsProxy);
			}
		}

		CHAOS_API virtual ~TGeometryParticlesImp()
		{}

		FORCEINLINE const TRotation<T, d>& R(const int32 Index) const { return MR[Index]; }
		FORCEINLINE TRotation<T, d>& R(const int32 Index) { return MR[Index]; }

		CHAOS_API FUniqueIdx UniqueIdx(const int32 Index) const { return MUniqueIdx[Index]; }
		CHAOS_API FUniqueIdx& UniqueIdx(const int32 Index) { return MUniqueIdx[Index]; }

		CHAOS_API ESyncState& SyncState(const int32 Index) { return MSyncState[Index].State; }
		CHAOS_API ESyncState SyncState(const int32 Index) const { return MSyncState[Index].State; }

		CHAOS_API TSerializablePtr<FImplicitObject> Geometry(const int32 Index) const { return MGeometry[Index]; }

		CHAOS_API const TUniquePtr<FImplicitObject>& DynamicGeometry(const int32 Index) const { return MDynamicGeometry[Index]; }

		CHAOS_API const TSharedPtr<const FImplicitObject, ESPMode::ThreadSafe>& SharedGeometry(const int32 Index) const { return MSharedGeometry[Index]; }

		CHAOS_API bool HasCollision(const int32 Index) const { return MHasCollision[Index]; }
		CHAOS_API bool& HasCollision(const int32 Index) { return MHasCollision[Index]; }

		CHAOS_API const FShapesArray& ShapesArray(const int32 Index) const { return reinterpret_cast<const FShapesArray&>(MShapesArray[Index]); }

		CHAOS_API const FShapeInstanceArray& ShapeInstances(const int32 Index) const { return MShapesArray[Index]; }

#if CHAOS_DETERMINISTIC
		CHAOS_API FParticleID ParticleID(const int32 Idx) const { return MParticleIDs[Idx]; }
		CHAOS_API FParticleID& ParticleID(const int32 Idx) { return MParticleIDs[Idx]; }
#endif
		// Set a dynamic geometry. Note that X and R must be initialized before calling this function.
		CHAOS_API void SetDynamicGeometry(const int32 Index, TUniquePtr<FImplicitObject>&& InUnique)
		{
			check(!SharedGeometry(Index));	// If shared geometry exists we should not be setting dynamic geometry on top
			SetGeometryImpl(Index, MakeSerializable(InUnique));
			MDynamicGeometry[Index] = MoveTemp(InUnique);
		}

		// Set a shared geometry. Note that X and R must be initialized before calling this function.
		CHAOS_API void SetSharedGeometry(const int32 Index, TSharedPtr<const FImplicitObject, ESPMode::ThreadSafe> InShared)
		{
			check(!DynamicGeometry(Index));	// If dynamic geometry exists we should not be setting shared geometry on top
			SetGeometryImpl(Index, MakeSerializable(InShared));
			MSharedGeometry[Index] = InShared;
		}
		
		CHAOS_API void SetGeometry(const int32 Index, TSerializablePtr<FImplicitObject> InGeometry)
		{
			check(!DynamicGeometry(Index));
			check(!SharedGeometry(Index));
			SetGeometryImpl(Index, InGeometry);
		}

	private:
		CHAOS_API void SetGeometryImpl(const int32 Index, TSerializablePtr<FImplicitObject> InGeometry)
		{
			MGeometry[Index] = InGeometry;

			UpdateShapesArray(Index);

			MHasBounds[Index] = (InGeometry && InGeometry->HasBoundingBox());
			if (MHasBounds[Index])
			{
				MLocalBounds[Index] = TAABB<T, d>(InGeometry->BoundingBox());

				if (CVars::CCDAxisThresholdMode == 0)
				{
					// Use object extents as CCD axis threshold
					MCCDAxisThreshold[Index] = MLocalBounds[Index].Extents();
				}
				else if (CVars::CCDAxisThresholdMode == 1)
				{
					// Use thinnest object extents as all axis CCD thresholds
					MCCDAxisThreshold[Index] = FVec3(MLocalBounds[Index].Extents().GetMin());
				}
				else
				{
					// Find minimum shape bounds thickness on each axis
					FVec3 ThinnestBoundsPerAxis = MLocalBounds[Index].Extents();
					for (const TUniquePtr<FPerShapeData>& Shape : ShapesArray(Index))
					{
						// Only sim-enabled shapes should ever be swept with CCD, so make sure the
						// sim-enabled flag is on for each shape before considering it's min bounds
						// for CCD extents.
						if (Shape->GetSimEnabled() && (CVars::bCCDAxisThresholdUsesProbeShapes || !Shape->GetIsProbe()))
						{
							const TSerializablePtr<FImplicitObject> Geometry = Shape->GetGeometry();
							if (Geometry->HasBoundingBox())
							{
								const TVector<T, d> ShapeExtents = Geometry->BoundingBox().Extents();
								TVector<T, d>& CCDAxisThreshold = MCCDAxisThreshold[Index];
								for (int32 AxisIndex = 0; AxisIndex < d; ++AxisIndex)
								{
									ThinnestBoundsPerAxis[AxisIndex] = FMath::Min(ShapeExtents[AxisIndex], ThinnestBoundsPerAxis[AxisIndex]);
								}
							}
						}
					}

					if (CVars::CCDAxisThresholdMode == 2)
					{
						// On each axis, use the thinnest shape bound on that axis
						MCCDAxisThreshold[Index] = ThinnestBoundsPerAxis;
					}
					else if (CVars::CCDAxisThresholdMode == 3)
					{
						// Find the thinnest shape bound on any axis and use this for all axes
						MCCDAxisThreshold[Index] = FVec3(ThinnestBoundsPerAxis.GetMin());
					}
				}

				// Update the world-space stat of all the shapes - must be called after UpdateShapesArray
				// world space inflated bounds needs to take expansion into account - this is done in integrate for dynamics anyway, so
				// this computation is mainly for statics
				UpdateWorldSpaceState(Index, TRigidTransform<FReal, 3>(X(Index), R(Index)), FVec3(0));
			}
		}
	public:

		CHAOS_API const TAABB<T,d>& LocalBounds(const int32 Index) const
		{
			return MLocalBounds[Index];
		}

		CHAOS_API TAABB<T, d>& LocalBounds(const int32 Index)
		{
			return MLocalBounds[Index];
		}

		CHAOS_API const TVector<T,d>& CCDAxisThreshold(const int32 Index) const
		{
			return MCCDAxisThreshold[Index];
		}

		CHAOS_API bool HasBounds(const int32 Index) const
		{
			return MHasBounds[Index];
		}

		CHAOS_API bool& HasBounds(const int32 Index)
		{
			return MHasBounds[Index];
		}

		CHAOS_API FSpatialAccelerationIdx SpatialIdx(const int32 Index) const
		{
			return MSpatialIdx[Index];
		}

		CHAOS_API FSpatialAccelerationIdx& SpatialIdx(const int32 Index)
		{
			return MSpatialIdx[Index];
		}

#if CHAOS_DEBUG_NAME
		const TSharedPtr<FString, ESPMode::ThreadSafe>& DebugName(const int32 Index) const
		{
			return MDebugName[Index];
		}

		TSharedPtr<FString, ESPMode::ThreadSafe>& DebugName(const int32 Index)
		{
			return MDebugName[Index];
		}
#endif

		CHAOS_API const TAABB<T, d>& WorldSpaceInflatedBounds(const int32 Index) const
		{
			return MWorldSpaceInflatedBounds[Index];
		}

		CHAOS_API void UpdateWorldSpaceState(const int32 Index, const FRigidTransform3& WorldTransform, const FVec3& BoundsExpansion)
		{
			// NOTE: Particle bounds are expanded for use by the spatial partitioning and broad phase but individual
			// shape bounds are not. If expanded Shape bounds are required, the expansion should be done at the calling end.
			FAABB3 WorldBounds = FAABB3::EmptyAABB();

			const FShapesArray& Shapes = ShapesArray(Index);
			for (const auto& Shape : Shapes)
			{
				Shape->UpdateWorldSpaceState(WorldTransform, BoundsExpansion);
				WorldBounds.GrowToInclude(Shape->GetWorldSpaceInflatedShapeBounds());
			}

			MWorldSpaceInflatedBounds[Index] = TAABB<T, d>(WorldBounds);
		}

		void UpdateWorldSpaceStateSwept(const int32 Index, const FRigidTransform3& EndWorldTransform, const FVec3& BoundsExpansion, const FVec3& DeltaX)
		{
			UpdateWorldSpaceState(Index, EndWorldTransform, BoundsExpansion);
			MWorldSpaceInflatedBounds[Index].GrowByVector(DeltaX);
		}

		const TArray<TSerializablePtr<FImplicitObject>>& GetAllGeometry() const { return MGeometry; }

		typedef FGeometryParticleHandle THandleType;
		FORCEINLINE THandleType* Handle(int32 Index) const { return const_cast<THandleType*>(MGeometryParticleHandle[Index].Get()); }

		CHAOS_API void SetHandle(int32 Index, FGeometryParticleHandle* Handle);
		
		CHAOS_API FGeometryParticle* GTGeometryParticle(const int32 Index) const { return MGeometryParticle[Index]; }
		CHAOS_API FGeometryParticle*& GTGeometryParticle(const int32 Index) { return MGeometryParticle[Index]; }

		CHAOS_API const IPhysicsProxyBase* PhysicsProxy(const int32 Index) const { return MPhysicsProxy[Index];  }
		CHAOS_API IPhysicsProxyBase* PhysicsProxy(const int32 Index) { return MPhysicsProxy[Index]; }
		CHAOS_API void SetPhysicsProxy(const int32 Index, IPhysicsProxyBase* InPhysicsProxy)
		{
			MPhysicsProxy[Index] = InPhysicsProxy;
		}

		CHAOS_API FWeakParticleHandle& WeakParticleHandle(const int32 Index)
		{
			FWeakParticleHandle& WeakHandle = MWeakParticleHandle[Index];
			if(WeakHandle.IsInitialized())
			{
				return WeakHandle;
			}

			WeakHandle = FWeakParticleHandle(Handle(Index));
			return WeakHandle;
		}

		/**
		 * @brief All of the persistent (non-collision) constraints affecting the particle
		*/
		CHAOS_API FConstraintHandleArray& ParticleConstraints(const int32 Index)
		{
			return MParticleConstraints[Index];
		}

		CHAOS_API void AddConstraintHandle(const int32& Index, FConstraintHandle* InConstraintHandle)
		{
			CHAOS_ENSURE(!MParticleConstraints[Index].Contains(InConstraintHandle));
			MParticleConstraints[Index].Add(InConstraintHandle);
		}


		CHAOS_API void RemoveConstraintHandle(const int32& Index, FConstraintHandle* InConstraintHandle)
		{
			MParticleConstraints[Index].RemoveSingleSwap(InConstraintHandle);
			CHAOS_ENSURE(!MParticleConstraints[Index].Contains(InConstraintHandle));
		}

		/**
		 * @brief All of the collision constraints affecting the particle
		*/
		CHAOS_API FParticleCollisions& ParticleCollisions(const int32 Index)
		{
			return MParticleCollisions[Index];
		}

		FORCEINLINE const int32 ConstraintGraphIndex(const int32 Index) const { return MGraphIndex[Index]; }
		FORCEINLINE int32& ConstraintGraphIndex(const int32 Index) { return MGraphIndex[Index]; }

		FORCEINLINE EResimType ResimType(const int32 Index) const { return MResimType[Index]; }
		FORCEINLINE EResimType& ResimType(const int32 Index) { return MResimType[Index]; }

		FORCEINLINE bool EnabledDuringResim(const int32 Index) const { return MEnabledDuringResim[Index]; }
		FORCEINLINE bool& EnabledDuringResim(const int32 Index) { return MEnabledDuringResim[Index]; }

		FORCEINLINE bool LightWeightDisabled(const int32 Index) const { return MLightWeightDisabled[Index]; }
		FORCEINLINE bool& LightWeightDisabled(const int32 Index) { return MLightWeightDisabled[Index]; }

private:
		friend THandleType;
		CHAOS_API void ResetWeakParticleHandle(const int32 Index)
		{
			FWeakParticleHandle& WeakHandle = MWeakParticleHandle[Index];
			if(WeakHandle.IsInitialized())
			{
				return WeakHandle.ResetHandle();
			}
		}
public:

		FString ToString(int32 index) const
		{
			FString BaseString = TParticles<T, d>::ToString(index);
			return FString::Printf(TEXT("%s, MUniqueIdx:%d MR:%s, MGeometry:%s, IsDynamic:%d"), *BaseString, UniqueIdx(index).Idx, *R(index).ToString(), (Geometry(index) ? *(Geometry(index)->ToString()) : TEXT("none")), (DynamicGeometry(index) != nullptr));
		}

		CHAOS_API virtual void Serialize(FChaosArchive& Ar)
		{
			LLM_SCOPE(ELLMTag::ChaosParticles);
			TParticles<T, d>::Serialize(Ar);
			Ar << MGeometry << MDynamicGeometry << MR;
			Ar.UsingCustomVersion(FPhysicsObjectVersion::GUID);
			if (Ar.CustomVer(FPhysicsObjectVersion::GUID) >= FPhysicsObjectVersion::PerShapeData)
			{
				Ar << MShapesArray;
			}

			if (Ar.CustomVer(FPhysicsObjectVersion::GUID) >= FPhysicsObjectVersion::SerializeGTGeometryParticles)
			{
				SerializeGeometryParticleHelper(Ar, this);
			}

			Ar.UsingCustomVersion(FExternalPhysicsCustomObjectVersion::GUID);
			if (Ar.CustomVer(FExternalPhysicsCustomObjectVersion::GUID) >= FExternalPhysicsCustomObjectVersion::SerializeParticleBounds)
			{
				TBox<FReal, 3>::SerializeAsAABBs(Ar, MLocalBounds);
				TBox<FReal, 3>::SerializeAsAABBs(Ar, MWorldSpaceInflatedBounds);
				Ar << MHasBounds;

				if (Ar.CustomVer(FExternalPhysicsCustomObjectVersion::GUID) < FExternalPhysicsCustomObjectVersion::SerializeShapeWorldSpaceBounds)
				{
					for (int32 Idx = 0; Idx < MShapesArray.Num(); ++Idx)
					{
						UpdateWorldSpaceState(Idx, FRigidTransform3(X(Idx), R(Idx)), FVec3(0));
					}
				}
			}
			else
			{
				//just assume all bounds come from geometry (technically wrong for pbd rigids with only sample points, but backwards compat is not that important right now)
				for (int32 Idx = 0; Idx < MGeometry.Num(); ++Idx)
				{
					MHasBounds[Idx] = MGeometry[Idx] && MGeometry[Idx]->HasBoundingBox();
					if (MHasBounds[Idx])
					{
						MLocalBounds[Idx] = TAABB<T, d>(MGeometry[Idx]->BoundingBox());
						//ignore velocity too, really just trying to get something reasonable)
						UpdateWorldSpaceState(Idx, FRigidTransform3(X(Idx), R(Idx)), FVec3(0));
					}
				}
			}

			if (Ar.CustomVer(FExternalPhysicsCustomObjectVersion::GUID) < FExternalPhysicsCustomObjectVersion::SpatialIdxSerialized)
			{
				MSpatialIdx.AddZeroed(MGeometry.Num());
			}
			else
			{
				Ar << MSpatialIdx;
			}

			if (Ar.CustomVer(FExternalPhysicsCustomObjectVersion::GUID) < FExternalPhysicsCustomObjectVersion::SerializeHashResult)
			{
				//no longer care about hash so don't read it and don't do anything
			}
		}

		FORCEINLINE EParticleType ParticleType() const { return MParticleType; }

		FORCEINLINE TArray<TRotation<T, d>>& AllR() { return MR; }
		FORCEINLINE TArray<TAABB<T, d>>& AllLocalBounds() { return MLocalBounds; }
		FORCEINLINE TArray<TAABB<T, d>>& AllWorldSpaceInflatedBounds() { return MWorldSpaceInflatedBounds; }
		FORCEINLINE TArray<bool>& AllHasBounds() { return MHasBounds; }

	protected:
		EParticleType MParticleType;

	private:
		TArrayCollectionArray<FUniqueIdx> MUniqueIdx;
		TArrayCollectionArray<TRotation<T, d>> MR;
		// MGeometry contains raw ptrs to every entry in both MSharedGeometry and MDynamicGeometry.
		// It may also contain raw ptrs to geometry which is managed outside of Chaos.
		TArrayCollectionArray<TSerializablePtr<FImplicitObject>> MGeometry;
		// MSharedGeometry entries are owned by the solver, shared between *representations* of a particle.
		// This is NOT for sharing geometry resources between particle's A and B, this is for sharing the
		// geometry between particle A's various representations.
		TArrayCollectionArray<TSharedPtr<const FImplicitObject, ESPMode::ThreadSafe>> MSharedGeometry;
		// MDynamicGeometry entries are used for geo which is by the evolution. It is not set from the game side.
		TArrayCollectionArray<TUniquePtr<FImplicitObject>> MDynamicGeometry;
		TArrayCollectionArray<TSerializablePtr<FGeometryParticleHandle>> MGeometryParticleHandle;
		TArrayCollectionArray<FGeometryParticle*> MGeometryParticle;
		TArrayCollectionArray<IPhysicsProxyBase*> MPhysicsProxy;
		TArrayCollectionArray<bool> MHasCollision;
		TArrayCollectionArray<FShapeInstanceArray> MShapesArray;
		TArrayCollectionArray<TAABB<T,d>> MLocalBounds;
		TArrayCollectionArray<TVector<T,d>> MCCDAxisThreshold;
		TArrayCollectionArray<TAABB<T, d>> MWorldSpaceInflatedBounds;
		TArrayCollectionArray<bool> MHasBounds;
		TArrayCollectionArray<FSpatialAccelerationIdx> MSpatialIdx;
		TArrayCollectionArray<FSyncState> MSyncState;
		TArrayCollectionArray<FWeakParticleHandle> MWeakParticleHandle;
		TArrayCollectionArray<FConstraintHandleArray> MParticleConstraints;
		TArrayCollectionArray<FParticleCollisions> MParticleCollisions;
		TArrayCollectionArray<int32> MGraphIndex;
		TArrayCollectionArray<EResimType> MResimType;
		TArrayCollectionArray<bool> MEnabledDuringResim;
		TArrayCollectionArray<bool> MLightWeightDisabled;

		CHAOS_API void UpdateShapesArray(const int32 Index);

		template <typename T2, int d2, EGeometryParticlesSimType SimType2>
		friend class TGeometryParticlesImp;

		CHAOS_API void SerializeGeometryParticleHelper(FChaosArchive& Ar, TGeometryParticlesImp<T, d, EGeometryParticlesSimType::RigidBodySim>* GeometryParticles);
		
		void SerializeGeometryParticleHelper(FChaosArchive& Ar, TGeometryParticlesImp<T, d, EGeometryParticlesSimType::Other>* GeometryParticles)
		{
			check(false);	//cannot serialize this sim type
		}

#if CHAOS_DETERMINISTIC
		TArrayCollectionArray<FParticleID> MParticleIDs;
#endif
#if CHAOS_DEBUG_NAME
		TArrayCollectionArray<TSharedPtr<FString, ESPMode::ThreadSafe>> MDebugName;
#endif
	};

	template <typename T, int d, EGeometryParticlesSimType SimType>
	FChaosArchive& operator<<(FChaosArchive& Ar, TGeometryParticlesImp<T, d, SimType>& Particles)
	{
		Particles.Serialize(Ar);
		return Ar;
	}

	template <>
	void TGeometryParticlesImp<FReal, 3, EGeometryParticlesSimType::Other>::SetHandle(int32 Index, TGeometryParticleHandle<FReal, 3>* Handle);

	template<>
	TGeometryParticlesImp<FReal, 3, EGeometryParticlesSimType::Other>* TGeometryParticlesImp<FReal, 3, EGeometryParticlesSimType::Other>::SerializationFactory(FChaosArchive& Ar, TGeometryParticlesImp<FReal, 3, EGeometryParticlesSimType::Other>* Particles);

}

