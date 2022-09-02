﻿// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Misc/FrameNumber.h"
#include "Templates/SharedPointer.h"

class UTransformableHandle;
class UTickableTransformConstraint;
class ISequencer;
class UWorld;
struct FFrameNumber;
enum class EMovieSceneTransformChannel : uint32;

struct FConstraintBaker
{
public:
	/** Bake constraint over specified frames, frames must be in order*/
	static void Bake(UWorld* InWorld, UTickableTransformConstraint* InConstraint, const TSharedPtr<ISequencer>& InSequencer, const TArray<FFrameNumber>& InFrames);

	/** Stores InHandle local (or global) transforms at InFrames. */
	static void GetHandleTransforms(
		UWorld* InWorld,
		const TSharedPtr<ISequencer>& InSequencer,
		const UTransformableHandle* InHandle,
		const TArray<FFrameNumber>& InFrames,
		const bool bLocal,
		TArray<FTransform>& OutTransforms);
	
	/** Add InTransforms keys at InFrames into the InHandle transform animation channels. */
	static void AddTransformKeys(
		const TSharedPtr<ISequencer>& InSequencer,
		UTransformableHandle* InHandle,
		const TArray<FFrameNumber>& InFrames,
		const TArray<FTransform>& InTransforms,
		const EMovieSceneTransformChannel& InChannels);
	
private:

	/** Evaluates the constraint at each frames and stores the resulting child transforms. */
	static void GetHandleTransforms(
		const TSharedPtr<ISequencer>& InSequencer,
		const UTransformableHandle* InHandle,
		const TArray<UTickableTransformConstraint*>& InConstraintsToEvaluate,
		const TArray<FFrameNumber>& InFrames,
		const bool bLocal,
		TArray<FTransform>& OutTransforms);
};

