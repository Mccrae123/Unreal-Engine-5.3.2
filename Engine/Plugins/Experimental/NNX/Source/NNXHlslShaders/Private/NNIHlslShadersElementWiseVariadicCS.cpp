// Copyright Epic Games, Inc. All Rights Reserved.

#include "NNIHlslShadersElementWiseVariadicCS.h"
#include "NNXCore.h"

namespace UE::NNIHlslShaders::Internal
{
	void TElementWiseVariadicCS::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& InParameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(InParameters, OutEnvironment);

		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), FElementWiseVariadicConstants::NUM_GROUP_THREADS);

		FPermutationDomain PermutationVector(InParameters.PermutationId);

		const FString OpFunc = GetOpFunc(PermutationVector.Get<FOperatorType>());

		OutEnvironment.SetDefine(TEXT("ELEMENTWISE_OP(X,Y)"), *OpFunc);
	}

	const FString TElementWiseVariadicCS::GetOpFunc(EMLElementWiseVariadicOperatorType OpType)
	{
		FString OpTable[(int32)EMLElementWiseVariadicOperatorType::MAX];

		for (int32 Idx = 0; Idx < (int32)EMLElementWiseVariadicOperatorType::MAX; ++Idx)
		{
			OpTable[Idx] = FString("");
		}

#define OP(OpName, OpFunc) OpTable[(int32) EMLElementWiseVariadicOperatorType::OpName] = OpFunc
		OP(Max, TEXT("max(X,Y)"));
		OP(Min, TEXT("min(X,Y)"));
		OP(Mean, TEXT("((X)+(Y))"));
		OP(Sum, TEXT("((X)+(Y))"));
#undef OP

		FString OpFunc = OpTable[(int32)OpType];

		if (OpFunc == "")
		{
			UE_LOG(LogNNX, Warning, TEXT("Undefined ElementWise Variadic operator name for operator:%d"), OpType);
		}

		return OpFunc;
	}

	IMPLEMENT_GLOBAL_SHADER(TElementWiseVariadicCS, "/NNX/ElementWiseVariadicOp.usf", "ElementWiseVariadicOp", SF_Compute);
} // UE::NNIHlslShaders::Internal