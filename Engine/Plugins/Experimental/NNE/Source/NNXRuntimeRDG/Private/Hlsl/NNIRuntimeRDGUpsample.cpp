// Copyright Epic Games, Inc. All Rights Reserved.

#include "NNIRuntimeRDGUpsample.h"
#include "NNEHlslShadersUpsampleCS.h"
#include "NNXRuntimeHLSLHelper.h"

namespace UE::NNIRuntimeRDG::Private::Hlsl
{
	DECLARE_GPU_STAT_NAMED(FNNIOperatorUpsample, TEXT("NNI.Operator.Hlsl.Upsample"));

	/**
	 * Upsample operator implementation
	 */
	class FUpsample : public NNX::FMLOperatorHlsl
	{
	public:

		FUpsample() {}
		virtual ~FUpsample() = default;

	public:

		virtual int PrepareOutputs(TConstArrayView<NNX::FTensorRef> InputTensors, TArrayView<NNX::FTensorRef> OutputTensors) const override
		{
			check(InputTensors.Num() == 2);
			check(OutputTensors.Num() == 1);

			const NNX::FTensor& X = *InputTensors[0];
			const NNX::FTensor& Scales = *InputTensors[1];
			
			if (!Scales.HasPreparedData())
			{
				UE_LOG(LogNNX, Warning, TEXT("Upsample input 'Scale' (name: %s) should be constant for shape inference to succeed, however it is not."), *Scales.GetName());
				return -1;
			}

			TConstArrayView<float> ScalesData = Scales.GetPreparedData<float>();

			if (ScalesData.Num() != X.GetShape().Rank())
			{
				UE_LOG(LogNNX, Warning, TEXT("Upsample input 'Scale' (name: %s) have %d elements. While it should be the same as the rank of input 'X' (name : %s) witch is %d"), *Scales.GetName(), ScalesData.Num(), *X.GetName(), X.GetShape().Rank());
				return -1;
			}

			NNX::FTensorShape OutputShape;
			for (int32 i = 0; i < X.GetShape().Rank(); ++i)
			{
				
				OutputShape.Data.Emplace(FMath::FloorToInt32(X.GetShape().Data[i] * ScalesData[i]));
			}

			OutputTensors[0]->SetShape(OutputShape);

			return 0;
		};

		virtual bool Initialize(TConstArrayView<NNX::FTensorDesc> InputTensorDescs, TConstArrayView<NNX::FTensorDesc> OutputTensorDescs, const UE::NNECore::FAttributeMap& Attributes) override
		{
			check(InputTensorDescs.Num() == 2);
			check(OutputTensorDescs.Num() == 1);

			return true;
		}

		virtual void Dispatch(FRDGBuilder& GraphBuilder, TConstArrayView<NNX::FTensorRDGRef> InputTensors, TConstArrayView<NNX::FTensorRDGRef> OutputTensors) override
		{
			using namespace UE::NNEHlslShaders::Internal;
			
			check(InputTensors.Num() == 2);
			check(OutputTensors.Num() == 1);
			check(InputTensors[0] != nullptr);
			check(InputTensors[1] != nullptr);
			check(OutputTensors[0] != nullptr);
			const NNX::FTensorRDG& Input = *InputTensors[0];
			const NNX::FTensorRDG& Scales = *InputTensors[1];
			const NNX::FTensorRDG& Output = *OutputTensors[0];

			check(Scales.HasPreparedData());
			TConstArrayView<float> ScalesData = Scales.GetPreparedData<float>();

			FRDGBufferSRVRef InputSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Input.GetBuffer(), PF_R32_FLOAT));
			FRDGBufferUAVRef OutputUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.GetBuffer(), PF_R32_FLOAT));

			FIntVector ThreadGroupCount = NNX::ComputeElementWiseThreadGroups(Output.GetVolume(), FUpsampleConstants::NUM_GROUP_THREADS);

			// Set parameters
			FUpsampleCS::FParameters* Params = GraphBuilder.AllocParameters<FUpsampleCS::FParameters>();
			Params->Input = InputSRV;
			Params->Output = OutputUAV;
			FillTensorStrideShaderParameters(Input, Params->TensorInfo, 0);
			FillTensorStrideShaderParameters(Output, Params->TensorInfo, 1);
			FillTensorSizeShaderParameters(Input, Params->TensorInfo, 2);
			FillTensorSizeShaderParameters(Output, Params->TensorInfo, 3);
			Params->Num = Output.GetVolume();
			Params->ThreadCountX = ThreadGroupCount.X * FUpsampleConstants::NUM_GROUP_THREADS;

			FUpsampleCS::FPermutationDomain PermutationVector;

			PermutationVector.Set<FUpsampleCS::FUpsampleNumDimensions>(Output.GetShape().Rank());

			TShaderMapRef<FUpsampleCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);

			RDG_EVENT_SCOPE(GraphBuilder, "NNI.Operator.Hlsl.Upsample");
			RDG_GPU_STAT_SCOPE(GraphBuilder, FNNIOperatorUpsample);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("NNI.Operator.Hlsl.Upsample.Dispatch"),
				ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
				ComputeShader,
				Params,
				ThreadGroupCount);
		}
	};

	bool ValidateUpsampleOperator(const UE::NNECore::FAttributeMap& AttributeMap, TConstArrayView<EMLTensorDataType> InputTypes, TConstArrayView<NNX::FSymbolicTensorShape> InputShapes)
	{
		bool bIsValid = true;

		NNX::FAttributeValidator AttributeValidator;
		AttributeValidator.AddOptional(TEXT("mode"), ENNEAttributeDataType::String);
		bIsValid &= AttributeValidator.Validate(AttributeMap);
		
		FString Mode = AttributeMap.GetValueOrDefault<FString>(TEXT("mode"), TEXT("nearest"));
		if (!Mode.Equals(TEXT("nearest")))
		{
			UE_LOG(LogNNX, Warning, TEXT("Upsample HLSL operator only supports nearest mode for now"));
			return false;
		}

		NNX::FInputValidator InputValidator;
		InputValidator.AddSupportedType(EMLTensorDataType::Float);
		InputValidator.AddRequired();
		InputValidator.AddRequired();
		bIsValid &= InputValidator.Validate(InputTypes);

		return bIsValid;
	}

	NNX::FMLOperatorHlsl* CreateUpsampleOperator()
	{
		return new FUpsample();
	}

	bool RegisterUpsampleOperator(NNX::FMLOperatorRegistryHlsl& Registry)
	{
		Registry.OpAdd(TEXT("Upsample"), CreateUpsampleOperator, ValidateUpsampleOperator);
		return true;
	}
} // UE::NNIRuntimeRDG::Private::Hlsl
