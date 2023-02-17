// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Chaos/Core.h"

#include "Chaos/CollisionFilterData.h"
#include "Chaos/Framework/PhysicsProxyBase.h"
#include "Chaos/ImplicitFwd.h"
#include "Chaos/ParticleDirtyFlags.h"
#include "Chaos/ParticleHandleFwd.h"
#include "Chaos/PhysicalMaterials.h"
#include "Chaos/Properties.h"
#include "Chaos/Serializable.h"
#include "Chaos/ShapeInstanceFwd.h"

namespace Chaos
{
	class FImplicitObject;
	class FShapeInstance;
	class FShapeInstanceProxy;

	namespace Private
	{
		class FShapeInstanceExtended;
	}


	/**
	 * FPerShapeData is going to be deprecated. See FShapeInstance and FShapeInstanceProxy
	 * 
	 * @todo(chaos): 
	 * - change ShapesArray() and all code using it to use ShapeInstance or ShapeInstanceProxy as appropriate
	 * - deprecate FPerShapeData
	 */
	class CHAOS_API FPerShapeData
	{
	protected:
		enum class EPerShapeDataType : uint8
		{
			Proxy,
			Sim,
			SimExtended,
		};

		EPerShapeDataType GetType() const
		{
			return Type;
		}

		FShapeInstanceProxy* AsShapeInstanceProxy();
		const FShapeInstanceProxy* AsShapeInstanceProxy() const;

		FShapeInstance* AsShapeInstance();
		const FShapeInstance* AsShapeInstance() const;

		Private::FShapeInstanceExtended* AsShapeInstanceExtended();
		const Private::FShapeInstanceExtended* AsShapeInstanceExtended() const;

		// Call a function on the concrete type
		template<typename TLambda> decltype(auto) DownCast(const TLambda& Lambda);
		template<typename TLambda> decltype(auto) DownCast(const TLambda& Lambda) const;

	public:
		static constexpr bool AlwaysSerializable = true;

		UE_DEPRECATED(5.3, "Not used")
		static bool RequiresCachedLeafInfo(const FImplicitObject* Geometry) { return false; }

		UE_DEPRECATED(5.3, "Call FShapeInstanceProxy::Make for game thread objects, FShapeInstance::Make for physics thread objects")
		static TUniquePtr<FPerShapeData> CreatePerShapeData(int32 InShapeIdx, TSerializablePtr<FImplicitObject> InGeometry);

		UE_DEPRECATED(5.3, "Call FShapeInstanceProxy::UpdateGeometry for game thread objects, FShapeInstance::UpdateGeometry for physics thread objects")
		static void UpdateGeometry(TUniquePtr<FPerShapeData>& InOutShapePtr, TSerializablePtr<FImplicitObject> InGeometry);


		static FPerShapeData* SerializationFactory(FChaosArchive& Ar, FPerShapeData*);

		virtual ~FPerShapeData() {}

		virtual void Serialize(FChaosArchive& Ar);

		void UpdateShapeBounds(const FRigidTransform3& WorldTM, const FVec3& BoundsExpansion = FVec3(0));

		void* GetUserData() const;
		void SetUserData(void* InUserData);

		const FCollisionFilterData& GetQueryData() const;
		void SetQueryData(const FCollisionFilterData& InQueryData);

		const FCollisionFilterData& GetSimData() const;
		void SetSimData(const FCollisionFilterData& InSimData);

		TSerializablePtr<FImplicitObject> GetGeometry() const;

		const TAABB<FReal, 3>& GetWorldSpaceInflatedShapeBounds() const;

		void UpdateWorldSpaceState(const FRigidTransform3& WorldTransform, const FVec3& BoundsExpansion);

		// The leaf shape (with transformed and implicit wrapper removed).
		const FImplicitObject* GetLeafGeometry() const;

		// The actor-relative transform of the leaf geometry.
		FRigidTransform3 GetLeafRelativeTransform() const;

		// The world-space transform of the leaf geometry.
		// If we have non-identity leaf relative transform, is cached from the last call to UpdateWorldSpaceState.
		// If not cahced, is constructed from arguments.
		FRigidTransform3 GetLeafWorldTransform(const FGeometryParticleHandle* Particle) const;

		void UpdateLeafWorldTransform(FGeometryParticleHandle* Particle);

		const TArray<FMaterialHandle>& GetMaterials() const;
		void SetMaterial(FMaterialHandle InMaterial);
		void SetMaterials(const TArray<FMaterialHandle>& InMaterials);

		const TArray<FMaterialMaskHandle>& GetMaterialMasks() const;
		void SetMaterialMasks(const TArray<FMaterialMaskHandle>& InMaterialMasks);

		const TArray<uint32>& GetMaterialMaskMaps() const;
		void SetMaterialMaskMaps(const TArray<uint32>& InMaterialMaskMaps);

		const TArray<FMaterialHandle>& GetMaterialMaskMapMaterials() const;
		void SetMaterialMaskMapMaterials(const TArray<FMaterialHandle>& InMaterialMaskMapMaterials);

		const FShapeDirtyFlags GetDirtyFlags() const;

		bool GetQueryEnabled() const;
		void SetQueryEnabled(const bool bEnable);

		bool GetSimEnabled() const;
		void SetSimEnabled(const bool bEnable);

		bool GetIsProbe() const;
		void SetIsProbe(const bool bIsProbe);

		EChaosCollisionTraceFlag GetCollisionTraceType() const;
		void SetCollisionTraceType(const EChaosCollisionTraceFlag InTraceFlag);

		const FCollisionData& GetCollisionData() const;
		void SetCollisionData(const FCollisionData& Data);

		const FMaterialData& GetMaterialData() const;
		void SetMaterialData(const FMaterialData& Data);

		void SyncRemoteData(FDirtyPropertiesManager& Manager, int32 ShapeDataIdx, FShapeDirtyData& RemoteData);
		
		void SetProxy(IPhysicsProxyBase* InProxy);
		
		int32 GetShapeIndex() const;
		void ModifyShapeIndex(int32 NewShapeIndex);

		template <typename Lambda> void ModifySimData(const Lambda& LambdaFunc);
		template <typename Lambda> void ModifyMaterials(const Lambda& LambdaFunc);
		template <typename Lambda> void ModifyMaterialMasks(const Lambda& LambdaFunc);
		template <typename Lambda> void ModifyMaterialMaskMaps(const Lambda& LambdaFunc);
		template <typename Lambda> void ModifyMaterialMaskMapMaterials(const Lambda& LambdaFunc);

	protected:
		FPerShapeData(const EPerShapeDataType InType, int32 InShapeIdx)
			: Type(InType)
			, ShapeIdx(InShapeIdx)
			, Geometry()
			, WorldSpaceInflatedShapeBounds(FAABB3(FVec3(0), FVec3(0)))
		{
		}

		FPerShapeData(const EPerShapeDataType InType, int32 InShapeIdx, TSerializablePtr<FImplicitObject> InGeometry)
			: Type(InType)
			, ShapeIdx(InShapeIdx)
			, Geometry(InGeometry)
			, WorldSpaceInflatedShapeBounds(FAABB3(FVec3(0), FVec3(0)))
		{
		}

		FPerShapeData(const EPerShapeDataType InType, const FPerShapeData& Other)
			: Type(InType)
			, ShapeIdx(Other.ShapeIdx)
			, Geometry(Other.Geometry)
			, WorldSpaceInflatedShapeBounds(Other.WorldSpaceInflatedShapeBounds)
		{
		}

		EPerShapeDataType Type;
		int32 ShapeIdx;
		TSerializablePtr<FImplicitObject> Geometry;
		TAABB<FReal, 3> WorldSpaceInflatedShapeBounds;
	};


	///////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////
	// 
	// FShapeInstanceProxy
	// 
	///////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////

	/*
	 * NOTE: FShapeInstanceProxy is a Game-Thread object. 
	 * See FShapeInstance for the physics-thread equivalent
	 * 
	 * FShapeInstanceProxy contains the per-shape data associated with a single shape on a particle. 
	 * This contains data like the collision / query filters, material properties etc.
	 *
	 * Every particle holds one FShapeInstanceProxy object for each geometry they use.
	 * If the particle has a Union of geometries there will be one FShapeInstanceProxy
	 * for each geometry in the union. (Except ClusteredUnions - these are not flattened
	 * because they contain their own query acceleration structure.)
	 *
	 * NOTE: keep size to a minimum. There can be millions of these in s scene.
	 *
	 * @todo(chaos) : try to remove the GT Proxy pointer - this could easily be passed into the
	 * relevant functions instead.
	 *
	 * @todo(chaos) : reduce the cost of MaterialData for shapes with one materialand no masks etc.
	 */
	class CHAOS_API FShapeInstanceProxy : public FPerShapeData
	{
	public:
		friend class FPerShapeData;

		static TUniquePtr<FShapeInstanceProxy> Make(int32 InShapeIdx, TSerializablePtr<FImplicitObject> InGeometry);
		static void UpdateGeometry(TUniquePtr<FShapeInstanceProxy>& InOutShapePtr, TSerializablePtr<FImplicitObject> InGeometry);
		static FShapeInstanceProxy* SerializationFactory(FChaosArchive& Ar, FShapeInstanceProxy*);

		void UpdateShapeBounds(const FRigidTransform3& WorldTM, const FVec3& BoundsExpansion = FVec3(0));

		void* GetUserData() const { return CollisionData.Read().UserData; }
		void SetUserData(void* InUserData)
		{
			CollisionData.Modify(true, DirtyFlags, Proxy, ShapeIdx, [InUserData](FCollisionData& Data) { Data.UserData = InUserData; });
		}

		const FCollisionFilterData& GetQueryData() const { return CollisionData.Read().QueryData; }
		void SetQueryData(const FCollisionFilterData& InQueryData)
		{
			CollisionData.Modify(true, DirtyFlags, Proxy, ShapeIdx, [InQueryData](FCollisionData& Data) { Data.QueryData = InQueryData; });
		}

		const FCollisionFilterData& GetSimData() const { return CollisionData.Read().SimData; }
		void SetSimData(const FCollisionFilterData& InSimData)
		{
			CollisionData.Modify(true, DirtyFlags, Proxy, ShapeIdx, [InSimData](FCollisionData& Data) { Data.SimData = InSimData; });
		}

		void UpdateWorldSpaceState(const FRigidTransform3& WorldTransform, const FVec3& BoundsExpansion);

		// The leaf shape (with transformed and implicit wrapper removed).
		const FImplicitObject* GetLeafGeometry() const;

		// The actor-relative transform of the leaf geometry.
		FRigidTransform3 GetLeafRelativeTransform() const;

		// The world-space transform of the leaf geometry.
		// If we have non-identity leaf relative transform, is cached from the last call to UpdateWorldSpaceState.
		// If not cahced, is constructed from arguments.
		FRigidTransform3 GetLeafWorldTransform(const FGeometryParticleHandle* Particle) const;
		void UpdateLeafWorldTransform(FGeometryParticleHandle* Particle);

		const TArray<FMaterialHandle>& GetMaterials() const { return Materials.Read().Materials; }
		const TArray<FMaterialMaskHandle>& GetMaterialMasks() const { return Materials.Read().MaterialMasks; }
		const TArray<uint32>& GetMaterialMaskMaps() const { return Materials.Read().MaterialMaskMaps; }
		const TArray<FMaterialHandle>& GetMaterialMaskMapMaterials() const { return Materials.Read().MaterialMaskMapMaterials; }

		const FShapeDirtyFlags GetDirtyFlags() const { return DirtyFlags; }

		void SetMaterial(FMaterialHandle InMaterial)
		{
			Materials.Modify(true, DirtyFlags, Proxy, ShapeIdx, [InMaterial](FMaterialData& Data)
				{
					Data.Materials.Reset(1);
					Data.Materials.Add(InMaterial);
				});
		}

		void SetMaterials(const TArray<FMaterialHandle>& InMaterials)
		{
			Materials.Modify(true, DirtyFlags, Proxy, ShapeIdx, [&InMaterials](FMaterialData& Data)
				{
					Data.Materials = InMaterials;
				});
		}

		void SetMaterialMasks(const TArray<FMaterialMaskHandle>& InMaterialMasks)
		{
			Materials.Modify(true, DirtyFlags, Proxy, ShapeIdx, [&InMaterialMasks](FMaterialData& Data)
				{
					Data.MaterialMasks = InMaterialMasks;
				});
		}

		void SetMaterialMaskMaps(const TArray<uint32>& InMaterialMaskMaps)
		{
			Materials.Modify(true, DirtyFlags, Proxy, ShapeIdx, [&InMaterialMaskMaps](FMaterialData& Data)
				{
					Data.MaterialMaskMaps = InMaterialMaskMaps;
				});
		}

		void SetMaterialMaskMapMaterials(const TArray<FMaterialHandle>& InMaterialMaskMapMaterials)
		{
			Materials.Modify(true, DirtyFlags, Proxy, ShapeIdx, [&InMaterialMaskMapMaterials](FMaterialData& Data)
				{
					Data.MaterialMaskMapMaterials = InMaterialMaskMapMaterials;
				});
		}

		bool GetQueryEnabled() const { return CollisionData.Read().bQueryCollision; }
		void SetQueryEnabled(const bool bEnable)
		{
			CollisionData.Modify(true, DirtyFlags, Proxy, ShapeIdx, [bEnable](FCollisionData& Data) { Data.bQueryCollision = bEnable; });
		}

		bool GetSimEnabled() const { return CollisionData.Read().bSimCollision; }
		void SetSimEnabled(const bool bEnable)
		{
			CollisionData.Modify(true, DirtyFlags, Proxy, ShapeIdx, [bEnable](FCollisionData& Data) { Data.bSimCollision = bEnable; });
		}

		bool GetIsProbe() const { return CollisionData.Read().bIsProbe; }
		void SetIsProbe(const bool bIsProbe)
		{
			CollisionData.Modify(true, DirtyFlags, Proxy, ShapeIdx, [bIsProbe](FCollisionData& Data) { Data.bIsProbe = bIsProbe; });
		}

		EChaosCollisionTraceFlag GetCollisionTraceType() const { return CollisionData.Read().CollisionTraceType; }
		void SetCollisionTraceType(const EChaosCollisionTraceFlag InTraceFlag)
		{
			CollisionData.Modify(true, DirtyFlags, Proxy, ShapeIdx, [InTraceFlag](FCollisionData& Data) { Data.CollisionTraceType = InTraceFlag; });
		}

		const FCollisionData& GetCollisionData() const { return CollisionData.Read(); }

		void SetCollisionData(const FCollisionData& Data)
		{
			CollisionData.Write(Data, true, DirtyFlags, Proxy, ShapeIdx);
		}

		const FMaterialData& GetMaterialData() const { return Materials.Read(); }

		void SetMaterialData(const FMaterialData& Data)
		{
			Materials.Write(Data, true, DirtyFlags, Proxy, ShapeIdx);
		}

		void SyncRemoteData(FDirtyPropertiesManager& Manager, int32 ShapeDataIdx, FShapeDirtyData& RemoteData)
		{
			RemoteData.SetFlags(DirtyFlags);
			CollisionData.SyncRemote(Manager, ShapeDataIdx, RemoteData);
			Materials.SyncRemote(Manager, ShapeDataIdx, RemoteData);
			DirtyFlags.Clear();
		}

		void SetProxy(IPhysicsProxyBase* InProxy)
		{
			Proxy = InProxy;
			if (Proxy)
			{
				if (DirtyFlags.IsDirty())
				{
					if (FPhysicsSolverBase* PhysicsSolverBase = Proxy->GetSolver<FPhysicsSolverBase>())
					{
						PhysicsSolverBase->AddDirtyProxyShape(Proxy, ShapeIdx);
					}
				}
			}
		}

		template <typename Lambda>
		void ModifySimData(const Lambda& LambdaFunc)
		{
			CollisionData.Modify(true, DirtyFlags, Proxy, ShapeIdx, [&LambdaFunc](FCollisionData& Data) { LambdaFunc(Data.SimData); });
		}

		template <typename Lambda>
		void ModifyMaterials(const Lambda& LambdaFunc)
		{
			Materials.Modify(true, DirtyFlags, Proxy, ShapeIdx, [&LambdaFunc](FMaterialData& Data)
				{
					LambdaFunc(Data.Materials);
				});
		}

		template <typename Lambda>
		void ModifyMaterialMasks(const Lambda& LambdaFunc)
		{
			Materials.Modify(true, DirtyFlags, Proxy, ShapeIdx, [&LambdaFunc](FMaterialData& Data)
				{
					LambdaFunc(Data.MaterialMasks);
				});
		}

		template <typename Lambda>
		void ModifyMaterialMaskMaps(const Lambda& LambdaFunc)
		{
			Materials.Modify(true, DirtyFlags, Proxy, ShapeIdx, [&LambdaFunc](FMaterialData& Data)
				{
					LambdaFunc(Data.MaterialMaskMaps);
				});
		}

		template <typename Lambda>
		void ModifyMaterialMaskMapMaterials(const Lambda& LambdaFunc)
		{
			Materials.Modify(true, DirtyFlags, Proxy, ShapeIdx, [&LambdaFunc](FMaterialData& Data)
				{
					LambdaFunc(Data.MaterialMaskMapMaterials);
				});
		}

	protected:
		explicit FShapeInstanceProxy(int32 InShapeIdx)
			: FPerShapeData(EPerShapeDataType::Proxy, InShapeIdx)
			, Proxy(nullptr)
			, DirtyFlags()
			, CollisionData()
			, Materials()
		{
		}

		FShapeInstanceProxy(int32 InShapeIdx, TSerializablePtr<FImplicitObject> InGeometry)
			: FPerShapeData(EPerShapeDataType::Proxy, InShapeIdx, InGeometry)
			, Proxy(nullptr)
			, DirtyFlags()
			, CollisionData()
			, Materials()
		{
		}

		IPhysicsProxyBase* Proxy;
		FShapeDirtyFlags DirtyFlags;

		TShapeProperty<FCollisionData, EShapeProperty::CollisionData> CollisionData;
		TShapeProperty<FMaterialData, EShapeProperty::Materials> Materials;
	};


	///////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////
	// 
	// FShapeInstance
	// 
	///////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////


	/*
	 * NOTE: FShapeInstance is a Physics-Thread object.
	 * See FShapeInstanceProxy for the game-thread equivalent
	 *
	 * FShapeInstance contains the per-shape data associated with a single shape on a particle.
	 * This contains data like the collision / query filters, material properties etc.
	 *
	 * Every particle holds one FShapeInstance object for each geometry they use.
	 * If the particle has a Union of geometries there will be one FShapeInstance
	 * for each geometry in the union. (Except ClusteredUnions - these are not flattened
	 * because they contain their own query acceleration structure.)
	 *
	 * NOTE: keep size to a minimum. There can be millions of these in s scene.
	 *
	 * @todo(chaos) : reduce the cost of MaterialData for shapes with one materialand no masks etc.
	 */
	class CHAOS_API FShapeInstance : public FPerShapeData
	{
	public:
		friend class FPerShapeData;

		static TUniquePtr<FShapeInstance> Make(int32 InShapeIdx, TSerializablePtr<FImplicitObject> InGeometry);
		static void UpdateGeometry(TUniquePtr<FShapeInstance>& InOutShapePtr, TSerializablePtr<FImplicitObject> InGeometry);
		static FShapeInstance* SerializationFactory(FChaosArchive& Ar, FShapeInstance*);

		void UpdateShapeBounds(const FRigidTransform3& WorldTM, const FVec3& BoundsExpansion = FVec3(0));

		void* GetUserData() const { return CollisionData.UserData; }
		void SetUserData(void* InUserData) { CollisionData.UserData = InUserData; }

		const FCollisionFilterData& GetQueryData() const { return CollisionData.QueryData; }
		void SetQueryData(const FCollisionFilterData& InQueryData) { CollisionData.QueryData = InQueryData; }

		const FCollisionFilterData& GetSimData() const { return CollisionData.SimData; }
		void SetSimData(const FCollisionFilterData& InSimData) { CollisionData.SimData = InSimData; }

		void UpdateWorldSpaceState(const FRigidTransform3& WorldTransform, const FVec3& BoundsExpansion);

		// The leaf shape (with transformed and implicit wrapper removed).
		const FImplicitObject* GetLeafGeometry() const;

		// The actor-relative transform of the leaf geometry.
		FRigidTransform3 GetLeafRelativeTransform() const;

		// The world-space transform of the leaf geometry.
		// If we have non-identity leaf relative transform, is cached from the last call to UpdateWorldSpaceState.
		// If not cahced, is constructed from arguments.
		FRigidTransform3 GetLeafWorldTransform(const FGeometryParticleHandle* Particle) const;
		void UpdateLeafWorldTransform(FGeometryParticleHandle* Particle);

		const TArray<FMaterialHandle>& GetMaterials() const { return Materials.Materials; }
		const TArray<FMaterialMaskHandle>& GetMaterialMasks() const { return Materials.MaterialMasks; }
		const TArray<uint32>& GetMaterialMaskMaps() const { return Materials.MaterialMaskMaps; }
		const TArray<FMaterialHandle>& GetMaterialMaskMapMaterials() const { return Materials.MaterialMaskMapMaterials; }

		void SetMaterial(FMaterialHandle InMaterial) { Materials.Materials.Reset(1); Materials.Materials.Add(InMaterial); }
		void SetMaterials(const TArray<FMaterialHandle>& InMaterials) { Materials.Materials = InMaterials; }
		void SetMaterialMasks(const TArray<FMaterialMaskHandle>& InMaterialMasks) { Materials.MaterialMasks = InMaterialMasks; }
		void SetMaterialMaskMaps(const TArray<uint32>& InMaterialMaskMaps) { Materials.MaterialMaskMaps = InMaterialMaskMaps; }
		void SetMaterialMaskMapMaterials(const TArray<FMaterialHandle>& InMaterialMaskMapMaterials) { Materials.MaterialMaskMapMaterials = InMaterialMaskMapMaterials; }

		bool GetQueryEnabled() const { return CollisionData.bQueryCollision; }
		void SetQueryEnabled(const bool bEnable) { CollisionData.bQueryCollision = bEnable; }

		bool GetSimEnabled() const { return CollisionData.bSimCollision; }
		void SetSimEnabled(const bool bEnable) { CollisionData.bSimCollision = bEnable; }

		bool GetIsProbe() const { return CollisionData.bIsProbe; }
		void SetIsProbe(const bool bIsProbe) { CollisionData.bIsProbe = bIsProbe; }

		EChaosCollisionTraceFlag GetCollisionTraceType() const { return CollisionData.CollisionTraceType; }
		void SetCollisionTraceType(const EChaosCollisionTraceFlag InTraceFlag) { CollisionData.CollisionTraceType = InTraceFlag; }

		const FCollisionData& GetCollisionData() const { return CollisionData; }
		void SetCollisionData(const FCollisionData& Data) { CollisionData = Data; }

		const FMaterialData& GetMaterialData() const { return Materials; }
		void SetMaterialData(const FMaterialData& Data) { Materials = Data; }

		// @todo(chaos): remove when FPerShapeData is removed
		const FShapeDirtyFlags GetDirtyFlags() const { check(false); return FShapeDirtyFlags(); }
		void SyncRemoteData(FDirtyPropertiesManager& Manager, int32 ShapeDataIdx, FShapeDirtyData& RemoteData) { check(false); }
		void SetProxy(IPhysicsProxyBase* InProxy) { check(false); }

		template <typename Lambda> void ModifySimData(const Lambda& LambdaFunc) { LambdaFunc(CollisionData.SimData); }
		template <typename Lambda> void ModifyMaterials(const Lambda& LambdaFunc) { LambdaFunc(Materials.Materials); }
		template <typename Lambda> void ModifyMaterialMasks(const Lambda& LambdaFunc) { LambdaFunc(Materials.MaterialMasks); }
		template <typename Lambda> void ModifyMaterialMaskMaps(const Lambda& LambdaFunc) { LambdaFunc(Materials.MaterialMaskMaps); }
		template <typename Lambda> void ModifyMaterialMaskMapMaterials(const Lambda& LambdaFunc) { LambdaFunc(Materials.MaterialMaskMapMaterials); }

	protected:
		explicit FShapeInstance(int32 InShapeIdx)
			: FPerShapeData(EPerShapeDataType::Sim, InShapeIdx)
			, CollisionData()
			, Materials()
		{
		}

		FShapeInstance(int32 InShapeIdx, TSerializablePtr<FImplicitObject> InGeometry)
			: FPerShapeData(EPerShapeDataType::Sim, InShapeIdx, InGeometry)
			, CollisionData()
			, Materials()
		{
		}

		explicit FShapeInstance(FShapeInstance&& Other)
			: FPerShapeData(EPerShapeDataType::Sim, Other)
			, CollisionData(MoveTemp(Other.CollisionData))
			, Materials(MoveTemp(Other.Materials))
		{
		}

		FShapeInstance(const EPerShapeDataType InType, int32 InShapeIdx, TSerializablePtr<FImplicitObject> InGeometry)
			: FPerShapeData(InType, InShapeIdx, InGeometry)
			, CollisionData()
			, Materials()
		{
		}

		FShapeInstance(const EPerShapeDataType InType, FShapeInstance&& Other)
			: FPerShapeData(InType, Other)
			, CollisionData(MoveTemp(Other.CollisionData))
			, Materials(MoveTemp(Other.Materials))
		{
		}

		FCollisionData CollisionData;
		FMaterialData Materials;
	};

	namespace Private
	{

		///////////////////////////////////////////////////////////////////////////////////////////////
		///////////////////////////////////////////////////////////////////////////////////////////////
		// 
		// FShapeInstanceExtended
		// 
		///////////////////////////////////////////////////////////////////////////////////////////////
		///////////////////////////////////////////////////////////////////////////////////////////////

		/**
		 * An extended version of FShapeInstance (physics-thread shape instance data) that caches
		 * some world-space state of the shape for use in collision detection. This extended data
		 * if only required for shapes that have a transform relative to the particle they are
		 * attached to. It helps by avoiding the need to recalculate the shape transform every
		 * time it is needed in collision detection, which is once for each other shape we
		 * may be in contact with.
		 * 
		 * NOTE: keep size to a minimum. There can be millions of these in s scene.
		 */
		class CHAOS_API FShapeInstanceExtended : public FShapeInstance
		{
		public:
			friend class FShapeInstance;

			FRigidTransform3 GetWorldTransform() const
			{
				return FRigidTransform3(WorldPosition, WorldRotation);
			}

			void SetWorldTransform(const FRigidTransform3& LeafWorldTransform)
			{
				WorldPosition = LeafWorldTransform.GetTranslation();
				WorldRotation = LeafWorldTransform.GetRotation();
			}

		protected:
			FShapeInstanceExtended(int32 InShapeIdx, TSerializablePtr<FImplicitObject> InGeometry)
				: FShapeInstance(EPerShapeDataType::SimExtended, InShapeIdx, InGeometry)
			{
			}

			FShapeInstanceExtended(FShapeInstance&& PerShapeData)
				: FShapeInstance(EPerShapeDataType::SimExtended, MoveTemp(PerShapeData))
			{
			}

			FVec3 WorldPosition;
			FRotation3 WorldRotation;
		};
	}


	///////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////
	// 
	// Downcasts
	// 
	///////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////

	inline FShapeInstanceProxy* FPerShapeData::AsShapeInstanceProxy()
	{
		if (Type == EPerShapeDataType::Proxy)
		{
			return static_cast<FShapeInstanceProxy*>(this);
		}
		return nullptr;
	}
	
	inline const FShapeInstanceProxy* FPerShapeData::AsShapeInstanceProxy() const
	{
		if (Type == EPerShapeDataType::Proxy)
		{
			return static_cast<const FShapeInstanceProxy*>(this);
		}
		return nullptr;
	}

	inline FShapeInstance* FPerShapeData::AsShapeInstance()
	{
		if (Type != EPerShapeDataType::Proxy)
		{
			return static_cast<FShapeInstance*>(this);
		}
		return nullptr;
	}

	inline const FShapeInstance* FPerShapeData::AsShapeInstance() const
	{
		if (Type != EPerShapeDataType::Proxy)
		{
			return static_cast<const FShapeInstance*>(this);
		}
		return nullptr;
	}

	inline Private::FShapeInstanceExtended* FPerShapeData::AsShapeInstanceExtended()
	{
		if (Type == EPerShapeDataType::SimExtended)
		{
			return static_cast<Private::FShapeInstanceExtended*>(this);
		}
		return nullptr;
	}

	inline const Private::FShapeInstanceExtended* FPerShapeData::AsShapeInstanceExtended() const
	{
		if (Type == EPerShapeDataType::SimExtended)
		{
			return static_cast<const Private::FShapeInstanceExtended*>(this);
		}
		return nullptr;
	}

	template<typename TLambda>
	inline decltype(auto) FPerShapeData::DownCast(const TLambda& Lambda)
	{
		// NOTE: only FShapeInstanceProxy and FShapeInstance implement the full interface
		// FShapeInstanceExtended is a hidden derived type as far as we are concerned here
		if (Type == EPerShapeDataType::Proxy)
		{
			return Lambda(*AsShapeInstanceProxy());
		}
		else
		{
			return Lambda(*AsShapeInstance());
		}
	}

	template<typename TLambda>
	inline  decltype(auto) FPerShapeData::DownCast(const TLambda& Lambda) const
	{
		// See comments in non-const DownCast()
		if (Type == EPerShapeDataType::Proxy)
		{
			return Lambda(*AsShapeInstanceProxy());
		}
		else
		{
			return Lambda(*AsShapeInstance());
		}
	}


	///////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////
	// 
	// FPerShapeData implementation
	// 
	///////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////


	inline void FPerShapeData::UpdateShapeBounds(const FRigidTransform3& WorldTM, const FVec3& BoundsExpansion)
	{
		DownCast([&WorldTM, &BoundsExpansion](auto& ShapeInstance) { ShapeInstance.UpdateShapeBounds(WorldTM, BoundsExpansion); });
	}

	inline void* FPerShapeData::GetUserData() const
	{
		return DownCast([](auto& ShapeInstance) { return ShapeInstance.GetUserData(); });
	}

	inline void FPerShapeData::SetUserData(void* InUserData)
	{
		DownCast([InUserData](auto& ShapeInstance) { ShapeInstance.SetUserData(InUserData); });
	}

	inline const FCollisionFilterData& FPerShapeData::GetQueryData() const
	{
		return DownCast([](const auto& ShapeInstance) -> const auto& { return ShapeInstance.GetQueryData(); });
	}

	inline void FPerShapeData::SetQueryData(const FCollisionFilterData& InQueryData)
	{
		DownCast([&InQueryData](auto& ShapeInstance) { ShapeInstance.SetQueryData(InQueryData); });
	}

	inline const FCollisionFilterData& FPerShapeData::GetSimData() const
	{
		return DownCast([](const auto& ShapeInstance) -> const auto& { return ShapeInstance.GetSimData(); });
	}

	inline void FPerShapeData::SetSimData(const FCollisionFilterData& InSimData)
	{
		return DownCast([&InSimData](auto& ShapeInstance) { ShapeInstance.SetSimData(InSimData); });
	}

	inline TSerializablePtr<FImplicitObject> FPerShapeData::GetGeometry() const
	{
		return Geometry;
	}

	inline const TAABB<FReal, 3>& FPerShapeData::GetWorldSpaceInflatedShapeBounds() const
	{
		return WorldSpaceInflatedShapeBounds;
	}

	inline void FPerShapeData::UpdateWorldSpaceState(const FRigidTransform3& WorldTransform, const FVec3& BoundsExpansion)
	{
		DownCast([&WorldTransform, &BoundsExpansion](auto& ShapeInstance) { ShapeInstance.UpdateWorldSpaceState(WorldTransform, BoundsExpansion); });
	}

	inline const FImplicitObject* FPerShapeData::GetLeafGeometry() const
	{
		return DownCast([](auto& ShapeInstance) { return ShapeInstance.GetLeafGeometry(); });
	}

	inline FRigidTransform3 FPerShapeData::GetLeafRelativeTransform() const
	{
		return DownCast([](auto& ShapeInstance) { return ShapeInstance.GetLeafRelativeTransform(); });
	}

	inline FRigidTransform3 FPerShapeData::GetLeafWorldTransform(const FGeometryParticleHandle* Particle) const
	{
		return DownCast([Particle](auto& ShapeInstance) { return ShapeInstance.GetLeafWorldTransform(Particle); });
	}

	inline void FPerShapeData::UpdateLeafWorldTransform(FGeometryParticleHandle* Particle)
	{
		DownCast([Particle](auto& ShapeInstance) { ShapeInstance.UpdateLeafWorldTransform(Particle); });
	}

	inline const TArray<FMaterialHandle>& FPerShapeData::GetMaterials() const
	{
		return DownCast([](const auto& ShapeInstance) -> const auto& { return ShapeInstance.GetMaterials(); });
	}

	inline const TArray<FMaterialMaskHandle>& FPerShapeData::GetMaterialMasks() const
	{
		return DownCast([](const auto& ShapeInstance) -> const auto& { return ShapeInstance.GetMaterialMasks(); });
	}

	inline const TArray<uint32>& FPerShapeData::GetMaterialMaskMaps() const
	{
		return DownCast([](const auto& ShapeInstance) -> const auto& { return ShapeInstance.GetMaterialMaskMaps(); });
	}

	inline const TArray<FMaterialHandle>& FPerShapeData::GetMaterialMaskMapMaterials() const
	{
		return DownCast([](const auto& ShapeInstance) -> const auto& { return ShapeInstance.GetMaterialMaskMapMaterials(); });
	}

	inline const FShapeDirtyFlags FPerShapeData::GetDirtyFlags() const
	{
		return DownCast([](auto& ShapeInstance) { return ShapeInstance.GetDirtyFlags(); });
	}

	inline void FPerShapeData::SetMaterial(FMaterialHandle InMaterial)
	{
		DownCast([&InMaterial](auto& ShapeInstance) { ShapeInstance.SetMaterial(InMaterial); });
	}

	inline void FPerShapeData::SetMaterials(const TArray<FMaterialHandle>& InMaterials)
	{
		DownCast([&InMaterials](auto& ShapeInstance) { ShapeInstance.SetMaterials(InMaterials); });
	}

	inline void FPerShapeData::SetMaterialMasks(const TArray<FMaterialMaskHandle>& InMaterialMasks)
	{
		DownCast([&InMaterialMasks](auto& ShapeInstance) { ShapeInstance.SetMaterialMasks(InMaterialMasks); });
	}

	inline void FPerShapeData::SetMaterialMaskMaps(const TArray<uint32>& InMaterialMaskMaps)
	{
		DownCast([&InMaterialMaskMaps](auto& ShapeInstance) { ShapeInstance.SetMaterialMaskMaps(InMaterialMaskMaps); });
	}

	inline void FPerShapeData::SetMaterialMaskMapMaterials(const TArray<FMaterialHandle>& InMaterialMaskMapMaterials)
	{
		DownCast([&InMaterialMaskMapMaterials](auto& ShapeInstance) { ShapeInstance.SetMaterialMaskMapMaterials(InMaterialMaskMapMaterials); });
	}

	inline bool FPerShapeData::GetQueryEnabled() const
	{
		return DownCast([](auto& ShapeInstance) { return ShapeInstance.GetQueryEnabled(); });
	}

	inline void FPerShapeData::SetQueryEnabled(const bool bEnable)
	{
		DownCast([bEnable](auto& ShapeInstance) { ShapeInstance.SetQueryEnabled(bEnable); });
	}

	inline bool FPerShapeData::GetSimEnabled() const
	{
		return DownCast([](auto& ShapeInstance) { return ShapeInstance.GetSimEnabled(); });
	}

	inline void FPerShapeData::SetSimEnabled(const bool bEnable)
	{
		DownCast([bEnable](auto& ShapeInstance) { ShapeInstance.SetSimEnabled(bEnable); });
	}

	inline bool FPerShapeData::GetIsProbe() const
	{
		return DownCast([](auto& ShapeInstance) { return ShapeInstance.GetIsProbe(); });
	}

	inline void FPerShapeData::SetIsProbe(const bool bIsProbe)
	{
		DownCast([bIsProbe](auto& ShapeInstance) { ShapeInstance.SetIsProbe(bIsProbe); });
	}

	inline EChaosCollisionTraceFlag FPerShapeData::GetCollisionTraceType() const
	{
		return DownCast([](auto& ShapeInstance) { return ShapeInstance.GetCollisionTraceType(); });
	}

	inline void FPerShapeData::SetCollisionTraceType(const EChaosCollisionTraceFlag InTraceFlag)
	{
		DownCast([InTraceFlag](auto& ShapeInstance) { ShapeInstance.SetCollisionTraceType(InTraceFlag); });
	}

	inline const FCollisionData& FPerShapeData::GetCollisionData() const
	{
		return DownCast([](const auto& ShapeInstance) -> const auto& { return ShapeInstance.GetCollisionData(); });
	}

	inline void FPerShapeData::SetCollisionData(const FCollisionData& Data)
	{
		DownCast([&Data](auto& ShapeInstance) { ShapeInstance.SetCollisionData(Data); });
	}

	inline const FMaterialData& FPerShapeData::GetMaterialData() const
	{
		return DownCast([](auto& ShapeInstance) -> const auto& { return ShapeInstance.GetMaterialData(); });
	}

	inline void FPerShapeData::SetMaterialData(const FMaterialData& Data)
	{
		DownCast([&Data](auto& ShapeInstance) { ShapeInstance.SetMaterialData(Data); });
	}

	inline void FPerShapeData::SyncRemoteData(FDirtyPropertiesManager& Manager, int32 ShapeDataIdx, FShapeDirtyData& RemoteData)
	{
		DownCast([&Manager, ShapeDataIdx, &RemoteData](auto& ShapeInstance) { ShapeInstance.SyncRemoteData(Manager, ShapeDataIdx, RemoteData); });
	}

	inline void FPerShapeData::SetProxy(IPhysicsProxyBase* InProxy)
	{
		DownCast([InProxy](auto& ShapeInstance) { ShapeInstance.SetProxy(InProxy); });
	}

	inline int32 FPerShapeData::GetShapeIndex() const
	{
		return ShapeIdx;
	}

	inline void FPerShapeData::ModifyShapeIndex(int32 NewShapeIndex)
	{
		ShapeIdx = NewShapeIndex;
	}

	template <typename Lambda> 
	void FPerShapeData::ModifySimData(const Lambda& LambdaFunc)
	{
		DownCast([&LambdaFunc](auto& ShapeInstance) { ShapeInstance.ModifySimData(LambdaFunc); });
	}

	template <typename Lambda> 
	void FPerShapeData::ModifyMaterials(const Lambda& LambdaFunc)
	{
		DownCast([&LambdaFunc](auto& ShapeInstance) { ShapeInstance.ModifyMaterials(LambdaFunc); });
	}

	template <typename Lambda>
	void FPerShapeData::ModifyMaterialMasks(const Lambda& LambdaFunc)
	{
		DownCast([&LambdaFunc](auto& ShapeInstance) { ShapeInstance.ModifyMaterialMasks(LambdaFunc); });
	}

	template <typename Lambda> 
	void FPerShapeData::ModifyMaterialMaskMaps(const Lambda& LambdaFunc)
	{
		DownCast([&LambdaFunc](auto& ShapeInstance) { ShapeInstance.ModifyMaterialMaskMaps(LambdaFunc); });
	}

	template <typename Lambda> 
	void FPerShapeData::ModifyMaterialMaskMapMaterials(const Lambda& LambdaFunc)
	{
		DownCast([&LambdaFunc](auto& ShapeInstance) { ShapeInstance.ModifyMaterialMaskMapMaterials(LambdaFunc); });
	}


	///////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////
	// 
	// Misc stuff
	// 
	///////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////


	inline FChaosArchive& operator<<(FChaosArchive& Ar, FPerShapeData& Shape)
	{
		Shape.Serialize(Ar);
		return Ar;
	}

	UE_DEPRECATED(5.3, "Not for external use")
	void CHAOS_API UpdateShapesArrayFromGeometry(FShapesArray& ShapesArray, TSerializablePtr<FImplicitObject> Geometry, const FRigidTransform3& ActorTM, IPhysicsProxyBase* Proxy);

}