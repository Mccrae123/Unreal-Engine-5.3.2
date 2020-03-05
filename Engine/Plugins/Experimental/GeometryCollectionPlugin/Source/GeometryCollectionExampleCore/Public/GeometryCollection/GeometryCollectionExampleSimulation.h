// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "GeometryCollection/GeometryCollectionExample.h"
#include "GeometryCollection/GeometryCollectionExampleResponse.h"

namespace GeometryCollectionExample
{	
	
	template<class T>
	void RigidBodies_SingleFallingUnderGravity();

	template<class T>
	void RigidBodiesCollidingWithSolverFloor();

	template<class T>
	void RigidBodiesSingleSphereCollidingWithSolverFloor();

	template<class T>
	void RigidBodiesSingleSphereIntersectingWithSolverFloor();

	template<class T>
	void RigidBodiesKinematic();

	template<class T>
	void RigidBodiesSleepingActivation();

	template<class T>
	void RigidBodies_CollisionGroup();

	template<class T>
	void RigidBodies_Initialize_ParticleImplicitCollisionGeometry();

	
}
