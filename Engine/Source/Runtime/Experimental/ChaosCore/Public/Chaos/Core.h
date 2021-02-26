// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Chaos/Vector.h"
#include "Chaos/Matrix.h"
#include "Chaos/Rotation.h"
#include "Chaos/Transform.h"
#include "Containers/UnrealString.h"

namespace Chaos
{
	template<class T, int d> class TAABB;

	/**
	 * Common data types for the Chaos physics engine. Unless a specific
	 * precision of type is required most code should use these existing types
	 * (e.g. FVec3) to adapt to global changes in precision.
	 */
	using FReal = float;

	using FVec2 = TVector<FReal, 2>;
	using FVec3 = TVector<FReal, 3>;
	using FMatrix33 = PMatrix<FReal, 3, 3>;
	using FRigidTransform3 = TRigidTransform<FReal, 3>;
	using FRotation3 = TRotation<FReal, 3>;
	using FAABB3 = TAABB<FReal, 3>;

	template <typename T>
	using TVec3 = TVector<T, 3>;

	template <typename T>
	using TVec4 = TVector<T, 4>;

	// NOTE: if you get a merge conflict on the GUID, you must replace it with a new GUID - do not accept the source or target
	// or you will likely get DDC version conflicts resulting in crashes during load.
	// Core version string for Chaos data. Any DDC builder dependent on Chaos for serialization should depend on this version
	static const FString ChaosVersionString = TEXT("69257516-ABA2-4BD7-BE88-47B6C401D231");
}
