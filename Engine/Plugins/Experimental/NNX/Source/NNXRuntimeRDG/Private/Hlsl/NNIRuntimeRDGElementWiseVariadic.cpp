// Copyright Epic Games, Inc. All Rights Reserved.

#include "NNIRuntimeRDGElementWiseVariadic.h"
#include "NNEHlslShadersElementWiseVariadicCS.h"
#include "NNXRuntimeHLSLHelper.h"

namespace UE::NNIRuntimeRDG::Private::Hlsl
{
	DECLARE_GPU_STAT_NAMED(FNNIOperatorElementWiseVariadic, TEXT("NNI.Operator.Hlsl.ElementWise.Variadic"));

	using TElementWiseVariadicCS = typename UE::NNEHlslShaders::Internal::TElementWiseVariadicCS;
	using FElementWiseVariadicConstants = UE::NNEHlslShaders::Internal::FElementWiseVariadicConstants;

	void AddOneVariadicOpPass(FRDGBuilder& GraphBuilder, 
		TConstArrayView<NNX::FTensorRDG> InputTensors,
		const NNX::FTensorRDG& OutputTensor,
		bool OutputAsInput,
		EMLElementWiseVariadicOperatorType OpType,
		float Scale)
	{
		static_assert(FElementWiseVariadicConstants::MAX_NUM_INPUT == 4, "This algorithm need to be adapted to math the shader.");
		check(InputTensors.Num() <= FElementWiseVariadicConstants::MAX_NUM_INPUT);
		check(InputTensors.Num() > 0);

		// SRVs & UAV creations
		FRDGBufferUAVRef OutputUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutputTensor.GetBuffer(), PF_R32_FLOAT));
		FRDGBufferSRVRef InputsSRV[FElementWiseVariadicConstants::MAX_NUM_INPUT] = { nullptr, nullptr, nullptr, nullptr };

		for (int32 i = 0; i < InputTensors.Num(); ++i)
		{
			InputsSRV[i] = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(InputTensors[i].GetBuffer(), PF_R32_FLOAT));
		}

		// Set parameters
		FIntVector ThreadGroupCount = NNX::ComputeElementWiseThreadGroups(OutputTensor.GetVolume(), FElementWiseVariadicConstants::NUM_GROUP_THREADS);
		TElementWiseVariadicCS::FParameters* Params = GraphBuilder.AllocParameters<TElementWiseVariadicCS::FParameters>();

		Params->Input0 = InputsSRV[0];
		Params->Input1 = InputsSRV[1];
		Params->Input2 = InputsSRV[2];
		Params->Input3 = InputsSRV[3];
		Params->Output = OutputUAV;
		FillTensorStrideForBroadcastShaderParameters(InputTensors[0], OutputTensor.GetShape().Rank(), Params->InputTensorInfo, 0);
		if (InputTensors.Num() >= 2)
		{
			FillTensorStrideForBroadcastShaderParameters(InputTensors[1], OutputTensor.GetShape().Rank(), Params->InputTensorInfo, 1);
		}
		if (InputTensors.Num() >= 3)
		{
			FillTensorStrideForBroadcastShaderParameters(InputTensors[2], OutputTensor.GetShape().Rank(), Params->InputTensorInfo, 2);
		}
		if (InputTensors.Num() >= 4)
		{
			FillTensorStrideForBroadcastShaderParameters(InputTensors[3], OutputTensor.GetShape().Rank(), Params->InputTensorInfo, 3);
		}
		FillTensorStrideShaderParameters(OutputTensor, Params->OutputTensorInfo, 0);
		Params->Num = OutputTensor.GetVolume();
		Params->ThreadCountX = ThreadGroupCount.X * FElementWiseVariadicConstants::NUM_GROUP_THREADS;
		Params->Scale = Scale;

		// Shader variation
		TElementWiseVariadicCS::FPermutationDomain PermutationVector;

		PermutationVector.Set<TElementWiseVariadicCS::FOperatorType>(OpType);
		PermutationVector.Set<TElementWiseVariadicCS::FApplyScale>(Scale != 1.0f);
		PermutationVector.Set<TElementWiseVariadicCS::FOutputAsInput>(OutputAsInput);
		PermutationVector.Set<TElementWiseVariadicCS::FNumInput>(InputTensors.Num());
		PermutationVector.Set<TElementWiseVariadicCS::FVariadicNumDimensions>(OutputTensor.GetShape().Rank());

		// Add the pass to RDG
		TShaderMapRef<TElementWiseVariadicCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("NNI.Operator.Hlsl.ElementWise.Variadic.Dispatch"),
			ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
			ComputeShader,
			Params,
			ThreadGroupCount);
	}

	/**
	 * Variadic Element-wise ML operator implementation
	 */
	template<EMLElementWiseVariadicOperatorType OpType>
	class TElementWiseVariadic : public NNX::FMLOperatorHlsl
	{
	public:

		TElementWiseVariadic() {}
		virtual ~TElementWiseVariadic() = default;

	public:

		virtual int ComputeOutputShape(TConstArrayView<NNX::FTensorShape> InputShapes, TArray<NNX::FTensorShape>& OutputShapes) const override
		{
			OutputShapes.Empty();
			check(InputShapes.Num() > 0);

			const int32 NumInput = InputShapes.Num();
			int32 OutputRank = 0;
			for (int32 i = 0; i < NumInput; ++i)
			{
				OutputRank = FMath::Max(OutputRank, InputShapes[i].Rank());
			}

			NNX::FTensorShape OutputShape;

			OutputShape.Data.SetNumUninitialized(OutputRank);

			for (int32 i = 0; i < OutputRank; ++i)
			{
				int32 OutputValue = 1;
				for (int32 InputIdx = 0; InputIdx < NumInput; ++InputIdx)
				{
					int32 InputIndex = InputShapes[InputIdx].Rank() - 1 - i;
					int32 InputValue = InputIndex >= 0 ? InputShapes[InputIdx].Data[InputIndex] : 1;
					if (InputValue != OutputValue && InputValue != 1 && OutputValue != 1)
					{
						UE_LOG(LogNNX, Warning, TEXT("Error while computing shape for element wise variadic op, input shapes are not compatible"));
						return -1;
					}
					OutputValue = FMath::Max(InputValue, OutputValue);
				}
				OutputShape.Data[OutputRank - 1 - i] = OutputValue;
			}

			OutputShapes.Emplace(OutputShape);

			return 0;
		}
		
		virtual bool Initialize(TConstArrayView<NNX::FTensorDesc> InputTensorDescs, TConstArrayView<NNX::FTensorDesc> OutputTensorDescs, const UE::NNECore::FAttributeMap& Attributes) override
		{
			check(InputTensorDescs.Num() > 0);
			check(OutputTensorDescs.Num() == 1);
			return true;
		}

		virtual void Dispatch(FRDGBuilder& GraphBuilder, TConstArrayView<NNX::FTensorRDG> InInputTensors, TConstArrayView<NNX::FTensorRDG> InOutputTensors) override
		{
			check(InInputTensors.Num() > 0);
			check(InOutputTensors.Num() == 1);

			RDG_EVENT_SCOPE(GraphBuilder, "NNI.Operator.Hlsl.ElementWise.Variadic");
			RDG_GPU_STAT_SCOPE(GraphBuilder, FNNIOperatorElementWiseVariadic);

			NNX::FTensorRDG PassInputTensors[FElementWiseVariadicConstants::MAX_NUM_INPUT];
			for (int32 InputOffset = 0; InputOffset < InInputTensors.Num(); InputOffset += FElementWiseVariadicConstants::MAX_NUM_INPUT)
			{
				uint32 NumInputLeftToHandle = InInputTensors.Num() - InputOffset;
				uint32 NumInputForPass = FMath::Min(FElementWiseVariadicConstants::MAX_NUM_INPUT, NumInputLeftToHandle);
				bool bIsFirstPass = (InputOffset == 0);
				bool bIsLastPass = (NumInputLeftToHandle <= FElementWiseVariadicConstants::MAX_NUM_INPUT);
				float Scale = 1.0f;

				for (uint32 i = 0; i < NumInputForPass; ++i)
				{
					PassInputTensors[i] = InInputTensors[InputOffset + i];
				}

				if (OpType == EMLElementWiseVariadicOperatorType::Mean && bIsLastPass)
				{
					Scale = 1.0f / InInputTensors.Num();
				}
			
				AddOneVariadicOpPass(GraphBuilder,
					MakeArrayView(PassInputTensors, NumInputForPass),
					InOutputTensors[0],
					!bIsFirstPass,
					OpType, Scale);
			}
		}
	};

	bool ValidateElementWiseVariadicOperator(const UE::NNECore::FAttributeMap& AttributeMap, TConstArrayView<EMLTensorDataType> InputTypes, TConstArrayView<NNX::FSymbolicTensorShape> InputShapes)
	{
		bool bIsValid = true;

		NNX::FAttributeValidator AttributeValidator;
		bIsValid &= AttributeValidator.Validate(AttributeMap);

		if (InputTypes.Num() == 0)
		{
			UE_LOG(LogNNX, Error, TEXT("Element-wise variadic operator requires at least 1 input"));
			bIsValid = false;
		}
		for (int32 i = 0; i < InputTypes.Num(); ++i)
		{
			if (InputTypes[i] != EMLTensorDataType::Float)
			{
				UE_LOG(LogNNX, Warning, TEXT("Element-wise variadic operator input '%d' of type '%d' is not supported, should be float at the moment."), i, InputTypes[i]);
				bIsValid = false;
			}
		}
		
		NNX::FInputValidator InputValidator;
		InputValidator.AddSupportedType(EMLTensorDataType::Float);
		InputValidator.AddRequired();
		InputValidator.AddRequired();
		bIsValid &= InputValidator.Validate(InputTypes);

		return bIsValid;
	}

	template<EMLElementWiseVariadicOperatorType OpType>
	NNX::FMLOperatorHlsl* CreateElementWiseVariadicOperator()
	{
		return new TElementWiseVariadic<OpType>();
	}

	bool RegisterElementWiseVariadicOperators(NNX::FMLOperatorRegistryHlsl& Registry)
	{
#define OP(Name) Registry.OpAdd(TEXT(#Name), CreateElementWiseVariadicOperator<EMLElementWiseVariadicOperatorType::Name>)
		OP(Max);
		OP(Min);
		OP(Mean);
		OP(Sum);
#undef OP

		return true;
	}
} // UE::NNIRuntimeRDG::Private::Hlsl
