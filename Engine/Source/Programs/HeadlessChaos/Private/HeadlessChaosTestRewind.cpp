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
		X.Add(Particle->X());
		V.Add(Particle->V());

		FRewindData* RewindData = Solver->GetRewindData();
		EXPECT_TRUE(RewindData->RewindToFrame(0));
		
		//make sure recorded data is still valid even at head
		for(int Step = 0; Step < 11; ++Step)
		{
			const FGeometryParticleState State = RewindData->GetStateAtFrame(*Particle,Step);
			EXPECT_EQ(State.X()[2],X[Step][2]);
			EXPECT_EQ(State.V()[2],V[Step][2]);
		}

		//rewind to each frame and make sure data is recorded
		for(int Step = 0; Step < 10; ++Step)
		{
			EXPECT_TRUE(RewindData->RewindToFrame(Step));
			EXPECT_EQ(Particle->X()[2],X[Step][2]);
			EXPECT_EQ(Particle->V()[2],V[Step][2]);
		}

		//can't rewind earlier than latest rewind
		EXPECT_FALSE(RewindData->RewindToFrame(5));

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

	GTEST_TEST(RewindTest,BufferLimit)
	{
		auto Sphere = TSharedPtr<FImplicitObject,ESPMode::ThreadSafe>(new TSphere<float,3>(TVector<float,3>(0),10));

		FChaosSolversModule* Module = FChaosSolversModule::GetModule();
		Module->ChangeThreadingMode(EChaosThreadingMode::SingleThread);

		// Make a solver
		FPhysicsSolver* Solver = Module->CreateSolver(nullptr,ESolverFlags::Standalone);
		Solver->SetEnabled(true);

		Solver->EnableRewindCapture(5);


		// Make particles
		auto Particle = TPBDRigidParticle<float,3>::CreateParticle();

		Particle->SetGeometry(Sphere);
		Solver->RegisterObject(Particle.Get());
		Particle->SetGravityEnabled(true);
		Particle->SetX(FVec3(0,0,100));

		TArray<FVec3> X;
		TArray<FVec3> V;

		const int32 NumSteps = 20;
		for(int Step = 0; Step < NumSteps; ++Step)
		{
			//teleport from GT
			if(Step == 15)
			{
				Particle->SetX(FVec3(0,0,10));
				Particle->SetV(FVec3(0,0,1));
			}

			X.Add(Particle->X());
			V.Add(Particle->V());
			TickSolverHelper(Module,Solver);
		}
		X.Add(Particle->X());
		V.Add(Particle->V());

		FRewindData* RewindData = Solver->GetRewindData();
		const int32 LastValidStep = NumSteps - 1;
		const int32 FirstValid = NumSteps - RewindData->Capacity() + 1;	//we lose 1 step because we have to save head
		for(int Step = 0; Step < FirstValid; ++Step)
		{
			//can't go back that far
			EXPECT_FALSE(RewindData->RewindToFrame(Step));
		}

		for(int Step = FirstValid; Step <= LastValidStep; ++Step)
		{
			EXPECT_TRUE(RewindData->RewindToFrame(Step));
			EXPECT_EQ(Particle->X()[2],X[Step][2]);
			EXPECT_EQ(Particle->V()[2],V[Step][2]);
		}

		// Throw out the proxy
		Solver->UnregisterObject(Particle.Get());

		Module->DestroySolver(Solver);
	}

	GTEST_TEST(RewindTest,NumDirty)
	{
		auto Sphere = TSharedPtr<FImplicitObject,ESPMode::ThreadSafe>(new TSphere<float,3>(TVector<float,3>(0),10));

		FChaosSolversModule* Module = FChaosSolversModule::GetModule();
		Module->ChangeThreadingMode(EChaosThreadingMode::SingleThread);

		// Make a solver
		FPhysicsSolver* Solver = Module->CreateSolver(nullptr,ESolverFlags::Standalone);
		Solver->SetEnabled(true);

		Solver->EnableRewindCapture(5);


		// Make particles
		auto Particle = TPBDRigidParticle<float,3>::CreateParticle();

		Particle->SetGeometry(Sphere);
		Solver->RegisterObject(Particle.Get());
		Particle->SetGravityEnabled(true);
		
		for(int Step = 0; Step < 10; ++Step)
		{
			TickSolverHelper(Module,Solver);

			const FRewindData* RewindData = Solver->GetRewindData();
			EXPECT_EQ(RewindData->GetNumDirtyParticles(),1);
		}

		//stop movement
		Particle->SetGravityEnabled(false);
		Particle->SetV(FVec3(0));

		for(int Step = 0; Step < 10; ++Step)
		{
			TickSolverHelper(Module,Solver);
		}

		{
			//enough frames with no changes so no longer dirty
			const FRewindData* RewindData = Solver->GetRewindData();
			EXPECT_EQ(RewindData->GetNumDirtyParticles(),0);
		}

		{
			//single change so back to being dirty
			Particle->SetGravityEnabled(true);
			TickSolverHelper(Module,Solver);

			const FRewindData* RewindData = Solver->GetRewindData();
			EXPECT_EQ(RewindData->GetNumDirtyParticles(),1);
		}

		// Throw out the proxy
		Solver->UnregisterObject(Particle.Get());

		Module->DestroySolver(Solver);
	}

	GTEST_TEST(RewindTest,Resim)
	{
		auto Sphere = TSharedPtr<FImplicitObject,ESPMode::ThreadSafe>(new TSphere<float,3>(TVector<float,3>(0),10));

		FChaosSolversModule* Module = FChaosSolversModule::GetModule();
		Module->ChangeThreadingMode(EChaosThreadingMode::SingleThread);

		// Make a solver
		FPhysicsSolver* Solver = Module->CreateSolver(nullptr,ESolverFlags::Standalone);
		Solver->SetEnabled(true);

		Solver->EnableRewindCapture(5);

		// Make particles
		auto Particle = TPBDRigidParticle<float,3>::CreateParticle();

		Particle->SetGeometry(Sphere);
		Solver->RegisterObject(Particle.Get());
		Particle->SetGravityEnabled(true);

		auto Kinematic = TKinematicGeometryParticle<float,3>::CreateParticle();

		Kinematic->SetGeometry(Sphere);
		Solver->RegisterObject(Kinematic.Get());
		Kinematic->SetX(FVec3(2,2,2));

		TArray<FVec3> X;

		for(int Step = 0; Step < 10; ++Step)
		{
			X.Add(Particle->X());

			if(Step == 9)
			{
				Kinematic->SetX(FVec3(50,50,50));
			}

			TickSolverHelper(Module,Solver);
		}

		const int RewindStep = 7;

		FRewindData* RewindData = Solver->GetRewindData();
		EXPECT_TRUE(RewindData->RewindToFrame(RewindStep));

		//Move particle and rerun
		Particle->SetX(FVec3(0,0,100));
		for(int Step = RewindStep; Step < 10; ++Step)
		{
			X[Step] = Particle->X();
			TickSolverHelper(Module,Solver);
		}

		EXPECT_EQ(Kinematic->X()[2],2);	//Rewound kinematic and never updated it so back to original value

		//Make sure we recorded the new data
		for(int Step = RewindStep; Step < 10; ++Step)
		{
			const FGeometryParticleState State = RewindData->GetStateAtFrame(*Particle,Step);
			EXPECT_EQ(State.X()[2],X[Step][2]);

			const FGeometryParticleState KinState = RewindData->GetStateAtFrame(*Kinematic,Step);
			//TODO: this is a form of desync, need support for that still
			//EXPECT_EQ(KinState.X()[2],2);	//even though not dirty, data is properly stored as not moving
		}



		// Throw out the proxy
		Solver->UnregisterObject(Particle.Get());

		Module->DestroySolver(Solver);
	}
}
