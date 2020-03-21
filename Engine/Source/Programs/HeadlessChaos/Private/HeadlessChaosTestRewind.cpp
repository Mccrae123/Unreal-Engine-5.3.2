// Copyright Epic Games, Inc. All Rights Reserved.

#include "HeadlessChaosTestUtility.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/ErrorReporter.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "PhysicsProxy/GeometryCollectionPhysicsProxy.h"
#include "Chaos/Utilities.h"
#include "PBDRigidsSolver.h"
#include "ChaosSolversModule.h"

#include "Modules/ModuleManager.h"
#include "Framework/PhysicsTickTask.h"
#include "RewindData.h"


namespace ChaosTest {

    using namespace Chaos;

	void TickSolverHelper(FChaosSolversModule* Module, FPhysicsSolver* Solver)
	{
		Solver->PushPhysicsState(Module->GetDispatcher());
		FPhysicsSolverAdvanceTask AdvanceTask(Solver,1.0f);
		AdvanceTask.DoTask(ENamedThreads::GameThread,FGraphEventRef());
		Solver->BufferPhysicsResults();
		Solver->FlipBuffers();
		Solver->UpdateGameThreadStructures();
	}

	GTEST_TEST(RewindTest,MovingGeomChange)
	{
		auto Sphere = TSharedPtr<FImplicitObject,ESPMode::ThreadSafe>(new TSphere<float,3>(TVector<float,3>(0),10));
		auto Box = TSharedPtr<FImplicitObject,ESPMode::ThreadSafe>(new TBox<float,3>(FVec3(0),FVec3(1)));
		auto Box2 = TSharedPtr<FImplicitObject,ESPMode::ThreadSafe>(new TBox<float,3>(FVec3(2),FVec3(3)));

		FChaosSolversModule* Module = FChaosSolversModule::GetModule();
		Module->ChangeThreadingMode(EChaosThreadingMode::SingleThread);

		// Make a solver
		FPhysicsSolver* Solver = Module->CreateSolver(nullptr,ESolverFlags::Standalone);
		Solver->SetEnabled(true);

		Solver->EnableRewindCapture(20);


		// Make particles
		auto Particle = TKinematicGeometryParticle<float,3>::CreateParticle();

		Particle->SetGeometry(Sphere);
		Solver->RegisterObject(Particle.Get());

		for(int Step = 0; Step < 11; ++Step)
		{
			//property that changes every step
			Particle->SetX(FVec3(0,0,100 - Step));

			//property that changes once half way through
			if(Step == 3)
			{
				Particle->SetGeometry(Box);
			}

			if(Step == 5)
			{
				Particle->SetGeometry(Box2);
			}

			if(Step == 7)
			{
				Particle->SetGeometry(Box);
			}

			TickSolverHelper(Module, Solver);
		}

		//ended up at z = 90
		EXPECT_EQ(Particle->X()[2],90);

		//ended up with box geometry
		EXPECT_EQ(Box.Get(),Particle->Geometry().Get());
		
		const FRewindData* RewindData = Solver->GetRewindData();

		//check state at every step except latest
		for(int Step = 0; Step < 10; ++Step)
		{
			const auto ParticleState = RewindData->GetStateAtFrame(*Particle,Step);
			EXPECT_EQ(ParticleState.X()[2],100 - Step);

			if(Step < 3)
			{
				//was sphere
				EXPECT_EQ(ParticleState.Geometry().Get(),Sphere.Get());
			}
			else if(Step < 5 || Step >= 7)
			{
				//then became box
				EXPECT_EQ(ParticleState.Geometry().Get(),Box.Get());
			}
			else
			{
				//second box
				EXPECT_EQ(ParticleState.Geometry().Get(),Box2.Get());
			}
		}
		
		// Throw out the proxy
		Solver->UnregisterObject(Particle.Get());

		Module->DestroySolver(Solver);
	}


	GTEST_TEST(RewindTest,AddForce)
	{
		auto Sphere = TSharedPtr<FImplicitObject,ESPMode::ThreadSafe>(new TSphere<float,3>(TVector<float,3>(0),10));

		FChaosSolversModule* Module = FChaosSolversModule::GetModule();
		Module->ChangeThreadingMode(EChaosThreadingMode::SingleThread);

		// Make a solver
		FPhysicsSolver* Solver = Module->CreateSolver(nullptr,ESolverFlags::Standalone);
		Solver->SetEnabled(true);

		Solver->EnableRewindCapture(20);


		// Make particles
		auto Particle = TPBDRigidParticle<float,3>::CreateParticle();

		Particle->SetGeometry(Sphere);
		Solver->RegisterObject(Particle.Get());

		for(int Step = 0; Step < 11; ++Step)
		{
			//sim-writable property that changes every step
			Particle->SetF(FVec3(0,0,Step + 1));


			TickSolverHelper(Module,Solver);
		}

		const FRewindData* RewindData = Solver->GetRewindData();

		//check state at every step except latest
		for(int Step = 0; Step < 10; ++Step)
		{
			const auto ParticleState = RewindData->GetStateAtFrame(*Particle,Step);
			EXPECT_EQ(ParticleState.F()[2],Step+1);
		}

		// Throw out the proxy
		Solver->UnregisterObject(Particle.Get());

		Module->DestroySolver(Solver);
	}

	GTEST_TEST(RewindTest,IntermittentForce)
	{
		auto Sphere = TSharedPtr<FImplicitObject,ESPMode::ThreadSafe>(new TSphere<float,3>(TVector<float,3>(0),10));

		FChaosSolversModule* Module = FChaosSolversModule::GetModule();
		Module->ChangeThreadingMode(EChaosThreadingMode::SingleThread);

		// Make a solver
		FPhysicsSolver* Solver = Module->CreateSolver(nullptr,ESolverFlags::Standalone);
		Solver->SetEnabled(true);

		Solver->EnableRewindCapture(20);


		// Make particles
		auto Particle = TPBDRigidParticle<float,3>::CreateParticle();

		Particle->SetGeometry(Sphere);
		Solver->RegisterObject(Particle.Get());

		for(int Step = 0; Step < 11; ++Step)
		{	
			//sim-writable property that changes infrequently and not at beginning
			if(Step == 3)
			{
				Particle->SetF(FVec3(0,0,Step));
			}

			if(Step == 5)
			{
				Particle->SetF(FVec3(0,0,Step));
			}

			TickSolverHelper(Module,Solver);
		}
		
		const FRewindData* RewindData = Solver->GetRewindData();

		//check state at every step except latest
		for(int Step = 0; Step < 10; ++Step)
		{
			const auto ParticleState = RewindData->GetStateAtFrame(*Particle,Step);

			if(Step == 3)
			{
				EXPECT_EQ(ParticleState.F()[2],3);
			}
			else if(Step == 5)
			{
				EXPECT_EQ(ParticleState.F()[2],5);
			}
			else
			{
				EXPECT_EQ(ParticleState.F()[2],0);
			}
		}

		// Throw out the proxy
		Solver->UnregisterObject(Particle.Get());

		Module->DestroySolver(Solver);
	}

	GTEST_TEST(RewindTest,IntermittentGeomChange)
	{
		auto Sphere = TSharedPtr<FImplicitObject,ESPMode::ThreadSafe>(new TSphere<float,3>(TVector<float,3>(0),10));
		auto Box = TSharedPtr<FImplicitObject,ESPMode::ThreadSafe>(new TBox<float,3>(FVec3(0),FVec3(1)));
		auto Box2 = TSharedPtr<FImplicitObject,ESPMode::ThreadSafe>(new TBox<float,3>(FVec3(2),FVec3(3)));

		FChaosSolversModule* Module = FChaosSolversModule::GetModule();
		Module->ChangeThreadingMode(EChaosThreadingMode::SingleThread);

		// Make a solver
		FPhysicsSolver* Solver = Module->CreateSolver(nullptr,ESolverFlags::Standalone);
		Solver->SetEnabled(true);

		Solver->EnableRewindCapture(20);


		// Make particles
		auto Particle = TKinematicGeometryParticle<float,3>::CreateParticle();

		Particle->SetGeometry(Sphere);
		Solver->RegisterObject(Particle.Get());

		for(int Step = 0; Step < 11; ++Step)
		{
			//property that changes once half way through
			if(Step == 3)
			{
				Particle->SetGeometry(Box);
			}

			if(Step == 5)
			{
				Particle->SetGeometry(Box2);
			}

			if(Step == 7)
			{
				Particle->SetGeometry(Box);
			}

			TickSolverHelper(Module,Solver);
		}

		const FRewindData* RewindData = Solver->GetRewindData();

		//check state at every step except latest
		for(int Step = 0; Step < 10; ++Step)
		{
			const auto ParticleState = RewindData->GetStateAtFrame(*Particle,Step);

			
			if(Step < 3)
			{
				//was sphere
				EXPECT_EQ(ParticleState.Geometry().Get(),Sphere.Get());
			}
			else if(Step < 5 || Step >= 7)
			{
				//then became box
				EXPECT_EQ(ParticleState.Geometry().Get(),Box.Get());
			}
			else
			{
				//second box
				EXPECT_EQ(ParticleState.Geometry().Get(),Box2.Get());
			}
		}

		// Throw out the proxy
		Solver->UnregisterObject(Particle.Get());

		Module->DestroySolver(Solver);
	}

	GTEST_TEST(RewindTest,FallingObjectWithTeleport)
	{
		auto Sphere = TSharedPtr<FImplicitObject,ESPMode::ThreadSafe>(new TSphere<float,3>(TVector<float,3>(0),10));

		FChaosSolversModule* Module = FChaosSolversModule::GetModule();
		Module->ChangeThreadingMode(EChaosThreadingMode::SingleThread);

		// Make a solver
		FPhysicsSolver* Solver = Module->CreateSolver(nullptr,ESolverFlags::Standalone);
		Solver->SetEnabled(true);

		Solver->EnableRewindCapture(20);


		// Make particles
		auto Particle = TPBDRigidParticle<float,3>::CreateParticle();

		Particle->SetGeometry(Sphere);
		Solver->RegisterObject(Particle.Get());
		Particle->SetGravityEnabled(true);
		Particle->SetX(FVec3(0,0,100));

		TArray<FVec3> X;
		TArray<FVec3> V;

		for(int Step = 0; Step < 10; ++Step)
		{
			//teleport from GT
			if(Step == 5)
			{
				Particle->SetX(FVec3(0,0,10));
				Particle->SetV(FVec3(0,0,1));
			}

			X.Add(Particle->X());
			V.Add(Particle->V());
			TickSolverHelper(Module,Solver);
		}

		const FRewindData* RewindData = Solver->GetRewindData();


		for(int Step = 0; Step < 9; ++Step)
		{
			const auto ParticleState = RewindData->GetStateAtFrame(*Particle,Step);
			
			EXPECT_EQ(ParticleState.X()[2],X[Step][2]);
			EXPECT_EQ(ParticleState.V()[2],V[Step][2]);
		}

		// Throw out the proxy
		Solver->UnregisterObject(Particle.Get());

		Module->DestroySolver(Solver);
	}

	GTEST_TEST(RewindTest,ApplyRewind)
	{
		auto Sphere = TSharedPtr<FImplicitObject,ESPMode::ThreadSafe>(new TSphere<float,3>(TVector<float,3>(0),10));

		FChaosSolversModule* Module = FChaosSolversModule::GetModule();
		Module->ChangeThreadingMode(EChaosThreadingMode::SingleThread);

		// Make a solver
		FPhysicsSolver* Solver = Module->CreateSolver(nullptr,ESolverFlags::Standalone);
		Solver->SetEnabled(true);

		Solver->EnableRewindCapture(20);


		// Make particles
		auto Particle = TPBDRigidParticle<float,3>::CreateParticle();

		Particle->SetGeometry(Sphere);
		Solver->RegisterObject(Particle.Get());
		Particle->SetGravityEnabled(true);
		Particle->SetX(FVec3(0,0,100));

		TArray<FVec3> X;
		TArray<FVec3> V;

		for(int Step = 0; Step < 10; ++Step)
		{
			//teleport from GT
			if(Step == 5)
			{
				Particle->SetX(FVec3(0,0,10));
				Particle->SetV(FVec3(0,0,1));
			}

			X.Add(Particle->X());
			V.Add(Particle->V());
			TickSolverHelper(Module,Solver);
		}

		FRewindData* RewindData = Solver->GetRewindData();
		RewindData->RewindToFrame(3);
		
		EXPECT_EQ(Particle->X()[2],X[3][2]);
		EXPECT_EQ(Particle->V()[2],V[3][2]);

		// Throw out the proxy
		Solver->UnregisterObject(Particle.Get());

		Module->DestroySolver(Solver);
	}

	GTEST_TEST(RewindTest,Remove)
	{
		auto Sphere = TSharedPtr<FImplicitObject,ESPMode::ThreadSafe>(new TSphere<float,3>(TVector<float,3>(0),10));

		FChaosSolversModule* Module = FChaosSolversModule::GetModule();
		Module->ChangeThreadingMode(EChaosThreadingMode::SingleThread);

		// Make a solver
		FPhysicsSolver* Solver = Module->CreateSolver(nullptr,ESolverFlags::Standalone);
		Solver->SetEnabled(true);

		Solver->EnableRewindCapture(20);


		// Make particles
		auto Particle = TPBDRigidParticle<float,3>::CreateParticle();

		Particle->SetGeometry(Sphere);
		Solver->RegisterObject(Particle.Get());
		Particle->SetGravityEnabled(true);
		Particle->SetX(FVec3(0,0,100));

		TArray<FVec3> X;
		TArray<FVec3> V;

		for(int Step = 0; Step < 10; ++Step)
		{
			X.Add(Particle->X());
			V.Add(Particle->V());
			TickSolverHelper(Module,Solver);
		}

		FRewindData* RewindData = Solver->GetRewindData();

		{
			const FGeometryParticleState State = RewindData->GetStateAtFrame(*Particle,5);
			EXPECT_EQ(State.X(),X[5]);
		}

		// Throw out the proxy
		Solver->UnregisterObject(Particle.Get());

		// State should be the same as being at head because we removed it from solver
		{
			const FGeometryParticleState State = RewindData->GetStateAtFrame(*Particle,5);
			EXPECT_EQ(Particle->X(), State.X());
		}

		Module->DestroySolver(Solver);
	}
}
