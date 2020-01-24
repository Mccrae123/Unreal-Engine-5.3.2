// Copyright Epic Games, Inc. All Rights Reserved.

#include "Chaos/PBDCollisionConstraintsContact.h"
#include "Chaos/CollisionResolution.h"
#include "Chaos/CollisionResolutionUtil.h"
#include "Chaos/Defines.h"
#include "Chaos/Particle/ParticleUtilities.h"
#include "Chaos/Utilities.h"

//#pragma optimize("", off)

namespace Chaos
{
	namespace Collisions
	{
		extern void UpdateManifold(FRigidBodyMultiPointContactConstraint& Constraint, const FReal CullDistance)
		{
			const FRigidTransform3 Transform0 = GetTransform(Constraint.Particle[0]);
			const FRigidTransform3 Transform1 = GetTransform(Constraint.Particle[1]);

			UpdateManifold(Constraint, Transform0, Transform1, CullDistance);
		}

		void Update(FRigidBodyPointContactConstraint& Constraint, const FReal CullDistance)
		{
			const FRigidTransform3 Transform0 = GetTransform(Constraint.Particle[0]);
			const FRigidTransform3 Transform1 = GetTransform(Constraint.Particle[1]);

			Constraint.ResetPhi(CullDistance);
			UpdateConstraint<ECollisionUpdateType::Deepest>(Constraint, Transform0, Transform1, CullDistance);
		}

		void Update(FRigidBodyMultiPointContactConstraint& Constraint, const FReal CullDistance)
		{
			const FRigidTransform3 Transform0 = GetTransform(Constraint.Particle[0]);
			const FRigidTransform3 Transform1 = GetTransform(Constraint.Particle[1]);

			Constraint.ResetPhi(CullDistance);
			UpdateConstraintFromManifold(Constraint, Transform0, Transform1, CullDistance);
		}

		FVec3 ApplyContact(FCollisionContact& Contact,
			TGenericParticleHandle<FReal, 3> Particle0, 
			TGenericParticleHandle<FReal, 3> Particle1,
			const TContactIterationParameters<FReal> & IterationParameters,
			const TContactParticleParameters<FReal> & ParticleParameters)
		{
			FVec3 AccumulatedImpulse(0);

			TPBDRigidParticleHandle<FReal, 3>* PBDRigid0 = Particle0->CastToRigidParticle();
			TPBDRigidParticleHandle<FReal, 3>* PBDRigid1 = Particle1->CastToRigidParticle();

			bool bIsRigidDynamic0 = PBDRigid0 && PBDRigid0->ObjectState() == EObjectStateType::Dynamic;
			bool bIsRigidDynamic1 = PBDRigid1 && PBDRigid1->ObjectState() == EObjectStateType::Dynamic;

			const FVec3 ZeroVector = FVec3(0);
			FVec3 P0 = FParticleUtilities::GetCoMWorldPosition(Particle0);
			FVec3 P1 = FParticleUtilities::GetCoMWorldPosition(Particle1);
			FRotation3 Q0 = FParticleUtilities::GetCoMWorldRotation(Particle0);
			FRotation3 Q1 = FParticleUtilities::GetCoMWorldRotation(Particle1);

			FVec3 VectorToPoint1 = Contact.Location - P0;
			FVec3 VectorToPoint2 = Contact.Location - P1;
			FVec3 Body1Velocity = FParticleUtilities::GetVelocityAtCoMRelativePosition(Particle0, VectorToPoint1);
			FVec3 Body2Velocity = FParticleUtilities::GetVelocityAtCoMRelativePosition(Particle1, VectorToPoint2);
			FVec3 RelativeVelocity = Body1Velocity - Body2Velocity;
			FReal RelativeNormalVelocity = FVec3::DotProduct(RelativeVelocity, Contact.Normal);

			if (RelativeNormalVelocity < 0) // ignore separating constraints
			{
				FMatrix33 WorldSpaceInvI1 = bIsRigidDynamic0 ? Utilities::ComputeWorldSpaceInertia(Q0, PBDRigid0->InvI()) : FMatrix33(0);
				FMatrix33 WorldSpaceInvI2 = bIsRigidDynamic1 ? Utilities::ComputeWorldSpaceInertia(Q1, PBDRigid1->InvI()) : FMatrix33(0);
				FMatrix33 Factor =
					(bIsRigidDynamic0 ? ComputeFactorMatrix3(VectorToPoint1, WorldSpaceInvI1, PBDRigid0->InvM()) : FMatrix33(0)) +
					(bIsRigidDynamic1 ? ComputeFactorMatrix3(VectorToPoint2, WorldSpaceInvI2, PBDRigid1->InvM()) : FMatrix33(0));
				FVec3 Impulse;
				FVec3 AngularImpulse(0);

				// Resting contact if very close to the surface
				bool bApplyRestitution = (RelativeVelocity.Size() > (2 * 980 * IterationParameters.Dt));
				FReal Restitution = (bApplyRestitution) ? Contact.Restitution : (FReal)0;
				FReal Friction = Contact.Friction;
				FReal AngularFriction = Contact.AngularFriction;

				if (Friction > 0)
				{
					FVec3 VelocityChange = -(Restitution * RelativeNormalVelocity * Contact.Normal + RelativeVelocity);
					FReal NormalVelocityChange = FVec3::DotProduct(VelocityChange, Contact.Normal);
					FMatrix33 FactorInverse = Factor.Inverse();
					FVec3 MinimalImpulse = FactorInverse * VelocityChange;
					const FReal MinimalImpulseDotNormal = FVec3::DotProduct(MinimalImpulse, Contact.Normal);
					const FReal TangentialSize = (MinimalImpulse - MinimalImpulseDotNormal * Contact.Normal).Size();
					if (TangentialSize <= Friction * MinimalImpulseDotNormal)
					{
						//within friction cone so just solve for static friction stopping the object
						Impulse = MinimalImpulse;
						if (AngularFriction)
						{
							FVec3 RelativeAngularVelocity = Particle0->W() - Particle1->W();
							FReal AngularNormal = FVec3::DotProduct(RelativeAngularVelocity, Contact.Normal);
							FVec3 AngularTangent = RelativeAngularVelocity - AngularNormal * Contact.Normal;
							FVec3 FinalAngularVelocity = FMath::Sign(AngularNormal) * FMath::Max((FReal)0, FMath::Abs(AngularNormal) - AngularFriction * NormalVelocityChange) * Contact.Normal + FMath::Max((FReal)0, AngularTangent.Size() - AngularFriction * NormalVelocityChange) * AngularTangent.GetSafeNormal();
							FVec3 Delta = FinalAngularVelocity - RelativeAngularVelocity;
							if (!bIsRigidDynamic0 && bIsRigidDynamic1)
							{
								FMatrix33 WorldSpaceI2 = (Q1 * FMatrix::Identity) * PBDRigid1->I() * (Q1 * FMatrix::Identity).GetTransposed();
								FVec3 ImpulseDelta = PBDRigid1->M() * FVec3::CrossProduct(VectorToPoint2, Delta);
								Impulse += ImpulseDelta;
								AngularImpulse += WorldSpaceI2 * Delta - FVec3::CrossProduct(VectorToPoint2, ImpulseDelta);
							}
							else if (bIsRigidDynamic0 && !bIsRigidDynamic1)
							{
								FMatrix33 WorldSpaceI1 = (Q0 * FMatrix::Identity) * PBDRigid0->I() * (Q0 * FMatrix::Identity).GetTransposed();
								FVec3 ImpulseDelta = PBDRigid0->M() * FVec3::CrossProduct(VectorToPoint1, Delta);
								Impulse += ImpulseDelta;
								AngularImpulse += WorldSpaceI1 * Delta - FVec3::CrossProduct(VectorToPoint1, ImpulseDelta);
							}
							else if (bIsRigidDynamic0 && bIsRigidDynamic1)
							{
								FMatrix33 Cross1(0, VectorToPoint1.Z, -VectorToPoint1.Y, -VectorToPoint1.Z, 0, VectorToPoint1.X, VectorToPoint1.Y, -VectorToPoint1.X, 0);
								FMatrix33 Cross2(0, VectorToPoint2.Z, -VectorToPoint2.Y, -VectorToPoint2.Z, 0, VectorToPoint2.X, VectorToPoint2.Y, -VectorToPoint2.X, 0);
								FMatrix33 CrossI1 = Cross1 * WorldSpaceInvI1;
								FMatrix33 CrossI2 = Cross2 * WorldSpaceInvI2;
								FMatrix33 Diag1 = CrossI1 * Cross1.GetTransposed() + CrossI2 * Cross2.GetTransposed();
								Diag1.M[0][0] += PBDRigid0->InvM() + PBDRigid1->InvM();
								Diag1.M[1][1] += PBDRigid0->InvM() + PBDRigid1->InvM();
								Diag1.M[2][2] += PBDRigid0->InvM() + PBDRigid1->InvM();
								FMatrix33 OffDiag1 = (CrossI1 + CrossI2) * -1;
								FMatrix33 Diag2 = (WorldSpaceInvI1 + WorldSpaceInvI2).Inverse();
								FMatrix33 OffDiag1Diag2 = OffDiag1 * Diag2;
								FVec3 ImpulseDelta = FMatrix33((Diag1 - OffDiag1Diag2 * OffDiag1.GetTransposed()).Inverse())* ((OffDiag1Diag2 * -1) * Delta);
								Impulse += ImpulseDelta;
								AngularImpulse += Diag2 * (Delta - FMatrix33(OffDiag1.GetTransposed()) * ImpulseDelta);
							}
						}
					}
					else
					{
						//outside friction cone, solve for normal relative velocity and keep tangent at cone edge
						FVec3 Tangent = (RelativeVelocity - FVec3::DotProduct(RelativeVelocity, Contact.Normal) * Contact.Normal).GetSafeNormal();
						FVec3 DirectionalFactor = Factor * (Contact.Normal - Friction * Tangent);
						FReal ImpulseDenominator = FVec3::DotProduct(Contact.Normal, DirectionalFactor);
						if (!ensureMsgf(FMath::Abs(ImpulseDenominator) > SMALL_NUMBER, TEXT("Contact:%s\n\nParticle:%s\n\nLevelset:%s\n\nDirectionalFactor:%s, ImpulseDenominator:%f"),
							*Contact.ToString(),
							*Particle0->ToString(),
							*Particle1->ToString(),
							*DirectionalFactor.ToString(), ImpulseDenominator))
						{
							ImpulseDenominator = (FReal)1;
						}

						const FReal ImpulseMag = -(1 + Restitution) * RelativeNormalVelocity / ImpulseDenominator;
						Impulse = ImpulseMag * (Contact.Normal - Friction * Tangent);
					}
				}
				else
				{
					FReal ImpulseDenominator = FVec3::DotProduct(Contact.Normal, Factor * Contact.Normal);
					FVec3 ImpulseNumerator = -(1 + Restitution) * FVec3::DotProduct(RelativeVelocity, Contact.Normal)* Contact.Normal;
					if (!ensureMsgf(FMath::Abs(ImpulseDenominator) > SMALL_NUMBER, TEXT("Contact:%s\n\nParticle:%s\n\nLevelset:%s\n\nFactor*Constraint.Normal:%s, ImpulseDenominator:%f"),
						*Contact.ToString(),
						*Particle0->ToString(),
						*Particle1->ToString(),
						*(Factor * Contact.Normal).ToString(), ImpulseDenominator))
					{
						ImpulseDenominator = (FReal)1;
					}
					Impulse = ImpulseNumerator / ImpulseDenominator;
				}

				Impulse = GetEnergyClampedImpulse(Particle0->CastToRigidParticle(), Particle1->CastToRigidParticle(), Impulse, VectorToPoint1, VectorToPoint2, Body1Velocity, Body2Velocity);
				AccumulatedImpulse += Impulse;

				if (bIsRigidDynamic0)
				{
					// Velocity update for next step
					FVec3 NetAngularImpulse = FVec3::CrossProduct(VectorToPoint1, Impulse) + AngularImpulse;
					FVec3 DV = PBDRigid0->InvM() * Impulse;
					FVec3 DW = WorldSpaceInvI1 * NetAngularImpulse;
					PBDRigid0->V() += DV;
					PBDRigid0->W() += DW;
					// Position update as part of pbd
					P0 += (DV * IterationParameters.Dt);
					Q0 += FRotation3::FromElements(DW, 0.f) * Q0 * IterationParameters.Dt * FReal(0.5);
					Q0.Normalize();
					FParticleUtilities::SetCoMWorldTransform(PBDRigid0, P0, Q0);
				}
				if (bIsRigidDynamic1)
				{
					// Velocity update for next step
					FVec3 NetAngularImpulse = FVec3::CrossProduct(VectorToPoint2, -Impulse) - AngularImpulse;
					FVec3 DV = -PBDRigid1->InvM() * Impulse;
					FVec3 DW = WorldSpaceInvI2 * NetAngularImpulse;
					PBDRigid1->V() += DV;
					PBDRigid1->W() += DW;
					// Position update as part of pbd
					P1 += (DV * IterationParameters.Dt);
					Q1 += FRotation3::FromElements(DW, 0.f) * Q1 * IterationParameters.Dt * FReal(0.5);
					Q1.Normalize();
					FParticleUtilities::SetCoMWorldTransform(PBDRigid1, P1, Q1);
				}
			}
			return AccumulatedImpulse;
		}

		template<typename T_CONSTRAINT>
		void ApplyImpl(T_CONSTRAINT& Constraint, const FContactIterationParameters & IterationParameters, const FContactParticleParameters & ParticleParameters)
		{
			TGenericParticleHandle<FReal, 3> Particle0 = TGenericParticleHandle<FReal, 3>(Constraint.Particle[0]);
			TGenericParticleHandle<FReal, 3> Particle1 = TGenericParticleHandle<FReal, 3>(Constraint.Particle[1]);

			for (int32 PairIt = 0; PairIt < IterationParameters.NumPairIterations; ++PairIt)
			{
				// Collision is already up-to-date on first iteration (we either just detected it, or updated it in DetectCollisions)
				// @todo(ccaulfield): this is not great - try to do something nicer like a dirty flag on the constraint?
				// In particular it is not right if the Collisions are not the first constraints to be solved...
				const bool bNeedCollisionUpdate = (PairIt > 0) || (IterationParameters.Iteration > 0);
				if (bNeedCollisionUpdate)
				{
					Collisions::Update(Constraint, ParticleParameters.CullDistance);
				}

				if (Constraint.GetPhi() >= ParticleParameters.ShapePadding)
				{
					return;
				}

				// @todo(ccaulfield): CHAOS_PARTICLEHANDLE_TODO what's the best way to manage external per-particle data?
				if (ParticleParameters.Collided)
				{
					Particle0->AuxilaryValue(*ParticleParameters.Collided) = true;
					Particle1->AuxilaryValue(*ParticleParameters.Collided) = true;
				}

				//
				// @todo(chaos) : Collision Constraints
				//   Consider applying all constraints in ::Apply at each iteration, right now it just takes the deepest.
				//   For example, and iterative constraint might have 4 penetrating points that need to be resolved. 
				//


				Constraint.AccumulatedImpulse +=
					ApplyContact(Constraint.Manifold, Particle0, Particle1, IterationParameters, ParticleParameters);
			}
		}

		void Apply(FCollisionConstraintBase& Constraint, const FContactIterationParameters & IterationParameters, const FContactParticleParameters & ParticleParameters)
		{
			if (Constraint.GetType() == FCollisionConstraintBase::FType::SinglePoint)
			{
				ApplyImpl(*Constraint.As<FRigidBodyPointContactConstraint>(), IterationParameters, ParticleParameters);
			}
			else if (Constraint.GetType() == FCollisionConstraintBase::FType::MultiPoint)
			{
				ApplyImpl(*Constraint.As<FRigidBodyMultiPointContactConstraint>(), IterationParameters, ParticleParameters);
			}
		}

		void Apply(FRigidBodyPointContactConstraint& Constraint, const FContactIterationParameters & IterationParameters, const FContactParticleParameters & ParticleParameters)
		{
			ApplyImpl(Constraint, IterationParameters, ParticleParameters);
		}

		void Apply(FRigidBodyMultiPointContactConstraint& Constraint, const FContactIterationParameters & IterationParameters, const FContactParticleParameters & ParticleParameters)
		{
			ApplyImpl(Constraint, IterationParameters, ParticleParameters);
		}


		FVec3 ApplyPushOutContact(
			FCollisionContact& Contact,
			TGenericParticleHandle<FReal, 3> Particle0, 
			TGenericParticleHandle<FReal, 3> Particle1,
			const TSet<const TGeometryParticleHandle<FReal, 3>*>& IsTemporarilyStatic,
			const FContactIterationParameters & IterationParameters, const FContactParticleParameters & ParticleParameters)
		{
			FVec3 AccumulatedImpulse(0);

			TPBDRigidParticleHandle<FReal, 3>* PBDRigid0 = Particle0->CastToRigidParticle();
			TPBDRigidParticleHandle<FReal, 3>* PBDRigid1 = Particle1->CastToRigidParticle();
			const bool bIsRigidDynamic0 = PBDRigid0 && PBDRigid0->ObjectState() == EObjectStateType::Dynamic;
			const bool bIsRigidDynamic1 = PBDRigid1 && PBDRigid1->ObjectState() == EObjectStateType::Dynamic;

			const FVec3 ZeroVector = FVec3(0);
			FVec3 P0 = FParticleUtilities::GetCoMWorldPosition(Particle0);
			FVec3 P1 = FParticleUtilities::GetCoMWorldPosition(Particle1);
			FRotation3 Q0 = FParticleUtilities::GetCoMWorldRotation(Particle0);
			FRotation3 Q1 = FParticleUtilities::GetCoMWorldRotation(Particle1);
			const bool IsTemporarilyStatic0 = IsTemporarilyStatic.Contains(Particle0->GeometryParticleHandle());
			const bool IsTemporarilyStatic1 = IsTemporarilyStatic.Contains(Particle1->GeometryParticleHandle());

			if (Contact.Phi >= ParticleParameters.ShapePadding)
			{
				return AccumulatedImpulse;
			}

			if ((!bIsRigidDynamic0 || IsTemporarilyStatic0) && (!bIsRigidDynamic1 || IsTemporarilyStatic1))
			{
				return AccumulatedImpulse;
			}

			if (IterationParameters.NeedsAnotherIteration)
			{
				*IterationParameters.NeedsAnotherIteration = true;
			}

			FMatrix33 WorldSpaceInvI1 = bIsRigidDynamic0 ? Utilities::ComputeWorldSpaceInertia(Q0, PBDRigid0->InvI()) : FMatrix33(0);
			FMatrix33 WorldSpaceInvI2 = bIsRigidDynamic1 ? Utilities::ComputeWorldSpaceInertia(Q1, PBDRigid1->InvI()) : FMatrix33(0);
			FVec3 VectorToPoint1 = Contact.Location - P0;
			FVec3 VectorToPoint2 = Contact.Location - P1;
			FMatrix33 Factor =
				(bIsRigidDynamic0 ? ComputeFactorMatrix3(VectorToPoint1, WorldSpaceInvI1, PBDRigid0->InvM()) : FMatrix33(0)) +
				(bIsRigidDynamic1 ? ComputeFactorMatrix3(VectorToPoint2, WorldSpaceInvI2, PBDRigid1->InvM()) : FMatrix33(0));
			FReal Numerator = FMath::Min((FReal)(IterationParameters.Iteration + 2), (FReal)IterationParameters.NumIterations);
			FReal ScalingFactor = Numerator / (FReal)IterationParameters.NumIterations;

			//if pushout is needed we better fix relative velocity along normal. Treat it as if 0 restitution
			FVec3 Body1Velocity = FParticleUtilities::GetVelocityAtCoMRelativePosition(Particle0, VectorToPoint1);
			FVec3 Body2Velocity = FParticleUtilities::GetVelocityAtCoMRelativePosition(Particle1, VectorToPoint2);
			FVec3 RelativeVelocity = Body1Velocity - Body2Velocity;
			const FReal RelativeVelocityDotNormal = FVec3::DotProduct(RelativeVelocity, Contact.Normal);
			if (RelativeVelocityDotNormal < 0)
			{
				const FVec3 ImpulseNumerator = -FVec3::DotProduct(RelativeVelocity, Contact.Normal) * Contact.Normal * ScalingFactor;
				const FVec3 FactorContactNormal = Factor * Contact.Normal;
				FReal ImpulseDenominator = FVec3::DotProduct(Contact.Normal, FactorContactNormal);
				if (!ensureMsgf(FMath::Abs(ImpulseDenominator) > SMALL_NUMBER, 
					TEXT("ApplyPushout Contact:%s\n\nParticle:%s\n\nLevelset:%s\n\nFactor*Contact.Normal:%s, ImpulseDenominator:%f"),
					*Contact.ToString(),
					*Particle0->ToString(),
					*Particle1->ToString(),
					*FactorContactNormal.ToString(), ImpulseDenominator))
				{
					ImpulseDenominator = (FReal)1;
				}

				FVec3 VelocityFixImpulse = ImpulseNumerator / ImpulseDenominator;
				VelocityFixImpulse = GetEnergyClampedImpulse(Particle0->CastToRigidParticle(), Particle1->CastToRigidParticle(), VelocityFixImpulse, VectorToPoint1, VectorToPoint2, Body1Velocity, Body2Velocity);
				AccumulatedImpulse += VelocityFixImpulse;	//question: should we track this?
				if (!IsTemporarilyStatic0 && bIsRigidDynamic0)
				{
					FVec3 AngularImpulse = FVec3::CrossProduct(VectorToPoint1, VelocityFixImpulse);
					PBDRigid0->V() += PBDRigid0->InvM() * VelocityFixImpulse;
					PBDRigid0->W() += WorldSpaceInvI1 * AngularImpulse;

				}

				if (!IsTemporarilyStatic1 && bIsRigidDynamic1)
				{
					FVec3 AngularImpulse = FVec3::CrossProduct(VectorToPoint2, -VelocityFixImpulse);
					PBDRigid1->V() -= PBDRigid1->InvM() * VelocityFixImpulse;
					PBDRigid1->W() += WorldSpaceInvI2 * AngularImpulse;
				}

			}


			FVec3 Impulse = FMatrix33(Factor.Inverse()) * ((-Contact.Phi + ParticleParameters.ShapePadding) * ScalingFactor * Contact.Normal);
			FVec3 AngularImpulse1 = FVec3::CrossProduct(VectorToPoint1, Impulse);
			FVec3 AngularImpulse2 = FVec3::CrossProduct(VectorToPoint2, -Impulse);
			if (!IsTemporarilyStatic0 && bIsRigidDynamic0)
			{
				P0 += PBDRigid0->InvM() * Impulse;
				Q0 = FRotation3::FromVector(WorldSpaceInvI1 * AngularImpulse1) * Q0;
				Q0.Normalize();
				FParticleUtilities::SetCoMWorldTransform(Particle0, P0, Q0);
			}
			if (!IsTemporarilyStatic1 && bIsRigidDynamic1)
			{
				P1 -= PBDRigid1->InvM() * Impulse;
				Q1 = FRotation3::FromVector(WorldSpaceInvI2 * AngularImpulse2) * Q1;
				Q1.Normalize();
				FParticleUtilities::SetCoMWorldTransform(Particle1, P1, Q1);
			}

			return AccumulatedImpulse;
		}


		template<typename T_CONSTRAINT>
		void ApplyPushOutImpl(T_CONSTRAINT& Constraint, const TSet<const TGeometryParticleHandle<FReal, 3>*>& IsTemporarilyStatic,
			const FContactIterationParameters & IterationParameters, const FContactParticleParameters & ParticleParameters)
		{
			TGenericParticleHandle<FReal, 3> Particle0 = TGenericParticleHandle<FReal, 3>(Constraint.Particle[0]);
			TGenericParticleHandle<FReal, 3> Particle1 = TGenericParticleHandle<FReal, 3>(Constraint.Particle[1]);

			for (int32 PairIt = 0; PairIt < IterationParameters.NumPairIterations; ++PairIt)
			{
				Update(Constraint, ParticleParameters.CullDistance);

				Constraint.AccumulatedImpulse += 
					ApplyPushOutContact(Constraint.Manifold, Particle0, Particle1, IsTemporarilyStatic, IterationParameters, ParticleParameters);
			}
		}

		void ApplyPushOut(FCollisionConstraintBase& Constraint, const TSet<const TGeometryParticleHandle<FReal, 3>*>& IsTemporarilyStatic, 
			const FContactIterationParameters & IterationParameters, const FContactParticleParameters & ParticleParameters)
		{
			if (Constraint.GetType() == FCollisionConstraintBase::FType::SinglePoint)
			{
				ApplyPushOutImpl(*Constraint.As<FRigidBodyPointContactConstraint>(), IsTemporarilyStatic, IterationParameters, ParticleParameters);
			}
			else if (Constraint.GetType() == FCollisionConstraintBase::FType::MultiPoint)
			{
				ApplyPushOutImpl(*Constraint.As<FRigidBodyMultiPointContactConstraint>(), IsTemporarilyStatic, IterationParameters, ParticleParameters);
			}
		}

		void ApplyPushOut(FRigidBodyPointContactConstraint& Constraint, const TSet<const TGeometryParticleHandle<FReal, 3>*>& IsTemporarilyStatic,
			const FContactIterationParameters & IterationParameters, const FContactParticleParameters & ParticleParameters)
		{
			ApplyPushOutImpl(Constraint, IsTemporarilyStatic, IterationParameters, ParticleParameters);
		}

		void ApplyPushOut(FRigidBodyMultiPointContactConstraint& Constraint, const TSet<const TGeometryParticleHandle<FReal, 3>*>& IsTemporarilyStatic,
			const FContactIterationParameters & IterationParameters, const FContactParticleParameters & ParticleParameters)
		{
			ApplyPushOutImpl(Constraint, IsTemporarilyStatic, IterationParameters, ParticleParameters);
		}

	} // Collisions

}// Chaos

