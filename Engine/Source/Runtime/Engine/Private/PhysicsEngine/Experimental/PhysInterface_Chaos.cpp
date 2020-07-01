// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_CHAOS

#include "Physics/Experimental/PhysInterface_Chaos.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "Physics/Experimental/ChaosInterfaceUtils.h"
#include "Physics/PhysicsInterfaceTypes.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "Templates/UniquePtr.h"

#include "PhysicsSolver.h"
#include "Chaos/Box.h"
#include "Chaos/Cylinder.h"
#include "Chaos/TaperedCylinder.h"
#include "Chaos/Capsule.h"
#include "Chaos/ImplicitObjectTransformed.h"
#include "Chaos/ImplicitObjectUnion.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "Chaos/Levelset.h"
#include "Chaos/PBDRigidParticles.h"
#include "Chaos/Sphere.h"
#include "Chaos/Matrix.h"
#include "Chaos/MassProperties.h"
#include "ChaosSolversModule.h"
#include "Chaos/ErrorReporter.h"
#include "Chaos/ImplicitObjectScaled.h"
#include "Chaos/Convex.h"
#include "Chaos/GeometryQueries.h"
#include "Chaos/Plane.h"
#include "ChaosCheck.h"
#include "Chaos/Particle/ParticleUtilities.h"
#include "Chaos/PBDJointConstraints.h"

#include "Async/ParallelFor.h"
#include "Components/PrimitiveComponent.h"
#include "Physics/PhysicsFiltering.h"
#include "Collision/CollisionConversions.h"
#include "PhysicsInterfaceUtilsCore.h"
#include "Components/SkeletalMeshComponent.h"
#include "PBDRigidsSolver.h"
#include "PhysicalMaterials/PhysicalMaterialMask.h"
#include "PhysicalMaterials/PhysicalMaterial.h"

#if PHYSICS_INTERFACE_PHYSX
#include "geometry/PxConvexMesh.h"
#include "geometry/PxTriangleMesh.h"
#include "foundation/PxVec3.h"
#include "extensions/PxMassProperties.h"
#include "Containers/ArrayView.h"
#endif

//#ifndef USE_CHAOS_JOINT_CONSTRAINTS
//#define USE_CHAOS_JOINT_CONSTRAINTS 
//#endif

DEFINE_STAT(STAT_TotalPhysicsTime);
DEFINE_STAT(STAT_NumCloths);
DEFINE_STAT(STAT_NumClothVerts);

DECLARE_CYCLE_STAT(TEXT("Start Physics Time (sync)"), STAT_PhysicsKickOffDynamicsTime, STATGROUP_Physics);
DECLARE_CYCLE_STAT(TEXT("Fetch Results Time (sync)"), STAT_PhysicsFetchDynamicsTime, STATGROUP_Physics);

DECLARE_CYCLE_STAT(TEXT("Start Physics Time (async)"), STAT_PhysicsKickOffDynamicsTime_Async, STATGROUP_Physics);
DECLARE_CYCLE_STAT(TEXT("Fetch Results Time (async)"), STAT_PhysicsFetchDynamicsTime_Async, STATGROUP_Physics);

DECLARE_CYCLE_STAT(TEXT("Update Kinematics On Deferred SkelMeshes"), STAT_UpdateKinematicsOnDeferredSkelMeshes, STATGROUP_Physics);

DECLARE_CYCLE_STAT(TEXT("Phys Events Time"), STAT_PhysicsEventTime, STATGROUP_Physics);
DECLARE_CYCLE_STAT(TEXT("SyncComponentsToBodies (sync)"), STAT_SyncComponentsToBodies, STATGROUP_Physics);
DECLARE_CYCLE_STAT(TEXT("SyncComponentsToBodies (async)"), STAT_SyncComponentsToBodies_Async, STATGROUP_Physics);
DECLARE_CYCLE_STAT(TEXT("Query PhysicalMaterialMask Hit"), STAT_QueryPhysicalMaterialMaskHit, STATGROUP_Physics);

DECLARE_DWORD_COUNTER_STAT(TEXT("Broadphase Adds"), STAT_NumBroadphaseAdds, STATGROUP_Physics);
DECLARE_DWORD_COUNTER_STAT(TEXT("Broadphase Removes"), STAT_NumBroadphaseRemoves, STATGROUP_Physics);
DECLARE_DWORD_COUNTER_STAT(TEXT("Active Constraints"), STAT_NumActiveConstraints, STATGROUP_Physics);
DECLARE_DWORD_COUNTER_STAT(TEXT("Active Simulated Bodies"), STAT_NumActiveSimulatedBodies, STATGROUP_Physics);
DECLARE_DWORD_COUNTER_STAT(TEXT("Active Kinematic Bodies"), STAT_NumActiveKinematicBodies, STATGROUP_Physics);
DECLARE_DWORD_COUNTER_STAT(TEXT("Mobile Bodies"), STAT_NumMobileBodies, STATGROUP_Physics);
DECLARE_DWORD_COUNTER_STAT(TEXT("Static Bodies"), STAT_NumStaticBodies, STATGROUP_Physics);
DECLARE_DWORD_COUNTER_STAT(TEXT("Shapes"), STAT_NumShapes, STATGROUP_Physics);

DECLARE_DWORD_COUNTER_STAT(TEXT("(ASync) Broadphase Adds"), STAT_NumBroadphaseAddsAsync, STATGROUP_Physics);
DECLARE_DWORD_COUNTER_STAT(TEXT("(ASync) Broadphase Removes"), STAT_NumBroadphaseRemovesAsync, STATGROUP_Physics);
DECLARE_DWORD_COUNTER_STAT(TEXT("(ASync) Active Constraints"), STAT_NumActiveConstraintsAsync, STATGROUP_Physics);
DECLARE_DWORD_COUNTER_STAT(TEXT("(ASync) Active Simulated Bodies"), STAT_NumActiveSimulatedBodiesAsync, STATGROUP_Physics);
DECLARE_DWORD_COUNTER_STAT(TEXT("(ASync) Active Kinematic Bodies"), STAT_NumActiveKinematicBodiesAsync, STATGROUP_Physics);
DECLARE_DWORD_COUNTER_STAT(TEXT("(ASync) Mobile Bodies"), STAT_NumMobileBodiesAsync, STATGROUP_Physics);
DECLARE_DWORD_COUNTER_STAT(TEXT("(ASync) Static Bodies"), STAT_NumStaticBodiesAsync, STATGROUP_Physics);
DECLARE_DWORD_COUNTER_STAT(TEXT("(ASync) Shapes"), STAT_NumShapesAsync, STATGROUP_Physics);

ECollisionShapeType GetGeometryType(const Chaos::FPerShapeData& Shape)
{
	return GetType(*Shape.GetGeometry());
}

Chaos::FChaosPhysicsMaterial* GetMaterialFromInternalFaceIndex(const FPhysicsShape& Shape, const FPhysicsActor& Actor, uint32 InternalFaceIndex)
{
	const auto& Materials = Shape.GetMaterials();
	if(Materials.Num() > 0 && Actor.GetProxy())
	{
		Chaos::FPBDRigidsSolver* Solver = Actor.GetProxy()->GetSolver<Chaos::FPBDRigidsSolver>();

		if(ensure(Solver))
		{
			if(Materials.Num() == 1)
			{
				Chaos::TSolverQueryMaterialScope<Chaos::ELockType::Read> Scope(Solver);
				return Solver->GetQueryMaterials().Get(Materials[0].InnerHandle);
			}

			uint8 Index = Shape.GetGeometry()->GetMaterialIndex(InternalFaceIndex);

			if(Materials.IsValidIndex(Index))
			{
				Chaos::TSolverQueryMaterialScope<Chaos::ELockType::Read> Scope(Solver);
				return Solver->GetQueryMaterials().Get(Materials[Index].InnerHandle);
			}
		}
	}

	return nullptr;
}

Chaos::FChaosPhysicsMaterial* GetMaterialFromInternalFaceIndexAndHitLocation(const FPhysicsShape& Shape, const FPhysicsActor& Actor, uint32 InternalFaceIndex, const FVector& HitLocation)
{
	{
		SCOPE_CYCLE_COUNTER(STAT_QueryPhysicalMaterialMaskHit);

		if (Shape.GetMaterials().Num() > 0 && Actor.GetProxy())
		{
			Chaos::FPBDRigidsSolver* Solver = Actor.GetProxy()->GetSolver<Chaos::FPBDRigidsSolver>();

			if (ensure(Solver))
			{
				if (Shape.GetMaterialMasks().Num() > 0)
				{
					UBodySetup* BodySetup = nullptr;

					if (const FBodyInstance* BodyInst = GetUserData(Actor))
					{
						BodyInst = FPhysicsInterface::ShapeToOriginalBodyInstance(BodyInst, &Shape);
						BodySetup = BodyInst->BodySetup.Get();	//this data should be immutable at runtime so ok to check from worker thread.
						ECollisionShapeType GeomType = GetGeometryType(Shape);

						if (BodySetup && BodySetup->bSupportUVsAndFaceRemap && GetGeometryType(Shape) == ECollisionShapeType::Trimesh)
						{
							FVector Scale(1.0f, 1.0f, 1.0f);
							const Chaos::FImplicitObject* Geometry = Shape.GetGeometry().Get();
							if (const Chaos::TImplicitObjectScaled<Chaos::FTriangleMeshImplicitObject>* ScaledTrimesh = Chaos::TImplicitObjectScaled<Chaos::FTriangleMeshImplicitObject>::AsScaled(*Geometry))
							{
								Scale = ScaledTrimesh->GetScale();
							}

							// Convert hit location to local
							Chaos::FRigidTransform3 ActorToWorld(Actor.X(), Actor.R(), Scale);
							const FVector LocalHitPos = ActorToWorld.InverseTransformPosition(HitLocation);

							uint8 Index = Shape.GetGeometry()->GetMaterialIndex(InternalFaceIndex);
							if (Shape.GetMaterialMasks().IsValidIndex(Index))
							{
								Chaos::FChaosPhysicsMaterialMask* Mask = nullptr;
								{
									Chaos::TSolverQueryMaterialScope<Chaos::ELockType::Read> Scope(Solver);
									Mask = Solver->GetQueryMaterialMasks().Get(Shape.GetMaterialMasks()[Index].InnerHandle);
								}

								if (Mask && InternalFaceIndex < (uint32)BodySetup->FaceRemap.Num())
								{
									int32 RemappedFaceIndex = BodySetup->FaceRemap[InternalFaceIndex];
									FVector2D UV;


									if (BodySetup->CalcUVAtLocation(LocalHitPos, RemappedFaceIndex, Mask->UVChannelIndex, UV))
									{
										uint32 MapIdx = UPhysicalMaterialMask::GetPhysMatIndex(Mask->MaskData, Mask->SizeX, Mask->SizeY, Mask->AddressX, Mask->AddressY, UV.X, UV.Y);
										uint32 AdjustedMapIdx = Index * EPhysicalMaterialMaskColor::MAX + MapIdx;
										if (Shape.GetMaterialMaskMaps().IsValidIndex(AdjustedMapIdx))
										{
											uint32 MaterialIdx = Shape.GetMaterialMaskMaps()[AdjustedMapIdx];
											if (Shape.GetMaterialMaskMapMaterials().IsValidIndex(MaterialIdx))
											{
												Chaos::TSolverQueryMaterialScope<Chaos::ELockType::Read> Scope(Solver);
												return Solver->GetQueryMaterials().Get(Shape.GetMaterialMaskMapMaterials()[MaterialIdx].InnerHandle);
											}
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}

	return GetMaterialFromInternalFaceIndex(Shape, Actor, InternalFaceIndex);
}

FPhysInterface_Chaos::FPhysInterface_Chaos(const AWorldSettings* Settings) 
{

}

FPhysInterface_Chaos::~FPhysInterface_Chaos()
{
}


// Interface functions
void FPhysInterface_Chaos::CreateActor(const FActorCreationParams& InParams, FPhysicsActorHandle& Handle)
{
	LLM_SCOPE(ELLMTag::Chaos);
	
	// Set object state based on the requested particle type
	if (InParams.bStatic)
	{
		Handle = Chaos::TGeometryParticle<float, 3>::CreateParticle().Release();
	}
	else
	{
		// Create an underlying dynamic particle
		Chaos::TPBDRigidParticle<float, 3>* RigidHandle = Chaos::TPBDRigidParticle<float, 3>::CreateParticle().Release(); //todo: should BodyInstance use a unique ptr to manage this memory?
		Handle = RigidHandle;
		RigidHandle->SetGravityEnabled(InParams.bEnableGravity);
		if (InParams.BodyInstance && InParams.BodyInstance->ShouldInstanceSimulatingPhysics())
		{
			if (InParams.BodyInstance->bStartAwake)
			{
				RigidHandle->SetObjectState(Chaos::EObjectStateType::Dynamic);
			}
			else
			{
				RigidHandle->SetObjectState(Chaos::EObjectStateType::Sleeping);
			}
		}
		else
		{
			RigidHandle->SetObjectState(Chaos::EObjectStateType::Kinematic);
		}
	}

	// Set up the new particle's game-thread data. This will be sent to physics-thread when
	// the particle is added to the scene later.
	Handle->SetX(InParams.InitialTM.GetLocation(), /*bInvalidate=*/false);	//do not generate wake event since this is part of initialization
	Handle->SetR(InParams.InitialTM.GetRotation(), /*bInvalidate=*/false);
#if CHAOS_CHECKED
	Handle->SetDebugName(InParams.DebugName);
#endif
}


void FPhysInterface_Chaos::AddActorToSolver(FPhysicsActorHandle& Handle, Chaos::FPhysicsSolver* Solver)
{
	LLM_SCOPE(ELLMTag::Chaos);

	Solver->RegisterObject(Handle);
}

void FPhysInterface_Chaos::ReleaseActor(FPhysicsActorHandle& Handle, FPhysScene* InScene, bool bNeverDerferRelease)
{
	if(!Handle)
	{
		UE_LOG(LogChaos, Warning, TEXT("Attempting to release an actor with a null handle"));
		CHAOS_ENSURE(false);

		return;
	}

	if (InScene)
	{
		InScene->GetScene().RemoveActorFromAccelerationStructure(Handle);
		RemoveActorFromSolver(Handle, InScene->GetSolver());
	}

	delete Handle;

	Handle = nullptr;
}

void FPhysInterface_Chaos::RemoveActorFromSolver(FPhysicsActorHandle& Handle, Chaos::FPhysicsSolver* Solver)
{
	if (Solver && Handle->GetProxy())
	{
		Solver->UnregisterObject(Handle);
	}
}

// Aggregate is not relevant for Chaos yet
FPhysicsAggregateReference_Chaos FPhysInterface_Chaos::CreateAggregate(int32 MaxBodies)
{
	// #todo : Implement
    FPhysicsAggregateReference_Chaos NewAggregate;
    return NewAggregate;
}

void FPhysInterface_Chaos::ReleaseAggregate(FPhysicsAggregateReference_Chaos& InAggregate) {}
int32 FPhysInterface_Chaos::GetNumActorsInAggregate(const FPhysicsAggregateReference_Chaos& InAggregate) { return 0; }
void FPhysInterface_Chaos::AddActorToAggregate_AssumesLocked(const FPhysicsAggregateReference_Chaos& InAggregate, const FPhysicsActorHandle& InActor) {}


FPhysicsMaterialHandle FPhysInterface_Chaos::CreateMaterial(const UPhysicalMaterial* InMaterial)
{
	Chaos::FMaterialHandle NewHandle = Chaos::FPhysicalMaterialManager::Get().Create();

	return NewHandle;
}

void FPhysInterface_Chaos::ReleaseMaterial(FPhysicsMaterialHandle& InHandle)
{
	Chaos::FPhysicalMaterialManager::Get().Destroy(InHandle);
}

Chaos::FChaosPhysicsMaterial::ECombineMode UToCCombineMode(EFrictionCombineMode::Type Mode)
{
	using namespace Chaos;
	switch(Mode)
	{
	case EFrictionCombineMode::Average: return FChaosPhysicsMaterial::ECombineMode::Avg;
	case EFrictionCombineMode::Min: return FChaosPhysicsMaterial::ECombineMode::Min;
	case EFrictionCombineMode::Multiply: return FChaosPhysicsMaterial::ECombineMode::Multiply;
	case EFrictionCombineMode::Max: return FChaosPhysicsMaterial::ECombineMode::Max;
	default: ensure(false);
	}

	return FChaosPhysicsMaterial::ECombineMode::Avg;
}


void FPhysInterface_Chaos::UpdateMaterial(FPhysicsMaterialHandle& InHandle, UPhysicalMaterial* InMaterial)
{
	if(Chaos::FChaosPhysicsMaterial* Material = InHandle.Get())
	{
		Material->Friction = InMaterial->Friction;
		Material->FrictionCombineMode = UToCCombineMode(InMaterial->FrictionCombineMode);
		Material->Restitution = InMaterial->Restitution;
		Material->RestitutionCombineMode = UToCCombineMode(InMaterial->RestitutionCombineMode);
		Material->SleepingLinearThreshold = InMaterial->SleepLinearVelocityThreshold;
		Material->SleepingAngularThreshold = InMaterial->SleepAngularVelocityThreshold;
		Material->SleepCounterThreshold = InMaterial->SleepCounterThreshold;
	}

	Chaos::FPhysicalMaterialManager::Get().UpdateMaterial(InHandle);
}

void FPhysInterface_Chaos::SetUserData(FPhysicsMaterialHandle& InHandle, void* InUserData)
{
	if(Chaos::FChaosPhysicsMaterial* Material = InHandle.Get())
	{
		Material->UserData = InUserData;
	}

	Chaos::FPhysicalMaterialManager::Get().UpdateMaterial(InHandle);
}

FPhysicsMaterialMaskHandle FPhysInterface_Chaos::CreateMaterialMask(const UPhysicalMaterialMask* InMaterialMask)
{
	Chaos::FMaterialMaskHandle NewHandle = Chaos::FPhysicalMaterialManager::Get().CreateMask();
	FPhysInterface_Chaos::UpdateMaterialMask(NewHandle, InMaterialMask);
	return NewHandle;
}

void FPhysInterface_Chaos::ReleaseMaterialMask(FPhysicsMaterialMaskHandle& InHandle)
{
	Chaos::FPhysicalMaterialManager::Get().Destroy(InHandle);
}

void FPhysInterface_Chaos::UpdateMaterialMask(FPhysicsMaterialMaskHandle& InHandle, const UPhysicalMaterialMask* InMaterialMask)
{
	if (Chaos::FChaosPhysicsMaterialMask* MaterialMask = InHandle.Get())
	{
		InMaterialMask->GenerateMaskData(MaterialMask->MaskData, MaterialMask->SizeX, MaterialMask->SizeY);
		MaterialMask->UVChannelIndex = InMaterialMask->UVChannelIndex;
		MaterialMask->AddressX = static_cast<int32>(InMaterialMask->AddressX);
		MaterialMask->AddressY = static_cast<int32>(InMaterialMask->AddressY);
	}

	Chaos::FPhysicalMaterialManager::Get().UpdateMaterialMask(InHandle);
}

void FPhysInterface_Chaos::SetUserData(const FPhysicsShapeHandle& InShape, void* InUserData)
{
	if (CHAOS_ENSURE(InShape.Shape))
	{
		InShape.Shape->SetUserData(InUserData);
	}
}

void* FPhysInterface_Chaos::GetUserData(const FPhysicsShapeHandle& InShape)
{
	if (ensure(InShape.Shape))
	{
		return InShape.Shape->GetUserData();
	}
	return nullptr;
}

int32 FPhysInterface_Chaos::GetNumShapes(const FPhysicsActorHandle& InHandle)
{
	// #todo : Implement
	return InHandle->ShapesArray().Num();
}

void FPhysInterface_Chaos::ReleaseShape(const FPhysicsShapeHandle& InShape)
{
    check(!IsValid(InShape.ActorRef));
	//no need to delete because ownership is on actor. Is this an invalid assumption with the current API?
	//delete InShape.Shape;
}

void FPhysInterface_Chaos::AttachShape(const FPhysicsActorHandle& InActor, const FPhysicsShapeHandle& InNewShape)
{
	// #todo : Implement
	CHAOS_ENSURE(false);
}

void FPhysInterface_Chaos::DetachShape(const FPhysicsActorHandle& InActor, FPhysicsShapeHandle& InShape, bool bWakeTouching)
{
	// #todo : Implement
	CHAOS_ENSURE(false);
}

void FPhysInterface_Chaos::SetActorUserData_AssumesLocked(FPhysicsActorHandle& InActorReference, FPhysicsUserData* InUserData)
{
	InActorReference->SetUserData(InUserData);
}

bool FPhysInterface_Chaos::IsRigidBody(const FPhysicsActorHandle& InActorReference)
{
	return !IsStatic(InActorReference);
}

bool FPhysInterface_Chaos::IsDynamic(const FPhysicsActorHandle& InActorReference)
{
	// Do this to match the PhysX interface behavior: :( :( :(
	return !IsStatic(InActorReference);
}

bool FPhysInterface_Chaos::IsStatic(const FPhysicsActorHandle& InActorReference)
{
	return InActorReference->ObjectState() == Chaos::EObjectStateType::Static;
}

bool FPhysInterface_Chaos::IsKinematic(const FPhysicsActorHandle& InActorReference)
{
	return InActorReference->ObjectState() == Chaos::EObjectStateType::Kinematic;
}

bool FPhysInterface_Chaos::IsKinematic_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
	return IsKinematic(InActorReference);
}

bool FPhysInterface_Chaos::IsSleeping(const FPhysicsActorHandle& InActorReference)
{
	return InActorReference->ObjectState() == Chaos::EObjectStateType::Sleeping;
}

bool FPhysInterface_Chaos::IsCcdEnabled(const FPhysicsActorHandle& InActorReference)
{
    return false;
}

bool FPhysInterface_Chaos::IsInScene(const FPhysicsActorHandle& InActorReference)
{
	return (GetCurrentScene(InActorReference) != nullptr);
}

FPhysScene* FPhysInterface_Chaos::GetCurrentScene(const FPhysicsActorHandle& InHandle)
{
	if(!InHandle)
	{
		UE_LOG(LogChaos, Warning, TEXT("Attempting to get the current scene for a null handle."));
		CHAOS_ENSURE(false);
		return nullptr;
	}

	if (IPhysicsProxyBase* Proxy = InHandle->GetProxy())
	{
		Chaos::FPBDRigidsSolver* Solver = Proxy->GetSolver<Chaos::FPBDRigidsSolver>();
		return static_cast<FPhysScene*>(Solver ? Solver->PhysSceneHack : nullptr);
	}
	return nullptr;
}

void FPhysInterface_Chaos::FlushScene(FPhysScene* InScene)
{
	FPhysicsCommand::ExecuteWrite(InScene, [&]()
	{
		InScene->Flush_AssumesLocked();
	});
}

bool FPhysInterface_Chaos::CanSimulate_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
	// #todo : Implement
	return true;
}

float FPhysInterface_Chaos::GetMass_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
	if (const Chaos::TPBDRigidParticle<float,3>* RigidParticle = InActorReference->CastToRigidParticle())
	{
		return RigidParticle->M();
	}
	return 0.f;
}

void FPhysInterface_Chaos::SetSendsSleepNotifies_AssumesLocked(const FPhysicsActorHandle& InActorReference, bool bSendSleepNotifies)
{
	// # todo: Implement
    //check(bSendSleepNotifies == false);
}

void FPhysInterface_Chaos::PutToSleep_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
	Chaos::TPBDRigidParticle<float, 3>* Particle = InActorReference->CastToRigidParticle();
	if(Particle && Particle->ObjectState() == Chaos::EObjectStateType::Dynamic)
	{
		Particle->SetObjectState(Chaos::EObjectStateType::Sleeping);
	}
	
}

void FPhysInterface_Chaos::WakeUp_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
	Chaos::TPBDRigidParticle<float, 3>* Particle = InActorReference->CastToRigidParticle();
	if(Particle && Particle->ObjectState() == Chaos::EObjectStateType::Sleeping)
	{
		Particle->SetObjectState(Chaos::EObjectStateType::Dynamic);
		Particle->ClearEvents();
	}
}

void FPhysInterface_Chaos::SetIsKinematic_AssumesLocked(const FPhysicsActorHandle& InActorReference, bool bIsKinematic)
{
	if (Chaos::TPBDRigidParticle<float, 3>* Particle = InActorReference->CastToRigidParticle())
	{
		const Chaos::EObjectStateType NewState
			= bIsKinematic
			? Chaos::EObjectStateType::Kinematic
			: Chaos::EObjectStateType::Dynamic;

		bool AllowedToChangeToNewState = false;

		switch (Particle->ObjectState())
		{
		case Chaos::EObjectStateType::Kinematic:
			// from kinematic we can only go dynamic
			if (NewState == Chaos::EObjectStateType::Dynamic)
			{
				AllowedToChangeToNewState = true;
			}
			break;

		case Chaos::EObjectStateType::Dynamic:
			// from dynamic we can go to sleeping or to kinematic
			if (NewState == Chaos::EObjectStateType::Kinematic)
			{
				AllowedToChangeToNewState = true;
			}
			break;

		case Chaos::EObjectStateType::Sleeping:
			// from sleeping we can't change state without waking first
			break;
		}
		
		if (AllowedToChangeToNewState)
		{
			Particle->SetObjectState(NewState);
		}
	}
	else
	{
		CHAOS_ENSURE_MSG(false, TEXT("Can only set kinematic state of underlying dynamic particles"));
	}
}

void FPhysInterface_Chaos::SetCcdEnabled_AssumesLocked(const FPhysicsActorHandle& InActorReference, bool bIsCcdEnabled)
{
	// #todo: Implement
    //check(bIsCcdEnabled == false);
}

void FPhysInterface_Chaos::SetIgnoreAnalyticCollisions_AssumesLocked(const FPhysicsActorHandle& InActorReference, bool bIgnoreAnalyticCollisions)
{
	InActorReference->SetIgnoreAnalyticCollisions(bIgnoreAnalyticCollisions);
}

FTransform FPhysInterface_Chaos::GetGlobalPose_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
	return Chaos::TRigidTransform<float, 3>(InActorReference->X(), InActorReference->R());
}

void FPhysInterface_Chaos::SetGlobalPose_AssumesLocked(const FPhysicsActorHandle& InActorReference, const FTransform& InNewPose, bool bAutoWake)
{
	InActorReference->SetX(InNewPose.GetLocation());
	InActorReference->SetR(InNewPose.GetRotation());
	InActorReference->UpdateShapeBounds();

	FPhysScene* Scene = GetCurrentScene(InActorReference);
	Scene->GetScene().UpdateActorInAccelerationStructure(InActorReference);
}

FTransform FPhysInterface_Chaos::GetTransform_AssumesLocked(const FPhysicsActorHandle& InRef, bool bForceGlobalPose /*= false*/)
{
	if(!bForceGlobalPose)
	{
		if(IsDynamic(InRef))
		{
			if(HasKinematicTarget_AssumesLocked(InRef))
			{
				return GetKinematicTarget_AssumesLocked(InRef);
			}
		}
	}

	return GetGlobalPose_AssumesLocked(InRef);
}

bool FPhysInterface_Chaos::HasKinematicTarget_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
    return IsStatic(InActorReference);
}

FTransform FPhysInterface_Chaos::GetKinematicTarget_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
	// #todo : Implement
	//for now just use global pose
	return FPhysInterface_Chaos::GetGlobalPose_AssumesLocked(InActorReference);
}

void FPhysInterface_Chaos::SetKinematicTarget_AssumesLocked(const FPhysicsActorHandle& InActorReference, const FTransform& InNewTarget)
{
	// #todo : Implement
	//for now just use global pose
	FPhysInterface_Chaos::SetGlobalPose_AssumesLocked(InActorReference, InNewTarget);
}

FVector FPhysInterface_Chaos::GetLinearVelocity_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
	if (ensure(FPhysicsInterface::IsValid(InActorReference)))
	{
		Chaos::TKinematicGeometryParticle<float, 3>* Kinematic = InActorReference->CastToKinematicParticle();
		if (ensure(Kinematic))
		{
			return Kinematic->V();
		}
	}

	return FVector(0);
}

void FPhysInterface_Chaos::SetLinearVelocity_AssumesLocked(const FPhysicsActorHandle& InActorReference, const FVector& InNewVelocity, bool bAutoWake)
{
	// TODO: Implement bAutoWake == false.
	// For now we don't support auto-awake == false.
	// This feature is meant to detect when velocity change small
	// and the velocity is nearly zero, and to not wake up the
	// body in that case.
	ensure(bAutoWake);

	if (ensure(FPhysicsInterface::IsValid(InActorReference)))
	{
		Chaos::TKinematicGeometryParticle<float, 3>* Kinematic = InActorReference->CastToKinematicParticle();
		if (ensure(Kinematic))
		{
			Kinematic->SetV(InNewVelocity);
		}
	}
}

FVector FPhysInterface_Chaos::GetAngularVelocity_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
	if (ensure(FPhysicsInterface::IsValid(InActorReference)))
	{
		Chaos::TKinematicGeometryParticle<float, 3>* Kinematic = InActorReference->CastToKinematicParticle();
		if (ensure(Kinematic))
		{
			return Kinematic->W();
		}
	}

	return FVector(0);
}

void FPhysInterface_Chaos::SetAngularVelocity_AssumesLocked(const FPhysicsActorHandle& InActorReference, const FVector& InNewAngularVelocity, bool bAutoWake)
{
	// TODO: Implement bAutoWake == false.
	ensure(bAutoWake);

	if (ensure(FPhysicsInterface::IsValid(InActorReference)))
	{
		Chaos::TKinematicGeometryParticle<float, 3>* Kinematic = InActorReference->CastToKinematicParticle();
		if (ensure(Kinematic))
		{
			return Kinematic->SetW(InNewAngularVelocity);
		}
	}
}

float FPhysInterface_Chaos::GetMaxAngularVelocity_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
	CHAOS_ENSURE(false);
    return FLT_MAX;
}

void FPhysInterface_Chaos::SetMaxAngularVelocity_AssumesLocked(const FPhysicsActorHandle& InActorReference, float InMaxAngularVelocity)
{
	CHAOS_ENSURE(false);
}

float FPhysInterface_Chaos::GetMaxDepenetrationVelocity_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
	CHAOS_ENSURE(false);
    return FLT_MAX;
}

void FPhysInterface_Chaos::SetMaxDepenetrationVelocity_AssumesLocked(const FPhysicsActorHandle& InActorReference, float InMaxDepenetrationVelocity)
{
	CHAOS_ENSURE(false);
}

FVector FPhysInterface_Chaos::GetWorldVelocityAtPoint_AssumesLocked(const FPhysicsActorHandle& InActorReference, const FVector& InPoint)
{
	if (ensure(FPhysicsInterface::IsValid(InActorReference)))
	{
		Chaos::TKinematicGeometryParticle<float, 3>* Kinematic = InActorReference->CastToKinematicParticle();
		if (ensure(Kinematic))
		{
			const Chaos::TPBDRigidParticle<float,3>* Rigid = Kinematic->CastToRigidParticle();
			const Chaos::FVec3 COM = Rigid ? Chaos::FParticleUtilitiesGT::GetCoMWorldPosition(Rigid) : Chaos::FParticleUtilitiesGT::GetActorWorldTransform(Rigid).GetTranslation();
			const Chaos::FVec3 Diff = InPoint - COM;
			return Kinematic->V() - Chaos::FVec3::CrossProduct(Diff, Kinematic->W());
		}
	}
	return FVector(0);
}

FTransform FPhysInterface_Chaos::GetComTransform_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
	if (ensure(FPhysicsInterface::IsValid(InActorReference)))
	{
		if (const auto* Rigid = InActorReference->CastToRigidParticle())
		{
			return Chaos::FParticleUtilitiesGT::GetCoMWorldTransform(Rigid);
		}
	}
	return FTransform();
}

FTransform FPhysInterface_Chaos::GetComTransformLocal_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
	if (ensure(FPhysicsInterface::IsValid(InActorReference)))
	{
		if (auto* Rigid = InActorReference->CastToRigidParticle())
		{
			return FTransform(Rigid->RotationOfMass(), Rigid->CenterOfMass());
		}
	}
	return FTransform();
}

FVector FPhysInterface_Chaos::GetLocalInertiaTensor_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
	if (Chaos::TPBDRigidParticle<float, 3 >* RigidParticle = InActorReference->CastToRigidParticle())
	{
		const Chaos::PMatrix<float, 3, 3> & Tensor = RigidParticle->I();
		return FVector(Tensor.M[0][0], Tensor.M[1][1], Tensor.M[2][2]) ;
	}
	return FVector::ZeroVector;
}

FBox FPhysInterface_Chaos::GetBounds_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
	using namespace Chaos;
	if(const FImplicitObject* Geometry = InActorReference->Geometry().Get())
	{
		if(Geometry->HasBoundingBox())
		{
			const TBox<FReal,3> LocalBounds = Geometry->BoundingBox();
			const FRigidTransform3 WorldTM(InActorReference->X(),InActorReference->R());
			const TBox<FReal,3> WorldBounds = LocalBounds.TransformedBox(WorldTM);
			return FBox(WorldBounds.Min(), WorldBounds.Max());
		}	
	}
	
	return FBox(EForceInit::ForceInitToZero);
}

void FPhysInterface_Chaos::SetLinearDamping_AssumesLocked(const FPhysicsActorHandle& InActorReference, float InDrag)
{
	if (ensure(FPhysicsInterface::IsValid(InActorReference)))
	{
		Chaos::TPBDRigidParticle<float, 3>* Rigid = InActorReference->CastToRigidParticle();
		if (ensure(Rigid))
		{
			Rigid->SetLinearEtherDrag(InDrag);
		}
	}
}

void FPhysInterface_Chaos::SetAngularDamping_AssumesLocked(const FPhysicsActorHandle& InActorReference, float InDamping)
{
	if (ensure(FPhysicsInterface::IsValid(InActorReference)))
	{
		Chaos::TPBDRigidParticle<float, 3>* Rigid = InActorReference->CastToRigidParticle();
		if (ensure(Rigid))
		{
			Rigid->SetAngularEtherDrag(InDamping);
		}
	}
}

void FPhysInterface_Chaos::AddImpulse_AssumesLocked(const FPhysicsActorHandle& InActorReference, const FVector& InForce)
{
	if (ensure(FPhysicsInterface::IsValid(InActorReference)))
	{
		Chaos::TPBDRigidParticle<float, 3>* Rigid = InActorReference->CastToRigidParticle();
		if (ensure(Rigid))
		{
			Rigid->SetLinearImpulse(Rigid->LinearImpulse() + InForce);
		}
	}
}

void FPhysInterface_Chaos::AddAngularImpulseInRadians_AssumesLocked(const FPhysicsActorHandle& InActorReference, const FVector& InTorque)
{
	if (ensure(FPhysicsInterface::IsValid(InActorReference)))
	{
		Chaos::TPBDRigidParticle<float, 3>* Rigid = InActorReference->CastToRigidParticle();
		if (ensure(Rigid))
		{
			Rigid->SetAngularImpulse(Rigid->AngularImpulse() + InTorque);
		}
	}
}

void FPhysInterface_Chaos::AddVelocity_AssumesLocked(const FPhysicsActorHandle& InActorReference, const FVector& InVelocityDelta)
{
	if (ensure(FPhysicsInterface::IsValid(InActorReference)))
	{
		Chaos::TPBDRigidParticle<float, 3>* Rigid = InActorReference->CastToRigidParticle();
		if (ensure(Rigid))
		{
			AddImpulse_AssumesLocked(InActorReference, Rigid->M() * InVelocityDelta);
		}
	}
}

void FPhysInterface_Chaos::AddAngularVelocityInRadians_AssumesLocked(const FPhysicsActorHandle& InActorReference, const FVector& InAngularVelocityDeltaRad)
{
	if (ensure(FPhysicsInterface::IsValid(InActorReference)))
	{
		Chaos::TPBDRigidParticle<float, 3>* Rigid = InActorReference->CastToRigidParticle();
		if (ensure(Rigid))
		{
			const Chaos::FMatrix33 WorldI = Chaos::FParticleUtilitiesXR::GetWorldInertia(Rigid);
			AddAngularImpulseInRadians_AssumesLocked(InActorReference, WorldI * InAngularVelocityDeltaRad);
		}
	}
}

void FPhysInterface_Chaos::AddImpulseAtLocation_AssumesLocked(const FPhysicsActorHandle& InActorReference, const FVector& InImpulse, const FVector& InLocation)
{
	if (ensure(FPhysicsInterface::IsValid(InActorReference)))
	{
		Chaos::TPBDRigidParticle<float, 3>* Rigid = InActorReference->CastToRigidParticle();
		if (ensure(Rigid))
		{
			const Chaos::FVec3 WorldCOM = Chaos::FParticleUtilitiesGT::GetCoMWorldPosition(Rigid);
			const Chaos::FVec3 AngularImpulse = Chaos::FVec3::CrossProduct(InLocation - WorldCOM, InImpulse);
			AddImpulse_AssumesLocked(InActorReference, InImpulse);
			AddAngularImpulseInRadians_AssumesLocked(InActorReference, AngularImpulse);
		}
	}
}

void FPhysInterface_Chaos::AddRadialImpulse_AssumesLocked(const FPhysicsActorHandle& InActorReference, const FVector& InOrigin, float InRadius, float InStrength, ERadialImpulseFalloff InFalloff, bool bInVelChange)
{
    // @todo(mlentine): We don't currently have a way to apply an instantaneous force. Do we need this?
	CHAOS_ENSURE(false);
}

bool FPhysInterface_Chaos::IsGravityEnabled_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
	if (Chaos::TPBDRigidParticle<float, 3 >* RigidParticle = InActorReference->CastToRigidParticle())
	{
		return RigidParticle->GravityEnabled();
	}
	return false;
}
void FPhysInterface_Chaos::SetGravityEnabled_AssumesLocked(const FPhysicsActorHandle& InActorReference, bool bEnabled)
{
	if (Chaos::TPBDRigidParticle<float, 3 >* RigidParticle = InActorReference->CastToRigidParticle())
	{
		RigidParticle->SetGravityEnabled(bEnabled);
		FPhysicsCommand::ExecuteWrite(InActorReference, [&](const FPhysicsActorHandle& Actor)
		{
			// todo : This is currently synced in FSingleParticlePhysicsProxy<Chaos::TPBDRigidParticle<float, 3>>::PushToPhysicsState. 
			//        Ideally this would execute a write command to the gravity forces on the physics thread. However,
			//        the Actor.Handle() does not have access to the Evolution, so the PerParticleGravityForces are not accessible. 
			//        This will need to be fixed. 
		});
	}
}

float FPhysInterface_Chaos::GetSleepEnergyThreshold_AssumesLocked(const FPhysicsActorHandle& InActorReference)
{
    return 0;
}
void FPhysInterface_Chaos::SetSleepEnergyThreshold_AssumesLocked(const FPhysicsActorHandle& InActorReference, float InEnergyThreshold)
{
}

void FPhysInterface_Chaos::SetMass_AssumesLocked(FPhysicsActorHandle& InActorReference, float InMass)
{
	if (Chaos::TPBDRigidParticle<float, 3 >* RigidParticle = InActorReference->CastToRigidParticle())
	{
		RigidParticle->SetM(InMass);
		if (CHAOS_ENSURE(!FMath::IsNearlyZero(InMass)))
		{
			RigidParticle->SetInvM(1./InMass);
		}
		else
		{
			RigidParticle->SetInvM(0);
		}
	}
}

void FPhysInterface_Chaos::SetMassSpaceInertiaTensor_AssumesLocked(FPhysicsActorHandle& InActorReference, const FVector& InTensor)
{
	if (Chaos::TPBDRigidParticle<float, 3 >* RigidParticle = InActorReference->CastToRigidParticle())
	{
		if(CHAOS_ENSURE(!FMath::IsNearlyZero(InTensor.X)) && CHAOS_ENSURE(!FMath::IsNearlyZero(InTensor.Y)) && CHAOS_ENSURE(!FMath::IsNearlyZero(InTensor.Z)) )
		{
			RigidParticle->SetI(Chaos::PMatrix<float, 3, 3>(InTensor.X, InTensor.Y, InTensor.Z));
			RigidParticle->SetInvI(Chaos::PMatrix<float, 3, 3>(1./InTensor.X, 1./InTensor.Y, 1./InTensor.Z));
		}
	}
}

void FPhysInterface_Chaos::SetComLocalPose_AssumesLocked(const FPhysicsActorHandle& InHandle, const FTransform& InComLocalPose)
{
    //@todo(mlentine): What is InComLocalPose? If the center of an object is not the local pose then many things break including the three vector represtnation of inertia.

	if (auto Rigid = InHandle->CastToRigidParticle())
	{
		Rigid->SetCenterOfMass(InComLocalPose.GetLocation());
		Rigid->SetRotationOfMass(InComLocalPose.GetRotation());
	}
}

void FPhysInterface_Chaos::SetIsSimulationShape(const FPhysicsShapeHandle& InShape, bool bIsSimShape)
{
	InShape.Shape->SetSimEnabled(bIsSimShape);
}

void FPhysInterface_Chaos::SetIsQueryShape(const FPhysicsShapeHandle& InShape, bool bIsQueryShape)
{
	InShape.Shape->SetQueryEnabled(bIsQueryShape);
}

float FPhysInterface_Chaos::GetStabilizationEnergyThreshold_AssumesLocked(const FPhysicsActorHandle& InHandle)
{
	// #todo : Implement
	return 0.0f;
}

void FPhysInterface_Chaos::SetStabilizationEnergyThreshold_AssumesLocked(const FPhysicsActorHandle& InHandle, float InThreshold)
{
	// #todo : Implement
}

uint32 FPhysInterface_Chaos::GetSolverPositionIterationCount_AssumesLocked(const FPhysicsActorHandle& InHandle)
{
	// #todo : Implement
	return 0;
}

void FPhysInterface_Chaos::SetSolverPositionIterationCount_AssumesLocked(const FPhysicsActorHandle& InHandle, uint32 InSolverIterationCount)
{
	// #todo : Implement
}

uint32 FPhysInterface_Chaos::GetSolverVelocityIterationCount_AssumesLocked(const FPhysicsActorHandle& InHandle)
{
	// #todo : Implement
	return 0;
}

void FPhysInterface_Chaos::SetSolverVelocityIterationCount_AssumesLocked(const FPhysicsActorHandle& InHandle, uint32 InSolverIterationCount)
{
	// #todo : Implement
}

float FPhysInterface_Chaos::GetWakeCounter_AssumesLocked(const FPhysicsActorHandle& InHandle)
{
	// #todo : Implement
	return 0.0f;
}

void FPhysInterface_Chaos::SetWakeCounter_AssumesLocked(const FPhysicsActorHandle& InHandle, float InWakeCounter)
{
	// #todo : Implement
}

void FPhysInterface_Chaos::SetInitialized_AssumesLocked(const FPhysicsActorHandle& InHandle, bool InInitialized)
{
	Chaos::TPBDRigidParticle<float, 3>* Rigid = InHandle->CastToRigidParticle();
	if (Rigid)
	{
		Rigid->SetInitialized(InInitialized);
	}
}

SIZE_T FPhysInterface_Chaos::GetResourceSizeEx(const FPhysicsActorHandle& InActorRef)
{
    return sizeof(FPhysicsActorHandle);
}
	
// Constraints
FPhysicsConstraintHandle FPhysInterface_Chaos::CreateConstraint( const FPhysicsActorHandle& InActorRef1, const FPhysicsActorHandle& InActorRef2, const FTransform& InLocalFrame1, const FTransform& InLocalFrame2 )
{
	FPhysicsConstraintHandle ConstraintRef;
#ifdef USE_CHAOS_JOINT_CONSTRAINTS
	{
		if (InActorRef1 != nullptr && InActorRef2 != nullptr)
		{
			if (InActorRef1->GetProxy() != nullptr && InActorRef2->GetProxy() != nullptr)
			{
				LLM_SCOPE(ELLMTag::Chaos);

				ConstraintRef.Constraint = new Chaos::FJointConstraint();

				Chaos::FJointConstraint::FParticlePair JointParticles = { InActorRef1, InActorRef2 };
				ConstraintRef.Constraint->SetJointParticles({ InActorRef1, InActorRef2 });
				ConstraintRef.Constraint->SetJointTransforms({ InLocalFrame1, InLocalFrame2 });
				
				Chaos::FPhysicsSolver* Solver = InActorRef1->GetProxy()->GetSolver<Chaos::FPhysicsSolver>();
				checkSlow(Solver == InActorRef2->GetProxy()->GetSolver<Chaos::FPhysicsSolver>());
				Solver->RegisterObject(ConstraintRef.Constraint);
			}
		}
	}
#endif // USE_CHAOS_JOINT_CONSTRAINTS
	return ConstraintRef;
}

void FPhysInterface_Chaos::SetConstraintUserData(const FPhysicsConstraintHandle& InConstraintRef, void* InUserData)
{
	// #todo : Implement
}

void FPhysInterface_Chaos::ReleaseConstraint(FPhysicsConstraintHandle& InConstraintRef)
{
#ifdef USE_CHAOS_JOINT_CONSTRAINTS
	{
		LLM_SCOPE(ELLMTag::Chaos);

		check(InConstraintRef.Constraint->GetProxy<FJointConstraintPhysicsProxy>());
        FJointConstraintPhysicsProxy* Proxy = InConstraintRef.Constraint->GetProxy<FJointConstraintPhysicsProxy>();

		check(Proxy->GetSolver<Chaos::FPhysicsSolver>());
		Chaos::FPhysicsSolver* Solver = Proxy->GetSolver<Chaos::FPhysicsSolver>();

		Solver->UnregisterObject(InConstraintRef.Constraint);

		delete InConstraintRef.Constraint;
		InConstraintRef.Constraint = nullptr;
	}
#endif // USE_CHAOS_JOINT_CONSTRAINTS
}

FTransform FPhysInterface_Chaos::GetLocalPose(const FPhysicsConstraintHandle& InConstraintRef, EConstraintFrame::Type InFrame)
{
	// #todo : Implement
	//
	//int32 Index1 = InConstraintRef.GetScene()->MSpringConstraints->Constraints()[InConstraintRef.GetScene()->GetConstraintIndexFromId(InConstraintRef.GetId())][0];
	//int32 Index2 = InConstraintRef.GetScene()->MSpringConstraints->Constraints()[InConstraintRef.GetScene()->GetConstraintIndexFromId(InConstraintRef.GetId())][1];
	//Chaos::TRigidTransform<float, 3> Transform1(InConstraintRef.GetScene()->Scene.GetSolver()->GetRigidParticles().X(Index1), InConstraintRef.GetScene()->Scene.GetSolver()->GetRigidParticles().R(Index1));
	//Chaos::TRigidTransform<float, 3> Transform2(InConstraintRef.GetScene()->Scene.GetSolver()->GetRigidParticles().X(Index2), InConstraintRef.GetScene()->Scene.GetSolver()->GetRigidParticles().R(Index2));
	// @todo(mlentine): This is likely broken
	//FTransform(Transform1.Inverse() * Transform2);

	return  FTransform();
}

FTransform FPhysInterface_Chaos::GetGlobalPose(const FPhysicsConstraintHandle& InConstraintRef, EConstraintFrame::Type InFrame)
{
	// #todo : Implement
	return  FTransform();
}

FVector FPhysInterface_Chaos::GetLocation(const FPhysicsConstraintHandle& InConstraintRef)
{
	// #todo : Implement
	return  FVector(0.f);
}

void FPhysInterface_Chaos::GetForce(const FPhysicsConstraintHandle& InConstraintRef, FVector& OutLinForce, FVector& OutAngForce)
{
	// #todo : Implement
}

void FPhysInterface_Chaos::GetDriveLinearVelocity(const FPhysicsConstraintHandle& InConstraintRef, FVector& OutLinVelocity)
{
	// #todo : Implement
}

void FPhysInterface_Chaos::GetDriveAngularVelocity(const FPhysicsConstraintHandle& InConstraintRef, FVector& OutAngVelocity)
{
	// #todo : Implement
}

float FPhysInterface_Chaos::GetCurrentSwing1(const FPhysicsConstraintHandle& InConstraintRef)
{
    return GetLocalPose(InConstraintRef, EConstraintFrame::Frame2).GetRotation().Euler().X;
}

float FPhysInterface_Chaos::GetCurrentSwing2(const FPhysicsConstraintHandle& InConstraintRef)
{
    return GetLocalPose(InConstraintRef, EConstraintFrame::Frame2).GetRotation().Euler().Y;
}

float FPhysInterface_Chaos::GetCurrentTwist(const FPhysicsConstraintHandle& InConstraintRef)
{
    return GetLocalPose(InConstraintRef, EConstraintFrame::Frame2).GetRotation().Euler().Z;
}

void FPhysInterface_Chaos::SetCanVisualize(const FPhysicsConstraintHandle& InConstraintRef, bool bInCanVisualize)
{

}

void FPhysInterface_Chaos::SetCollisionEnabled(const FPhysicsConstraintHandle& InConstraintRef, bool bInCollisionEnabled)
{
	if (InConstraintRef.IsValid())
	{
		if (Chaos::FJointConstraint* Constraint = InConstraintRef.Constraint)
		{
			Constraint->SetCollisionEnabled(bInCollisionEnabled);
		}
	}
}

void FPhysInterface_Chaos::SetProjectionEnabled_AssumesLocked(const FPhysicsConstraintHandle& InConstraintRef, bool bInProjectionEnabled, float InLinearTolerance, float InAngularToleranceDegrees)
{
	if (InConstraintRef.IsValid())
	{
		if (Chaos::FJointConstraint* Constraint = InConstraintRef.Constraint)
		{
			Constraint->SetProjectionEnabled(bInProjectionEnabled);

			// @todo(chaos) : Constraint solver data is solver specific, so it needs and interface against the solver not the constraint handle. 
			//Constraint->SetSolverPositionTolerance(InLinearTolerance);
			//Constraint->SetSolverAngularTolerance(InAngularToleranceDegrees);
		}
	}
}

void FPhysInterface_Chaos::SetParentDominates_AssumesLocked(const FPhysicsConstraintHandle& InConstraintRef, bool bInParentDominates)
{
	if (InConstraintRef.IsValid())
	{
		if (Chaos::FJointConstraint* Constraint = InConstraintRef.Constraint)
		{
			if (bInParentDominates)
			{
				Constraint->SetParentInvMassScale(0.f);
			}
			else
			{
				Constraint->SetParentInvMassScale(1.f);
			}
		}
	}
}

void FPhysInterface_Chaos::SetBreakForces_AssumesLocked(const FPhysicsConstraintHandle& InConstraintRef, float InLinearBreakForce, float InAngularBreakForce)
{
}

void FPhysInterface_Chaos::SetLocalPose(const FPhysicsConstraintHandle& InConstraintRef, const FTransform& InPose, EConstraintFrame::Type InFrame)
{

}

void FPhysInterface_Chaos::SetLinearMotionLimitType_AssumesLocked(const FPhysicsConstraintHandle& InConstraintRef, PhysicsInterfaceTypes::ELimitAxis InAxis, ELinearConstraintMotion InMotion)
{

}

void FPhysInterface_Chaos::SetAngularMotionLimitType_AssumesLocked(const FPhysicsConstraintHandle& InConstraintRef, PhysicsInterfaceTypes::ELimitAxis InAxis, EAngularConstraintMotion InMotion)
{

}

void FPhysInterface_Chaos::UpdateLinearLimitParams_AssumesLocked(const FPhysicsConstraintHandle& InConstraintRef, float InLimit, float InAverageMass, const FLinearConstraint& InParams)
{

}

void FPhysInterface_Chaos::UpdateConeLimitParams_AssumesLocked(const FPhysicsConstraintHandle& InConstraintRef, float InAverageMass, const FConeConstraint& InParams)
{

}

void FPhysInterface_Chaos::UpdateTwistLimitParams_AssumesLocked(const FPhysicsConstraintHandle& InConstraintRef, float InAverageMass, const FTwistConstraint& InParams)
{

}

void FPhysInterface_Chaos::UpdateLinearDrive_AssumesLocked(const FPhysicsConstraintHandle& InConstraintRef, const FLinearDriveConstraint& InDriveParams)
{

}

void FPhysInterface_Chaos::UpdateAngularDrive_AssumesLocked(const FPhysicsConstraintHandle& InConstraintRef, const FAngularDriveConstraint& InDriveParams)
{

}

void FPhysInterface_Chaos::UpdateDriveTarget_AssumesLocked(const FPhysicsConstraintHandle& InConstraintRef, const FLinearDriveConstraint& InLinDrive, const FAngularDriveConstraint& InAngDrive)
{

}

void FPhysInterface_Chaos::SetDrivePosition(const FPhysicsConstraintHandle& InConstraintRef, const FVector& InPosition)
{

}

void FPhysInterface_Chaos::SetDriveOrientation(const FPhysicsConstraintHandle& InConstraintRef, const FQuat& InOrientation)
{

}

void FPhysInterface_Chaos::SetDriveLinearVelocity(const FPhysicsConstraintHandle& InConstraintRef, const FVector& InLinVelocity)
{

}

void FPhysInterface_Chaos::SetDriveAngularVelocity(const FPhysicsConstraintHandle& InConstraintRef, const FVector& InAngVelocity)
{

}

void FPhysInterface_Chaos::SetTwistLimit(const FPhysicsConstraintHandle& InConstraintRef, float InLowerLimit, float InUpperLimit, float InContactDistance)
{

}

void FPhysInterface_Chaos::SetSwingLimit(const FPhysicsConstraintHandle& InConstraintRef, float InYLimit, float InZLimit, float InContactDistance)
{

}

void FPhysInterface_Chaos::SetLinearLimit(const FPhysicsConstraintHandle& InConstraintRef, float InLimit)
{

}

bool FPhysInterface_Chaos::IsBroken(const FPhysicsConstraintHandle& InConstraintRef)
{
	// #todo : Implement
	return true;
}

enum class EPhysicsInterfaceScopedLockType : uint8
{
	Read,
	Write
};

struct FScopedSceneLock_Chaos
{
	FScopedSceneLock_Chaos(FPhysicsActorHandle const * InActorHandle, EPhysicsInterfaceScopedLockType InLockType)
		: LockType(InLockType)
	{
		Scene = GetSceneForActor(InActorHandle);
		LockScene();
	}

	FScopedSceneLock_Chaos(FPhysicsActorHandle const * InActorHandleA, FPhysicsActorHandle const * InActorHandleB, EPhysicsInterfaceScopedLockType InLockType)
		: LockType(InLockType)
	{
		FPhysScene_ChaosInterface* SceneA = GetSceneForActor(InActorHandleA);
		FPhysScene_ChaosInterface* SceneB = GetSceneForActor(InActorHandleB);

		if(SceneA == SceneB)
		{
			Scene = SceneA;
		}
		else if(!SceneA || !SceneB)
		{
			Scene = SceneA ? SceneA : SceneB;
		}
		else
		{
			UE_LOG(LogPhysics, Warning, TEXT("Attempted to aquire a physics scene lock for two paired actors that were not in the same scene. Skipping lock"));
		}

		LockScene();
	}

	FScopedSceneLock_Chaos(FPhysicsConstraintHandle const * InHandle, EPhysicsInterfaceScopedLockType InLockType)
		: Scene(nullptr)
		, LockType(InLockType)
	{
		UE_LOG(LogPhysics, Warning, TEXT("Constraint instance attempted scene lock, Constraints currently unimplemented"));
	}

	FScopedSceneLock_Chaos(USkeletalMeshComponent* InSkelMeshComp, EPhysicsInterfaceScopedLockType InLockType)
		: LockType(InLockType)
	{
		Scene = nullptr;

		if(InSkelMeshComp)
		{
			for(FBodyInstance* BI : InSkelMeshComp->Bodies)
			{
				Scene = GetSceneForActor(&BI->GetPhysicsActorHandle());
				if(Scene)
				{
					break;
				}
			}
		}

		LockScene();
	}

	FScopedSceneLock_Chaos(FPhysScene_ChaosInterface* InScene, EPhysicsInterfaceScopedLockType InLockType)
		: Scene(InScene)
		, LockType(InLockType)
	{
		LockScene();
	}

	~FScopedSceneLock_Chaos()
	{
		UnlockScene();
	}

private:

	void LockScene()
	{
		if(!Scene)
		{
			return;
		}

		switch(LockType)
		{
		case EPhysicsInterfaceScopedLockType::Read:
			Scene->GetScene().ExternalDataLock.ReadLock();
			break;
		case EPhysicsInterfaceScopedLockType::Write:
			Scene->GetScene().ExternalDataLock.WriteLock();
			break;
		}
	}

	void UnlockScene()
	{
		if(!Scene)
		{
			return;
		}

		switch(LockType)
		{
		case EPhysicsInterfaceScopedLockType::Read:
			Scene->GetScene().ExternalDataLock.ReadUnlock();
			break;
		case EPhysicsInterfaceScopedLockType::Write:
			Scene->GetScene().ExternalDataLock.WriteUnlock();
			break;
		}
	}

	FPhysScene_ChaosInterface* GetSceneForActor(FPhysicsActorHandle const * InActorHandle)
	{
		FBodyInstance* ActorInstance = (*InActorHandle) ? FPhysicsUserData_Chaos::Get<FBodyInstance>((*InActorHandle)->UserData()) : nullptr;

		if(ActorInstance)
		{
			return ActorInstance->GetPhysicsScene();
		}

		return nullptr;
	}

	FPhysScene_ChaosInterface* Scene;
	EPhysicsInterfaceScopedLockType LockType;
};

bool FPhysInterface_Chaos::ExecuteOnUnbrokenConstraintReadOnly(const FPhysicsConstraintHandle& InConstraintRef, TFunctionRef<void(const FPhysicsConstraintHandle&)> Func)
{
    if (!IsBroken(InConstraintRef))
    {
		FScopedSceneLock_Chaos SceneLock(&InConstraintRef, EPhysicsInterfaceScopedLockType::Read);
        Func(InConstraintRef);
        return true;
    }
    return false;
}

bool FPhysInterface_Chaos::ExecuteOnUnbrokenConstraintReadWrite(const FPhysicsConstraintHandle& InConstraintRef, TFunctionRef<void(const FPhysicsConstraintHandle&)> Func)
{
    if (!IsBroken(InConstraintRef))
    {
		FScopedSceneLock_Chaos SceneLock(&InConstraintRef, EPhysicsInterfaceScopedLockType::Write);
        Func(InConstraintRef);
        return true;
    }
    return false;
}

bool FPhysInterface_Chaos::ExecuteRead(const FPhysicsActorHandle& InActorReference, TFunctionRef<void(const FPhysicsActorHandle& Actor)> InCallable)
{
	if(InActorReference)
	{
		FScopedSceneLock_Chaos SceneLock(&InActorReference, EPhysicsInterfaceScopedLockType::Read);

		InCallable(InActorReference);
		return true;
	}
	return false;
}

bool FPhysInterface_Chaos::ExecuteRead(USkeletalMeshComponent* InMeshComponent, TFunctionRef<void()> InCallable)
{
	FScopedSceneLock_Chaos SceneLock(InMeshComponent, EPhysicsInterfaceScopedLockType::Read);
	InCallable();
	return true;
}

bool FPhysInterface_Chaos::ExecuteRead(const FPhysicsActorHandle& InActorReferenceA, const FPhysicsActorHandle& InActorReferenceB, TFunctionRef<void(const FPhysicsActorHandle& ActorA, const FPhysicsActorHandle& ActorB)> InCallable)
{
	FScopedSceneLock_Chaos SceneLock(&InActorReferenceA, &InActorReferenceB, EPhysicsInterfaceScopedLockType::Read);
	InCallable(InActorReferenceA, InActorReferenceB);
	return true;
}

bool FPhysInterface_Chaos::ExecuteRead(const FPhysicsConstraintHandle& InConstraintRef, TFunctionRef<void(const FPhysicsConstraintHandle& Constraint)> InCallable)
{
	if(InConstraintRef.IsValid())
	{
		FScopedSceneLock_Chaos SceneLock(&InConstraintRef, EPhysicsInterfaceScopedLockType::Read);
		InCallable(InConstraintRef);
		return true;
	}

	return false;
}

bool FPhysInterface_Chaos::ExecuteRead(FPhysScene* InScene, TFunctionRef<void()> InCallable)
{
	if(InScene)
	{
		FScopedSceneLock_Chaos SceneLock(InScene, EPhysicsInterfaceScopedLockType::Read);
		InCallable();
		return true;
	}

	return false;
}

bool FPhysInterface_Chaos::ExecuteWrite(const FPhysicsActorHandle& InActorReference, TFunctionRef<void(const FPhysicsActorHandle& Actor)> InCallable)
{
	//why do we have a write that takes in a const handle?
	if(InActorReference)
	{
		FScopedSceneLock_Chaos SceneLock(&InActorReference, EPhysicsInterfaceScopedLockType::Write);
		InCallable(InActorReference);
		return true;
	}
	return false;
}

bool FPhysInterface_Chaos::ExecuteWrite(FPhysicsActorHandle& InActorReference, TFunctionRef<void(FPhysicsActorHandle& Actor)> InCallable)
{
	if(InActorReference)
	{
		FScopedSceneLock_Chaos SceneLock(&InActorReference, EPhysicsInterfaceScopedLockType::Write);
		InCallable(InActorReference);
		return true;
	}

	return false;
}

bool FPhysInterface_Chaos::ExecuteWrite(USkeletalMeshComponent* InMeshComponent, TFunctionRef<void()> InCallable)
{
	FScopedSceneLock_Chaos SceneLock(InMeshComponent, EPhysicsInterfaceScopedLockType::Write);
	InCallable();
	return true;
}

bool FPhysInterface_Chaos::ExecuteWrite(const FPhysicsActorHandle& InActorReferenceA, const FPhysicsActorHandle& InActorReferenceB, TFunctionRef<void(const FPhysicsActorHandle& ActorA, const FPhysicsActorHandle& ActorB)> InCallable)
{
	FScopedSceneLock_Chaos SceneLock(&InActorReferenceA, &InActorReferenceB, EPhysicsInterfaceScopedLockType::Write);
	InCallable(InActorReferenceA, InActorReferenceB);
	return true;
}

bool FPhysInterface_Chaos::ExecuteWrite(const FPhysicsConstraintHandle& InConstraintRef, TFunctionRef<void(const FPhysicsConstraintHandle& Constraint)> InCallable)
{
	if(InConstraintRef.IsValid())
	{
		FScopedSceneLock_Chaos SceneLock(&InConstraintRef, EPhysicsInterfaceScopedLockType::Write);
		InCallable(InConstraintRef);
		return true;
	}

	return false;
}

bool FPhysInterface_Chaos::ExecuteWrite(FPhysScene* InScene, TFunctionRef<void()> InCallable)
{
	if(InScene)
	{
		FScopedSceneLock_Chaos SceneLock(InScene, EPhysicsInterfaceScopedLockType::Write);
		InCallable();
		return true;
	}

	return false;
}

void FPhysInterface_Chaos::ExecuteShapeWrite(FBodyInstance* InInstance, FPhysicsShapeHandle& InShape, TFunctionRef<void(FPhysicsShapeHandle& InShape)> InCallable)
{
	if(InInstance && InShape.IsValid())
	{
		FScopedSceneLock_Chaos SceneLock(&InInstance->GetPhysicsActorHandle(), EPhysicsInterfaceScopedLockType::Write);
		InCallable(InShape);
	}
}


FPhysicsShapeHandle FPhysInterface_Chaos::CreateShape(physx::PxGeometry* InGeom, bool bSimulation, bool bQuery, UPhysicalMaterial* InSimpleMaterial, TArray<UPhysicalMaterial*>* InComplexMaterials)
{
	// #todo : Implement
	// @todo(mlentine): Should we be doing anything with the InGeom here?
    FPhysicsActorHandle NewActor = nullptr;
	return { nullptr, NewActor };
}

const FBodyInstance* FPhysInterface_Chaos::ShapeToOriginalBodyInstance(const FBodyInstance* InCurrentInstance, const Chaos::FPerShapeData* InShape)
{
	//question: this is identical to physx version, should it be in body instance?
	check(InCurrentInstance);
	check(InShape);

	const FBodyInstance* TargetInstance = InCurrentInstance->WeldParent ? InCurrentInstance->WeldParent : InCurrentInstance;
	const FBodyInstance* OutInstance = TargetInstance;

	if (const TMap<FPhysicsShapeHandle, FBodyInstance::FWeldInfo>* WeldInfo = InCurrentInstance->GetCurrentWeldInfo())
	{
		for (const TPair<FPhysicsShapeHandle, FBodyInstance::FWeldInfo>& Pair : *WeldInfo)
		{
			if (Pair.Key.Shape == InShape)
			{
				TargetInstance = Pair.Value.ChildBI;
			}
		}
	}

	return TargetInstance;
}



void FPhysInterface_Chaos::AddGeometry(FPhysicsActorHandle& InActor, const FGeometryAddParams& InParams, TArray<FPhysicsShapeHandle>* OutOptShapes)
{
	LLM_SCOPE(ELLMTag::ChaosGeometry);
	TArray<TUniquePtr<Chaos::FImplicitObject>> Geoms;
	Chaos::FShapesArray Shapes;
	ChaosInterface::CreateGeometry(InParams, Geoms, Shapes);

#if WITH_CHAOS
	if (InActor && Geoms.Num())
	{
		for (TUniquePtr<Chaos::FPerShapeData>& Shape : Shapes)
		{
			FPhysicsShapeHandle NewHandle(Shape.Get(), InActor);
			if (OutOptShapes)
			{
				OutOptShapes->Add(NewHandle);
			}

			FBodyInstance::ApplyMaterialToShape_AssumesLocked(NewHandle, InParams.SimpleMaterial, InParams.ComplexMaterials, &InParams.ComplexMaterialMasks);

			//TArrayView<UPhysicalMaterial*> SimpleView = MakeArrayView(&(const_cast<UPhysicalMaterial*>(InParams.SimpleMaterial)), 1);
			//FPhysInterface_Chaos::SetMaterials(NewHandle, InParams.ComplexMaterials.Num() > 0 ? InParams.ComplexMaterials : SimpleView);
		}

		//todo: we should not be creating unique geometry per actor
		if(Geoms.Num() > 1)
		{
			InActor->SetGeometry(MakeUnique<Chaos::FImplicitObjectUnion>(MoveTemp(Geoms)));
		}
		else
		{
			InActor->SetGeometry(MoveTemp(Geoms[0]));
		}
		InActor->SetShapesArray(MoveTemp(Shapes));
	}
#endif
}


// @todo(chaos): We probably need to actually duplicate the data here, add virtual TImplicitObject::NewCopy()
FPhysicsShapeHandle FPhysInterface_Chaos::CloneShape(const FPhysicsShapeHandle& InShape)
{
	FPhysicsActorHandle NewActor = nullptr;
	return { InShape.Shape, NewActor };
}

FPhysicsGeometryCollection_Chaos FPhysInterface_Chaos::GetGeometryCollection(const FPhysicsShapeHandle& InShape)
{
	FPhysicsGeometryCollection_Chaos NewCollection(InShape);
	return NewCollection;
}


FCollisionFilterData FPhysInterface_Chaos::GetSimulationFilter(const FPhysicsShapeReference_Chaos& InShape)
{
	if (ensure(InShape.Shape))
	{
		return InShape.Shape->GetSimData();
	}
	else
	{
		return FCollisionFilterData();
	}
}

FCollisionFilterData FPhysInterface_Chaos::GetQueryFilter(const FPhysicsShapeReference_Chaos& InShape)
{
	if (ensure(InShape.Shape))
	{
		return InShape.Shape->GetQueryData();
	}
	else
	{
		return FCollisionFilterData();
	}
}

void FPhysInterface_Chaos::SetQueryFilter(const FPhysicsShapeReference_Chaos& InShapeRef, const FCollisionFilterData& InFilter)
{
	InShapeRef.Shape->SetQueryData(InFilter);
}

void FPhysInterface_Chaos::SetSimulationFilter(const FPhysicsShapeReference_Chaos& InShapeRef, const FCollisionFilterData& InFilter)
{
	InShapeRef.Shape->SetSimData(InFilter);
}

bool FPhysInterface_Chaos::IsSimulationShape(const FPhysicsShapeHandle& InShape)
{
	return InShape.Shape->GetSimEnabled();
}

bool FPhysInterface_Chaos::IsQueryShape(const FPhysicsShapeHandle& InShape)
{
	// This data is not stored on concrete shape. TODO: Remove ensure if we actually use this flag when constructing shape handles.
	CHAOS_ENSURE(false);
	return InShape.Shape->GetQueryEnabled();
}

ECollisionShapeType FPhysInterface_Chaos::GetShapeType(const FPhysicsShapeReference_Chaos& InShapeRef)
{
	return GetImplicitType(*InShapeRef.Shape->GetGeometry());
}

FTransform FPhysInterface_Chaos::GetLocalTransform(const FPhysicsShapeReference_Chaos& InShapeRef)
{
    // Transforms are baked into the object so there is never a local transform
    if (InShapeRef.Shape->GetGeometry()->GetType() == Chaos::ImplicitObjectType::Transformed && FPhysicsInterface::IsValid(InShapeRef.ActorRef))
    {
        return InShapeRef.Shape->GetGeometry()->GetObject<Chaos::TImplicitObjectTransformed<float, 3>>()->GetTransform();
    }
    else
    {
        return FTransform();
    }
}

void FPhysInterface_Chaos::SetLocalTransform(const FPhysicsShapeHandle& InShape, const FTransform& NewLocalTransform)
{
#if !WITH_CHAOS_NEEDS_TO_BE_FIXED
    if (InShape.ActorRef.IsValid())
    {
        TArray<RigidBodyId> Ids = {InShape.ActorRef.GetId()};
        const auto Index = InShape.ActorRef.GetScene()->GetIndexFromId(InShape.ActorRef.GetId());
        if (InShape.Object->GetType() == Chaos::ImplicitObjectType::Transformed)
        {
            // @todo(mlentine): We can avoid creating a new object here by adding delayed update support for the object transforms
            LocalParticles.SetDynamicGeometry(Index, MakeUnique<Chaos::TImplicitObjectTransformed<float, 3>>(InShape.Object->GetObject<Chaos::TImplicitObjectTransformed<float, 3>>()->Object(), NewLocalTransform));
        }
        else
        {
            LocalParticles.SetDynamicGeometry(Index, MakeUnique<Chaos::TImplicitObjectTransformed<float, 3>>(InShape.Object, NewLocalTransform));
        }
    }
    {
        if (InShape.Object->GetType() == Chaos::ImplicitObjectType::Transformed)
        {
            InShape.Object->GetObject<Chaos::TImplicitObjectTransformed<float, 3>>()->SetTransform(NewLocalTransform);
        }
        else
        {
            const_cast<FPhysicsShapeHandle&>(InShape).Object = new Chaos::TImplicitObjectTransformed<float, 3>(InShape.Object, NewLocalTransform);
        }
    }
#endif
}

void FPhysInterface_Chaos::SetMaterials(const FPhysicsShapeHandle& InShape, const TArrayView<UPhysicalMaterial*> InMaterials)
{
	// Build a list of handles to store on the shape
	TArray<Chaos::FMaterialHandle> NewMaterialHandles;
	NewMaterialHandles.Reserve(InMaterials.Num());

	for(UPhysicalMaterial* UnrealMaterial : InMaterials)
	{
		NewMaterialHandles.Add(UnrealMaterial->GetPhysicsMaterial());
	}

	InShape.Shape->SetMaterials(NewMaterialHandles);
}

void FPhysInterface_Chaos::SetMaterials(const FPhysicsShapeHandle& InShape, const TArrayView<UPhysicalMaterial*> InMaterials, const TArrayView<FPhysicalMaterialMaskParams>& InMaterialMasks)
{
	SetMaterials(InShape, InMaterials);

	if (InMaterialMasks.Num() > 0)
	{
		// Build a list of handles to store on the shape
		TArray<Chaos::FMaterialMaskHandle> NewMaterialMaskHandles;
		TArray<uint32> NewMaterialMaskMaps;
		TArray<Chaos::FMaterialHandle> NewMaterialMaskMaterialHandles;

		NewMaterialMaskHandles.Reserve(InMaterialMasks.Num());

		int MaskMapMatIdx = 0;

		InShape.Shape->ModifyMaterialMaskMaps([&](auto& MaterialMaskMaps)
		{
			for(FPhysicalMaterialMaskParams& MaterialMaskData : InMaterialMasks)
		{
				if(MaterialMaskData.PhysicalMaterialMask && ensure(MaterialMaskData.PhysicalMaterialMap))
			{
				NewMaterialMaskHandles.Add(MaterialMaskData.PhysicalMaterialMask->GetPhysicsMaterialMask());
					for(int i = 0; i < EPhysicalMaterialMaskColor::MAX; i++)
				{
						if(UPhysicalMaterial* MapMat = MaterialMaskData.PhysicalMaterialMap->GetPhysicalMaterialFromMap(i))
					{
							MaterialMaskMaps.Emplace(MaskMapMatIdx);
						MaskMapMatIdx++;
						} else
					{
							MaterialMaskMaps.Emplace(INDEX_NONE);
				}
			}
				} else
			{
				NewMaterialMaskHandles.Add(Chaos::FMaterialMaskHandle());
					for(int i = 0; i < EPhysicalMaterialMaskColor::MAX; i++)
				{
						MaterialMaskMaps.Emplace(INDEX_NONE);
				}
			}
		}

		});
		

		
		if (MaskMapMatIdx > 0)
		{
			NewMaterialMaskMaterialHandles.Reserve(MaskMapMatIdx);

			uint32 Offset = 0;

			for (FPhysicalMaterialMaskParams& MaterialMaskData : InMaterialMasks)
			{
				if (MaterialMaskData.PhysicalMaterialMask)
				{
					for (int i = 0; i < EPhysicalMaterialMaskColor::MAX; i++)
					{
						if (UPhysicalMaterial* MapMat = MaterialMaskData.PhysicalMaterialMap->GetPhysicalMaterialFromMap(i))
						{
							NewMaterialMaskMaterialHandles.Add(MapMat->GetPhysicsMaterial());
						}
					}
				}
			}
		}

		InShape.Shape->SetMaterialMasks(NewMaterialMaskHandles);
		InShape.Shape->SetMaterialMaskMapMaterials(NewMaterialMaskMaterialHandles);
	}
}

void FinishSceneStat()
{
}

void FPhysInterface_Chaos::CalculateMassPropertiesFromShapeCollection(Chaos::TMassProperties<float, 3>& OutProperties, const TArray<FPhysicsShapeHandle>& InShapes, float InDensityKGPerCM)
{
	ChaosInterface::CalculateMassPropertiesFromShapeCollection(OutProperties, InShapes, InDensityKGPerCM);
}

bool FPhysInterface_Chaos::LineTrace_Geom(FHitResult& OutHit, const FBodyInstance* InInstance, const FVector& WorldStart, const FVector& WorldEnd, bool bTraceComplex, bool bExtractPhysMaterial)
{
	// Need an instance to trace against
	check(InInstance);

	OutHit.TraceStart = WorldStart;
	OutHit.TraceEnd = WorldEnd;

	bool bHitSomething = false;

	const FVector Delta = WorldEnd - WorldStart;
	const float DeltaMag = Delta.Size();
	if (DeltaMag > KINDA_SMALL_NUMBER)
	{
		{
			// #PHYS2 Really need a concept for "multi" locks here - as we're locking ActorRef but not TargetInstance->ActorRef
			FPhysicsCommand::ExecuteRead(InInstance->ActorHandle, [&](const FPhysicsActorHandle& Actor)
			{
				// If we're welded then the target instance is actually our parent
				const FBodyInstance* TargetInstance = InInstance->WeldParent ? InInstance->WeldParent : InInstance;
				if(const Chaos::TGeometryParticle<float, 3>* RigidBody = TargetInstance->ActorHandle)
				{
					FRaycastHit BestHit;
					BestHit.Distance = FLT_MAX;

					// Get all the shapes from the actor
					PhysicsInterfaceTypes::FInlineShapeArray Shapes;
					const int32 NumShapes = FillInlineShapeArray_AssumesLocked(Shapes, Actor);

					const FTransform WorldTM(RigidBody->R(), RigidBody->X());
					const FVector LocalStart = WorldTM.InverseTransformPositionNoScale(WorldStart);
					const FVector LocalDelta = WorldTM.InverseTransformVectorNoScale(Delta);

					// Iterate over each shape
					for (int32 ShapeIdx = 0; ShapeIdx < NumShapes; ShapeIdx++)
					{
						// #PHYS2 - SHAPES - Resolve this single cast case
						FPhysicsShapeReference_Chaos& ShapeRef = Shapes[ShapeIdx];
						Chaos::FPerShapeData* Shape = ShapeRef.Shape;
						check(Shape);

						if (TargetInstance->IsShapeBoundToBody(ShapeRef) == false)
						{
							continue;
						}

						// Filter so we trace against the right kind of collision
						FCollisionFilterData ShapeFilter = Shape->GetQueryData();
						const bool bShapeIsComplex = (ShapeFilter.Word3 & EPDF_ComplexCollision) != 0;
						const bool bShapeIsSimple = (ShapeFilter.Word3 & EPDF_SimpleCollision) != 0;
						if ((bTraceComplex && bShapeIsComplex) || (!bTraceComplex && bShapeIsSimple))
						{

							float Distance;
							Chaos::TVector<float, 3> LocalPosition;
							Chaos::TVector<float, 3> LocalNormal;

							int32 FaceIndex;
							if (Shape->GetGeometry()->Raycast(LocalStart, LocalDelta / DeltaMag, DeltaMag, 0, Distance, LocalPosition, LocalNormal, FaceIndex))
							{
								if (Distance < BestHit.Distance)
								{
									BestHit.Distance = Distance;
									BestHit.WorldNormal = LocalNormal;	//will convert to world when best is chosen
									BestHit.WorldPosition = LocalPosition;
									BestHit.Shape = Shape;
									BestHit.Actor = Actor;
									BestHit.FaceIndex = FaceIndex;
								}
							}
						}
					}

					if (BestHit.Distance < FLT_MAX)
					{
						BestHit.WorldNormal = WorldTM.TransformVectorNoScale(BestHit.WorldNormal);
						BestHit.WorldPosition = WorldTM.TransformPositionNoScale(BestHit.WorldPosition);
						SetFlags(BestHit, EHitFlags::Distance | EHitFlags::Normal | EHitFlags::Position);

						// we just like to make sure if the hit is made, set to test touch
						FCollisionFilterData QueryFilter;
						QueryFilter.Word2 = 0xFFFFF;

						FTransform StartTM(WorldStart);
						const UPrimitiveComponent* OwnerComponentInst = InInstance->OwnerComponent.Get();
						ConvertQueryImpactHit(OwnerComponentInst ? OwnerComponentInst->GetWorld() : nullptr, BestHit, OutHit, DeltaMag, QueryFilter, WorldStart, WorldEnd, nullptr, StartTM, true, bExtractPhysMaterial);
						bHitSomething = true;
					}
				}
			});
		}
	}

	return bHitSomething;
}

bool FPhysInterface_Chaos::Sweep_Geom(FHitResult& OutHit, const FBodyInstance* InInstance, const FVector& InStart, const FVector& InEnd, const FQuat& InShapeRotation, const FCollisionShape& InShape, bool bSweepComplex)
{
	bool bSweepHit = false;

	if (InShape.IsNearlyZero())
	{
		bSweepHit = LineTrace_Geom(OutHit, InInstance, InStart, InEnd, bSweepComplex);
	}
	else
	{
		OutHit.TraceStart = InStart;
		OutHit.TraceEnd = InEnd;

		const FBodyInstance* TargetInstance = InInstance->WeldParent ? InInstance->WeldParent : InInstance;

		FPhysicsCommand::ExecuteRead(TargetInstance->ActorHandle, [&](const FPhysicsActorHandle& Actor)
		{
			const Chaos::TGeometryParticle<float, 3>* RigidBody = Actor;

			if (RigidBody && InInstance->OwnerComponent.Get())
			{
				FPhysicsShapeAdapter ShapeAdapter(InShapeRotation, InShape);

				const FVector Delta = InEnd - InStart;
				const float DeltaMag = Delta.Size();
				if (DeltaMag > KINDA_SMALL_NUMBER)
				{
					const FTransform ActorTM(RigidBody->R(), RigidBody->X());

					UPrimitiveComponent* OwnerComponentInst = InInstance->OwnerComponent.Get();
					FTransform StartTM(ShapeAdapter.GetGeomOrientation(), InStart);
					FTransform CompTM(OwnerComponentInst->GetComponentTransform());

					Chaos::TVector<float,3> Dir = Delta / DeltaMag;

					FSweepHit Hit;

					// Get all the shapes from the actor
					PhysicsInterfaceTypes::FInlineShapeArray Shapes;
					// #PHYS2 - SHAPES - Resolve this function to not use px stuff
					const int32 NumShapes = FillInlineShapeArray_AssumesLocked(Shapes, Actor); // #PHYS2 - Need a lock/execute here?

					// Iterate over each shape
					for (int32 ShapeIdx = 0; ShapeIdx < NumShapes; ShapeIdx++)
					{
						FPhysicsShapeReference_Chaos& ShapeRef = Shapes[ShapeIdx];
						Chaos::FPerShapeData* Shape = ShapeRef.Shape;
						check(Shape);

						// Skip shapes not bound to this instance
						if (!TargetInstance->IsShapeBoundToBody(ShapeRef))
						{
							continue;
						}

						// Filter so we trace against the right kind of collision
						FCollisionFilterData ShapeFilter = Shape->GetQueryData();
						const bool bShapeIsComplex = (ShapeFilter.Word3 & EPDF_ComplexCollision) != 0;
						const bool bShapeIsSimple = (ShapeFilter.Word3 & EPDF_SimpleCollision) != 0;
						if ((bSweepComplex && bShapeIsComplex) || (!bSweepComplex && bShapeIsSimple))
						{
							//question: this is returning first result, is that valid? Keeping it the same as physx for now
							Chaos::TVector<float, 3> WorldPosition;
							Chaos::TVector<float, 3> WorldNormal;
							int32 FaceIdx;
							if (Chaos::Utilities::CastHelper(ShapeAdapter.GetGeometry(), ActorTM, [&](const auto& Downcast, const auto& FullActorTM) { return Chaos::SweepQuery(*Shape->GetGeometry(), FullActorTM, Downcast, StartTM, Dir, DeltaMag, Hit.Distance, WorldPosition, WorldNormal, FaceIdx, 0.f, false); }))
							{
								// we just like to make sure if the hit is made
								FCollisionFilterData QueryFilter;
								QueryFilter.Word2 = 0xFFFFF;

								// we don't get Shape information when we access via PShape, so I filled it up
								Hit.Shape = Shape;
								Hit.Actor = ShapeRef.ActorRef;
								Hit.WorldPosition = WorldPosition;
								Hit.WorldNormal = WorldNormal;
								Hit.FaceIndex = FaceIdx;
								if (!HadInitialOverlap(Hit))
								{
									Hit.FaceIndex = FindFaceIndex(Hit, Dir);
								}
								SetFlags(Hit, EHitFlags::Distance | EHitFlags::Normal | EHitFlags::Position | EHitFlags::FaceIndex);

								FTransform StartTransform(InStart);
								ConvertQueryImpactHit(OwnerComponentInst->GetWorld(), Hit, OutHit, DeltaMag, QueryFilter, InStart, InEnd, nullptr, StartTransform, false, false);
								bSweepHit = true;
							}
						}
					}
				}
			}
		});
	}

	return bSweepHit;
}

bool Overlap_GeomInternal(const FBodyInstance* InInstance, const Chaos::FImplicitObject& InGeom, const FTransform& GeomTransform, FMTDResult* OutOptResult)
{
	const FBodyInstance* TargetInstance = InInstance->WeldParent ? InInstance->WeldParent : InInstance;
	Chaos::TGeometryParticle<float, 3>* RigidBody = TargetInstance->ActorHandle;

	if (RigidBody == nullptr)
	{
		return false;
	}

	// Get all the shapes from the actor
	PhysicsInterfaceTypes::FInlineShapeArray Shapes;
	const int32 NumShapes = FillInlineShapeArray_AssumesLocked(Shapes, RigidBody);

	const FTransform ActorTM(RigidBody->R(), RigidBody->X());

	// Iterate over each shape
	for (int32 ShapeIdx = 0; ShapeIdx < NumShapes; ++ShapeIdx)
	{
		FPhysicsShapeReference_Chaos& ShapeRef = Shapes[ShapeIdx];
		const Chaos::FPerShapeData* Shape = ShapeRef.Shape;
		check(Shape);

		if (TargetInstance->IsShapeBoundToBody(ShapeRef))
		{
			if (OutOptResult)
			{
				Chaos::FMTDInfo MTDInfo;
				if (Chaos::Utilities::CastHelper(InGeom, ActorTM, [&](const auto& Downcast, const auto& FullActorTM) { return Chaos::OverlapQuery(*Shape->GetGeometry(), FullActorTM, Downcast, GeomTransform, /*Thickness=*/0, &MTDInfo); }))
				{
					OutOptResult->Distance = MTDInfo.Penetration;
					OutOptResult->Direction = MTDInfo.Normal;
					return true;	//question: should we take most shallow penetration?
				}
			}
			else	//question: why do we even allow user to not pass in MTD info?
			{
				if (Chaos::Utilities::CastHelper(InGeom, ActorTM, [&](const auto& Downcast, const auto& FullActorTM) { return Chaos::OverlapQuery(*Shape->GetGeometry(), FullActorTM, Downcast, GeomTransform); }))
				{
					return true;
				}
			}

		}
	}

	return false;
}

bool FPhysInterface_Chaos::Overlap_Geom(const FBodyInstance* InBodyInstance, const FPhysicsGeometryCollection& InGeometry, const FTransform& InShapeTransform, FMTDResult* OutOptResult)
{
	return Overlap_GeomInternal(InBodyInstance, InGeometry.GetGeometry(), InShapeTransform, OutOptResult);
}

bool FPhysInterface_Chaos::Overlap_Geom(const FBodyInstance* InBodyInstance, const FCollisionShape& InCollisionShape, const FQuat& InShapeRotation, const FTransform& InShapeTransform, FMTDResult* OutOptResult)
{
	FPhysicsShapeAdapter Adaptor(InShapeRotation, InCollisionShape);
	return Overlap_GeomInternal(InBodyInstance, Adaptor.GetGeometry(), Adaptor.GetGeomPose(InShapeTransform.GetTranslation()), OutOptResult);
}

bool FPhysInterface_Chaos::GetSquaredDistanceToBody(const FBodyInstance* InInstance, const FVector& InPoint, float& OutDistanceSquared, FVector* OutOptPointOnBody)
{
	if (OutOptPointOnBody)
	{
		*OutOptPointOnBody = InPoint;
		OutDistanceSquared = 0.f;
	}

	float ReturnDistance = -1.f;
	float MinPhi = BIG_NUMBER;
	bool bFoundValidBody = false;
	bool bEarlyOut = true;

	const FBodyInstance* UseBI = InInstance->WeldParent ? InInstance->WeldParent : InInstance;
	const FTransform BodyTM = UseBI->GetUnrealWorldTransform();
	const FVector LocalPoint = BodyTM.InverseTransformPositionNoScale(InPoint);

	FPhysicsCommand::ExecuteRead(UseBI->ActorHandle, [&](const FPhysicsActorHandle& Actor)
	{

		bEarlyOut = false;

		TArray<FPhysicsShapeReference_Chaos> Shapes;
		InInstance->GetAllShapes_AssumesLocked(Shapes);
		for (const FPhysicsShapeReference_Chaos& Shape : Shapes)
		{
			if (UseBI->IsShapeBoundToBody(Shape) == false)	//skip welded shapes that do not belong to us
			{
				continue;
			}

			ECollisionShapeType GeomType = FPhysicsInterface::GetShapeType(Shape);

			if (!Shape.GetGeometry().IsConvex())
			{
				// Type unsupported for this function, but some other shapes will probably work. 
				continue;
			}

			bFoundValidBody = true;

			Chaos::TVector<float, 3> Normal;
			const float Phi = Shape.Shape->GetGeometry()->PhiWithNormal(LocalPoint, Normal);
			if (Phi <= 0)
			{
				OutDistanceSquared = 0;
				if (OutOptPointOnBody)
				{
					*OutOptPointOnBody = InPoint;
				}
				break;
			}
			else if (Phi < MinPhi)
			{
				MinPhi = Phi;
				OutDistanceSquared = Phi * Phi;
				if (OutOptPointOnBody)
				{
					const Chaos::TVector<float, 3> LocalClosestPoint = LocalPoint - Phi * Normal;
					*OutOptPointOnBody = BodyTM.TransformPositionNoScale(LocalClosestPoint);
				}
			}
		}
	});

	if (!bFoundValidBody && !bEarlyOut)
	{
		UE_LOG(LogPhysics, Verbose, TEXT("GetDistanceToBody: Component (%s) has no simple collision and cannot be queried for closest point."), InInstance->OwnerComponent.Get() ? *(InInstance->OwnerComponent->GetPathName()) : TEXT("NONE"));
	}

	return bFoundValidBody;
}

uint32 GetTriangleMeshExternalFaceIndex(const FPhysicsShape& Shape, uint32 InternalFaceIndex)
{
	using namespace Chaos;
	uint8 OuterType = Shape.GetGeometry()->GetType();
	uint8 InnerType = GetInnerType(OuterType);
	if (ensure(InnerType == ImplicitObjectType::TriangleMesh))
	{
		const FTriangleMeshImplicitObject* TriangleMesh = nullptr;

		if (IsScaled(OuterType))
		{
			const TImplicitObjectScaled<FTriangleMeshImplicitObject>& ScaledTriangleMesh = Shape.GetGeometry()->GetObjectChecked<TImplicitObjectScaled<FTriangleMeshImplicitObject>>();
			TriangleMesh = ScaledTriangleMesh.GetUnscaledObject();
		}
		else if(IsInstanced(OuterType))
		{
			TriangleMesh = Shape.GetGeometry()->GetObjectChecked<TImplicitObjectInstanced<FTriangleMeshImplicitObject>>().GetInstancedObject();
		}
		else
		{
			TriangleMesh = &Shape.GetGeometry()->GetObjectChecked<FTriangleMeshImplicitObject>();
		}

		return TriangleMesh->GetExternalFaceIndexFromInternal(InternalFaceIndex);
	}

	return -1;
}

template<typename AllocatorType>
int32 GetAllShapesInternal_AssumedLocked(const FPhysicsActorHandle& InActorHandle, TArray<FPhysicsShapeReference_Chaos, AllocatorType>& OutShapes)
{
	const Chaos::FShapesArray& ShapesArray = InActorHandle->ShapesArray();
	OutShapes.Reset(ShapesArray.Num());
	//todo: can we avoid this construction?
	for (const TUniquePtr<Chaos::FPerShapeData>& Shape : ShapesArray)
	{
		OutShapes.Add(FPhysicsShapeReference_Chaos(Shape.Get(), InActorHandle));
	}
	return OutShapes.Num();
}

template <>
int32 FPhysInterface_Chaos::GetAllShapes_AssumedLocked(const FPhysicsActorHandle& InActorHandle, TArray<FPhysicsShapeReference_Chaos, FDefaultAllocator>& OutShapes)
{
	return GetAllShapesInternal_AssumedLocked(InActorHandle, OutShapes);
}

template <>
int32 FPhysInterface_Chaos::GetAllShapes_AssumedLocked(const FPhysicsActorHandle& InActorHandle, PhysicsInterfaceTypes::FInlineShapeArray& OutShapes)
{
	return GetAllShapesInternal_AssumedLocked(InActorHandle, OutShapes);
}

#endif
