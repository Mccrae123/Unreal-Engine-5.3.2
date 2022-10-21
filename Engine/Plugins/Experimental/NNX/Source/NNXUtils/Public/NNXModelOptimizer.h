// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "NNXRuntimeFormat.h"
#include "NNXModelOptimizerInterface.h"

namespace NNX
{
class IModelOptimizer;
	
/** Create a model optimizer */
NNXUTILS_API TUniquePtr<IModelOptimizer> CreateModelOptimizer(ENNXInferenceFormat InputFormat, ENNXInferenceFormat OutputFormat);

inline TUniquePtr<IModelOptimizer> CreateONNXToNNXModelOptimizer()
{
	return CreateModelOptimizer(ENNXInferenceFormat::ONNX, ENNXInferenceFormat::NNXRT);
}

inline TUniquePtr<IModelOptimizer> CreateONNXToORTModelOptimizer()
{
	return CreateModelOptimizer(ENNXInferenceFormat::ONNX, ENNXInferenceFormat::ORT);
}

inline TUniquePtr<IModelOptimizer> CreateONNXToONNXModelOptimizer()
{
	return CreateModelOptimizer(ENNXInferenceFormat::ONNX, ENNXInferenceFormat::ONNX);
}

/** Helper to create an optimized model for a given runtime from ONNX */
NNXUTILS_API bool CreateRuntimeModelFromONNX(FNNIModelRaw& OutputModel, const FNNIModelRaw& ONNXModel, FString RuntimeName, const FOptimizerOptionsMap& Options);

} // NNX
