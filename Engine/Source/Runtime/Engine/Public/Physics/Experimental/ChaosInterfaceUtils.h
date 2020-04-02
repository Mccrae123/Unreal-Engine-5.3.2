// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Chaos/Declares.h"
#include "Containers/ContainersFwd.h"
#include "Containers/ContainerAllocationPolicies.h"
#include "Templates/UniquePtr.h"

struct FGeometryAddParams;

namespace ChaosInterface
{
	Chaos::EChaosCollisionTraceFlag ConvertCollisionTraceFlag(ECollisionTraceFlag Flag);
	
	/**
	 * Create the Chaos Geometry based on the geometry parameters.
	 */
	void CreateGeometry(const FGeometryAddParams& InParams, TArray<TUniquePtr<Chaos::FImplicitObject>>& OutGeoms, TArray<TUniquePtr<Chaos::FPerShapeData>, TInlineAllocator<1>>& OutShapes);
}
