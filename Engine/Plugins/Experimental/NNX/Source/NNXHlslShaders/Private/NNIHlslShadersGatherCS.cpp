// Copyright Epic Games, Inc. All Rights Reserved.

#include "NNIHlslShadersGatherCS.h"

namespace UE::NNIHlslShaders::Internal
{
	// template <>
	void TGatherCS::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& InParameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(InParameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DATA_TYPE"), 0);
		OutEnvironment.SetDefine(TEXT("INDICES_TYPE"), 0);
		OutEnvironment.SetDefine(TEXT("MAX_NUM_DIMENSIONS"), FGatherConstants::MAX_NUM_DIMENSIONS);
		OutEnvironment.SetDefine(TEXT("NUM_GROUP_THREADS"), FGatherConstants::NUM_GROUP_THREADS);
	}

	//template <typename DataElementType, typename IndicesElementType>
	void TGatherCS::FillInParameters(int32 Axis, const NNX::FMLTensorDesc& Data, const NNX::FMLTensorDesc& Indices, FParameters& Parameters)
	{
		Parameters.Axis = Axis;

		TArray<int32> OutputShape;
		OutputShape.SetNum(Indices.Shape.Num() + (Data.Shape.Num() - 1));
		int32 index = 0;
		Parameters.OutputSize = 1;
		for (int32 i = 0; i < Axis; i++, index++)
		{
			OutputShape[index] = Data.Shape[i];
			Parameters.OutputSize *= OutputShape[index];
		}
		for (int32 i = 0; i < Indices.Shape.Num(); i++, index++)
		{
			OutputShape[index] = Indices.Shape[i];
			Parameters.OutputSize *= OutputShape[index];
		}
		for (int32 i = (Axis + 1); i < Data.Shape.Num(); i++, index++)
		{
			OutputShape[index] = Data.Shape[i];
			Parameters.OutputSize *= OutputShape[index];
		}

		Parameters.NumDataDimensions = Data.Shape.Num();
		Parameters.NumIndicesDimensions = Indices.Shape.Num();

		int32 DataStride = 1;
		for (int32 i = Data.Shape.Num() - 1; i >= 0; i--)
		{
			Parameters.DataStride_IndicesStride_OutputStride[i].X = DataStride;
			Parameters.OneDivDataStride_OneDivIndicesStride_OneDivOutputStride[i].X = 1.0 / (float)DataStride;
			DataStride *= Data.Shape[i];
		}

		int32 IndicesStride = 1;
		for (int32 i = Indices.Shape.Num() - 1; i >= 0; i--)
		{
			Parameters.DataStride_IndicesStride_OutputStride[i].Y = IndicesStride;
			Parameters.OneDivDataStride_OneDivIndicesStride_OneDivOutputStride[i].Y = 1.0 / (float)IndicesStride;
			IndicesStride *= Indices.Shape[i];
		}

		int32 OutputStride = 1;
		for (int32 i = OutputShape.Num() - 1; i >= 0; i--)
		{
			Parameters.DataStride_IndicesStride_OutputStride[i].Z = OutputStride;
			Parameters.OneDivDataStride_OneDivIndicesStride_OneDivOutputStride[i].Z = 1.0 / (float)OutputStride;
			OutputStride *= OutputShape[i];
		}
	}

	// template <typename DataElementType, typename IndicesElementType>
	FIntVector TGatherCS::GetGroupCount(const FParameters& Parameters)
	{
		return FIntVector(FMath::DivideAndRoundUp(Parameters.OutputSize, FGatherConstants::NUM_GROUP_THREADS), 1, 1);
	}

	//typedef TGatherCS<float, int32> FGatherFloatInt32CS;
	//IMPLEMENT_SHADER_TYPE(template<>, TGatherCS, TEXT("/NNX/GatherOp.usf"), TEXT("main"), SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(TGatherCS, "/NNX/GatherOp.usf", "main", SF_Compute);
	//template class TGatherCS<float, int32>;

} // UE::NNIHlslShaders::Internal