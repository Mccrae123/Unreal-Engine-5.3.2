// Copyright Epic Games, Inc. All Rights Reserved.

/**
 * Contains Transform Solver Execution
 *
 */

#pragma once

#include "IKRigSolver.h"
#include "FBIKShared.h"
#include "FullBodyIKSolver.generated.h"

struct FFBIKLinkData;

 // run time for UFullBodyIKSolverDefinition
UCLASS()
class FULLBODYIK_API UFullBodyIKSolver : public UIKRigSolver
{
	GENERATED_BODY()

	/** list of Link Data for solvers - joints */
	TArray<FFBIKLinkData> LinkData;
	/** Effector Targets - search key is LinkData Index */
	TMap<int32, FFBIKEffectorTarget> EffectorTargets; 
	/** End Effector Link Indices - EndEffector index to LinkData index*/
	TArray<int32> EffectorLinkIndices;
	/** Map from LinkData index to Hierarchy Index*/
	TMap<int32, int32> LinkDataToHierarchyIndices;
	/** Map from Rig Hierarchy Index to LinkData index*/
	TMap<int32, int32> HierarchyToLinkDataMap;
	/** Constraints data */
	TArray<ConstraintType> InternalConstraints;
	/* Current Solver */
	FJacobianSolver_FullbodyIK IKSolver;
	/** Debug Data */
	TArray<FJacobianDebugData> DebugData;

protected:
	virtual void InitInternal(const FIKRigTransformModifier& InGlobalTransform) override;
	virtual void SolveInternal(FIKRigTransformModifier& InOutGlobalTransform, FControlRigDrawInterface* InOutDrawInterface) override;
	virtual bool IsSolverActive() const override;
};



