// Copyright Epic Games, Inc. All Rights Reserved.

#include "NNIRuntimeRDGElementWiseUnary.h"
#include "NNEHlslShadersElementWiseUnaryCS.h"
#include "NNXRuntimeHLSLHelper.h"
#include "NNECoreAttributeMap.h"

namespace UE::NNIRuntimeRDG::Private::Hlsl
{
	DECLARE_GPU_STAT_NAMED(FNNIOperatorElementWiseUnary, TEXT("NNI.Operator.Hlsl.ElementWise.Unary"));

	using TElementWiseUnaryCS = typename UE::NNEHlslShaders::Internal::TElementWiseUnaryCS;
	using FElementWiseUnaryConstants = UE::NNEHlslShaders::Internal::FElementWiseUnaryConstants;

	/**
	 * Unary element-wise operator implementation
	 */
	template<EMLElementWiseUnaryOperatorType OpType>
	class TElementWiseUnary : public NNX::FMLOperatorHlsl
	{
	public:

		TElementWiseUnary() {}
		virtual ~TElementWiseUnary() = default;

	private:

		float Alpha = 0.0f;
		float Beta = 0.0f;
		float Gamma = 0.0f;

	public:

		virtual int ComputeOutputShape(TConstArrayView<NNX::FTensorShape> InputShapes, TArray<NNX::FTensorShape>& OutputShapes) const override
		{
			check(InputShapes.Num() == 1);
			OutputShapes = InputShapes;
			return 0;
		}

		virtual bool Initialize(TConstArrayView<NNX::FTensorDesc> InputTensorDescs, TConstArrayView<NNX::FTensorDesc> OutputTensorDescs, const UE::NNECore::FAttributeMap& Attributes) override
		{
			check(InputTensorDescs.Num() == 1);
			check(OutputTensorDescs.Num() == 1);

			Alpha = Attributes.GetValueOrDefault(TEXT("alpha"), Alpha);
			Beta = Attributes.GetValueOrDefault(TEXT("beta"), Beta);
			Gamma = Attributes.GetValueOrDefault(TEXT("gamma"), Gamma);
			return true;
		}

		virtual void Dispatch(FRDGBuilder& GraphBuilder, TConstArrayView<NNX::FTensorRDG> InInputTensors, TConstArrayView<NNX::FTensorRDG> InOutputTensors) override
		{
			FRDGBufferSRVRef InputSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(InInputTensors[0].GetBuffer(), PF_R32_FLOAT));
			FRDGBufferUAVRef OutputUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(InOutputTensors[0].GetBuffer(), PF_R32_FLOAT));
		
			int32 NumElements = InOutputTensors[0].GetVolume();
			FIntVector ThreadGroupCount = NNX::ComputeElementWiseThreadGroups(NumElements, FElementWiseUnaryConstants::NUM_GROUP_THREADS);

			// Set parameters
			TElementWiseUnaryCS::FParameters* Params = GraphBuilder.AllocParameters<TElementWiseUnaryCS::FParameters>();
			Params->Input = InputSRV;
			Params->Output = OutputUAV;
			Params->Alpha = Alpha;
			Params->Beta = Beta;
			Params->Gamma = Gamma;
			Params->Num = NumElements;
			Params->ThreadCountX = ThreadGroupCount.X * FElementWiseUnaryConstants::NUM_GROUP_THREADS;

			TElementWiseUnaryCS::FPermutationDomain PermutationVector;

			PermutationVector.Set<TElementWiseUnaryCS::FOperatorType>(OpType);

			TShaderMapRef<TElementWiseUnaryCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);

			RDG_EVENT_SCOPE(GraphBuilder, "NNI.Operator.Hlsl.ElementWise.Unary");
			RDG_GPU_STAT_SCOPE(GraphBuilder, FNNIOperatorElementWiseUnary);
		
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("NNI.Operator.Hlsl.ElementWise.Unary.Dispatch"),
				ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
				ComputeShader,
				Params,
				ThreadGroupCount);
		}
	};

	template<> TElementWiseUnary<EMLElementWiseUnaryOperatorType::Selu>::TElementWiseUnary()
		: Alpha(1.67326319217681884765625f), Beta(0.0f), Gamma(1.05070102214813232421875f)
	{
	}

	template<> TElementWiseUnary<EMLElementWiseUnaryOperatorType::Elu>::TElementWiseUnary()
		: Alpha(1.0f), Beta(0.0f), Gamma(0.0f) 
	{
	}

	template<> TElementWiseUnary<EMLElementWiseUnaryOperatorType::HardSigmoid>::TElementWiseUnary()
		: Alpha(0.2f), Beta(0.5f), Gamma(0.0f)
	{
	}

	template<> TElementWiseUnary<EMLElementWiseUnaryOperatorType::LeakyRelu>::TElementWiseUnary()
		: Alpha(0.01f), Beta(0.0f), Gamma(0.0f)
	{
	}

	template<EMLElementWiseUnaryOperatorType OpType>
	NNX::FMLOperatorHlsl* CreateElementWiseUnaryOperator()
	{
		return new TElementWiseUnary<OpType>();
	}

	template<EMLElementWiseUnaryOperatorType OpType>
	bool ValidateElementWiseUnaryOperator(const UE::NNECore::FAttributeMap& AttributeMap, TConstArrayView<EMLTensorDataType> InputTypes, TConstArrayView<NNX::FSymbolicTensorShape> InputShapes)
	{
		bool bIsValid = true;

		NNX::FAttributeValidator AttributeValidator;
		bIsValid &= AttributeValidator.Validate(AttributeMap);

		NNX::FInputValidator InputValidator;
		InputValidator.AddSupportedType(EMLTensorDataType::Float);
		InputValidator.AddRequired();
		bIsValid &= InputValidator.Validate(InputTypes);

		return bIsValid;
	}

	template<>
	bool ValidateElementWiseUnaryOperator<EMLElementWiseUnaryOperatorType::Selu>(const UE::NNECore::FAttributeMap& AttributeMap, TConstArrayView<EMLTensorDataType> InputTypes, TConstArrayView<NNX::FSymbolicTensorShape> InputShapes)
	{
		bool bIsValid = true;

		NNX::FAttributeValidator AttributeValidator;
		AttributeValidator.AddOptional(TEXT("alpha"), ENNEAttributeDataType::Float);
		AttributeValidator.AddOptional(TEXT("gamma"), ENNEAttributeDataType::Float);
		bIsValid &= AttributeValidator.Validate(AttributeMap);

		NNX::FInputValidator InputValidator;
		InputValidator.AddSupportedType(EMLTensorDataType::Float);
		InputValidator.AddRequired();
		bIsValid &= InputValidator.Validate(InputTypes);

		return bIsValid;
	}

	template<>
	bool ValidateElementWiseUnaryOperator<EMLElementWiseUnaryOperatorType::Elu>(const UE::NNECore::FAttributeMap& AttributeMap, TConstArrayView<EMLTensorDataType> InputTypes, TConstArrayView<NNX::FSymbolicTensorShape> InputShapes)
	{
		bool bIsValid = true;

		NNX::FAttributeValidator AttributeValidator;
		AttributeValidator.AddOptional(TEXT("alpha"), ENNEAttributeDataType::Float);
		bIsValid &= AttributeValidator.Validate(AttributeMap);

		NNX::FInputValidator InputValidator;
		InputValidator.AddSupportedType(EMLTensorDataType::Float);
		InputValidator.AddRequired();
		bIsValid &= InputValidator.Validate(InputTypes);

		return bIsValid;
	}

	template<>
	bool ValidateElementWiseUnaryOperator<EMLElementWiseUnaryOperatorType::HardSigmoid>(const UE::NNECore::FAttributeMap& AttributeMap, TConstArrayView<EMLTensorDataType> InputTypes, TConstArrayView<NNX::FSymbolicTensorShape> InputShapes)
	{
		bool bIsValid = true;

		NNX::FAttributeValidator AttributeValidator;
		AttributeValidator.AddOptional(TEXT("alpha"), ENNEAttributeDataType::Float);
		AttributeValidator.AddOptional(TEXT("beta"), ENNEAttributeDataType::Float);
		bIsValid &= AttributeValidator.Validate(AttributeMap);

		NNX::FInputValidator InputValidator;
		InputValidator.AddSupportedType(EMLTensorDataType::Float);
		InputValidator.AddRequired();
		bIsValid &= InputValidator.Validate(InputTypes);

		return bIsValid;
	}

	template<>
	bool ValidateElementWiseUnaryOperator<EMLElementWiseUnaryOperatorType::LeakyRelu>(const UE::NNECore::FAttributeMap& AttributeMap, TConstArrayView<EMLTensorDataType> InputTypes, TConstArrayView<NNX::FSymbolicTensorShape> InputShapes)
	{
		bool bIsValid = true;

		NNX::FAttributeValidator AttributeValidator;
		AttributeValidator.AddOptional(TEXT("alpha"), ENNEAttributeDataType::Float);
		bIsValid &= AttributeValidator.Validate(AttributeMap);

		NNX::FInputValidator InputValidator;
		InputValidator.AddSupportedType(EMLTensorDataType::Float);
		InputValidator.AddRequired();
		bIsValid &= InputValidator.Validate(InputTypes);

		return bIsValid;
	}
	
	bool RegisterElementWiseUnaryOperators(NNX::FMLOperatorRegistryHlsl& Registry)
	{
#define OP(Name) Registry.OpAdd(TEXT(#Name), CreateElementWiseUnaryOperator<EMLElementWiseUnaryOperatorType::Name>, ValidateElementWiseUnaryOperator<EMLElementWiseUnaryOperatorType::Name>)
		OP(Abs);
		OP(Acos);
		OP(Acosh);
		OP(Asin);
		OP(Asinh);
		OP(Atan);
		OP(Atanh);
		//OP(BitShift);
		//OP(Cast);
		OP(Ceil);
		//OP(Clip);
		OP(Cos);
		OP(Cosh);
		OP(Elu);
		OP(Erf);
		OP(Exp);
		OP(Floor);
		OP(IsInf);
		OP(IsNan);
		OP(HardSigmoid);
		OP(HardSwish);
		OP(LeakyRelu);
		OP(Log);
		OP(Neg);
		//OP(Not);
		OP(Reciprocal);
		OP(Relu);
		OP(Round);
		OP(Selu);
		OP(Sigmoid);
		OP(Sign);
		OP(Sin);
		OP(Sinh);
		OP(Softplus);
		OP(Softsign);
		OP(Sqrt);
		OP(Tan);
		OP(Tanh);
#undef OP

		return true;
	}
} // UE::NNIRuntimeRDG::Private::Hlsl
