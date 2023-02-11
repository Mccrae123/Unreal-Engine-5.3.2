// Copyright Epic Games, Inc. All Rights Reserved.

#include "Chaos/Deformable/ChaosDeformableSolver.h"
#include "Chaos/Deformable/ChaosDeformableSolverTypes.h"
#include "Chaos/Deformable/ChaosDeformableSolverProxy.h"

#include "Chaos/DebugDrawQueue.h"
#include "Chaos/TriangleMesh.h"
#include "Chaos/PBDAltitudeSpringConstraints.h"
#include "Chaos/PBDBendingConstraints.h"
#include "Chaos/PBDSpringConstraints.h"
#include "Chaos/PBDVolumeConstraint.h"
#include "Chaos/PBDSpringConstraints.h"
#include "Chaos/PBDTriangleMeshCollisions.h"
#include "Chaos/PBDCollisionSpringConstraints.h"
#include "Chaos/PBDParticles.h"
#include "Chaos/PBDSoftsSolverParticles.h"
#include "Chaos/PBDTetConstraints.h"
#include "Chaos/PerParticleGravity.h"
#include "Chaos/XPBDCorotatedConstraints.h"
#include "Chaos/XPBDVolumeConstraints.h"
#include "Chaos/XPBDCorotatedFiberConstraints.h"
#include "Chaos/Plane.h"
#include "Chaos/Utilities.h"
#include "Chaos/PBDEvolution.h"
#include "Containers/StringConv.h"
#include "CoreMinimal.h"
#include "GeometryCollection/Facades/CollectionKinematicBindingFacade.h"
#include "GeometryCollection/Facades/CollectionVertexBoneWeightsFacade.h"
#include "GeometryCollection/Facades/CollectionPositionTargetFacade.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"

DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.Constructor"), STAT_ChaosDeformableSolver_Constructor, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.Destructor"), STAT_ChaosDeformableSolver_Destructor, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.UpdateProxyInputPackages"), STAT_ChaosDeformableSolver_UpdateProxyInputPackages, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.Simulate"), STAT_ChaosDeformableSolver_Simulate, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.AdvanceDt."), STAT_ChaosDeformableSolver_AdvanceDt, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.Reset"), STAT_ChaosDeformableSolver_Reset, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.Update"), STAT_ChaosDeformableSolver_Update, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.UpdateOutputState"), STAT_ChaosDeformableSolver_UpdateOutputState, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.PullOutputPackage"), STAT_ChaosDeformableSolver_PullOutputPackage, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.PushOutputPackage"), STAT_ChaosDeformableSolver_PushOutputPackage, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.PullInputPackage"), STAT_ChaosDeformableSolver_PullInputPackage, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.PushInputPackage"), STAT_ChaosDeformableSolver_PushInputPackage, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.InitializeSimulationObjects"), STAT_ChaosDeformableSolver_InitializeSimulationObjects, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.InitializeSimulationObject"), STAT_ChaosDeformableSolver_InitializeSimulationObject, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.InitializeDeformableParticles"), STAT_ChaosDeformableSolver_InitializeDeformableParticles, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.InitializeKinematicParticles"), STAT_ChaosDeformableSolver_InitializeKinematicParticles, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.InitializeTetrahedralConstraint"), STAT_ChaosDeformableSolver_InitializeTetrahedralConstraint, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.InitializeGidBasedConstraints"), STAT_ChaosDeformableSolver_InitializeGidBasedConstraints, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.InitializeWeakConstraints"), STAT_ChaosDeformableSolver_InitializeWeakConstraints, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.InitializeKinematicConstraint"), STAT_ChaosDeformableSolver_InitializeKinematicConstraint, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.InitializeCollisionBodies"), STAT_ChaosDeformableSolver_InitializeCollisionBodies, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.InitializeSelfCollisionVariables"), STAT_ChaosDeformableSolver_InitializeSelfCollisionVariables, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.InitializeGridBasedConstraintVariables"), STAT_ChaosDeformableSolver_InitializeGridBasedConstraintVariables, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.RemoveSimulationObjects"), STAT_ChaosDeformableSolver_RemoveSimulationObjects, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.RemoveProxy"), STAT_ChaosDeformableSolver_RemoveProxy, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("Chaos.Deformable.Solver.AddProxy"), STAT_ChaosDeformableSolver_AddProxy, STATGROUP_Chaos);


DEFINE_LOG_CATEGORY_STATIC(LogChaosDeformableSolver, Log, All);
namespace Chaos::Softs
{
	FDeformableDebugParams GDeformableDebugParams;

	FAutoConsoleVariableRef CVarDeformableDebugParamsDrawTetrahedralParticles(TEXT("p.Chaos.DebugDraw.Deformable.TetrahedralParticle"), GDeformableDebugParams.bDoDrawTetrahedralParticles, TEXT("Debug draw the deformable solvers tetrahedron. [def: false]"));
	FAutoConsoleVariableRef CVarDeformableDebugParamsDrawKinematicParticles(TEXT("p.Chaos.DebugDraw.Deformable.KinematicParticle"), GDeformableDebugParams.bDoDrawKinematicParticles, TEXT("Debug draw the deformables kinematic particles. [def: false]"));


	FCriticalSection FDeformableSolver::InitializationMutex;
	FCriticalSection FDeformableSolver::RemovalMutex;
	FCriticalSection FDeformableSolver::PackageOutputMutex;
	FCriticalSection FDeformableSolver::PackageInputMutex;
	FCriticalSection FDeformableSolver::SolverEnabledMutex;

	FDeformableSolver::FDeformableSolver(FDeformableSolverProperties InProp)
		: CurrentInputPackage(TUniquePtr < FDeformablePackage >(nullptr))
		, PreviousInputPackage(TUniquePtr < FDeformablePackage >(nullptr))
		, Property(InProp)
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_Constructor);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_Constructor);
		Reset(Property);
	}

	FDeformableSolver::~FDeformableSolver()
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_Destructor);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_Destructor);

		FScopeLock Lock(&InitializationMutex);
		for (FThreadingProxy* Proxy : UninitializedProxys_Internal)
		{
			delete Proxy;
		}
		UninitializedProxys_Internal.Empty();
		EventTeardown.Broadcast();
	}


	void FDeformableSolver::Reset(const FDeformableSolverProperties& InProps)
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_Reset);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_Reset);

		Property = InProps;
		MObjects = TArrayCollectionArray<const UObject*>();
		FSolverParticles LocalParticlesDummy;
		FSolverRigidParticles RigidParticles;
		Evolution.Reset(new FPBDEvolution(MoveTemp(LocalParticlesDummy), MoveTemp(RigidParticles), {},
			Property.NumSolverIterations, (FSolverReal)0.,
			/*SelfCollisionsThickness = */(FSolverReal)0.,
			/*CoefficientOfFriction = */(FSolverReal)0.,
			/*FSolverReal Damping = */Property.Damping,
			/*FSolverReal LocalDamping = */(FSolverReal)0.,
			Property.bDoQuasistatics));
		Evolution->Particles().AddArray(&MObjects);
		if (Property.bDoSelfCollision || Property.CacheToFile)
		{
			SurfaceElements.Reset(new TArray<Chaos::TVec3<int32>>());
		}

		if (Property.bDoSelfCollision)
		{
			SurfaceTriangleMesh.Reset(new Chaos::FTriangleMesh());
		}
		if (Property.bUseGridBasedConstraints)
		{
			AllElements.Reset(new TArray<Chaos::TVec4<int32>>());
		}

		InitializeKinematicConstraint();
		Frame = 0;
		Time = 0.f;
		Iteration = 0;
	}

	void FDeformableSolver::Simulate(FSolverReal DeltaTime)
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_Simulate);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_Simulate);

		if (Property.NumSolverIterations)
		{
			RemoveSimulationObjects();
			UpdateProxyInputPackages();
			InitializeSimulationObjects();
			AdvanceDt(DeltaTime);
		}
	}

	void FDeformableSolver::InitializeSimulationObjects()
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_InitializeSimulationObjects);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_InitializeSimulationObjects);
		{
			FScopeLock Lock(&InitializationMutex); // @todo(flesh) : change to threaded task based commands to prevent the lock. 
			if (UninitializedProxys_Internal.Num())
			{
				for (FThreadingProxy* Proxy : UninitializedProxys_Internal)
				{
					InitializeSimulationObject(*Proxy);

					FThreadingProxy::FKey Key = Proxy->GetOwner();
					Proxies.Add(Key, TUniquePtr<FThreadingProxy>(Proxy));
				}

				if (UninitializedProxys_Internal.Num() != 0)
				{
					if (Property.bDoSelfCollision)
					{
						InitializeSelfCollisionVariables();
					}

					if (Property.bUseGridBasedConstraints)
					{
						InitializeGridBasedConstraintVariables();
					}
				}
				UninitializedProxys_Internal.SetNum(0, true);
			}
		}

		InitializeCollisionBodies();
	}

	void FDeformableSolver::InitializeSimulationObject(FThreadingProxy& InProxy)
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_InitializeSimulationObject);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_InitializeSimulationObject);

		if (FFleshThreadingProxy* Proxy = InProxy.As<FFleshThreadingProxy>())
		{
			InitializeDeformableParticles(*Proxy);
			InitializeKinematicParticles(*Proxy);
			InitializeWeakConstraint(*Proxy);
			InitializeTetrahedralConstraint(*Proxy);
			InitializeGidBasedConstraints(*Proxy);
		}
	}

	void FDeformableSolver::InitializeDeformableParticles(FFleshThreadingProxy& Proxy)
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_InitializeDeformableParticles);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_InitializeDeformableParticles);

		const FManagedArrayCollection& Dynamic = Proxy.GetDynamicCollection();
		const FManagedArrayCollection& Rest = Proxy.GetRestCollection();

		const TManagedArray<FVector3f>& DynamicVertex = Dynamic.GetAttribute<FVector3f>("Vertex", FGeometryCollection::VerticesGroup);
		const TManagedArray<FSolverReal>* MassArray = Rest.FindAttribute<FSolverReal>("Mass", FGeometryCollection::VerticesGroup);
		const TManagedArray<FSolverReal>* DampingArray = Rest.FindAttribute<FSolverReal>("Damping", FGeometryCollection::VerticesGroup);
		FSolverReal Mass = 100.0;// @todo(chaos) : make user attributes

		auto ChaosVert = [](FVector3d V) { return Chaos::FVec3(V.X, V.Y, V.Z); };
		auto ChaosM = [](FSolverReal M, const TManagedArray<float>* AM, int32 Index, int32 Num) { return FSolverReal((AM != nullptr) ? (*AM)[Index] : M / FSolverReal(Num)); };
		auto ChaosInvM = [](FSolverReal M) { return FSolverReal(FMath::IsNearlyZero(M) ? 0.0 : 1 / M); };
		auto DoubleVert = [](FVector3f V) { return FVector3d(V.X, V.Y, V.Z); };
		uint32 NumParticles = Rest.NumElements(FGeometryCollection::VerticesGroup);

		int32 ParticleStart = Evolution->AddParticleRange(NumParticles, GroupOffset, true);
		GroupOffset += 1;
		for (uint32 vdx = 0; vdx < NumParticles; ++vdx)
		{
			MObjects[ParticleStart + vdx] = Proxy.GetOwner();
		}

		TArray<FSolverReal> MassWithMultiplier;
		TArray<FSolverReal> DampingWithMultiplier;
		MassWithMultiplier.Init(0.f, NumParticles);
		DampingWithMultiplier.Init(0.f, NumParticles);
		FSolverReal DampingMultiplier = 0.f;
		FSolverReal MassMultiplier = 0.f;
		if (const UObject* Owner = this->MObjects[ParticleStart]) {
			FFleshThreadingProxy::FFleshInputBuffer* FleshInputBuffer = nullptr;
			if (this->CurrentInputPackage->ObjectMap.Contains(Owner))
			{
				FleshInputBuffer = this->CurrentInputPackage->ObjectMap[Owner]->As<FFleshThreadingProxy::FFleshInputBuffer>();
				if (FleshInputBuffer)
				{
					DampingMultiplier = FleshInputBuffer->DampingMultiplier;
					MassMultiplier = FleshInputBuffer->MassMultiplier;
				}
			}
		}

		for (uint32 vdx = 0; vdx < NumParticles; ++vdx)
		{
			MassWithMultiplier[vdx] = ChaosM(Mass, MassArray, vdx, NumParticles) * MassMultiplier;
			if (DampingArray)
			{
				Evolution->SetParticleDamping((*DampingArray)[vdx], ParticleStart + vdx);
			}
		}

		Evolution->SetDamping(DampingMultiplier, GroupOffset - 1);

		const FTransform& InitialTransform = Proxy.GetInitialTransform();


		for (uint32 vdx = 0; vdx < NumParticles; ++vdx)
		{
			int32 SolverParticleIndex = ParticleStart + vdx;
			Evolution->Particles().X(SolverParticleIndex) = ChaosVert(InitialTransform.TransformPosition(DoubleVert(DynamicVertex[vdx])));
			Evolution->Particles().V(SolverParticleIndex) = Chaos::FVec3(0.f, 0.f, 0.f);
			Evolution->Particles().M(SolverParticleIndex) = MassWithMultiplier[vdx];
			Evolution->Particles().InvM(SolverParticleIndex) = ChaosInvM(Evolution->Particles().M(SolverParticleIndex));
			Evolution->Particles().PAndInvM(SolverParticleIndex).InvM = Evolution->Particles().InvM(SolverParticleIndex);
		}

		bool ObjectEnableGravity = false;

		//int32 SolverParticleIndex = ParticleStart;
		if (const UObject* Owner = this->MObjects[ParticleStart]) {
			FFleshThreadingProxy::FFleshInputBuffer* FleshInputBuffer = nullptr;
			if (this->CurrentInputPackage->ObjectMap.Contains(Owner))
			{
				FleshInputBuffer = this->CurrentInputPackage->ObjectMap[Owner]->As<FFleshThreadingProxy::FFleshInputBuffer>();
				if (FleshInputBuffer)
				{
					ObjectEnableGravity = FleshInputBuffer->bEnableGravity;
				}
			}
		}

		if (!ObjectEnableGravity || !Property.bEnableGravity)
		{
			FSolverVec3 ZeroGravity(0.f);
			Evolution->SetGravity(ZeroGravity, GroupOffset - 1);
		}

		Proxy.SetSolverParticleRange(ParticleStart, NumParticles);
	}

	void FDeformableSolver::InitializeKinematicParticles(FFleshThreadingProxy& Proxy)
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_InitializeKinematicParticles);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_InitializeKinematicParticles);

		const FManagedArrayCollection& Rest = Proxy.GetRestCollection();
		const FIntVector2& Range = Proxy.GetSolverParticleRange();

		if (Property.bEnableKinematics)
		{
			typedef GeometryCollection::Facades::FKinematicBindingFacade FKinematics;
			FKinematics Kinematics(Rest);

			// Add Kinematics Node
			for (int i = Kinematics.NumKinematicBindings() - 1; i >= 0; i--)
			{
				FKinematics::FBindingKey Key = Kinematics.GetKinematicBindingKey(i);

				int32 BoneIndex = INDEX_NONE;
				TArray<int32> BoundVerts;
				TArray<float> BoundWeights;
				Kinematics.GetBoneBindings(Key, BoneIndex, BoundVerts, BoundWeights);

				for (int32 vdx : BoundVerts)
				{
					int32 ParticleIndex = Range[0] + vdx;
					Evolution->Particles().InvM(ParticleIndex) = 0.f;
					Evolution->Particles().PAndInvM(ParticleIndex).InvM = 0.f;
				}
			}
		}

		
	}

	void FDeformableSolver::InitializeWeakConstraint(FFleshThreadingProxy& Proxy)
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_InitializeWeakConstraints);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_InitializeWeakConstraints);
		const FManagedArrayCollection& Rest = Proxy.GetRestCollection();
		const FIntVector2& Range = Proxy.GetSolverParticleRange();
		if (Property.bEnablePositionTargets)
		{
			typedef GeometryCollection::Facades::FPositionTargetFacade FPositionTargets;
			FPositionTargets PositionTargets(Rest);

			TSet<int32> ConstrainedVertices;

			TArray<TArray<int32>> PositionTargetIndices;
			TArray<TArray<FSolverReal>> PositionTargetWeights;
			TArray<TArray<int32>> PositionTargetSecondIndices;
			TArray<TArray<FSolverReal>> PositionTargetSecondWeights;
			TArray<FSolverReal> PositionTargetStiffness;

			PositionTargetIndices.SetNum(PositionTargets.NumPositionTargets());
			PositionTargetWeights.SetNum(PositionTargets.NumPositionTargets());
			PositionTargetSecondIndices.SetNum(PositionTargets.NumPositionTargets());
			PositionTargetSecondWeights.SetNum(PositionTargets.NumPositionTargets());
			PositionTargetStiffness.SetNum(PositionTargets.NumPositionTargets());

			// Read in position target info
			for (int i = PositionTargets.NumPositionTargets() - 1; i >= 0; i--)
			{
				GeometryCollection::Facades::FPositionTargetsData DataPackage = PositionTargets.GetPositionTarget(i);
				PositionTargetIndices[i] = DataPackage.SourceIndex;
				PositionTargetWeights[i] = DataPackage.SourceWeights;
				PositionTargetSecondIndices[i] = DataPackage.TargetIndex;
				PositionTargetSecondWeights[i] = DataPackage.TargetWeights;
				PositionTargetStiffness[i] = DataPackage.Stiffness;
			}


			int32 InitIndex = Evolution->AddConstraintInitRange(1, true);
			int32 ConstraintIndex = Evolution->AddConstraintRuleRange(1, true);

			FXPBDWeakConstraints<FSolverReal, FSolverParticles>* WeakConstraint =
				new FXPBDWeakConstraints<FSolverReal, FSolverParticles>(Evolution->Particles(),
					PositionTargetIndices, PositionTargetWeights, PositionTargetStiffness, PositionTargetSecondIndices, PositionTargetSecondWeights);

			Evolution->ConstraintInits()[InitIndex] =
				[WeakConstraint, this](FSolverParticles& InParticles, const FSolverReal Dt)
			{
				WeakConstraint->Init();
				//WeakConstraint->UpdateTargets(this->ComputeParticleTargets(WeakConstraint->GetIndices()));
			};

			Evolution->ConstraintRules()[ConstraintIndex] =
				[WeakConstraint](FSolverParticles& InParticles, const FSolverReal Dt)
			{
				WeakConstraint->ApplyInParallel(InParticles, Dt);
			};

			WeakConstraints.Add(TUniquePtr<FXPBDWeakConstraints<FSolverReal, FSolverParticles>>(WeakConstraint));

		}
	}


	void FDeformableSolver::DebugDrawTetrahedralParticles(FFleshThreadingProxy& Proxy)
	{
#if WITH_EDITOR
		auto ChaosTet = [](FIntVector4 V, int32 dp) { return Chaos::TVec4<int32>(dp + V.X, dp + V.Y, dp + V.Z, dp + V.W); };
		auto ChaosVert = [](FVector3d V) { return Chaos::FVec3(V.X, V.Y, V.Z); };
		auto DoubleVert = [](FVector3f V) { return FVector3d(V.X, V.Y, V.Z); };

		const FIntVector2& Range = Proxy.GetSolverParticleRange();
		const FManagedArrayCollection& Rest = Proxy.GetRestCollection();
		const TManagedArray<FIntVector4>& Tetrahedron = Rest.GetAttribute<FIntVector4>("Tetrahedron", "Tetrahedral");
		if (uint32 NumElements = Tetrahedron.Num())
		{
			const Chaos::Softs::FSolverParticles& P = Evolution->Particles();
			for (uint32 edx = 0; edx < NumElements; ++edx)
			{
				auto T = ChaosTet(Tetrahedron[edx], Range[0]);
				Chaos::FDebugDrawQueue::GetInstance().DrawDebugPoint(
					DoubleVert(P.X(T[0])), FColor::Blue, false, -1.0f, 0, 5);
			}
		}
#endif
	}


	void FDeformableSolver::InitializeTetrahedralConstraint(FFleshThreadingProxy& Proxy)
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_InitializeTetrahedralConstraint);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_InitializeTetrahedralConstraint);

		const FManagedArrayCollection& Rest = Proxy.GetRestCollection();

		auto ChaosTet = [](FIntVector4 V, int32 dp) { return Chaos::TVec4<int32>(dp + V.X, dp + V.Y, dp + V.Z, dp + V.W); };

		const TManagedArray<FIntVector4>& Tetrahedron = Rest.GetAttribute<FIntVector4>("Tetrahedron", "Tetrahedral");
		if (uint32 NumElements = Tetrahedron.Num())
		{
			const FIntVector2& Range = Proxy.GetSolverParticleRange();

			// Add Tetrahedral Elements Node
			TArray<Chaos::TVec4<int32>> Elements;
			Elements.SetNum(NumElements);
			for (uint32 edx = 0; edx < NumElements; ++edx)
			{
				Elements[edx] = ChaosTet(Tetrahedron[edx], Range[0]);
			}

			if (Property.bUseGridBasedConstraints)
			{
				int32 ElementsOffset = AllElements->Num();
				AllElements->SetNum(ElementsOffset + NumElements);
				for (uint32 edx = 0; edx < NumElements; ++edx)
				{
					(*AllElements)[edx + ElementsOffset] = ChaosTet(Tetrahedron[edx], Range[0]);
				}

			}

			if (Rest.HasAttributes({ FManagedArrayCollection::TManagedType<FSolverReal>("Stiffness", FGeometryCollection::VerticesGroup) }))
			{
				uint32 NumParticles = Rest.NumElements(FGeometryCollection::VerticesGroup);
				TArray<FSolverReal> StiffnessWithMultiplier;
				StiffnessWithMultiplier.Init(0.f, NumParticles);
				FSolverReal StiffnessMultiplier = 0.f;

				if (const UObject* Owner = this->MObjects[Range[0]]) {
					FFleshThreadingProxy::FFleshInputBuffer* FleshInputBuffer = nullptr;
					if (this->CurrentInputPackage->ObjectMap.Contains(Owner))
					{
						FleshInputBuffer = this->CurrentInputPackage->ObjectMap[Owner]->As<FFleshThreadingProxy::FFleshInputBuffer>();
						if (FleshInputBuffer)
						{
							StiffnessMultiplier = FleshInputBuffer->StiffnessMultiplier;
						}
					}
				}
				const TManagedArray<FSolverReal>* StiffnessArray = Rest.FindAttribute<FSolverReal>("Stiffness", FGeometryCollection::VerticesGroup);
				TArray<FSolverReal> TetStiffness;
				TetStiffness.Init(Property.EMesh, Elements.Num());
				if (StiffnessArray)
				{
					for (uint32 vdx = 0; vdx < NumParticles; ++vdx)
					{
						StiffnessWithMultiplier[vdx] = (*StiffnessArray)[vdx] * StiffnessMultiplier;
					}
					for (int32 edx = 0; edx < Elements.Num(); edx++)
					{
						TetStiffness[edx] = (StiffnessWithMultiplier[Tetrahedron[edx].X] + StiffnessWithMultiplier[Tetrahedron[edx].Y]
							+ StiffnessWithMultiplier[Tetrahedron[edx].Z] + StiffnessWithMultiplier[Tetrahedron[edx].W]) / 4.f;
					}

				}

				if (Property.bEnableCorotatedConstraints)
				{

					int32 InitIndex = Evolution->AddConstraintInitRange(1, true);
					int32 ConstraintIndex = Evolution->AddConstraintRuleRange(1, true);

					if (Property.bDoBlended)
					{
						FBlendedXPBDCorotatedConstraints<FSolverReal, FSolverParticles>* BlendedCorotatedConstraint =
							new FBlendedXPBDCorotatedConstraints<FSolverReal, FSolverParticles>(
								Evolution->Particles(), Elements, TetStiffness, (FSolverReal).3,/*bRecordMetric = */false, Property.BlendedZeta);

						Evolution->ConstraintInits()[InitIndex] =
							[BlendedCorotatedConstraint](FSolverParticles& InParticles, const FSolverReal Dt)
						{
							BlendedCorotatedConstraint->Init();
						};

						Evolution->ConstraintRules()[ConstraintIndex] =
							[BlendedCorotatedConstraint](FSolverParticles& InParticles, const FSolverReal Dt)
						{
							BlendedCorotatedConstraint->ApplyInParallel(InParticles, Dt);
						};

						BlendedCorotatedConstraints.Add(TUniquePtr<FBlendedXPBDCorotatedConstraints<FSolverReal, FSolverParticles>>(BlendedCorotatedConstraint));

					}
					else
					{
						FXPBDCorotatedConstraints<FSolverReal, FSolverParticles>* CorotatedConstraint =
							new FXPBDCorotatedConstraints<FSolverReal, FSolverParticles>(
								Evolution->Particles(), Elements, TetStiffness);

						Evolution->ConstraintInits()[InitIndex] =
							[CorotatedConstraint](FSolverParticles& InParticles, const FSolverReal Dt)
						{
							CorotatedConstraint->Init();
						};

						Evolution->ConstraintRules()[ConstraintIndex] =
							[CorotatedConstraint](FSolverParticles& InParticles, const FSolverReal Dt)
						{
							CorotatedConstraint->ApplyInParallel(InParticles, Dt);
						};

						CorotatedConstraints.Add(TUniquePtr<FXPBDCorotatedConstraints<FSolverReal, FSolverParticles>>(CorotatedConstraint));
					}
				}
			}

		}
	}


	void FDeformableSolver::InitializeGidBasedConstraints(FFleshThreadingProxy& Proxy)
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_InitializeGidBasedConstraints);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_InitializeGidBasedConstraints);

		if (Property.bUseGridBasedConstraints)
		{
			auto ChaosTet = [](FIntVector4 V, int32 dp) { return Chaos::TVec4<int32>(dp + V.X, dp + V.Y, dp + V.Z, dp + V.W); };

			const FManagedArrayCollection& Rest = Proxy.GetRestCollection();
			const TManagedArray<FIntVector4>& Tetrahedron = Rest.GetAttribute<FIntVector4>("Tetrahedron", "Tetrahedral");

			if (uint32 NumElements = Tetrahedron.Num())
			{
				const FIntVector2& Range = Proxy.GetSolverParticleRange();

				int32 ElementsOffset = AllElements->Num();
				AllElements->SetNum(ElementsOffset + NumElements);
				for (uint32 edx = 0; edx < NumElements; ++edx)
				{
					(*AllElements)[edx + ElementsOffset] = ChaosTet(Tetrahedron[edx], Range[0]);
				}
			}
		}
	}


	void FDeformableSolver::InitializeKinematicConstraint()
	{
		auto MKineticUpdate = [this](FSolverParticles& MParticles, const FSolverReal Dt, const FSolverReal MTime, const int32 Index)
		{
			SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_InitializeKinematicConstraint);
			TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_InitializeKinematicConstraint);

			if (0 <= Index && Index < this->MObjects.Num())
			{
				if (const UObject* Owner = this->MObjects[Index])
				{
					if (const FFleshThreadingProxy* Proxy = Proxies[Owner]->As<FFleshThreadingProxy>())
					{
						FTransform GlobalTransform = Proxy->GetInitialTransform();
						const FIntVector2& Range = Proxy->GetSolverParticleRange();
						const FManagedArrayCollection& Rest = Proxy->GetRestCollection();

						if (Rest.FindAttributeTyped<FVector3f>("Vertex", FGeometryCollection::VerticesGroup))
						{
							const TManagedArray<FVector3f>& Vertex = Rest.GetAttribute<FVector3f>("Vertex", FGeometryCollection::VerticesGroup);
							// @todo(chaos) : reduce conversions
							auto ChaosVert = [](FVector3f V) { return Chaos::FVec3(V.X, V.Y, V.Z); };
							auto ChaosVertfloat = [](FVector3f V) { return Chaos::TVector<FSolverReal, 3>(V.X, V.Y, V.Z); };
							auto SolverParticleToObjectVertexIndex = [&](int32 SolverParticleIndex) {return SolverParticleIndex - Range[0]; };

							FFleshThreadingProxy::FFleshInputBuffer* FleshInputBuffer = nullptr;
							if (this->CurrentInputPackage->ObjectMap.Contains(Owner))
							{
								FleshInputBuffer = this->CurrentInputPackage->ObjectMap[Owner]->As<FFleshThreadingProxy::FFleshInputBuffer>();
								if (FleshInputBuffer)
								{
									GlobalTransform = FleshInputBuffer->GlobalTransform;
								}
							}

							typedef GeometryCollection::Facades::FVertexBoneWeightsFacade FWeightsFacade;
							bool bParticleTouched = false;
							FWeightsFacade WeightsFacade(Rest);
							if (WeightsFacade.IsValid())
							{
								int32 NumObjectVertices = Rest.NumElements(FGeometryCollection::VerticesGroup);
								int32 ObjectVertexIndex = SolverParticleToObjectVertexIndex(Index);
								if (ensure(0 <= ObjectVertexIndex && ObjectVertexIndex < NumObjectVertices))
								{
									if (FleshInputBuffer)
									{
										TArray<int32> BoneIndices = WeightsFacade.GetBoneIndices()[ObjectVertexIndex];
										TArray<float> BoneWeights = WeightsFacade.GetBoneWeights()[ObjectVertexIndex];

										FFleshThreadingProxy::FFleshInputBuffer* PreviousFleshBuffer = nullptr;
										if (this->PreviousInputPackage && this->PreviousInputPackage->ObjectMap.Contains(Owner))
										{
											PreviousFleshBuffer = this->PreviousInputPackage->ObjectMap[Owner]->As<FFleshThreadingProxy::FFleshInputBuffer>();
										}

										MParticles.X(Index) = Chaos::TVector<FSolverReal, 3>((FSolverReal)0.);
										TVector<FSolverReal, 3> TargetPos((FSolverReal)0.);
										FSolverReal CurrentRatio = FSolverReal(this->Iteration) / FSolverReal(this->Property.NumSolverSubSteps);

										int32 RestNum = FleshInputBuffer->RestTransforms.Num();
										int32 TransformNum = FleshInputBuffer->Transforms.Num();
										if (RestNum > 0 && TransformNum > 0)
										{
											
											for (int32 i = BoneIndices.Num() - 1; 0 <= i; i--)
											{
												if (BoneIndices[i] > -1 && BoneIndices[i] < RestNum && BoneIndices[i] < TransformNum)
												{

													// @todo(flesh) : Add the pre-cached component space rest transforms to the rest collection. 
													// see  UFleshComponent::NewDeformableData for how its pulled from the SkeletalMesh
													FVec3 LocalPoint = FleshInputBuffer->RestTransforms[BoneIndices[i]].InverseTransformPosition(ChaosVert(Vertex[Index - Range[0]]));
													FVec3 ComponentPointAtT = FleshInputBuffer->Transforms[BoneIndices[i]].TransformPosition(LocalPoint);

													if (PreviousFleshBuffer)
													{
														FTransform BonePreviousTransform = PreviousFleshBuffer->Transforms[BoneIndices[i]];
														ComponentPointAtT = ComponentPointAtT * CurrentRatio + BonePreviousTransform.TransformPosition(LocalPoint) * ((FSolverReal)1. - CurrentRatio);
													}

													MParticles.X(Index) = GlobalTransform.TransformPosition(ComponentPointAtT);

													bParticleTouched = true;


													// @todo(flesh): Add non rigid skinning weights. 
													// Currently this just grabs the last joint, ideally
													// we would be using the weighted average for the vertex
													// positions.

#if WITH_EDITOR
													//debug draw
													//p.Chaos.DebugDraw.Enabled 1
													//p.Chaos.DebugDraw.Deformable.KinematicParticle 1
													if (GDeformableDebugParams.IsDebugDrawingEnabled() && GDeformableDebugParams.bDoDrawKinematicParticles)
													{
														auto DoubleVert = [](FVector3f V) { return FVector3d(V.X, V.Y, V.Z); };
														Chaos::FDebugDrawQueue::GetInstance().DrawDebugPoint(DoubleVert(MParticles.X(Index)), FColor::Red, false, -1.0f, 0, 5);
													}
#endif
													break;
												}
											}
										}
										MParticles.PAndInvM(Index).P = MParticles.X(Index);
									}
								}
							}
							if (!bParticleTouched)
							{
								MParticles.X(Index) = GlobalTransform.TransformPosition(ChaosVert(Vertex[Index - Range[0]]));
								MParticles.PAndInvM(Index).P = MParticles.X(Index);

#if WITH_EDITOR
								//debug draw
								//p.Chaos.DebugDraw.Enabled 1
								//p.Chaos.DebugDraw.Deformable.KinematicParticle 1
								if (GDeformableDebugParams.IsDebugDrawingEnabled() && GDeformableDebugParams.bDoDrawKinematicParticles)
								{
									auto DoubleVert = [](FVector3f V) { return FVector3d(V.X, V.Y, V.Z); };
									Chaos::FDebugDrawQueue::GetInstance().DrawDebugPoint(DoubleVert(MParticles.X(Index)), FColor::Red, false, -1.0f, 0, 5);
								}
#endif
							}
						}
					}
				}
			}
		};
		Evolution->SetKinematicUpdateFunction(MKineticUpdate);
	}

	void FDeformableSolver::InitializeCollisionBodies()
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_InitializeCollisionBodies);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_InitializeCollisionBodies);

		if (Property.bUseFloor && Evolution->CollisionParticles().Size() == 0)
		{
			Chaos::FVec3 Position(0.f);
			Chaos::FVec3 EulerRot(0.f);
			int32 CollisionParticleOffset = Evolution->AddCollisionParticleRange(1, INDEX_NONE, true);
			Evolution->CollisionParticles().X(0) = Position;
			Evolution->CollisionParticles().R(0) = Chaos::TRotation<Chaos::FReal, 3>::MakeFromEuler(EulerRot);
			Evolution->CollisionParticles().SetDynamicGeometry(0, MakeUnique<Chaos::TPlane<Chaos::FReal, 3>>(Chaos::FVec3(0.f, 0.f, 0.f), Chaos::FVec3(0.f, 0.f, 1.f)));

		}
	}

	void FDeformableSolver::InitializeSelfCollisionVariables()
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_InitializeSelfCollisionVariables);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_InitializeSelfCollisionVariables);


		int32 NumParticles = Evolution->Particles().Size();
		SurfaceTriangleMesh->Init(*SurfaceElements);
		TriangleMeshCollisions.Reset(new FPBDTriangleMeshCollisions(
			0, Evolution->Particles().Size(), *SurfaceTriangleMesh, false, false));
		TSet<Chaos::TVec2<int32>>* InDisabledCollisionElements = new TSet<Chaos::TVec2<int32>>();
		for (int32 i = 0; i < (int32)NumParticles; i++)
		{
			Chaos::TVec2<int32> LocalEdge = { i, i };
			InDisabledCollisionElements->Add(LocalEdge);
		}
		CollisionSpringConstraint.Reset(new FPBDCollisionSpringConstraints(0, (int32)NumParticles, *SurfaceTriangleMesh, nullptr, MoveTemp(*InDisabledCollisionElements), 1.f, 1.f));
		int32 InitIndex1 = Evolution->AddConstraintInitRange(1, true);
		Evolution->ConstraintInits()[InitIndex1] =
			[this](FSolverParticles& InParticles, const FSolverReal Dt)
		{
			this->TriangleMeshCollisions->Init(InParticles);
			TArray<FPBDTriangleMeshCollisions::FGIAColor> EmptyGIAColors;
			this->CollisionSpringConstraint->Init(InParticles, TriangleMeshCollisions->GetSpatialHash(), static_cast<TConstArrayView<FPBDTriangleMeshCollisions::FGIAColor>>(EmptyGIAColors), EmptyGIAColors);
		};
		int32 ConstraintIndex1 = Evolution->AddConstraintRuleRange(1, true);
		Evolution->ConstraintRules()[ConstraintIndex1] =
			[this](FSolverParticles& InParticles, const FSolverReal Dt)
		{
			this->CollisionSpringConstraint->Apply(InParticles, Dt);
		};
	}

	void FDeformableSolver::InitializeGridBasedConstraintVariables()
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_InitializeGridBasedConstraintVariables);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_InitializeGridBasedConstraintVariables);


		GridBasedCorotatedConstraint.Reset(new Chaos::Softs::FXPBDGridBasedCorotatedConstraints<FSolverReal, FSolverParticles>(
			Evolution->Particles(), *AllElements, Property.GridDx, /*bRecordMetric = */false, (Chaos::Softs::FSolverReal).1, (Chaos::Softs::FSolverReal).01, (Chaos::Softs::FSolverReal).4, (Chaos::Softs::FSolverReal)1000.0));
		Evolution->ResetConstraintRules();
		int32 InitIndex1 = Evolution->AddConstraintInitRange(1, true);
		Evolution->ConstraintInits()[InitIndex1] =
			[this](FSolverParticles& InParticles, const FSolverReal Dt)
		{
			this->GridBasedCorotatedConstraint->Init(InParticles, Dt);
		};
		int32 ConstraintIndex1 = Evolution->AddConstraintRuleRange(1, true);
		Evolution->ConstraintRules()[ConstraintIndex1] =
			[this](FSolverParticles& InParticles, const FSolverReal Dt)
		{
			this->GridBasedCorotatedConstraint->ApplyInParallel(InParticles, Dt);
		};
		int32 PostprocessingIndex1 = Evolution->AddConstraintPostprocessingsRange(1, true);
		Evolution->ConstraintPostprocessings()[PostprocessingIndex1] =
			[this](FSolverParticles& InParticles, const FSolverReal Dt)
		{
			this->GridBasedCorotatedConstraint->TimeStepPostprocessing(InParticles, Dt);
		};
	
	
	}

	void FDeformableSolver::RemoveSimulationObjects()
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_RemoveSimulationObjects);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_RemoveSimulationObjects);

		TArray< FThreadingProxy* > RemovedProxies;
		{
			FScopeLock Lock(&RemovalMutex); // @todo(flesh) : change to threaded task based commands to prevent the lock. 
			RemovedProxies = TArray< FThreadingProxy* >(RemovedProxys_Internal);
			RemovedProxys_Internal.Empty();
		}

		if (RemovedProxies.Num())
		{
			Evolution->ResetConstraintRules();
			Evolution->DeactivateParticleRanges();

			// delete the simulated particles in block moves
			for (FThreadingProxy* BaseProxy : RemovedProxies)
			{
				if (FFleshThreadingProxy* Proxy = BaseProxy->As<FFleshThreadingProxy>())
				{
					FIntVector2 Indices = Proxy->GetSolverParticleRange();
					Proxies.FindAndRemoveChecked(MObjects[Indices[0]]);
					Evolution->Particles().RemoveAt(Indices[0], Indices[1]);
				}
			}

			// reindex ranges on moved particles in the proxies. 
			const UObject* CurrentObject = nullptr;
			for (int Index = 0; Index < MObjects.Num(); Index++)
			{
				if (MObjects[Index] != CurrentObject)
				{
					CurrentObject = MObjects[Index];
					if (CurrentObject)
					{
						if (ensure(Proxies.Contains(CurrentObject)))
						{
							if (FFleshThreadingProxy* MovedProxy = Proxies[CurrentObject]->As<FFleshThreadingProxy>())
							{
								FIntVector2 Range = MovedProxy->GetSolverParticleRange();
								MovedProxy->SetSolverParticleRange(Index, Range[1]);
								int32 Offset = Evolution->AddParticleRange(Range[1]);
								//ensure(Offset == Range[0]);
							}
						}
					}
				}
			}

			// regenerate all constraints
			for (TPair< FThreadingProxy::FKey, TUniquePtr<FThreadingProxy> >& BaseProxyPair : Proxies)
			{
				if (FFleshThreadingProxy* Proxy = BaseProxyPair.Value->As<FFleshThreadingProxy>())
				{
					InitializeTetrahedralConstraint(*Proxy);
					InitializeGidBasedConstraints(*Proxy);
				}
			}
		}
	}

	void FDeformableSolver::AdvanceDt(FSolverReal DeltaTime)
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_AdvanceDt);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_AdvanceDt);

		EventPreSolve.Broadcast(DeltaTime);

		int32 NumIterations = FMath::Clamp<int32>(Property.NumSolverSubSteps, 0, INT_MAX);
		if (bEnableSolver && NumIterations)
		{
			FSolverReal SubDeltaTime = DeltaTime / (FSolverReal)NumIterations;
			if (!FMath::IsNearlyZero(SubDeltaTime))
			{
				for (int i = 0; i < NumIterations; ++i)
				{
					Iteration = i+1;
					Update(SubDeltaTime);
				}

				Frame++;
				EventPostSolve.Broadcast(DeltaTime);
			}
		}



		{
			// Update client state
			FDeformableDataMap OutputBuffers;
			for (TPair< FThreadingProxy::FKey, TUniquePtr<FThreadingProxy> >& BaseProxyPair : Proxies)
			{
				UpdateOutputState(*BaseProxyPair.Value);
				if (FFleshThreadingProxy* Proxy = BaseProxyPair.Value->As<FFleshThreadingProxy>())
				{
					OutputBuffers.Add(Proxy->GetOwner(), TSharedPtr<FThreadingProxy::FBuffer>(new FFleshThreadingProxy::FFleshOutputBuffer(*Proxy)));

					if (Property.CacheToFile)
					{
						WriteFrame(*Proxy, DeltaTime);
					}
				}
			}
			PushOutputPackage(Frame, MoveTemp(OutputBuffers));
		}

		{
#if WITH_EDITOR
			// debug draw
	
			//p.Chaos.DebugDraw.Enabled 1
			if (GDeformableDebugParams.IsDebugDrawingEnabled())
			{
				for (TPair< FThreadingProxy::FKey, TUniquePtr<FThreadingProxy> >& BaseProxyPair : Proxies)
				{
					if (FFleshThreadingProxy* Proxy = BaseProxyPair.Value->As<FFleshThreadingProxy>())
					{
						if (GDeformableDebugParams.bDoDrawTetrahedralParticles)
						{
							//p.Chaos.DebugDraw.Deformable.TetrahedralParticles 1
							DebugDrawTetrahedralParticles(*Proxy);
						}
					}
				}
			}
#endif
		}

		EventPreBuffer.Broadcast(DeltaTime);
	}

	void FDeformableSolver::PushInputPackage(int32 InFrame, FDeformableDataMap&& InPackage)
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_PushInputPackage);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_PushInputPackage);

		FScopeLock Lock(&PackageInputMutex);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_PushInputPackage);
		BufferedInputPackages.Push(TUniquePtr< FDeformablePackage >(new FDeformablePackage(InFrame, MoveTemp(InPackage))));
	}

	TUniquePtr<FDeformablePackage> FDeformableSolver::PullInputPackage()
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_PullInputPackage);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_PullInputPackage);

		FScopeLock Lock(&PackageInputMutex);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_PullInputPackage);
		if (BufferedInputPackages.Num())
			return BufferedInputPackages.Pop();
		return TUniquePtr<FDeformablePackage>(nullptr);
	}

	void FDeformableSolver::UpdateProxyInputPackages()
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_UpdateProxyInputPackages);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_UpdateProxyInputPackages);

		if (CurrentInputPackage)
		{
			PreviousInputPackage = TUniquePtr < FDeformablePackage >(CurrentInputPackage.Release());
			CurrentInputPackage = TUniquePtr < FDeformablePackage >(nullptr);
		}

		TUniquePtr < FDeformablePackage > TailPackage = PullInputPackage();
		while (TailPackage)
		{
			CurrentInputPackage = TUniquePtr < FDeformablePackage >(TailPackage.Release());
			TailPackage = PullInputPackage();
		}
	}

	void FDeformableSolver::Update(FSolverReal DeltaTime)
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_Update);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_Update);


		if (!Proxies.Num()) return;


		if (!Property.FixTimeStep)
		{
			Evolution->AdvanceOneTimeStep(DeltaTime);
			Time += DeltaTime;
		}
		else
		{
			Evolution->AdvanceOneTimeStep(Property.TimeStepSize);
			Time += Property.TimeStepSize;
		}

	}

	void FDeformableSolver::PushOutputPackage(int32 InFrame, FDeformableDataMap&& InPackage)
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_PushOutputPackage);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_PushOutputPackage);

		FScopeLock Lock(&PackageOutputMutex);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_PushPackage);
		BufferedOutputPackages.Push(TUniquePtr< FDeformablePackage >(new FDeformablePackage(InFrame, MoveTemp(InPackage))));
	}

	TUniquePtr<FDeformablePackage> FDeformableSolver::PullOutputPackage()
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_PullOutputPackage);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_PullOutputPackage);

		FScopeLock Lock(&PackageOutputMutex);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_PullPackage);
		if (BufferedOutputPackages.Num())
			return BufferedOutputPackages.Pop();
		return TUniquePtr<FDeformablePackage>(nullptr);
	}

	void FDeformableSolver::AddProxy(FThreadingProxy* InProxy)
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_AddProxy);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_AddProxy);

		FScopeLock Lock(&InitializationMutex);
		UninitializedProxys_Internal.Add(InProxy);
		InitializedObjects_External.Add(InProxy->GetOwner());
	}

	void FDeformableSolver::RemoveProxy(FThreadingProxy* InProxy)
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_RemoveProxy);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_RemoveProxy);

		FScopeLock LockA(&RemovalMutex);
		FScopeLock LockB(&InitializationMutex);

		InitializedObjects_External.Remove(InProxy->GetOwner());

		// If a proxy has not been initialized yet, then we need
		// to clean up the internal buffers. 
		int32 Index = UninitializedProxys_Internal.IndexOfByKey(InProxy);
		if(Index!=INDEX_NONE)
		{
			UninitializedProxys_Internal.RemoveAtSwap(Index);
			if (Proxies.Contains(InProxy->GetOwner()))
			{
				RemovedProxys_Internal.Add(InProxy);
			}
			else
			{
				delete InProxy;
			}
		}
		else if(Proxies.Contains(InProxy->GetOwner()))
		{
			RemovedProxys_Internal.Add(InProxy);
		}

	}

	void FDeformableSolver::UpdateOutputState(FThreadingProxy& InProxy)
	{
		SCOPE_CYCLE_COUNTER(STAT_ChaosDeformableSolver_UpdateOutputState);
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_UpdateOutputState);

		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_UpdateOutputState);
		if (FFleshThreadingProxy* Proxy = InProxy.As<FFleshThreadingProxy>())
		{

			const FIntVector2& Range = Proxy->GetSolverParticleRange();
			if (0 <= Range[0])
			{
				// @todo(chaos) : reduce conversions
				auto UEVertd = [](Chaos::FVec3 V) { return FVector3d(V.X, V.Y, V.Z); };
				auto UEVertf = [](FVector3d V) { return FVector3f((float)V.X, (float)V.Y, (float)V.Z); };

				TManagedArray<FVector3f>& Position = Proxy->GetDynamicCollection().ModifyAttribute<FVector3f>("Vertex", FGeometryCollection::VerticesGroup);
				for (int32 vdx = 0; vdx < Position.Num(); vdx++)
				{
					Position[vdx] = UEVertf(UEVertd(Evolution->Particles().X(vdx + Range[0])));
				}
			}
		}
	}

	void FDeformableSolver::WriteFrame(FThreadingProxy& InProxy, const FSolverReal DeltaTime)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(ChaosDeformableSolver_WriteFrame);
		if (FFleshThreadingProxy* Proxy = InProxy.As<FFleshThreadingProxy>())
		{
			if (const FManagedArrayCollection* Rest = &Proxy->GetRestCollection())
			{
				const TManagedArray<FIntVector>& Indices = Rest->GetAttribute<FIntVector>("Indices", FGeometryCollection::FacesGroup);

				WriteTrisGEO(Evolution->Particles(), *SurfaceElements);
				FString file = FPaths::ProjectDir();
				file.Append(TEXT("/HoudiniOutput/DtLog.txt"));
				if (Frame == 0)
				{
					FFileHelper::SaveStringToFile(FString(TEXT("DeltaTime\r\n")), *file);
				}
				FFileHelper::SaveStringToFile((FString::SanitizeFloat(DeltaTime) + FString(TEXT("\r\n"))), *file, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), EFileWrite::FILEWRITE_Append);
			}
		}
	}

	void FDeformableSolver::WriteTrisGEO(const FSolverParticles& Particles, const TArray<TVec3<int32>>& Mesh)
	{
		FString file = FPaths::ProjectDir();
		//file.Append(TEXT("\HoudiniOuput\Test.geo"));
		file.Append(TEXT("/HoudiniOutput/sim_frame_"));
		file.Append(FString::FromInt(Frame));
		file.Append(TEXT(".geo"));

		int32 Np = Particles.Size();
		int32 NPrims = Mesh.Num();

		// We will use this FileManager to deal with the file.
		IPlatformFile& FileManager = FPlatformFileManager::Get().GetPlatformFile();
		FFileHelper::SaveStringToFile(FString(TEXT("PGEOMETRY V5\r\n")), *file);
		FString HeaderInfo = FString(TEXT("NPoints ")) + FString::FromInt(Np) + FString(TEXT(" NPrims ")) + FString::FromInt(NPrims) + FString(TEXT("\r\n"));
		FString MoreHeader = FString(TEXT("NPointGroups 0 NPrimGroups 0\r\nNPointAttrib 0 NVertexAttrib 0 NPrimAttrib 0 NAttrib 0\r\n"));

		FFileHelper::SaveStringToFile(HeaderInfo, *file, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), EFileWrite::FILEWRITE_Append);
		FFileHelper::SaveStringToFile(MoreHeader, *file, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), EFileWrite::FILEWRITE_Append);

		for (int32 i = 0; i < Np; i++) {

			FString ParticleInfo = FString::SanitizeFloat(Particles.X(i)[0]) + FString(TEXT(" ")) + FString::SanitizeFloat(Particles.X(i)[1]) + FString(TEXT(" ")) + FString::SanitizeFloat(Particles.X(i)[2]) + FString(TEXT(" ")) + FString::FromInt(1) + FString(TEXT("\r\n"));
			FFileHelper::SaveStringToFile(ParticleInfo, *file, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), EFileWrite::FILEWRITE_Append);

		}

		for (int32 i = 0; i < Mesh.Num(); i++) {
			//outstream << "Poly 3 < ";
			FString ElementToWrite = FString(TEXT("Poly 3 < ")) + FString::FromInt(Mesh[i][0]) + FString(TEXT(" ")) + FString::FromInt(Mesh[i][1]) + FString(TEXT(" ")) + FString::FromInt(Mesh[i][2]) + FString(TEXT("\r\n"));
			FFileHelper::SaveStringToFile(ElementToWrite, *file, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), EFileWrite::FILEWRITE_Append);
		}

		FFileHelper::SaveStringToFile(FString(TEXT("beginExtra\n")), *file, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), EFileWrite::FILEWRITE_Append);
		FFileHelper::SaveStringToFile(FString(TEXT("endExtra\n")), *file, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), EFileWrite::FILEWRITE_Append);

	}
}; // Namespace Chaos::Softs