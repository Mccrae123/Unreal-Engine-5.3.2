// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once
#include "Box.h"
#include "Chaos/ExternalCollisionData.h"
#include "Chaos/ParticleHandleFwd.h"
#include "Chaos/PBDConstraintContainer.h"
#include "Chaos/Vector.h"

class UPhysicalMaterial;

namespace Chaos
{

	template<typename T, int d>
	class TPBDCollisionConstraints;

	template<typename  T, int d>
	class TPBDCollisionConstraintHandle;

	template<typename T, int d>
	class TRigidBodyPointContactConstraint;



	/** Specifies the type of work we should do*/
	enum class CHAOS_API ECollisionUpdateType
	{
		Any,	//stop if we have at least one deep penetration. Does not compute location or normal
		Deepest	//find the deepest penetration. Compute location and normal
	};

	/** Return value of the collision modification callback */
	enum class CHAOS_API ECollisionModifierResult
	{
		Unchanged,	/** No change to the collision */
		Modified,	/** Modified the collision, but want it to remain enabled */
		Disabled,	/** Collision should be disabled */
	};

	/** The shape types involved in a contact constraint. Used to look up the collision detection function */
	enum class CHAOS_API EContactShapesType
	{
		Unknown,
		CapsuleCapsule,
		CapsuleBox,
		BoxBox,
	};

	/*
	*
	*/
	template<class T, int d>
	class CHAOS_API TCollisionContact
	{
	public:
		TCollisionContact(const FImplicitObject* InImplicit0 = nullptr, const FImplicitObject* InImplicit1 = nullptr)
			: bDisabled(true), Normal(0), Location(0), Phi(FLT_MAX), Friction(0), AngularFriction(0), Restitution(0), ShapesType(EContactShapesType::Unknown)
		{
			Implicit[0] = InImplicit0;
			Implicit[1] = InImplicit1;
		}

		bool bDisabled;
		TVector<T, d> Normal;
		TVector<T, d> Location;
		T Phi;

		T Friction;
		T AngularFriction;
		T Restitution;

		EContactShapesType ShapesType;


		FString ToString() const
		{
			return FString::Printf(TEXT("Location:%s, Normal:%s, Phi:%f"), *Location.ToString(), *Normal.ToString(), Phi);
		}

		const FImplicitObject* Implicit[2]; // {Of Particle[0], Of Particle[1]}
	};
	typedef TCollisionContact<float, 3> FCollisionContact;

	/*
	*
	*/
	template<class T = float, int d = 3>
	class CHAOS_API TCollisionConstraintBase
	{
	public:
		using FGeometryParticleHandle = TGeometryParticleHandle<T, d>;
		using FManifold = TCollisionContact<T, d>;

		enum class FType
		{
			None = 0,     // default value also indicates a invalid constraint
			SinglePoint,  //   TRigidBodyPointContactConstraint
			MultiPoint    //   TRigidBodyMultiPointContactConstraint
		};

		TCollisionConstraintBase(FType InType = FType::None)
			: AccumulatedImpulse(0)
			, Timestamp(-INT_MAX)
			, Type(InType)
		{ 
			ImplicitTransform[0] = TRigidTransform<T, d>::Identity; ImplicitTransform[1] = TRigidTransform<T,d>::Identity;
			Manifold.Implicit[0] = nullptr; Manifold.Implicit[1] = nullptr;
			Particle[0] = nullptr; Particle[1] = nullptr; 
		}
		
		TCollisionConstraintBase(
			FGeometryParticleHandle* Particle0, const FImplicitObject* Implicit0, const TRigidTransform<T, d>& Transform0,
			FGeometryParticleHandle* Particle1, const FImplicitObject* Implicit1, const TRigidTransform<T, d>& Transform1,
			FType InType, EContactShapesType ShapesType, int32 InTimestamp = -INT_MAX)
			: AccumulatedImpulse(0)
			, Timestamp(InTimestamp)
			, Type(InType)
		{
			ImplicitTransform[0] = Transform0; ImplicitTransform[1] = Transform1;
			Manifold.Implicit[0] = Implicit0; Manifold.Implicit[1] = Implicit1;
			Manifold.ShapesType = ShapesType;
			Particle[0] = Particle0; Particle[1] = Particle1; 
		}

		FType GetType() const { return Type; }

		template<class AS_T> AS_T * As() { return static_cast<AS_T*>(this); }
		template<class AS_T> const AS_T * As() const { return static_cast<const AS_T*>(this); }

		bool ContainsManifold(const FImplicitObject* A, const FImplicitObject* B) const { return A == Manifold.Implicit[0] && B == Manifold.Implicit[1]; }
		void SetManifold(const FImplicitObject* A, const FImplicitObject* B) { Manifold.Implicit[0] = A; Manifold.Implicit[1] = B; }

		//
		// API
		//

		void ResetPhi(T InPhi) { SetPhi(InPhi); }

		void SetPhi(T InPhi) { Manifold.Phi = InPhi; }
		T GetPhi() const { return Manifold.Phi; }

		void SetDisabled(bool bInDisabled) { Manifold.bDisabled = bInDisabled; }
		TVector<T, d> GetDisabled() const { return Manifold.bDisabled; }

		void SetNormal(const TVector<T, d> & InNormal) { Manifold.Normal = InNormal; }
		TVector<T, d> GetNormal() const { return Manifold.Normal; }

		void SetLocation(const TVector<T, d> & InLocation) { Manifold.Location = InLocation; }
		TVector<T, d> GetLocation() const { return Manifold.Location; }


		FString ToString() const
		{
			return FString::Printf(TEXT("Particle:%s, Levelset:%s, AccumulatedImpulse:%s"), *Particle[0]->ToString(), *Particle[1]->ToString(), *AccumulatedImpulse.ToString());
		}


		TRigidTransform<T, d> ImplicitTransform[2]; // { Point, Volume }
		FGeometryParticleHandle* Particle[2]; // { Point, Volume }
		TVector<T, d> AccumulatedImpulse;
		FManifold Manifold;
		int32 Timestamp;

	private:

		FType Type;
	};
	typedef TCollisionConstraintBase<float, 3> FCollisionConstraintBase;


	/*
	*
	*/
	template<class T, int d>
	class CHAOS_API TRigidBodyPointContactConstraint : public TCollisionConstraintBase<T, d>
	{
	public:
		using Base = TCollisionConstraintBase<T, d>;
		using FGeometryParticleHandle = TGeometryParticleHandle<T, d>;
		using Base::Particle;

		TRigidBodyPointContactConstraint() : Base(Base::FType::SinglePoint) {}
		TRigidBodyPointContactConstraint(
			FGeometryParticleHandle* Particle0, const FImplicitObject* Implicit0, const TRigidTransform<T, d>& Transform0,
			FGeometryParticleHandle* Particle1, const FImplicitObject* Implicit1, const TRigidTransform<T, d>& Transform1,
			EContactShapesType ShapesType = EContactShapesType::Unknown)
			: Base(Particle0, Implicit0, Transform0, Particle1, Implicit1, Transform1, Base::FType::SinglePoint, ShapesType) {}

		static typename Base::FType StaticType() { return Base::FType::SinglePoint; };
	};
	typedef TRigidBodyPointContactConstraint<float, 3> FRigidBodyPointContactConstraint;


	/*
	*
	*/
	template<class T, int d>
	class CHAOS_API TRigidBodyMultiPointContactConstraint : public TCollisionConstraintBase<T, d>
	{
	public:
		using Base = TCollisionConstraintBase<T, d>;
		using FGeometryParticleHandle = TGeometryParticleHandle<T, d>;
		using FManifold = TCollisionContact<T, d>;
		using Base::Particle;

		TRigidBodyMultiPointContactConstraint() : Base(Base::FType::MultiPoint), PlaneOwnerIndex(INDEX_NONE), PlaneFaceIndex(INDEX_NONE), PlaneNormal(0), PlanePosition(0) {}
		TRigidBodyMultiPointContactConstraint(
			FGeometryParticleHandle* Particle0, const FImplicitObject* Implicit0, const TRigidTransform<T, d>& Transform0,
			FGeometryParticleHandle* Particle1, const FImplicitObject* Implicit1, const TRigidTransform<T, d>& Transform1,
			EContactShapesType ShapesType = EContactShapesType::Unknown)
			: Base(Particle0, Implicit0, Transform0, Particle1, Implicit1, Transform1, Base::FType::MultiPoint, ShapesType)
			, PlaneOwnerIndex(INDEX_NONE), PlaneFaceIndex(INDEX_NONE), PlaneNormal(0), PlanePosition(0)
		{}

		static typename Base::FType StaticType() { return Base::FType::MultiPoint; };

		// Get the particle that owns the plane
		FGeometryParticleHandle* PlaneParticleHandle() const { check(PlaneOwnerIndex >= 0 && PlaneOwnerIndex < 2); return Particle[PlaneOwnerIndex]; }

		// Get the particle that owns the manifold sample points
		FGeometryParticleHandle* PointsParticleHandle() const { check(PlaneOwnerIndex >= 0 && PlaneOwnerIndex < 2); return Particle[1 - PlaneOwnerIndex]; }

		int32 GetManifoldPlaneOwnerIndex() const { return PlaneOwnerIndex; }
		int32 GetManifoldPlaneFaceIndex() const { return PlaneFaceIndex; }
		const FVec3& GetManifoldPlaneNormal() const { return PlaneNormal; }
		const FVec3& GetManifoldPlanePosition() const { return PlanePosition; }
		void SetManifoldPlane(int32 OwnerIndex, int32 FaceIndex, const FVec3& Normal, const FVec3& Pos)
		{
			PlaneOwnerIndex = OwnerIndex;
			PlaneFaceIndex = FaceIndex;
			PlaneNormal = Normal;
			PlanePosition = Pos;
		}

		int32               NumManifoldPoints() const { return Points.Num(); }
		const FVec3&		GetManifoldPoint(int32 Index) const { check(0 <= Index && Index < NumManifoldPoints()); return Points[Index]; }
		void				SetManifoldPoint(int32 Index, const FVec3& Point) { check(0 <= Index && Index < NumManifoldPoints()); Points[Index] = Point; }
		void                AddManifoldPoint(const FVec3& Point) { Points.Add(Point); };
		void                ResetManifoldPoints(int32 NewSize=0)  { Points.Reset(NewSize); }

	private:
		// Manifold plane data
		int PlaneOwnerIndex; // index of particle which owns the plane. The other owns the sample positions
		int PlaneFaceIndex; // index of face used as the manifold plane on plane-owner body
		TVector<T, d> PlaneNormal; // local space contact normal on plane-owner
		TVector<T, d> PlanePosition; // local space surface position on plane-owner

		// Manifold point data
		static const int32 MaxPoints = 4;
		TArray<FVec3, TInlineAllocator<MaxPoints>> Points; // @todo(chaos): use TFixedAllocator when we handle building the best manifold properly
	};
	typedef TRigidBodyMultiPointContactConstraint<float, 3> FRigidBodyMultiPointContactConstraint;


	//
	//
	//

	template<class T, int d>
	struct CHAOS_API TRigidBodyContactConstraintPGS
	{
		TRigidBodyContactConstraintPGS() : AccumulatedImpulse(0.f) {}
		TGeometryParticleHandle<T, d>* Particle;
		TGeometryParticleHandle<T, d>* Levelset;
		TArray<TVector<T, d>> Normal;
		TArray<TVector<T, d>> Location;
		TArray<T> Phi;
		TVector<T, d> AccumulatedImpulse;
	};


	template<class T, int d>
	class CHAOS_API TPBDCollisionConstraintHandle : public TContainerConstraintHandle<TPBDCollisionConstraints<T, d>>
	{
	public:
		using Base = TContainerConstraintHandle<TPBDCollisionConstraints<T, d>>;
		using FConstraintContainer = TPBDCollisionConstraints<T, d>;
		using FConstraintBase = TCollisionConstraintBase<T, d>;

		using FImplicitPair = TPair<const FImplicitObject*, const FImplicitObject*>;
		using FGeometryPair = TPair<const TGeometryParticleHandle<T, d>*, const TGeometryParticleHandle<T, d>*>;
		using FHandleKey = TPair<FImplicitPair, FGeometryPair>;


		TPBDCollisionConstraintHandle()
			: ConstraintType(FConstraintBase::FType::None)
		{}

		TPBDCollisionConstraintHandle(FConstraintContainer* InConstraintContainer, int32 InConstraintIndex, typename FConstraintBase::FType InType)
			: TContainerConstraintHandle<TPBDCollisionConstraints<T, d>>(InConstraintContainer, InConstraintIndex)
			, ConstraintType(InType)
		{
		}

		FHandleKey GetKey()
		{
			const TCollisionConstraintBase<T, d>& Contact = GetContact();
			return FHandleKey(
				FImplicitPair( Contact.Manifold.Implicit[0],Contact.Manifold.Implicit[1]),
				FGeometryPair( Contact.Particle[0], Contact.Particle[1] ) );
		}

		static FHandleKey MakeKey(const TGeometryParticleHandle<T, d>* Particle0, const TGeometryParticleHandle<T, d>* Particle1, 
			const FImplicitObject* Implicit0, const FImplicitObject* Implicit1)
		{
			return FHandleKey(FImplicitPair(Implicit0,Implicit1), FGeometryPair(Particle0, Particle1));
		}

		static FHandleKey MakeKey(const TCollisionConstraintBase<T,d> * Base)
		{
			return FHandleKey(FImplicitPair(Base->Manifold.Implicit[0], Base->Manifold.Implicit[1]), FGeometryPair(Base->Particle[0], Base->Particle[1]));
		}


		const TCollisionConstraintBase<T, d>& GetContact() const 
		{ 
			if (GetType() == FConstraintBase::FType::SinglePoint)
			{
				return ConstraintContainer->PointConstraints[ConstraintIndex];
			}
			check(GetType() == FConstraintBase::FType::MultiPoint);
			return ConstraintContainer->IterativeConstraints[ConstraintIndex];
		}

		TCollisionConstraintBase<T, d>& GetContact()
		{
			if (GetType() == FConstraintBase::FType::SinglePoint)
			{
				return ConstraintContainer->PointConstraints[ConstraintIndex];
			}
			check(GetType() == FConstraintBase::FType::MultiPoint);
			return ConstraintContainer->IterativeConstraints[ConstraintIndex];
		}



		const TRigidBodyPointContactConstraint<T, d>& GetPointContact() const { check(GetType() == FConstraintBase::FType::SinglePoint); return ConstraintContainer->PointConstraints[ConstraintIndex]; }
		TRigidBodyPointContactConstraint<T, d>& GetPointContact() { check(GetType() == FConstraintBase::FType::SinglePoint); return ConstraintContainer->PointConstraints[ConstraintIndex]; }

		const TRigidBodyMultiPointContactConstraint<T, d>& GetMultiPointContact() const { check(GetType() == FConstraintBase::FType::MultiPoint); return ConstraintContainer->IterativeConstraints[ConstraintIndex]; }
		TRigidBodyMultiPointContactConstraint<T, d>& GetMultiPointContact() { check(GetType() == FConstraintBase::FType::MultiPoint); return ConstraintContainer->IterativeConstraints[ConstraintIndex]; }

		typename FConstraintBase::FType GetType() const { return ConstraintType; }

		void SetConstraintIndex(int32 IndexIn, typename FConstraintBase::FType InType)
		{
			ConstraintIndex = IndexIn;
			ConstraintType = InType;
		}

		TVector<T, d> GetContactLocation() const
		{
			return GetContact().GetLocation();
		}

		TVector<T, d> GetAccumulatedImpulse() const
		{
			return GetContact().AccumulatedImpulse;
		}

		TVector<const TGeometryParticleHandle<T, d>*, 2> GetConstrainedParticles() const
		{
			return { GetContact().Particle[0], GetContact().Particle[1] };
		}

		TVector<TGeometryParticleHandle<T, d>*, 2> GetConstrainedParticles()
		{
			return { GetContact().Particle[0], GetContact().Particle[1] };
		}


	protected:
		typename FConstraintBase::FType ConstraintType;
		using Base::ConstraintIndex;
		using Base::ConstraintContainer;


	};
	typedef TPBDCollisionConstraintHandle<float, 3> FPBDCollisionConstraintHandle;


	template<int T_MAXCONSTRAINTS>
	struct TCollisionConstraintsStore
	{
		TArray<TRigidBodyPointContactConstraint<FReal, 3>, TFixedAllocator<T_MAXCONSTRAINTS>> SinglePointConstraints;
		TArray<TRigidBodyMultiPointContactConstraint<FReal, 3>, TFixedAllocator<T_MAXCONSTRAINTS>> MultiPointConstraints;

		int32 Num() const { return SinglePointConstraints.Num() + MultiPointConstraints.Num(); }

		void Empty()
		{
			SinglePointConstraints.Empty();
			MultiPointConstraints.Empty();
		}

		TRigidBodyPointContactConstraint<FReal, 3>* TryAdd(FReal MaxPhi, const TRigidBodyPointContactConstraint<FReal, 3>& C)
		{
			if ((SinglePointConstraints.Num() < T_MAXCONSTRAINTS) && (C.GetPhi() < MaxPhi))
			{
				int32 ConstraintIndex = SinglePointConstraints.Add(C);
				return &SinglePointConstraints[ConstraintIndex];
			}
			return nullptr;
		}

		TRigidBodyMultiPointContactConstraint<FReal, 3>* TryAdd(FReal MaxPhi, const TRigidBodyMultiPointContactConstraint<FReal, 3>& C)
		{
			if ((MultiPointConstraints.Num() < T_MAXCONSTRAINTS) && (C.GetPhi() < MaxPhi))
			{
				int32 ConstraintIndex = MultiPointConstraints.Add(C);
				return &MultiPointConstraints[ConstraintIndex];
			}
			return nullptr;
		}
	};

	using FCollisionConstraintsArray = TCollisionConstraintsStore<8>;
}
