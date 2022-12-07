// Copyright Epic Games, Inc. All Rights Reserved.

#include "Chaos/PhysicsObjectInterface.h"
#include "Chaos/PhysicsObjectInternal.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "Math/Transform.h"

namespace
{
	template<Chaos::EThreadContext Id>
	void SetParticleStateHelper(Chaos::FPhysicsObjectHandle PhysicsObject, Chaos::EObjectStateType State)
	{
		if (!PhysicsObject)
		{
			return;
		}

		IPhysicsProxyBase* Proxy = PhysicsObject->PhysicsProxy();
		Chaos::TThreadParticle<Id>* Particle = PhysicsObject->GetParticle<Id>();
		if (!Particle || !Proxy)
		{
			return;
		}

		if (Chaos::TThreadRigidParticle<Id>* Rigid = Particle->CastToRigidParticle())
		{
			if constexpr (Id == Chaos::EThreadContext::External)
			{
				if (Proxy->GetType() == EPhysicsProxyType::SingleParticleProxy)
				{
					// Easiest way to maintain the same behavior as what we currently have for the single particle case on the game thread.
					static_cast<Chaos::FSingleParticlePhysicsProxy*>(Proxy)->GetGameThreadAPI().SetObjectState(State);
				}
				else
				{
					Rigid->SetObjectState(State, false, false);

					// In the case of the geometry collection, it won't marshal the state from the game thread to the physics thread
					// so we need to do it for it manually. 
					if (Proxy->GetType() == EPhysicsProxyType::GeometryCollectionType)
					{
						if (Chaos::FPhysicsSolverBase* Solver = Proxy->GetSolverBase())
						{
							Solver->EnqueueCommandImmediate(
								[PhysicsObject, State]() {
									SetParticleStateHelper<Chaos::EThreadContext::Internal>(PhysicsObject, State);
								}
							);
						}
					}
				}
			}
			else
			{
				if (Chaos::FPBDRigidsSolver* Solver = Proxy->GetSolver<Chaos::FPBDRigidsSolver>())
				{
					if (Chaos::FPBDRigidsSolver::FPBDRigidsEvolution* Evolution = Solver->GetEvolution())
					{
						Evolution->SetParticleObjectState(Rigid, State);
					}
				}
			}
		}
	}
}

FName FClosestPhysicsObjectResult::HitName() const
{
	if (!PhysicsObject)
	{
		return NAME_None;
	}
	return Chaos::FPhysicsObjectInterface::GetName(PhysicsObject);
}

namespace Chaos
{
	template<EThreadContext Id>
	FPhysicsObjectHandle FReadPhysicsObjectInterface<Id>::GetRootObject(FPhysicsObjectHandle Object)
	{
		if (!Object)
		{
			return nullptr;
		}

		return Object->GetRootObject<Id>();
	}

	template<EThreadContext Id>
	FTransform FReadPhysicsObjectInterface<Id>::GetTransform(FPhysicsObjectHandle Object)
	{
		return FTransform{ GetR(Object), GetX(Object) };
	}

	template<EThreadContext Id>
	FVector FReadPhysicsObjectInterface<Id>::GetX(FPhysicsObjectHandle Object)
	{
		if (!Object)
		{
			return FVector::Zero();
		}

		if (TThreadParticle<Id>* Particle = Object->GetParticle<Id>())
		{
			return Particle->X();
		}

		return FVector::Zero();
	}

	template<EThreadContext Id>
	FVector FReadPhysicsObjectInterface<Id>::GetCoM(FPhysicsObjectHandle Object)
	{
		if (!Object)
		{
			return FVector::Zero();
		}

		if (TThreadParticle<Id>* Particle = Object->GetParticle<Id>())
		{
			if (Chaos::TThreadRigidParticle<Id>* Rigid = Particle->CastToRigidParticle())
			{
				return Rigid->CenterOfMass();
			}
		}

		return FVector::Zero();
	}

	template<EThreadContext Id>
	FVector FReadPhysicsObjectInterface<Id>::GetWorldCoM(FPhysicsObjectHandle Object)
	{
		return GetX(Object) + GetR(Object).RotateVector(GetCoM(Object));
	}

	template<EThreadContext Id>
	FQuat FReadPhysicsObjectInterface<Id>::GetR(FPhysicsObjectHandle Object)
	{
		if (!Object)
		{
			return FQuat::Identity;
		}

		if (TThreadParticle<Id>* Particle = Object->GetParticle<Id>())
		{
			return Particle->R();
		}

		return FQuat::Identity;
	}

	template<EThreadContext Id>
	bool FReadPhysicsObjectInterface<Id>::AreAllValid(TArrayView<FPhysicsObjectHandle> InObjects)
	{
		bool bCheck = !InObjects.IsEmpty();
		for (FPhysicsObjectHandle Object : InObjects)
		{
			bCheck &= (Object != nullptr && Object->IsValid());
		}
		return bCheck;
	}

	template<EThreadContext Id>
	bool FReadPhysicsObjectInterface<Id>::AreAllKinematic(TArrayView<FPhysicsObjectHandle> InObjects)
	{
		bool bCheck = !InObjects.IsEmpty();
		for (FPhysicsObjectHandle Object : InObjects)
		{
			bCheck &= (Object && Object->IsValid() && Object->ObjectState<Id>() == EObjectStateType::Kinematic);
		}
		return bCheck;
	}

	template<EThreadContext Id>
	bool FReadPhysicsObjectInterface<Id>::AreAllSleeping(TArrayView<FPhysicsObjectHandle> InObjects)
	{
		bool bCheck = !InObjects.IsEmpty();
		for (FPhysicsObjectHandle Object : InObjects)
		{
			bCheck &= (Object && Object->IsValid() && Object->ObjectState<Id>() == EObjectStateType::Sleeping);
		}
		return bCheck;
	}

	template<EThreadContext Id>
	bool FReadPhysicsObjectInterface<Id>::AreAllRigidBody(TArrayView<FPhysicsObjectHandle> InObjects)
	{
		bool bCheck = !InObjects.IsEmpty();
		for (FPhysicsObjectHandle Object : InObjects)
		{
			bCheck &= (Object && Object->IsValid() && Object->ObjectState<Id>() != EObjectStateType::Static);
		}
		return bCheck;
	}

	template<EThreadContext Id>
	bool FReadPhysicsObjectInterface<Id>::AreAllDynamic(TArrayView<FPhysicsObjectHandle> InObjects)
	{
		bool bCheck = !InObjects.IsEmpty();
		for (FPhysicsObjectHandle Object : InObjects)
		{
			bCheck &= (Object && Object->IsValid() && Object->ObjectState<Id>() == EObjectStateType::Dynamic);
		}
		return bCheck;
	}

	template<EThreadContext Id>
	bool FReadPhysicsObjectInterface<Id>::AreAllDisabled(TArrayView<FPhysicsObjectHandle> InObjects)
	{
		bool bDisabled = !InObjects.IsEmpty();
		for (FPhysicsObjectHandle Object : InObjects)
		{
			bool bParticleDisabled = false;
			if (Object)
			{
				if (TThreadParticle<Id>* Particle = Object->GetParticle<Id>())
				{
					bParticleDisabled = FPhysicsObject::IsParticleDisabled<Id>(Particle);
				}
			}
			bDisabled &= bParticleDisabled;
		}
		return bDisabled;
	}

	template<EThreadContext Id>
	float FReadPhysicsObjectInterface<Id>::GetMass(TArrayView<FPhysicsObjectHandle> InObjects)
	{
		float Mass = 0.f;
		for (FPhysicsObjectHandle Object : InObjects)
		{
			if (Object)
			{
				if (TThreadParticle<Id>* Particle = Object->GetParticle<Id>())
				{
					if (TThreadRigidParticle<Id>* Rigid = Particle->CastToRigidParticle())
					{
						Mass += static_cast<float>(Rigid->M());
					}
				}
			}
		}
		return Mass;
	}

	template<EThreadContext Id>
	FBox FReadPhysicsObjectInterface<Id>::GetBounds(TArrayView<FPhysicsObjectHandle> InObjects)
	{
		FBox RetBox(ForceInit);
		for (FPhysicsObjectHandle Object : InObjects)
		{
			if (!Object)
			{
				continue;
			}

			const TThreadParticle<Id>* Particle = Object->GetParticle<Id>();
			if (!Particle)
			{
				continue;
			}

			FBox ParticleBox(ForceInit);
			if (const FImplicitObject* Geometry = Particle->Geometry().Get(); Geometry && Geometry->HasBoundingBox())
			{
				Chaos::FAABB3 Box = Geometry->BoundingBox();
				ParticleBox = FBox{ Box.Min(), Box.Max() };
			}

			if (ParticleBox.IsValid)
			{
				RetBox += ParticleBox;
			}
		}
		return RetBox;
	}

	template<EThreadContext Id>
	FClosestPhysicsObjectResult FReadPhysicsObjectInterface<Id>::GetClosestPhysicsBodyFromLocation(TArrayView<FPhysicsObjectHandle> InObjects, const FVector& WorldLocation)
	{
		FClosestPhysicsObjectResult AggregateResult;
		for (FPhysicsObjectHandle Object : InObjects)
		{
			if (!Object)
			{
				continue;
			}

			const TThreadParticle<Id>* Particle = Object->GetParticle<Id>();
			if (!Particle)
			{
				continue;
			}

			const FTransform WorldTransform = GetTransform(Object);
			const FVector LocalLocation = WorldTransform.InverseTransformPosition(WorldLocation);

			FClosestPhysicsObjectResult Result;

			if (const FImplicitObject* Geometry = Particle->Geometry().Get())
			{
				Result.PhysicsObject = Object;

				Chaos::FVec3 Normal;
				Result.ClosestDistance = static_cast<double>(Geometry->PhiWithNormal(LocalLocation, Normal));
				Result.ClosestLocation = WorldTransform.TransformPosition(LocalLocation - Result.ClosestDistance * Normal);
			}

			if (!Result)
			{
				continue;
			}

			if (!AggregateResult || Result.ClosestDistance < AggregateResult.ClosestDistance)
			{
				AggregateResult = Result;
			}
		}
		return AggregateResult;
	}

	template<EThreadContext Id>
	void FWritePhysicsObjectInterface<Id>::PutToSleep(TArrayView<FPhysicsObjectHandle> InObjects)
	{
		for (FPhysicsObjectHandle Object : InObjects)
		{
			if (!Object)
			{
				continue;
			}

			EObjectStateType State = Object->ObjectState<Id>();
			if (State == EObjectStateType::Dynamic || State == EObjectStateType::Sleeping)
			{
				SetParticleStateHelper<Id>(Object, EObjectStateType::Sleeping);
			}
		}
	}

	template<EThreadContext Id>
	void FWritePhysicsObjectInterface<Id>::WakeUp(TArrayView<FPhysicsObjectHandle> InObjects)
	{
		for (FPhysicsObjectHandle Object : InObjects)
		{
			if (!Object)
			{
				continue;
			}

			if (TThreadParticle<Id>* Particle = Object->GetParticle<Id>())
			{
				EObjectStateType State = Object->ObjectState<Id>();
				if (State == EObjectStateType::Dynamic || State == EObjectStateType::Sleeping)
				{
					SetParticleStateHelper<Id>(Object, EObjectStateType::Dynamic);
					if constexpr (Id == EThreadContext::External)
					{
						if (TThreadRigidParticle<Id>* Rigid = Particle->CastToRigidParticle())
						{
							Rigid->ClearEvents();
						}
					}
				}
			}
		}
	}

	template<EThreadContext Id>
	void FWritePhysicsObjectInterface<Id>::AddForce(TArrayView<FPhysicsObjectHandle> InObjects, const FVector& Force, bool bInvalidate)
	{
		for (FPhysicsObjectHandle Object : InObjects)
		{
			if (!Object)
			{
				continue;
			}

			if (TThreadParticle<Id>* Particle = Object->GetParticle<Id>())
			{
				if (Chaos::TThreadRigidParticle<Id>* Rigid = Particle->CastToRigidParticle())
				{
					if (Rigid->ObjectState() == EObjectStateType::Sleeping || Rigid->ObjectState() == EObjectStateType::Dynamic)
					{
						if (bInvalidate)
						{
							SetParticleStateHelper<Id>(Object, EObjectStateType::Dynamic);
						}

						Rigid->AddForce(Force, bInvalidate);
					}
				}
			}
		}
	}

	template<EThreadContext Id>
	void FWritePhysicsObjectInterface<Id>::AddTorque(TArrayView<FPhysicsObjectHandle> InObjects, const FVector& Torque, bool bInvalidate)
	{
		for (FPhysicsObjectHandle Object : InObjects)
		{
			if (!Object)
			{
				continue;
			}

			if (TThreadParticle<Id>* Particle = Object->GetParticle<Id>())
			{
				if (Chaos::TThreadRigidParticle<Id>* Rigid = Particle->CastToRigidParticle())
				{
					if (Rigid->ObjectState() == EObjectStateType::Sleeping || Rigid->ObjectState() == EObjectStateType::Dynamic)
					{
						if (bInvalidate)
						{
							SetParticleStateHelper<Id>(Object, EObjectStateType::Dynamic);
						}

						Rigid->AddTorque(Torque, bInvalidate);
					}
				}
			}
		}
	}

	bool FPhysicsObjectInterface::HasChildren(FPhysicsObjectHandle Object)
	{
		return Object ? Object->HasChildren() : false;
	}

	void FPhysicsObjectInterface::SetName(FPhysicsObjectHandle Object, const FName& InName)
	{
		if (!Object)
		{
			return;
		}

		Object->SetName(InName);
	}

	FName FPhysicsObjectInterface::GetName(FPhysicsObjectHandle Object)
	{
		if (!Object)
		{
			return NAME_None;
		}

		return Object->GetBodyName();
	}

	void FPhysicsObjectInterface::SetId(FPhysicsObjectHandle Object, int32 InId)
	{
		if (!Object)
		{
			return;
		}

		Object->SetBodyIndex(InId);
	}

	int32 FPhysicsObjectInterface::GetId(FPhysicsObjectHandle Object)
	{
		if (!Object)
		{
			return INDEX_NONE;
		}

		return Object->GetBodyIndex();
	}

	FPBDRigidsSolver* FPhysicsObjectInterface::GetSolver(TArrayView<FPhysicsObjectHandle> InObjects)
	{
		FPBDRigidsSolver* RetSolver = nullptr;
		for (FPhysicsObjectHandle Object : InObjects)
		{
			if (!Object)
			{
				continue;
			}

			FPBDRigidsSolver* Solver = nullptr;
			if (IPhysicsProxyBase* Proxy = Object->PhysicsProxy())
			{
				Solver = Proxy->GetSolver<FPBDRigidsSolver>();
			}

			if (!Solver)
			{
				return nullptr;
			}
			else if (!RetSolver)
			{
				RetSolver = Solver;
			}
			else if (Solver != RetSolver)
			{
				return nullptr;
			}
		}
		return RetSolver;
	}

	IPhysicsProxyBase* FPhysicsObjectInterface::GetProxy(TArrayView<FPhysicsObjectHandle> InObjects)
	{
		IPhysicsProxyBase* RetProxy = nullptr;
		for (FPhysicsObjectHandle Object : InObjects)
		{
			if (!Object)
			{
				continue;
			}

			IPhysicsProxyBase* Proxy = Object->PhysicsProxy();
			if (!Proxy)
			{
				return nullptr;
			}
			else if (!RetProxy)
			{
				RetProxy = Proxy;
			}
			else if (Proxy != RetProxy)
			{
				return nullptr;
			}
		}
		return RetProxy;
	}

	template class FReadPhysicsObjectInterface<EThreadContext::External>;
	template class FReadPhysicsObjectInterface<EThreadContext::Internal>;

	template class FWritePhysicsObjectInterface<EThreadContext::External>;
	template class FWritePhysicsObjectInterface<EThreadContext::Internal>;
}