// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	PathTracingUniformBuffers.h
=============================================================================*/

#pragma once

#include "UniformBuffer.h"
#include "RayTracingDefinitions.h"

BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FPathTracingData, )
	SHADER_PARAMETER(uint32, Iteration)
	SHADER_PARAMETER(uint32, TemporalSeed)
	SHADER_PARAMETER(uint32, MaxSamples)
	SHADER_PARAMETER(uint32, UseErrorDiffusion)
	SHADER_PARAMETER(uint32, MaxBounces)
	SHADER_PARAMETER(uint32, MaxSSSBounces)
	SHADER_PARAMETER(uint32, MISMode)
	SHADER_PARAMETER(uint32, ApproximateCaustics)
	SHADER_PARAMETER(uint32, EnableCameraBackfaceCulling)
	SHADER_PARAMETER(uint32, SkipDirectLighting)
	SHADER_PARAMETER(uint32, SkipEmissive)
	SHADER_PARAMETER(uint32, CoherentSampling)
	SHADER_PARAMETER(float, MaxPathIntensity)
	SHADER_PARAMETER(float, MaxNormalBias)
	SHADER_PARAMETER(float, FilterWidth)
END_GLOBAL_SHADER_PARAMETER_STRUCT()
