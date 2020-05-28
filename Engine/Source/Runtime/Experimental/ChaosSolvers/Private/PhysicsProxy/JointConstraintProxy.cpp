// Copyright Epic Games, Inc. All Rights Reserved.

#include "PhysicsProxy/JointConstraintProxy.h"

#include "ChaosStats.h"
#include "Chaos/ErrorReporter.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/Serializable.h"
#include "Chaos/PBDJointConstraints.h"
#include "Chaos/Framework/MultiBufferResource.h"
#include "PhysicsSolver.h"

template< class CONSTRAINT_TYPE >
TJointConstraintProxy<CONSTRAINT_TYPE>::TJointConstraintProxy(CONSTRAINT_TYPE* InConstraint, TJointConstraintProxy<CONSTRAINT_TYPE>::FConstraintHandle* InHandle, UObject* InOwner, Chaos::FPBDJointSettings InInitialState)
	: Base(InOwner)
	, InitialState(InInitialState)
	, Constraint(InConstraint)
	, Handle(InHandle)
	, bInitialized(false)
{
	Constraint->SetProxy(this);
}


template< class CONSTRAINT_TYPE >
TJointConstraintProxy<CONSTRAINT_TYPE>::~TJointConstraintProxy()
{
}

template < >
template < >
void TJointConstraintProxy<Chaos::FJointConstraint>::InitializeOnPhysicsThread(Chaos::TPBDRigidsSolver<Chaos::FNonRewindableEvolutionTraits>* InSolver)
{
	check(false);
}


template < >
template < >
void TJointConstraintProxy<Chaos::FJointConstraint>::DestroyOnPhysicsThread(Chaos::TPBDRigidsSolver<Chaos::FNonRewindableEvolutionTraits>* RBDSolver)
{
	// @todo(JointConstraint): Remove a constraint
}

template < >
template < >
void TJointConstraintProxy<Chaos::FJointConstraint>::InitializeOnPhysicsThread(Chaos::TPBDRigidsSolver<Chaos::FRewindableEvolutionTraits>* InSolver)
{
	auto& Handles = InSolver->GetParticles().GetParticleHandles();
	if (Handles.Size() && IsValid())
	{
		auto& JointConstraints = InSolver->GetEvolution()->GetJointConstraints();
		if (Constraint != nullptr)
		{
			auto Particles = Constraint->GetJointParticles();
			if (Particles[0] && Particles[0]->Handle())
			{
				if (Particles[1] && Particles[1]->Handle())
				{
					const auto& ParticleHandle0 = Particles[0]->Handle();
					const auto& ParticleHandle1 = Particles[1]->Handle();
					FTransform Particle0TM = FTransform(ParticleHandle0->R(), ParticleHandle0->X());
					FTransform Particle1TM = FTransform(ParticleHandle1->R(), ParticleHandle1->X());

					FVector JointWorldPosition = (Constraint->GetJointTransforms()[0] * Particle0TM).GetTranslation();
					FQuat JointRelativeRotation = Particle0TM.GetRelativeTransform(Particle1TM).GetRotation();

					Constraint->SetTransform(FTransform(JointRelativeRotation, JointWorldPosition));

					Handle = JointConstraints.AddConstraint({ Particles[0]->Handle() , Particles[1]->Handle() }, Constraint->GetTransform());
				}
			}
		}
	}
}


template < >
template < >
void TJointConstraintProxy<Chaos::FJointConstraint>::DestroyOnPhysicsThread(Chaos::TPBDRigidsSolver<Chaos::FRewindableEvolutionTraits>* RBDSolver)
{
	// @todo(chaos) : Implement
}


template<>
EPhysicsProxyType TJointConstraintProxy<Chaos::FJointConstraint>::ConcreteType()
{ 
	return EPhysicsProxyType::JointConstraintType;
}



template class TJointConstraintProxy< Chaos::FJointConstraint >;

