// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.
#include "ChaosCloth/ChaosClothingSimulation.h"
#include "ChaosCloth/ChaosClothPrivate.h"

#include "Async/ParallelFor.h"
#include "ClothingAsset.h"
#include "ClothingSimulation.h" // ClothingSystemRuntimeInterface
#include "Utils/ClothingMeshUtils.h" // ClothingSystemRuntimeCommon
#include "Components/SkeletalMeshComponent.h"
#include "Materials/Material.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Chaos/Box.h"
#include "Chaos/Capsule.h"
#include "Chaos/Cylinder.h"
#include "Chaos/ImplicitObjectIntersection.h"
#include "Chaos/ImplicitObjectUnion.h"
#include "Chaos/Levelset.h"
#include "Chaos/PBDAxialSpringConstraints.h"
#include "Chaos/PBDBendingConstraints.h"
#include "Chaos/PBDParticles.h"
#include "Chaos/PBDSpringConstraints.h"
#include "Chaos/PBDVolumeConstraint.h"
#include "Chaos/PerParticleGravity.h"
#include "Chaos/PerParticlePBDLongRangeConstraints.h"
#include "Chaos/PerParticlePBDShapeConstraints.h"
#include "Chaos/PBDSphericalConstraint.h"
#include "Chaos/Plane.h"
#include "Chaos/Sphere.h"
#include "Chaos/TaperedCylinder.h"
#include "Chaos/Transform.h"
#include "Chaos/Utilities.h"
#include "Chaos/Vector.h"

#if WITH_PHYSX && !PLATFORM_LUMIN && !PLATFORM_ANDROID
#include "PhysXIncludes.h"
#endif

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include "Chaos/ErrorReporter.h"

using namespace Chaos;

static TAutoConsoleVariable<int32> CVarClothNumIterations(TEXT("physics.ClothNumIterations"), 1, TEXT(""));
static TAutoConsoleVariable<float> CVarClothSelfCollisionThickness(TEXT("physics.ClothSelfCollisionThickness"), 2.f, TEXT(""));
static TAutoConsoleVariable<float> CVarClothCollisionThickness(TEXT("physics.ClothCollisionThickness"), 1.2f, TEXT(""));
static TAutoConsoleVariable<float> CVarClothCoefficientOfFriction(TEXT("physics.ClothCoefficientOfFriction"), 0.f, TEXT(""));
static TAutoConsoleVariable<float> CVarClothDamping(TEXT("physics.ClothDamping"), 0.01f, TEXT(""));
static TAutoConsoleVariable<float> CVarClothGravityMagnitude(TEXT("physics.ClothGravityMagnitude"), 490.f, TEXT(""));

ClothingSimulation::ClothingSimulation()
	: NumIterations(1)
	, EdgeStiffness(1.f)
	, BendingStiffness(1.f)
	, AreaStiffness(1.f)
	, VolumeStiffness(0.f)
	, StrainLimitingStiffness(1.f)
	, ShapeTargetStiffness(0.f)
	, SelfCollisionThickness(2.f)
	, CollisionThickness(1.2f)
	, GravityMagnitude(490.f)
	, bUseBendingElements(false)
	, bUseTetrahedralConstraints(false)
	, bUseThinShellVolumeConstraints(false)
	, bUseSelfCollisions(false)
	, bUseContinuousCollisionDetection(false)
{
#if WITH_EDITOR
	DebugClothMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EditorMaterials/Cloth/CameraLitDoubleSided.CameraLitDoubleSided"), nullptr, LOAD_None, nullptr);  // LOAD_EditorOnly
#endif  // #if WITH_EDITOR
}

ClothingSimulation::~ClothingSimulation()
{}

void ClothingSimulation::Initialize()
{
    NumIterations = CVarClothNumIterations.GetValueOnGameThread();
    SelfCollisionThickness = CVarClothSelfCollisionThickness.GetValueOnGameThread();
    CollisionThickness = CVarClothCollisionThickness.GetValueOnGameThread();
    CoefficientOfFriction = CVarClothCoefficientOfFriction.GetValueOnGameThread();
    Damping = CVarClothDamping.GetValueOnGameThread();
    GravityMagnitude = CVarClothGravityMagnitude.GetValueOnGameThread();

    Chaos::TPBDParticles<float, 3> LocalParticles;
    Chaos::TKinematicGeometryClothParticles<float, 3> TRigidParticles;
    Evolution.Reset(
		new Chaos::TPBDEvolution<float, 3>(
			MoveTemp(LocalParticles),
			MoveTemp(TRigidParticles),
			{}, // CollisionTriangles
			NumIterations,
			CollisionThickness,
			SelfCollisionThickness,
			CoefficientOfFriction,
			Damping));
    Evolution->CollisionParticles().AddArray(&BoneIndices);
	Evolution->CollisionParticles().AddArray(&BaseTransforms);
    Evolution->GetGravityForces().SetAcceleration(Chaos::TVector<float, 3>(0.f, 0.f, -1.f)*GravityMagnitude);

    Evolution->SetKinematicUpdateFunction(
		[&](Chaos::TPBDParticles<float, 3>& ParticlesInput, const float Dt, const float LocalTime, const int32 Index)
		{
			if (!OldAnimationPositions.IsValidIndex(Index) || ParticlesInput.InvM(Index) > 0)
				return;
			const float Alpha = (LocalTime - Time) / DeltaTime;
			ParticlesInput.X(Index) = Alpha * AnimationPositions[Index] + (1.f - Alpha) * OldAnimationPositions[Index];
		});

	Evolution->SetCollisionKinematicUpdateFunction(
//		[&](Chaos::TKinematicGeometryParticles<float, 3>& ParticlesInput, const float Dt, const float LocalTime, const int32 Index)
		[&](Chaos::TKinematicGeometryClothParticles<float, 3>& ParticlesInput, const float Dt, const float LocalTime, const int32 Index)
		{
			checkSlow(DeltaTime > SMALL_NUMBER);
			const float Alpha = (LocalTime - Time) / DeltaTime;
			const Chaos::TVector<float, 3> NewX =
				Alpha * AnimationTransforms[Index].GetTranslation() + (1.f - Alpha) * OldAnimationTransforms[Index].GetTranslation();
			ParticlesInput.V(Index) = (NewX - ParticlesInput.X(Index)) / DeltaTime;
			ParticlesInput.X(Index) = NewX;
			Chaos::TRotation<float, 3> NewR = FQuat::Slerp(OldAnimationTransforms[Index].GetRotation(), AnimationTransforms[Index].GetRotation(), Alpha);
			Chaos::TRotation<float, 3> Delta = NewR * ParticlesInput.R(Index).Inverse();
			Chaos::TVector<float, 3> Axis;
			float Angle;
			Delta.ToAxisAndAngle(Axis, Angle);
			ParticlesInput.W(Index) = Axis * Angle / Dt;
			ParticlesInput.R(Index) = NewR;
		});

    MaxDeltaTime = 1.0f;
    ClampDeltaTime = 0.f;
    Time = 0.f;
}

void ClothingSimulation::CreateActor(USkeletalMeshComponent* InOwnerComponent, UClothingAssetBase* InAsset, int32 InSimDataIndex)
{
	MassMode = InOwnerComponent->MassMode; // uniform, total, density
	UniformMass = InOwnerComponent->UniformMass;
	TotalMass = InOwnerComponent->TotalMass;
	Density = InOwnerComponent->Density;
	MinMass = InOwnerComponent->MinPerParticleMass;

	EdgeStiffness = InOwnerComponent->EdgeStiffness;
	BendingStiffness = InOwnerComponent->BendingStiffness;
	AreaStiffness = InOwnerComponent->AreaStiffness;
	VolumeStiffness = InOwnerComponent->VolumeStiffness;
	StrainLimitingStiffness = InOwnerComponent->StrainLimitingStiffness;
	ShapeTargetStiffness = InOwnerComponent->ShapeTargetStiffness;
	bUseBendingElements = InOwnerComponent->bUseBendingElements;
	bUseTetrahedralConstraints = InOwnerComponent->bUseTetrahedralConstraints;
	bUseThinShellVolumeConstraints = InOwnerComponent->bUseThinShellVolumeConstraints;
	bUseSelfCollisions = InOwnerComponent->bUseSelfCollisions;
	bUseContinuousCollisionDetection = InOwnerComponent->bUseContinuousCollisionDetection; // ccd

	//Evolution->SetCCD(bUseContinuousCollisionDetection);
	//Evolution->SetCCD(true); // ryan!!!

	ClothingSimulationContext Context;
    FillContext(InOwnerComponent, 0, &Context);

	UClothingAssetCommon* const Asset = Cast<UClothingAssetCommon>(InAsset);
	if (Assets.Num() <= InSimDataIndex)
        Assets.SetNumZeroed(InSimDataIndex + 1);
	Assets[InSimDataIndex] = Asset;

    check(Asset->GetNumLods() == 1);
	UClothLODDataBase* AssetLodData = Asset->ClothLodData[0];
	check(AssetLodData->PhysicalMeshData);
    UClothPhysicalMeshDataBase* PhysMesh = AssetLodData->PhysicalMeshData;

	// SkinPhysicsMesh() strips scale from RootBoneTransform ("Ignore any user scale.
	// It's already accounted for in our skinning matrices."), and returns all points
	// in that space.
	TArray<Chaos::TVector<float, 3>> TempAnimationPositions;
	TArray<Chaos::TVector<float, 3>> TempAnimationNormals;

	FTransform RootBoneTransform = Context.BoneTransforms[Asset->ReferenceBoneIndex];
	ClothingMeshUtils::SkinPhysicsMesh(
		Asset->UsedBoneIndices,
		*PhysMesh, // curr pos and norm
		RootBoneTransform,
		Context.RefToLocals.GetData(),
		Context.RefToLocals.Num(),
		reinterpret_cast<TArray<FVector>&>(TempAnimationPositions), 
		reinterpret_cast<TArray<FVector>&>(TempAnimationNormals));

	// Transform points & normals to world space
	RootBoneTransform.SetScale3D(FVector(1.0f));
	const FTransform RootBoneWorldTransform = RootBoneTransform * Context.ComponentToWorld;
	ParallelFor(TempAnimationPositions.Num(), 
		[&](int32 Index)
		{
			TempAnimationPositions[Index] = RootBoneWorldTransform.TransformPosition(TempAnimationPositions[Index]);
			TempAnimationNormals[Index] = RootBoneWorldTransform.TransformVector(TempAnimationNormals[Index]);
		});

	// Add particles
	TPBDParticles<float, 3>& Particles = Evolution->Particles();
	const uint32 Offset = Particles.Size();
	Particles.AddParticles(PhysMesh->Vertices.Num());

	AnimationPositions.SetNum(Particles.Size());
	AnimationNormals.SetNum(Particles.Size());

	if (IndexToRangeMap.Num() <= InSimDataIndex)
		 IndexToRangeMap.SetNum(InSimDataIndex + 1);
	IndexToRangeMap[InSimDataIndex] = Chaos::TVector<uint32, 2>(Offset, Particles.Size());

	for (uint32 i = Offset; i < Particles.Size(); ++i)
	{
		AnimationPositions[i] = TempAnimationPositions[i - Offset];
		AnimationNormals[i] = TempAnimationNormals[i - Offset];
		Particles.X(i) = AnimationPositions[i];
		Particles.V(i) = Chaos::TVector<float, 3>(0.f, 0.f, 0.f);
		// Initialize mass to 0, to be overridden later
		Particles.M(i) = 0.f;
	}

	OldAnimationPositions = AnimationPositions;  // Also update the old positions array to avoid any interpolation issues

	const int32 NumTriangles = PhysMesh->Indices.Num() / 3;
	TArray<Chaos::TVector<int32, 3>> InputSurfaceElements;
	InputSurfaceElements.Reserve(NumTriangles);
	for (int i = 0; i < NumTriangles; ++i)
	{
		const int32 Index = 3 * i;
		InputSurfaceElements.Add(
			{static_cast<int32>(Offset + PhysMesh->Indices[Index]),
			 static_cast<int32>(Offset + PhysMesh->Indices[Index + 1]),
			 static_cast<int32>(Offset + PhysMesh->Indices[Index + 2])});
	}
	check(InputSurfaceElements.Num() == NumTriangles);
	if (Meshes.Num() <= InSimDataIndex)
	{
		Meshes.SetNum(InSimDataIndex + 1);
		FaceNormals.SetNum(InSimDataIndex + 1);
		PointNormals.SetNum(InSimDataIndex + 1);
	}
	TUniquePtr<Chaos::TTriangleMesh<float>>& Mesh = Meshes[InSimDataIndex];
    Mesh.Reset(new Chaos::TTriangleMesh<float>(MoveTemp(InputSurfaceElements)));
	check(Mesh->GetNumElements() == NumTriangles);
    const auto& SurfaceElements = Mesh->GetSurfaceElements();
	Mesh->GetPointToTriangleMap(); // Builds map for later use by GetPointNormals().

	// Assign per particle mass proportional to connected area.
	float TotalArea = 0.0;
	for (const Chaos::TVector<int32, 3>& Tri : SurfaceElements)
	{
		const float TriArea = 0.5 * Chaos::TVector<float, 3>::CrossProduct(
			Particles.X(Tri[1]) - Particles.X(Tri[0]),
			Particles.X(Tri[2]) - Particles.X(Tri[0])).Size();
		TotalArea += TriArea;
		const float ThirdTriArea = TriArea / 3.0;
		Particles.M(Tri[0]) += ThirdTriArea;
		Particles.M(Tri[1]) += ThirdTriArea;
		Particles.M(Tri[2]) += ThirdTriArea;
	}
	const TSet<int32> Vertices = Mesh->GetVertices();
	switch (MassMode)
	{
	case EClothMassMode::UniformMass:
		for (const int32 Vertex : Vertices)
		{
			Particles.M(Vertex) = UniformMass;
		}
		break;
	case EClothMassMode::TotalMass:
	{
		const float MassPerUnitArea = TotalArea > 0.0 ? TotalMass / TotalArea : 1.0;
		for (const int32 Vertex : Vertices)
		{
			Particles.M(Vertex) *= MassPerUnitArea;
		}
		break;
	}
	case EClothMassMode::Density:
		for (const int32 Vertex : Vertices)
		{
			Particles.M(Vertex) *= Density;
		}
		break;
	};
	// Clamp and enslave
	for (uint32 i = Offset; i < Particles.Size(); i++)
	{
		Particles.M(i) = FMath::Max(Particles.M(i), MinMass);
		Particles.InvM(i) = PhysMesh->IsFixed(i-Offset) ? 0.0 : 1.0/Particles.M(i);
	}

    // Add Model
    if (ShapeTargetStiffness)
    {
        check(ShapeTargetStiffness > 0.f && ShapeTargetStiffness <= 1.f);
		Evolution->AddPBDConstraintFunction([ShapeConstraints = Chaos::TPerParticlePBDShapeConstraints<float, 3>(Evolution->Particles(), AnimationPositions, ShapeTargetStiffness)](TPBDParticles<float, 3>& InParticles, const float Dt) {
			ShapeConstraints.Apply(InParticles, Dt);
		});
    }
    if (EdgeStiffness)
    {
        check(EdgeStiffness > 0.f && EdgeStiffness <= 1.f);
        Evolution->AddPBDConstraintFunction([SpringConstraints = Chaos::TPBDSpringConstraints<float, 3>(Evolution->Particles(), SurfaceElements, EdgeStiffness)](TPBDParticles<float, 3>& InParticles, const float Dt) {
            SpringConstraints.Apply(InParticles, Dt);
        });
    }
    if (BendingStiffness)
    {
        check(BendingStiffness > 0.f && BendingStiffness <= 1.f);
        if (bUseBendingElements)
        {
            TArray<Chaos::TVector<int32, 4>> BendingConstraints = Mesh->GetUniqueAdjacentElements();
			Evolution->AddPBDConstraintFunction([BendConstraints = Chaos::TPBDBendingConstraints<float>(Evolution->Particles(), MoveTemp(BendingConstraints))](TPBDParticles<float, 3>& InParticles, const float Dt) {
				BendConstraints.Apply(InParticles, Dt);
			});
        }
        else
        {
            TArray<Chaos::TVector<int32, 2>> BendingConstraints = Mesh->GetUniqueAdjacentPoints();
            Evolution->AddPBDConstraintFunction([SpringConstraints = Chaos::TPBDSpringConstraints<float, 3>(Evolution->Particles(), MoveTemp(BendingConstraints), BendingStiffness)](TPBDParticles<float, 3>& InParticles, const float Dt) {
                SpringConstraints.Apply(InParticles, Dt);
            });
        }
    }
    if (AreaStiffness)
    {
        TArray<Chaos::TVector<int32, 3>> SurfaceConstraints = SurfaceElements;
		Evolution->AddPBDConstraintFunction([SurfConstraints = Chaos::TPBDAxialSpringConstraints<float, 3>(Evolution->Particles(), MoveTemp(SurfaceConstraints), AreaStiffness)](TPBDParticles<float, 3>& InParticles, const float Dt) {
			SurfConstraints.Apply(InParticles, Dt);
		});
    }
    if (VolumeStiffness)
    {
        check(VolumeStiffness > 0.f && VolumeStiffness <= 1.f);
        if (bUseTetrahedralConstraints)
        {
            // TODO(mlentine): Need to tetrahedralize surface to support this
            check(false);
        }
        else if (bUseThinShellVolumeConstraints)
        {
            TArray<Chaos::TVector<int32, 2>> BendingConstraints = Mesh->GetUniqueAdjacentPoints();
            TArray<Chaos::TVector<int32, 2>> DoubleBendingConstraints;
            {
                TMap<int32, TArray<int32>> BendingHash;
                for (int32 i = 0; i < BendingConstraints.Num(); ++i)
                {
                    BendingHash.FindOrAdd(BendingConstraints[i][0]).Add(BendingConstraints[i][1]);
                    BendingHash.FindOrAdd(BendingConstraints[i][1]).Add(BendingConstraints[i][0]);
                }
                TSet<Chaos::TVector<int32, 2>> Visited;
                for (auto Elem : BendingHash)
                {
                    for (int32 i = 0; i < Elem.Value.Num(); ++i)
                    {
                        for (int32 j = i + 1; j < Elem.Value.Num(); ++j)
                        {
                            if (Elem.Value[i] == Elem.Value[j])
                                continue;
                            auto NewElem = Chaos::TVector<int32, 2>(Elem.Value[i], Elem.Value[j]);
                            if (!Visited.Contains(NewElem))
                            {
                                DoubleBendingConstraints.Add(NewElem);
                                Visited.Add(NewElem);
                                Visited.Add(Chaos::TVector<int32, 2>(Elem.Value[j], Elem.Value[i]));
                            }
                        }
                    }
                }
            }
            Evolution->AddPBDConstraintFunction([SpringConstraints = Chaos::TPBDSpringConstraints<float, 3>(Evolution->Particles(), MoveTemp(DoubleBendingConstraints), VolumeStiffness)](TPBDParticles<float, 3>& InParticles, const float Dt) {
                SpringConstraints.Apply(InParticles, Dt);
            });
        }
        else
        {
            TArray<Chaos::TVector<int32, 3>> SurfaceConstraints = SurfaceElements;
			Chaos::TPBDVolumeConstraint<float> PBDVolumeConstraint (Evolution->Particles(), MoveTemp(SurfaceConstraints));
			Evolution->AddPBDConstraintFunction([=](TPBDParticles<float, 3>& InParticles, const float Dt)
			{
				PBDVolumeConstraint.Apply(InParticles, Dt);
			});            
        }
    }
    if (StrainLimitingStiffness)
    {
		check(Mesh->GetNumElements() > 0);
		Chaos::TPerParticlePBDLongRangeConstraints<float, 3> PerParticlePBDLongRangeConstraints(
			Evolution->Particles(),
			Mesh->GetPointToNeighborsMap(),
			10, // The max number of connected neighbors per particle.  ryan - What should this be?  Was k...
			StrainLimitingStiffness);

		Evolution->AddPBDConstraintFunction([=](TPBDParticles<float, 3>& InParticles, const float Dt)
		{
			PerParticlePBDLongRangeConstraints.Apply(InParticles, Dt);
		});
	}

	// Maximum Distance Constraints
	const UEnum* const MeshTargets = PhysMesh->GetFloatArrayTargets();	
	const uint32 PhysMeshMaxDistanceIndex = MeshTargets->GetValueByName(TEXT("MaxDistance"));;
	if (PhysMesh->GetFloatArray(PhysMeshMaxDistanceIndex)->Num() > 0)
	{
		check(Mesh->GetNumElements() > 0);
		Chaos::PBDSphericalConstraint<float, 3> SphericalContraint(Offset, PhysMesh->GetFloatArray(PhysMeshMaxDistanceIndex)->Num(), true, &AnimationPositions, PhysMesh->GetFloatArray(PhysMeshMaxDistanceIndex));
		Evolution->AddPBDConstraintFunction([=](TPBDParticles<float, 3>& InParticles, const float Dt)
		{
			SphericalContraint.Apply(InParticles, Dt);
		});
	}

	// Backstop Constraints
	const uint32 PhysMeshBackstopDistanceIndex = MeshTargets->GetValueByName(TEXT("BackstopDistance"));
	const uint32 PhysMeshBackstopRadiusIndex = MeshTargets->GetValueByName(TEXT("BackstopRadius"));
	if (PhysMesh->GetFloatArray(PhysMeshBackstopRadiusIndex)->Num() > 0 && PhysMesh->GetFloatArray(PhysMeshBackstopDistanceIndex)->Num() > 0)
	{
		check(Mesh->GetNumElements() > 0);
		check(PhysMesh->GetFloatArray(PhysMeshBackstopRadiusIndex)->Num() == PhysMesh->GetFloatArray(PhysMeshBackstopDistanceIndex)->Num());

		Chaos::PBDSphericalConstraint<float, 3> SphericalContraint(Offset, PhysMesh->GetFloatArray(PhysMeshBackstopRadiusIndex)->Num(), false, &AnimationPositions, 
			PhysMesh->GetFloatArray(PhysMeshBackstopRadiusIndex), PhysMesh->GetFloatArray(PhysMeshBackstopDistanceIndex), &AnimationNormals);
		Evolution->AddPBDConstraintFunction([=](TPBDParticles<float, 3>& InParticles, const float Dt)
		{
			SphericalContraint.Apply(InParticles, Dt);
		});		
	}

    // Add Self Collisions
    if (bUseSelfCollisions)
    {
        // TODO(mlentine): Parallelize these for multiple meshes
        Evolution->CollisionTriangles().Append(SurfaceElements);
        for (uint32 i = Offset; i < Particles.Size(); ++i)
        {
            auto Neighbors = Mesh->GetNRing(i, 5);
            for (const auto& Element : Neighbors)
            {
                check(i != Element);
                Evolution->DisabledCollisionElements().Add(Chaos::TVector<int32, 2>(i, Element));
                Evolution->DisabledCollisionElements().Add(Chaos::TVector<int32, 2>(Element, i));
            }
        }
    }
    // Add Collision Bodies
    TGeometryClothParticles<float, 3>& CollisionParticles = Evolution->CollisionParticles();
    //USkeletalMesh* TargetMesh = InOwnerComponent->SkeletalMesh;
	USkeletalMesh* TargetMesh = CastChecked<USkeletalMesh>(Asset->GetOuter());

		// TODO(mlentine): Support collision body activation on a per particle basis, preferably using a map but also can be a particle attribute
    if (UPhysicsAsset* PhysAsset = Asset->PhysicsAsset)
    {
        for (const USkeletalBodySetup* BodySetup : PhysAsset->SkeletalBodySetups)
        {
			if (!BodySetup)
				continue;

			const int32 MeshBoneIndex = TargetMesh->RefSkeleton.FindBoneIndex(BodySetup->BoneName);
			const int32 MappedBoneIndex =
				MeshBoneIndex != INDEX_NONE ?
				Asset->UsedBoneNames.AddUnique(BodySetup->BoneName) : INDEX_NONE;

			const FKAggregateGeom& AggGeom = BodySetup->AggGeom;
			if(AggGeom.SphylElems.Num())
            {
                uint32 OldSize = CollisionParticles.Size();
                CollisionParticles.AddParticles(AggGeom.SphylElems.Num());
                for (uint32 i = OldSize; i < CollisionParticles.Size(); ++i)
                {
                    const auto& Capsule = AggGeom.SphylElems[i - OldSize];
                    if (Capsule.Length == 0)
                    {
                        CollisionParticles.SetDynamicGeometry(i, MakeUnique<Chaos::TSphere<float,3>>(Chaos::TVector<float, 3>(0), Capsule.Radius));
                    }
                    else
                    {
                        Chaos::TVector<float, 3> half_extents(0, 0, Capsule.Length / 2);
                        CollisionParticles.SetDynamicGeometry(
							i,
							MakeUnique<Chaos::TCapsule<float>>(
								-half_extents, half_extents, Capsule.Radius));
                    }
                    BaseTransforms[i] = Chaos::TRigidTransform<float, 3>(Capsule.Center, Capsule.Rotation.Quaternion());
                    BoneIndices[i] = MappedBoneIndex;
                }
            }
			if(AggGeom.SphereElems.Num())
            {
                uint32 OldSize = CollisionParticles.Size();
                CollisionParticles.AddParticles(AggGeom.SphereElems.Num());
                for (uint32 i = OldSize; i < CollisionParticles.Size(); ++i)
                {
                    const auto& CollisionSphere = AggGeom.SphereElems[i - OldSize];
                    CollisionParticles.SetDynamicGeometry(
						i,
						MakeUnique<Chaos::TSphere<float,3>>(
							Chaos::TVector<float, 3>(0.f, 0.f, 0.f),
							CollisionSphere.Radius));
                    BaseTransforms[i] = Chaos::TRigidTransform<float, 3>(CollisionSphere.Center, Chaos::TRotation<float, 3>::MakeFromEuler(Chaos::TVector<float, 3>(0.f, 0.f, 0.f)));
                    BoneIndices[i] = MappedBoneIndex;
                }
            }
			if(AggGeom.BoxElems.Num())
            {
                uint32 OldSize = CollisionParticles.Size();
                CollisionParticles.AddParticles(AggGeom.BoxElems.Num());
                for (uint32 i = OldSize; i < CollisionParticles.Size(); ++i)
                {
                    const auto& Box = AggGeom.BoxElems[i - OldSize];
                    Chaos::TVector<float, 3> half_extents(Box.X / 2.f, Box.Y / 2.f, Box.Z / 2.f);
                    CollisionParticles.SetDynamicGeometry(i, MakeUnique<Chaos::TBox<float,3>>(-half_extents, half_extents));
                    BaseTransforms[i] = Chaos::TRigidTransform<float, 3>(Box.Center, Box.Rotation.Quaternion());
                    BoneIndices[i] = MappedBoneIndex;
                }
            }
            /*
			if(AggGeom.TaperedCapsuleElems.Num())
			{
                int32 OldSize = CollisionParticles.Size();
                CollisionParticles.AddParticles(AggGeom.TaperedCapsuleElems.Num());
                for (int32 i = OldSize; i < CollisionParticles.Size(); ++i)
                {
                    const auto& Capsule = AggGeom.TaperedCapsuleElems[i - OldSize];
                    if (Capsule.Length == 0)
                    {
                        CollisionParticles.Geometry(i) = new Chaos::Sphere<float, 3>(Chaos::TVector<float, 3>(0), Capsule.Radius1 > Capsule.Radius0 ? Capsule.Radius1 : Capsule.Radius0);
                    }
                    else
                    {
                        TArray<TUniquePtr<TImplicitObject<float, 3>>> Objects;
                        Chaos::TVector<float, 3> half_extents(0, 0, Capsule.Length / 2);
                        Objects.Add(TUniquePtr<Chaos::TImplicitObject<float, 3>>(
                            new Chaos::TTaperedCylinder<float>(-half_extents, half_extents, Capsule.Radius1, Capsule.Radius0)));
                        Objects.Add(TUniquePtr<Chaos::TImplicitObject<float, 3>>(
                            new Chaos::Sphere<float, 3>(-half_extents, Capsule.Radius1)));
                        Objects.Add(TUniquePtr<Chaos::TImplicitObject<float, 3>>(
                            new Chaos::Sphere<float, 3>(half_extents, Capsule.Radius0)));
                        CollisionParticles.Geometry(i) = new Chaos::TImplicitObjectUnion<float, 3>(MoveTemp(Objects));
                    }
					BaseTransforms[i] = Chaos::TRigidTransform<float, 3>(Capsule.Center, Capsule.Rotation.Quaternion());
                    BoneIndices[i] = MappedBoneIndex;
                }
			}*/
			if(AggGeom.ConvexElems.Num())
			{
// Collision bodies are stored in PhysX specific data structures so they can only be imported if we enable PhysX.
#if WITH_PHYSX && !PLATFORM_LUMIN && !PLATFORM_ANDROID
				uint32 OldSize = CollisionParticles.Size();
				CollisionParticles.AddParticles(AggGeom.ConvexElems.Num());
				for (uint32 i = OldSize; i < CollisionParticles.Size(); ++i)
				{
				const auto& CollisionBody = AggGeom.ConvexElems[i - OldSize];
				TArray<Chaos::TVector<int32, 3>> CollisionMeshElements;
				const auto PhysXMesh = CollisionBody.GetConvexMesh();
				for (int32 j = 0; j < static_cast<int32>(PhysXMesh->getNbPolygons()); ++j)
				{
					physx::PxHullPolygon Poly;
					PhysXMesh->getPolygonData(j, Poly);
					check(Poly.mNbVerts == 3);
					const auto Indices = PhysXMesh->getIndexBuffer() + Poly.mIndexBase;
					CollisionMeshElements.Add(Chaos::TVector<int32, 3>(Indices[0], Indices[1], Indices[2]));
				}
				Chaos::TParticles<float, 3> CollisionMeshParticles;
				CollisionMeshParticles.AddParticles(CollisionBody.VertexData.Num());
				for (uint32 j = 0; j < CollisionMeshParticles.Size(); ++j)
				{
					CollisionMeshParticles.X(j) = CollisionBody.VertexData[j];
					}
					Chaos::TBox<float, 3> BoundingBox(CollisionMeshParticles.X(0), CollisionMeshParticles.X(0));
					for (uint32 j = 1; j < CollisionMeshParticles.Size(); ++j)
					{
						BoundingBox.GrowToInclude(CollisionMeshParticles.X(i));
					}
					int32 MaxAxisSize = 100;
					int32 MaxAxis;
					const auto Extents = BoundingBox.Extents();
					if (Extents[0] > Extents[1] && Extents[0] > Extents[2])
					{
						MaxAxis = 0;
					}
					else if (Extents[1] > Extents[2])
					{
						MaxAxis = 1;
					}
					else
					{
						MaxAxis = 2;
					}
					Chaos::TUniformGrid<float, 3> Grid(BoundingBox.Min(), BoundingBox.Max(), Chaos::TVector<int32, 3>(100 * Extents[0] / Extents[MaxAxis], 100 * Extents[0] / Extents[MaxAxis], 100 * Extents[0] / Extents[MaxAxis]));
					Chaos::TTriangleMesh<float> CollisionMesh(MoveTemp(CollisionMeshElements));
					Chaos::FErrorReporter ErrorReporter;
					CollisionParticles.SetDynamicGeometry(i, MakeUnique<Chaos::TLevelSet<float,3>>(ErrorReporter, Grid, CollisionMeshParticles, CollisionMesh));
					BaseTransforms[i] = Chaos::TRigidTransform<float, 3>(Chaos::TVector<float, 3>(0.f, 0.f, 0.f), Chaos::TRotation<float, 3>::MakeFromEuler(Chaos::TVector<float, 3>(0.f, 0.f, 0.f)));
					BoneIndices[i] = MappedBoneIndex;
				}
#endif
			}
		} // end for
	} // end if PhysAsset

	// We can't just use AddExternalCollisions() because we need to add entries for bone mappings and lookups.
	const FClothCollisionData& LodCollData = AssetLodData->CollisionData;
	if (LodCollData.Spheres.Num() || LodCollData.SphereConnections.Num() || LodCollData.Convexes.Num())
	{
		UE_LOG(LogSkeletalMesh, Warning,
			TEXT("Actor '%s' component '%s' has %d sphere, %d capsule, and %d "
				"convex collision objects for physics authored as part of a LOD construct, "
				"probably by the Apex cloth authoring system.  This is deprecated.  "
				"Please update your asset!"),
			InOwnerComponent->GetOwner() ? *InOwnerComponent->GetOwner()->GetName() : TEXT("None"),
			*InOwnerComponent->GetName(),
			LodCollData.Spheres.Num(),
			LodCollData.SphereConnections.Num(),
			LodCollData.Convexes.Num());

		TSet<int32> CapsuleEnds;
		if (LodCollData.SphereConnections.Num())
		{
			const uint32 Size = CollisionParticles.Size();
			CollisionParticles.AddParticles(LodCollData.SphereConnections.Num());
			CapsuleEnds.Reserve(LodCollData.SphereConnections.Num() * 2);
			for (uint32 i = Size; i < CollisionParticles.Size(); ++i)
			{
				// This data was pulled from a FKSphylElem, which is a capsule.  So
				// it should only have 1 radius, and the BoneIndex for both spheres
				// should be the same.
				const FClothCollisionPrim_SphereConnection& Connection = LodCollData.SphereConnections[i - Size];

				const int32 SphereIndex0 = Connection.SphereIndices[0];
				const int32 SphereIndex1 = Connection.SphereIndices[1];
				checkSlow(SphereIndex0 != SphereIndex1);
				const float Radius = LodCollData.Spheres[SphereIndex0].Radius;
				checkSlow(LodCollData.Spheres[SphereIndex0].Radius - LodCollData.Spheres[SphereIndex1].Radius < SMALL_NUMBER);
				const Chaos::TVector<float, 3> X0 = LodCollData.Spheres[SphereIndex0].LocalPosition;
				const Chaos::TVector<float, 3> X1 = LodCollData.Spheres[SphereIndex1].LocalPosition;

				checkSlow(LodCollData.Spheres[SphereIndex0].BoneIndex == LodCollData.Spheres[SphereIndex1].BoneIndex);
				const int32 BoneIndex = LodCollData.Spheres[SphereIndex0].BoneIndex;
				const FName BoneName =
					TargetMesh->RefSkeleton.IsValidIndex(BoneIndex) ?
					TargetMesh->RefSkeleton.GetBoneName(BoneIndex) : NAME_None;
				const int32 MappedBoneIndex = BoneName != NAME_None ?
					Asset->UsedBoneNames.AddUnique(BoneName) : INDEX_NONE;
				BoneIndices[i] = MappedBoneIndex;

				const Chaos::TVector<float, 3> Center = (X0 + X1) * 0.5;
				const Chaos::TVector<float, 3> Axis = X1 - X0;
				const float HalfHeight = Axis.Size() * 0.5;

				// We construct a capsule centered at the origin along the Z axis, and
				// then move it into place with X and R.
				CollisionParticles.X(i) = Center;
				CollisionParticles.R(i) = Chaos::TRotation<float, 3>::FromRotatedVector(Chaos::TVector<float, 3>::AxisVector(2), Axis.GetSafeNormal());
				BaseTransforms[i] = Chaos::TRigidTransform<float, 3>(CollisionParticles.X(i), CollisionParticles.R(i));

				CollisionParticles.SetDynamicGeometry(
					i,
					MakeUnique<Chaos::TCapsule<float>>(
						Chaos::TVector<float, 3>(0.f, 0.f, -HalfHeight), // Min
						Chaos::TVector<float, 3>(0.f, 0.f, HalfHeight), // Max
						Radius));
				IndexAndCapsuleCollisionMap.Add(MakePair(i, Connection));

				// Skip spheres added as end caps for the capsule.
				CapsuleEnds.Add(SphereIndex0);
				CapsuleEnds.Add(SphereIndex1);
			}
		}
		if (LodCollData.Spheres.Num() - CapsuleEnds.Num())
		{
			const uint32 Size = CollisionParticles.Size();
			CollisionParticles.AddParticles(LodCollData.Spheres.Num() - CapsuleEnds.Num());
			// i = Spheres index, j = CollisionParticles index
			for (uint32 i = 0, j = Size; i < (uint32)LodCollData.Spheres.Num(); ++i)
			{
				// Skip spheres that are the end caps of capsules.
				if (CapsuleEnds.Contains(i))
					continue;

				const FClothCollisionPrim_Sphere& CollisionSphere = LodCollData.Spheres[i];

				const int32 BoneIndex = CollisionSphere.BoneIndex;
				const FName BoneName =
					TargetMesh->RefSkeleton.IsValidIndex(BoneIndex) ?
					TargetMesh->RefSkeleton.GetBoneName(BoneIndex) : NAME_None;
				const int32 MappedBoneIndex = BoneName != NAME_None ?
					Asset->UsedBoneNames.AddUnique(BoneName) : INDEX_NONE;
				BoneIndices[j] = MappedBoneIndex;

				CollisionParticles.X(j) = CollisionSphere.LocalPosition;
				CollisionParticles.R(j) = Chaos::TRotation<float, 3>::FromElements(1,0,0,0);
				BaseTransforms[j] = Chaos::TRigidTransform<float, 3>(CollisionParticles.X(j), CollisionParticles.R(j));

				CollisionParticles.SetDynamicGeometry(
					j,
					MakeUnique<Chaos::TSphere<float, 3>>(
						Chaos::TVector<float, 3>(0.f, 0.f, 0.f),
						CollisionSphere.Radius));
				IndexAndSphereCollisionMap.Add(MakePair(j, CollisionSphere));
				j++;
			}
		}

	} // end if LodCollData

	check(CollisionParticles.Size() == BaseTransforms.Num());	
	AnimationTransforms.SetNum(BaseTransforms.Num());
	for (uint32 i = 0; i < CollisionParticles.Size(); ++i)
	{
		const int32 MappedIndex = BoneIndices[i];
		if (Context.BoneTransforms.IsValidIndex(MappedIndex))
		{
			const FTransform& BoneTransform = Context.BoneTransforms[MappedIndex];
			AnimationTransforms[i] = BaseTransforms[i] * BoneTransform * Context.ComponentToWorld;
			CollisionParticles.X(i) = AnimationTransforms[i].GetTranslation();
			CollisionParticles.R(i) = AnimationTransforms[i].GetRotation();
		}
	}
}

void ClothingSimulation::FillContext(USkeletalMeshComponent* InComponent, float InDeltaTime, IClothingSimulationContext* InOutContext)
{
    ClothingSimulationContext* Context = static_cast<ClothingSimulationContext*>(InOutContext);
    Context->ComponentToWorld = InComponent->GetComponentToWorld();
    Context->DeltaTime = ClampDeltaTime > 0 ? std::min(InDeltaTime, ClampDeltaTime) : InDeltaTime;

	Context->RefToLocals.Reset();
    InComponent->GetCurrentRefToLocalMatrices(Context->RefToLocals, 0);

	const USkeletalMesh* SkelMesh = InComponent->SkeletalMesh;
    if (USkinnedMeshComponent* MasterComponent = InComponent->MasterPoseComponent.Get())
    {
		const TArray<int32>& MasterBoneMap = InComponent->GetMasterBoneMap();
        int32 NumBones = MasterBoneMap.Num();
        if (NumBones == 0)
        {
            if (InComponent->SkeletalMesh)
            {
                // This case indicates an invalid master pose component (e.g. no skeletal mesh)
                NumBones = InComponent->SkeletalMesh->RefSkeleton.GetNum();
            }
			Context->BoneTransforms.Reset(NumBones);
			Context->BoneTransforms.AddDefaulted(NumBones);
        }
        else
        {
            Context->BoneTransforms.Reset(NumBones);
            Context->BoneTransforms.AddDefaulted(NumBones);
			const TArray<FTransform>& MasterTransforms = MasterComponent->GetComponentSpaceTransforms();
            for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
            {
                bool bFoundMaster = false;
                if (MasterBoneMap.IsValidIndex(BoneIndex))
                {
                    const int32 MasterIndex = MasterBoneMap[BoneIndex];
                    if (MasterTransforms.IsValidIndex(MasterIndex))
                    {
                        Context->BoneTransforms[BoneIndex] = MasterTransforms[MasterIndex];
                        bFoundMaster = true;
                    }
                }

                if (!bFoundMaster && SkelMesh)
                {
                    const int32 ParentIndex = SkelMesh->RefSkeleton.GetParentIndex(BoneIndex);
					check(ParentIndex < BoneIndex);
					Context->BoneTransforms[BoneIndex] =
						Context->BoneTransforms.IsValidIndex(ParentIndex) && ParentIndex < BoneIndex ?
						Context->BoneTransforms[ParentIndex] * SkelMesh->RefSkeleton.GetRefBonePose()[BoneIndex] :
                        SkelMesh->RefSkeleton.GetRefBonePose()[BoneIndex];
                }
            }
        }
    }
    else
    {
        Context->BoneTransforms = InComponent->GetComponentSpaceTransforms();
    }
}

void ClothingSimulation::Simulate(IClothingSimulationContext* InContext)
{
	ClothingSimulationContext* Context = static_cast<ClothingSimulationContext*>(InContext);
	if (Context->DeltaTime == 0)
		return;

	// Get New Animation Positions and Normals
	OldAnimationTransforms = AnimationTransforms;
	OldAnimationPositions = AnimationPositions;

	for (int32 Index = 0; Index < IndexToRangeMap.Num(); ++Index)
	{
		const UClothingAssetCommon* const Asset = Assets[Index];
		if (!Asset)
			continue;

		const UClothLODDataBase* AssetLodData = Asset->ClothLodData[0];
		check(AssetLodData->PhysicalMeshData);
		const UClothPhysicalMeshDataBase* PhysMesh = AssetLodData->PhysicalMeshData;

		TArray<Chaos::TVector<float, 3>> TempAnimationPositions;
		TArray<Chaos::TVector<float, 3>> TempAnimationNormals;

		FTransform RootBoneTransform = Context->BoneTransforms[Asset->ReferenceBoneIndex];
		ClothingMeshUtils::SkinPhysicsMesh(
			Asset->UsedBoneIndices,
			*PhysMesh,
			RootBoneTransform,
			Context->RefToLocals.GetData(),
			Context->RefToLocals.Num(),
			reinterpret_cast<TArray<FVector>&>(TempAnimationPositions),
			reinterpret_cast<TArray<FVector>&>(TempAnimationNormals));

		RootBoneTransform.SetScale3D(FVector(1.0f));

		// Removing Context->ComponentToWorld means the sim doesn't see updates to the component level xf
		const FTransform RootBoneWorldTransform = RootBoneTransform * Context->ComponentToWorld;

		const int32 Offset = IndexToRangeMap[Index][0];
		check(TempAnimationPositions.Num() == IndexToRangeMap[Index][1] - IndexToRangeMap[Index][0]);

		ParallelFor(TempAnimationPositions.Num(),
		[&](int32 AnimationElementIndex)
		{
			AnimationPositions[Offset + AnimationElementIndex] = RootBoneWorldTransform.TransformPosition(TempAnimationPositions[AnimationElementIndex]);
			AnimationNormals[Offset + AnimationElementIndex] = RootBoneWorldTransform.TransformVector(TempAnimationNormals[AnimationElementIndex]);
		});
	}

	// Collision bodies
	TGeometryClothParticles<float, 3>& CollisionParticles = Evolution->CollisionParticles();
	for (uint32 i = 0; i < CollisionParticles.Size(); ++i)
	{
		const int32 MappedIndex = BoneIndices[i];
		if (Context->BoneTransforms.IsValidIndex(MappedIndex))
		{
			const FTransform& BoneTransform = Context->BoneTransforms[MappedIndex];
			AnimationTransforms[i] = BaseTransforms[i] * BoneTransform * Context->ComponentToWorld;
		}
	}
	// Advance Sim
	DeltaTime = Context->DeltaTime;
	while (Context->DeltaTime > MaxDeltaTime)
	{
		Evolution->AdvanceOneTimeStep(MaxDeltaTime);
		Context->DeltaTime -= MaxDeltaTime;
	}
	Evolution->AdvanceOneTimeStep(Context->DeltaTime);
	Time += DeltaTime;
}

void ClothingSimulation::GetSimulationData(
	TMap<int32, FClothSimulData>& OutData,
	USkeletalMeshComponent* InOwnerComponent,
	USkinnedMeshComponent* InOverrideComponent) const
{
	const FTransform& OwnerTransform = InOwnerComponent->GetComponentTransform();
	for (int32 i = 0; i < IndexToRangeMap.Num(); ++i)
	{
		const TUniquePtr<Chaos::TTriangleMesh<float>>& Mesh = Meshes[i];
		if (!Mesh)
			continue;
		Mesh->GetFaceNormals(FaceNormals[i], Evolution->Particles().X(), false);
		Mesh->GetPointNormals(PointNormals[i], FaceNormals[i], /*bReturnEmptyOnError =*/ false, /*bFillAtStartIndex =*/ false);

		FClothSimulData& Data = OutData.FindOrAdd(i);
		Data.Reset();

		const UClothingAssetCommon* const Asset = Assets[i];
		if (!Asset)
			continue;

		const TArray<FTransform>& ComponentSpaceTransforms = InOverrideComponent ?
			InOverrideComponent->GetComponentSpaceTransforms() :
			InOwnerComponent->GetComponentSpaceTransforms();
		if (!ComponentSpaceTransforms.IsValidIndex(Asset->ReferenceBoneIndex))
		{
			UE_LOG(LogSkeletalMesh, Warning,
				TEXT("Failed to write back clothing simulation data for component '%s' as bone transforms are invalid."),
				*InOwnerComponent->GetName());
			check(false);
			continue;
		}

		FTransform RootBoneTransform = ComponentSpaceTransforms[Asset->ReferenceBoneIndex];
		RootBoneTransform.SetScale3D(FVector(1.0f));
		RootBoneTransform *= OwnerTransform;
		Data.Transform = RootBoneTransform;
		Data.ComponentRelativeTransform = OwnerTransform.Inverse();

		const Chaos::TVector<uint32, 2>& VertexDomain = IndexToRangeMap[i];
		const uint32 VertexRange = VertexDomain[1] - VertexDomain[0];
		Data.Positions.SetNum(VertexRange);
        Data.Normals.SetNum(VertexRange);
		for (uint32 j = VertexDomain[0]; j < VertexDomain[1]; ++j)
        {
			const uint32 LocalIndex = j - VertexDomain[0];
            Data.Positions[LocalIndex] = Evolution->Particles().X(j);
            Data.Normals[LocalIndex] = PointNormals[i][LocalIndex];
		}
    }
}

void ClothingSimulation::AddExternalCollisions(const FClothCollisionData& InData)
{
    TGeometryClothParticles<float, 3>& CollisionParticles = Evolution->CollisionParticles();
	TSet<int32> CapsuleEnds;

	const uint32 NumParticles0 = CollisionParticles.Size();

	if(InData.SphereConnections.Num())
	{
		const uint32 Size = CollisionParticles.Size();
        CollisionParticles.AddParticles(InData.SphereConnections.Num());
		CapsuleEnds.Reserve(InData.SphereConnections.Num() * 2);
        for (uint32 i = Size; i < CollisionParticles.Size(); ++i)
        {
			// This data was pulled from a FKSphylElem, which is a capsule.  So
			// it should only have 1 radius, and the BoneIndex for both spheres
			// should be the same.
			const FClothCollisionPrim_SphereConnection& Connection = InData.SphereConnections[i - Size];
			const int32 SphereIndex0 = Connection.SphereIndices[0];
			const int32 SphereIndex1 = Connection.SphereIndices[1];
			checkSlow(SphereIndex0 != SphereIndex1);
			const float Radius = InData.Spheres[SphereIndex0].Radius;
			checkSlow(InData.Spheres[SphereIndex0].Radius - InData.Spheres[SphereIndex1].Radius < SMALL_NUMBER);
			const Chaos::TVector<float, 3> X0 = InData.Spheres[SphereIndex0].LocalPosition;
			const Chaos::TVector<float, 3> X1 = InData.Spheres[SphereIndex1].LocalPosition;
			const int32 BoneIndex = InData.Spheres[SphereIndex0].BoneIndex;
			checkSlow(InData.Spheres[SphereIndex0].BoneIndex == InData.Spheres[SphereIndex1].BoneIndex);

			const Chaos::TVector<float, 3> Center = (X0 + X1) * 0.5;
			const Chaos::TVector<float, 3> Axis = X1 - X0;
			const float HalfHeight = Axis.Size() * 0.5;

			// We construct a capsule centered at the origin along the Z axis, and
			// then move it into place with X and R.
			CollisionParticles.X(i) = Center;
			CollisionParticles.R(i) = Chaos::TRotation<float, 3>::FromRotatedVector(Chaos::TVector<float,3>::AxisVector(2), Axis.GetSafeNormal());
			BaseTransforms[i] = Chaos::TRigidTransform<float, 3>(CollisionParticles.X(i), CollisionParticles.R(i));
			BoneIndices[i] = BoneIndex;

			CollisionParticles.SetDynamicGeometry(
				i,
				MakeUnique<Chaos::TCapsule<float>>(
					Chaos::TVector<float, 3>(0.f, 0.f, -HalfHeight), // Min
					Chaos::TVector<float, 3>(0.f, 0.f, HalfHeight), // Max
					Radius));
            IndexAndCapsuleCollisionMap.Add(MakePair(i, Connection));

			// Skip spheres added as end caps for the capsule.
			CapsuleEnds.Add(SphereIndex0);
			CapsuleEnds.Add(SphereIndex1);
		}
	}

	if(InData.Spheres.Num() - CapsuleEnds.Num())
	{
		const uint32 Size = CollisionParticles.Size();
		CollisionParticles.AddParticles(InData.Spheres.Num() - CapsuleEnds.Num());
		for (uint32 i = 0, j = Size; i < (uint32)InData.Spheres.Num(); ++i)
		{
			// Skip spheres that are the end caps of capsules.
			if (CapsuleEnds.Contains(i))
				continue;

			const auto& CollisionSphere = InData.Spheres[i];
			CollisionParticles.X(j) = Chaos::TVector<float, 3>(0.f);
			CollisionParticles.R(j) = Chaos::TRotation<float, 3>::FromIdentity(); 
			BaseTransforms[i] = Chaos::TRigidTransform<float, 3>(CollisionParticles.X(i), CollisionParticles.R(i));
			BoneIndices[i] = CollisionSphere.BoneIndex;
			CollisionParticles.SetDynamicGeometry(
				j,
				MakeUnique<Chaos::TSphere<float,3>>(
					CollisionSphere.LocalPosition, //Chaos::TVector<float, 3>(0.f, 0.f, 0.f),
					CollisionSphere.Radius));
				IndexAndSphereCollisionMap.Add(MakePair(j, CollisionSphere));
			j++;
		}
	}
	if(InData.Convexes.Num())
	{
		const uint32 Size = CollisionParticles.Size();
		CollisionParticles.AddParticles(InData.Convexes.Num());
		for (uint32 i = Size; i < CollisionParticles.Size(); ++i)
		{
			const auto& convex = InData.Convexes[i - Size];
			CollisionParticles.X(i) = Chaos::TVector<float, 3>(0.f, 0.f, 0.f);
			CollisionParticles.R(i) = Chaos::TRotation<float, 3>::FromIdentity();
			BaseTransforms[i] = Chaos::TRigidTransform<float, 3>(CollisionParticles.X(i), CollisionParticles.R(i));
			BoneIndices[i] = convex.BoneIndex;
			TArray<TUniquePtr<TImplicitObject<float, 3>>> Planes;
			for (int32 j = 0; j < convex.Planes.Num(); ++j)
			{
				Planes.Add(TUniquePtr<Chaos::TImplicitObject<float, 3>>(
					new Chaos::TPlane<float, 3>(Chaos::TVector<float, 3>(0.f, 0.f, convex.Planes[j].W / convex.Planes[j].Z),
					Chaos::TVector<float, 3>(convex.Planes[j].X, convex.Planes[j].Y, convex.Planes[j].Z))));
			}
				CollisionParticles.SetDynamicGeometry(i, MakeUnique<Chaos::TImplicitObjectIntersection<float, 3>>(MoveTemp(Planes)));
				IndexAndConvexCollisionMap.Add(MakePair(i, convex));
		}
	}

	check(CollisionParticles.Size() == BaseTransforms.Num());
	// TODO: We need the context to initialize this animation data correctly, so do it in another function perhaps
	const uint32 PrevAnimationTransformsCount = AnimationTransforms.Num();
	const uint32 NewAnimationTransformsCount = BaseTransforms.Num();

	AnimationTransforms.SetNum(NewAnimationTransformsCount);
	OldAnimationTransforms.SetNum(NewAnimationTransformsCount);
	
	for (uint32 i = PrevAnimationTransformsCount; i < NewAnimationTransformsCount; ++i)
	{
		AnimationTransforms[i] = BaseTransforms[i];		
		OldAnimationTransforms[i] = BaseTransforms[i];
	}
}

void ClothingSimulation::ClearExternalCollisions()
{
	TGeometryClothParticles<float, 3>& CollisionParticles = Evolution->CollisionParticles();
	CollisionParticles.Resize(0);

	IndexAndSphereCollisionMap.Reset();
	IndexAndCapsuleCollisionMap.Reset();
	IndexAndConvexCollisionMap.Reset();
}

void ClothingSimulation::GetCollisions(FClothCollisionData& OutCollisions, bool bIncludeExternal) const
{
	OutCollisions.Spheres.Reset();
	OutCollisions.SphereConnections.Reset();
	OutCollisions.Convexes.Reset();
	for (const auto& IndexSphere : IndexAndSphereCollisionMap)
	{
		if (Evolution->Collided(IndexSphere.First))
		{
			OutCollisions.Spheres.Add(IndexSphere.Second);
		}
	}
	for (const auto& IndexCapsule : IndexAndCapsuleCollisionMap)
	{
		if (Evolution->Collided(IndexCapsule.First))
		{
			OutCollisions.SphereConnections.Add(IndexCapsule.Second);
		}
	}
	for (const auto& IndexConvex : IndexAndConvexCollisionMap)
	{
		if (Evolution->Collided(IndexConvex.First))
		{
			OutCollisions.Convexes.Add(IndexConvex.Second);
		}
	}
}

#if WITH_EDITOR
void ClothingSimulation::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(DebugClothMaterial);
}

void ClothingSimulation::DebugDrawPhysMeshWired(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	const TPBDParticles<float, 3>& Particles = Evolution->Particles();

	for (int32 MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
	{
		if (const TUniquePtr<Chaos::TTriangleMesh<float>>& Mesh = Meshes[MeshIndex])
		{
			const TArray<TVector<int32, 3>>& Elements = Mesh->GetElements();

			for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ++ElementIndex)
			{
				const auto& Element = Elements[ElementIndex];

				const FVector& Pos0 = Particles.X(Element.X);
				const FVector& Pos1 = Particles.X(Element.Y);
				const FVector& Pos2 = Particles.X(Element.Z);

				PDI->DrawLine(Pos0, Pos1, FLinearColor::White, SDPG_World, 0.0f, 0.001f);
				PDI->DrawLine(Pos1, Pos2, FLinearColor::White, SDPG_World, 0.0f, 0.001f);
				PDI->DrawLine(Pos2, Pos0, FLinearColor::White, SDPG_World, 0.0f, 0.001f);
			}
		}
	}
}

void ClothingSimulation::DebugDrawPhysMeshShaded(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	if (!DebugClothMaterial) { return; }

	FDynamicMeshBuilder MeshBuilder(PDI->View->GetFeatureLevel());
	const TPBDParticles<float, 3>& Particles = Evolution->Particles();

	int32 VertexIndex = 0;
	for (int32 MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
	{
		if (const TUniquePtr<Chaos::TTriangleMesh<float>>& Mesh = Meshes[MeshIndex])
		{
			const TArray<TVector<int32, 3>>& Elements = Mesh->GetElements();

			for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ++ElementIndex, VertexIndex += 3)
			{
				const auto& Element = Elements[ElementIndex];

				const FVector& Pos0 = Particles.X(Element.X);
				const FVector& Pos1 = Particles.X(Element.Y);
				const FVector& Pos2 = Particles.X(Element.Z);

				const FVector& Normal = FVector::CrossProduct(Pos1 - Pos0, Pos2 - Pos0).GetSafeNormal();
				const FVector Tangent = ((Pos1 + Pos2) * 0.5f - Pos0).GetSafeNormal();

				MeshBuilder.AddVertex(FDynamicMeshVertex(Pos0, Tangent, Normal, FVector2D(0.f, 0.f), FColor::White));
				MeshBuilder.AddVertex(FDynamicMeshVertex(Pos1, Tangent, Normal, FVector2D(0.f, 1.f), FColor::White));
				MeshBuilder.AddVertex(FDynamicMeshVertex(Pos2, Tangent, Normal, FVector2D(1.f, 1.f), FColor::White));
				MeshBuilder.AddTriangle(VertexIndex, VertexIndex + 1, VertexIndex + 2);
			}
		}
	}

	MeshBuilder.Draw(PDI, FMatrix::Identity, DebugClothMaterial->GetRenderProxy(), SDPG_World, false, false);
}

void ClothingSimulation::DebugDrawPointNormals(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	check(Meshes.Num() == IndexToRangeMap.Num());

	const TPBDParticles<float, 3>& Particles = Evolution->Particles();

	for (int32 MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
	{
		if (Meshes[MeshIndex])
		{
			const Chaos::TVector<uint32, 2> Range = IndexToRangeMap[MeshIndex];
			const TArray<Chaos::TVector<float, 3>>& MeshPointNormals = PointNormals[MeshIndex];

			for (uint32 ParticleIndex = Range[0]; ParticleIndex < Range[1]; ++ParticleIndex)
			{
				const TVector<float, 3>& Pos = Particles.X(ParticleIndex);
				const TVector<float, 3>& Normal = MeshPointNormals[ParticleIndex - Range[0]];

				PDI->DrawLine(Pos, Pos + Normal * 20.0f, FLinearColor::White, SDPG_World, 0.0f, 0.001f);
			}
		}
	}
}

void ClothingSimulation::DebugDrawInversedPointNormals(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	check(Meshes.Num() == IndexToRangeMap.Num());

	const TPBDParticles<float, 3>& Particles = Evolution->Particles();

	for (int32 MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
	{
		if (Meshes[MeshIndex])
		{
			const Chaos::TVector<uint32, 2> Range = IndexToRangeMap[MeshIndex];
			const TArray<Chaos::TVector<float, 3>>& MeshPointNormals = PointNormals[MeshIndex];

			for (uint32 ParticleIndex = Range[0]; ParticleIndex < Range[1]; ++ParticleIndex)
			{
				const TVector<float, 3>& Pos = Particles.X(ParticleIndex);
				const TVector<float, 3>& Normal = MeshPointNormals[ParticleIndex - Range[0]];

				PDI->DrawLine(Pos, Pos - Normal * 20.0f, FLinearColor::White, SDPG_World, 0.0f, 0.001f);
			}
		}
	}
}

void ClothingSimulation::DebugDrawFaceNormals(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	check(Meshes.Num() == IndexToRangeMap.Num());

	const TPBDParticles<float, 3>& Particles = Evolution->Particles();

	for (int32 MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
	{
		if (const TUniquePtr<Chaos::TTriangleMesh<float>>& Mesh = Meshes[MeshIndex])
		{
			const TArray<Chaos::TVector<float, 3>>& MeshFaceNormals = FaceNormals[MeshIndex];

			const TArray<TVector<int32, 3>>& Elements = Mesh->GetElements();
			for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ++ElementIndex)
			{
				const TVector<int32, 3>& Element = Elements[ElementIndex];

				const TVector<float, 3> Pos = (
					Particles.X(Element.X) +
					Particles.X(Element.Y) +
					Particles.X(Element.Z)) / 3.f;
				const TVector<float, 3>& Normal = MeshFaceNormals[ElementIndex];

				PDI->DrawLine(Pos, Pos + Normal * 20.0f, FLinearColor::Yellow, SDPG_World, 0.0f, 0.001f);
			}
		}
	}
}

void ClothingSimulation::DebugDrawInversedFaceNormals(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	check(Meshes.Num() == IndexToRangeMap.Num());

	const TPBDParticles<float, 3>& Particles = Evolution->Particles();

	for (int32 MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
	{
		if (const TUniquePtr<Chaos::TTriangleMesh<float>>& Mesh = Meshes[MeshIndex])
		{
			const TArray<Chaos::TVector<float, 3>>& MeshFaceNormals = FaceNormals[MeshIndex];

			const TArray<TVector<int32, 3>>& Elements = Mesh->GetElements();
			for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ++ElementIndex)
			{
				const TVector<int32, 3>& Element = Elements[ElementIndex];

				const TVector<float, 3> Pos = (
					Particles.X(Element.X) +
					Particles.X(Element.Y) +
					Particles.X(Element.Z)) / 3.f;
				const TVector<float, 3>& Normal = MeshFaceNormals[ElementIndex];

				PDI->DrawLine(Pos, Pos - Normal * 20.0f, FLinearColor::Yellow, SDPG_World, 0.0f, 0.001f);
			}
		}
	}
}

void ClothingSimulation::DebugDrawCollision(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	static const FLinearColor ValidColor(FColor::Cyan);
	static const FLinearColor InvalidColor(FColor::Red);

	const TGeometryClothParticles<float, 3>& CollisionParticles = Evolution->CollisionParticles();

	// Draw collision spheres
	for (const auto& IndexAndSphereCollision : IndexAndSphereCollisionMap)
	{
		const int32 Index = int32(IndexAndSphereCollision.First);

		const Chaos::TSphere<float, 3>* const Sphere = CollisionParticles.DynamicGeometry(Index)->GetObject<Chaos::TSphere<float, 3>>();
		if (Sphere)
		{
			const FLinearColor Color = (BoneIndices[Index] != INDEX_NONE) ? ValidColor : InvalidColor;

			const Chaos::TVector<float, 3>& Center = CollisionParticles.X(Index);
			const TRotation<float, 3>& Rotation = CollisionParticles.R(Index);

			const float Radius = Sphere ? Sphere->GetRadius() : 0.0f;

			DrawWireSphere(PDI, Center, Color, Radius, 12, SDPG_World, 0.0f, 0.001f, false);
		}
	}

	// Draw collision capsules
	for (const auto& IndexAndCapsuleCollision : IndexAndCapsuleCollisionMap)
	{
		const int32 Index = int32(IndexAndCapsuleCollision.First);

		const Chaos::TCapsule<float>* const Capsule = CollisionParticles.DynamicGeometry(Index)->GetObject<Chaos::TCapsule<float>>();
		if (Capsule)
		{
			const FLinearColor Color = (BoneIndices[Index] != INDEX_NONE) ? ValidColor : InvalidColor;

			const Chaos::TVector<float, 3>& Center = CollisionParticles.X(Index);
			const TRotation<float, 3>& Rotation = CollisionParticles.R(Index);

			const float HalfHeight = Capsule ? Capsule->GetHeight() * 0.5f : 0.0f;
			const float Radius = Capsule ? Capsule->GetRadius() : 0.0f;

			const FVector X = Rotation.RotateVector(FVector::ForwardVector);
			const FVector Y = Rotation.RotateVector(FVector::RightVector);
			const FVector Z = Rotation.RotateVector(FVector::UpVector);

			DrawWireCapsule(PDI, Center, X, Y, Z, Color, Radius, HalfHeight, 12, SDPG_World, 0.0f, 0.001f, false);
		}
	}
}

void ClothingSimulation::DebugDrawBackstops(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	// TODO: Add when GetCurrentSkinnedPositions is ever implemented
}

void ClothingSimulation::DebugDrawMaxDistances(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	// TODO: Add when GetCurrentSkinnedPositions is ever implemented
}

void ClothingSimulation::DebugDrawSelfCollision(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	if(!bUseSelfCollisions)
	{
		// No self collisions on this actor
		return;
	}
	const TPBDParticles<float, 3>& Particles = Evolution->Particles();
	for(int32 i = 0; i < IndexToRangeMap.Num(); ++i)
	{
		if(const UClothingAssetCommon* const Asset = Assets[i])
		{
			const FTransform RootBoneTransform = OwnerComponent->GetComponentSpaceTransforms()[Asset->ReferenceBoneIndex];

			const UClothLODDataBase* LodData = Asset->ClothLodData[0];
			const UClothPhysicalMeshDataBase* PhysMesh = LodData->PhysicalMeshData;
			const TArray<uint32>& SelfCollisionIndices = PhysMesh->SelfCollisionIndices;
			for (int32 SelfColIdx = 0; SelfColIdx < SelfCollisionIndices.Num(); ++SelfColIdx)
			{
				const FVector ParticlePosition =
					RootBoneTransform.TransformPosition(
						Particles.X(PhysMesh->SelfCollisionIndices[SelfColIdx]));
				DrawWireSphere(PDI, ParticlePosition, FColor::White, SelfCollisionThickness, 8, SDPG_World, 0.0f, 0.001f);
			}
		}
	}
}

void ClothingSimulation::DebugDrawAnimDrive(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	// TODO: Add when GetCurrentSkinnedPositions is ever implemented
}
#endif  // #if WITH_EDITOR
