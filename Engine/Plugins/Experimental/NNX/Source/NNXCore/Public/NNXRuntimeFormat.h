// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UObject/Object.h"
#include "NNXTypes.h"
#include "NNXRuntime.h"
#include "NNXRuntimeFormat.generated.h"

UENUM()
enum class EMLFormatTensorType : uint8
{
	None,	
	Input,
	Output,
	Intermediate
};

// Required by LoadModel() when loading operators in HLSL and DirectML runtime
USTRUCT()
struct FMLFormatAttributeDesc
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	FString Name;

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	FMLAttributeValue Value;
};

USTRUCT()
struct FMLFormatOperatorDesc
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	FString TypeName;			//!< For example "Relu"

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	TArray<uint32> InTensors;

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	TArray<uint32> OutTensors;

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	TArray<FMLFormatAttributeDesc> Attributes;

	//UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	//FString InstanceName;		//!< For example "Relu1"

	/*
	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	uint32 InTensorStart;

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	uint32 InTensorCount;

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	uint32 OutTensorStart;

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	uint32 OutTensorCount;
	*/
};

USTRUCT()
struct FMLFormatTensorDesc
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	FString Name;

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	TArray<uint32> Shape;

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	EMLFormatTensorType	Type;

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	EMLTensorDataType	DataType;

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	uint64	DataSize;

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	uint64	DataOffset;
};

/// NNX Runtime format
USTRUCT()
struct FMLRuntimeFormat
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	TArray<FMLFormatTensorDesc> Tensors;

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	TArray<FMLFormatOperatorDesc> Operators;

	UPROPERTY(VisibleAnywhere, Category = "Neural Network Inference")
	TArray<uint8> TensorData;
};