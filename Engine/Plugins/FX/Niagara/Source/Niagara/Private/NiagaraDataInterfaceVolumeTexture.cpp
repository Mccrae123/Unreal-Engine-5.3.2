// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraDataInterfaceVolumeTexture.h"
#include "NiagaraShader.h"
#include "ShaderParameterUtils.h"
#include "NiagaraCustomVersion.h"
#include "Engine/VolumeTexture.h"


#define LOCTEXT_NAMESPACE "UNiagaraDataInterfaceVolumeTexture"

const FName UNiagaraDataInterfaceVolumeTexture::SampleVolumeTextureName(TEXT("SampleVolumeTexture"));
const FName UNiagaraDataInterfaceVolumeTexture::TextureDimsName(TEXT("TextureDimensions3D"));
const FString UNiagaraDataInterfaceVolumeTexture::TextureName(TEXT("Texture_"));
const FString UNiagaraDataInterfaceVolumeTexture::SamplerName(TEXT("Sampler_"));
const FString UNiagaraDataInterfaceVolumeTexture::DimensionsBaseName(TEXT("Dimensions_"));

UNiagaraDataInterfaceVolumeTexture::UNiagaraDataInterfaceVolumeTexture(FObjectInitializer const& ObjectInitializer)
	: Super(ObjectInitializer)
	, Texture(nullptr)
{
	Proxy.Reset(new FNiagaraDataInterfaceProxyVolumeTexture());
	PushToRenderThread();
}

void UNiagaraDataInterfaceVolumeTexture::PostInitProperties()
{
	Super::PostInitProperties();

	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		FNiagaraTypeRegistry::Register(FNiagaraTypeDefinition(GetClass()), true, false, false);
	}

	PushToRenderThread();
}

void UNiagaraDataInterfaceVolumeTexture::PostLoad()
{
	Super::PostLoad();

	// Not safe since the UTexture might not have yet PostLoad() called and so UpdateResource() called.
	// This will affect whether the SamplerStateRHI will be available or not.
	PushToRenderThread();
}

#if WITH_EDITOR

void UNiagaraDataInterfaceVolumeTexture::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	PushToRenderThread();
}

#endif

bool UNiagaraDataInterfaceVolumeTexture::CopyToInternal(UNiagaraDataInterface* Destination) const
{
	if (!Super::CopyToInternal(Destination))
	{
		return false;
	}
	UNiagaraDataInterfaceVolumeTexture* DestinationTexture = CastChecked<UNiagaraDataInterfaceVolumeTexture>(Destination);
	DestinationTexture->Texture = Texture;
	DestinationTexture->PushToRenderThread();

	return true;
}

bool UNiagaraDataInterfaceVolumeTexture::Equals(const UNiagaraDataInterface* Other) const
{
	if (!Super::Equals(Other))
	{
		return false;
	}
	const UNiagaraDataInterfaceVolumeTexture* OtherTexture = CastChecked<const UNiagaraDataInterfaceVolumeTexture>(Other);
	return OtherTexture->Texture == Texture;
}

void UNiagaraDataInterfaceVolumeTexture::GetFunctions(TArray<FNiagaraFunctionSignature>& OutFunctions)
{
	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = SampleVolumeTextureName;
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("Texture")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("UVW")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("MipLevel")));
		Sig.SetDescription(LOCTEXT("TextureSampleVolumeTextureDesc", "Sample the specified mip level of the input 3d texture at the specified UVW coordinates. The UVW origin (0, 0, 0) is in the bottom left hand corner of the volume."));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("Value")));
		//Sig.Owner = *GetName();

		OutFunctions.Add(Sig);
	}

	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = TextureDimsName;
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("Texture")));
		Sig.SetDescription(LOCTEXT("TextureDimsDesc", "Get the dimensions of mip 0 of the texture."));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Dimensions3D")));
		//Sig.Owner = *GetName();

		OutFunctions.Add(Sig);
	}
}

DEFINE_NDI_DIRECT_FUNC_BINDER(UNiagaraDataInterfaceVolumeTexture, SampleVolumeTexture)
void UNiagaraDataInterfaceVolumeTexture::GetVMExternalFunction(const FVMExternalFunctionBindingInfo& BindingInfo, void* InstanceData, FVMExternalFunction &OutFunc)
{
	if (BindingInfo.Name == SampleVolumeTextureName)
	{
		check(BindingInfo.GetNumInputs() == 4 && BindingInfo.GetNumOutputs() == 4);
		NDI_FUNC_BINDER(UNiagaraDataInterfaceVolumeTexture, SampleVolumeTexture)::Bind(this, OutFunc);
	}
	else if (BindingInfo.Name == TextureDimsName)
	{
		check(BindingInfo.GetNumInputs() == 0 && BindingInfo.GetNumOutputs() == 3);
		OutFunc = FVMExternalFunction::CreateUObject(this, &UNiagaraDataInterfaceVolumeTexture::GetTextureDimensions);
	}
}

void UNiagaraDataInterfaceVolumeTexture::GetTextureDimensions(FVectorVMContext& Context)
{
	VectorVM::FExternalFuncRegisterHandler<float> OutWidth(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutHeight(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutDepth(Context);

	if (Texture == nullptr)
	{
		for (int32 i = 0; i < Context.NumInstances; ++i)
		{
			*OutWidth.GetDestAndAdvance() = 0.0f;
			*OutHeight.GetDestAndAdvance() = 0.0f;
			*OutDepth.GetDestAndAdvance() = 0.0f;
		}
	}
	else
	{
		float Width = Texture->GetSizeX();
		float Height = Texture->GetSizeY();
		float Depth = Texture->GetSizeZ();
		for (int32 i = 0; i < Context.NumInstances; ++i)
		{
			*OutWidth.GetDestAndAdvance() = Width;
			*OutHeight.GetDestAndAdvance() = Height;
			*OutDepth.GetDestAndAdvance() = Depth;
		}
	}
}

void UNiagaraDataInterfaceVolumeTexture::SampleVolumeTexture(FVectorVMContext& Context)
{
	VectorVM::FExternalFuncInputHandler<float> XParam(Context);
	VectorVM::FExternalFuncInputHandler<float> YParam(Context);
	VectorVM::FExternalFuncInputHandler<float> ZParam(Context);
	VectorVM::FExternalFuncInputHandler<float> MipLevelParam(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutSampleR(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutSampleG(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutSampleB(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutSampleA(Context);

	for (int32 i = 0; i < Context.NumInstances; ++i)
	{
		float X = XParam.GetAndAdvance();
		float Y = YParam.GetAndAdvance();
		float Z = YParam.GetAndAdvance();
		float Mip = MipLevelParam.GetAndAdvance();
		*OutSampleR.GetDestAndAdvance() = 1.0;
		*OutSampleG.GetDestAndAdvance() = 0.0;
		*OutSampleB.GetDestAndAdvance() = 1.0;
		*OutSampleA.GetDestAndAdvance() = 1.0;
	}

}

bool UNiagaraDataInterfaceVolumeTexture::GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, const FNiagaraDataInterfaceGeneratedFunction& FunctionInfo, int FunctionInstanceIndex, FString& OutHLSL)
{
	if (FunctionInfo.DefinitionName == SampleVolumeTextureName)
	{
		FString HLSLTextureName = TextureName + ParamInfo.DataInterfaceHLSLSymbol;
		FString HLSLSamplerName = SamplerName + ParamInfo.DataInterfaceHLSLSymbol;
		OutHLSL += TEXT("void ") + FunctionInfo.InstanceName + TEXT("(in float3 In_UV, in float MipLevel, out float4 Out_Value) \n{\n");
		OutHLSL += TEXT("\t Out_Value = ") + HLSLTextureName + TEXT(".SampleLevel(") + HLSLSamplerName + TEXT(", In_UV, MipLevel);\n");
		OutHLSL += TEXT("\n}\n");
		return true;
	}
	else if (FunctionInfo.DefinitionName == TextureDimsName)
	{
		FString DimsVar = DimensionsBaseName + ParamInfo.DataInterfaceHLSLSymbol;
		OutHLSL += TEXT("void ") + FunctionInfo.InstanceName + TEXT("(out float3 Out_Value) \n{\n");
		OutHLSL += TEXT("\t Out_Value = ") + DimsVar + TEXT(";\n");
		OutHLSL += TEXT("\n}\n");
		return true;
	}
	return false;
}

void UNiagaraDataInterfaceVolumeTexture::GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, FString& OutHLSL)
{
	FString HLSLTextureName = TextureName + ParamInfo.DataInterfaceHLSLSymbol;
	FString HLSLSamplerName = SamplerName + ParamInfo.DataInterfaceHLSLSymbol;
	OutHLSL += TEXT("Texture3D ") + HLSLTextureName + TEXT(";\n");
	OutHLSL += TEXT("SamplerState ") + HLSLSamplerName + TEXT(";\n");
	OutHLSL += TEXT("float3 ") + DimensionsBaseName + ParamInfo.DataInterfaceHLSLSymbol + TEXT(";\n");
}


struct FNiagaraDataInterfaceParametersCS_VolumeTexture : public FNiagaraDataInterfaceParametersCS
{
	DECLARE_TYPE_LAYOUT(FNiagaraDataInterfaceParametersCS_VolumeTexture, NonVirtual);
public:
	void Bind(const FNiagaraDataInterfaceGPUParamInfo& ParameterInfo, const class FShaderParameterMap& ParameterMap)
	{
		FString TexName = UNiagaraDataInterfaceVolumeTexture::TextureName + ParameterInfo.DataInterfaceHLSLSymbol;
		FString SampleName = (UNiagaraDataInterfaceVolumeTexture::SamplerName + ParameterInfo.DataInterfaceHLSLSymbol);
		TextureParam.Bind(ParameterMap, *TexName);
		SamplerParam.Bind(ParameterMap, *SampleName);

		if (!TextureParam.IsBound())
		{
			UE_LOG(LogNiagara, Warning, TEXT("Binding failed for FNiagaraDataInterfaceParametersCS_VolumeTexture Texture %s. Was it optimized out?"), *TexName)
		}

		if (!SamplerParam.IsBound())
		{
			UE_LOG(LogNiagara, Warning, TEXT("Binding failed for FNiagaraDataInterfaceParametersCS_VolumeTexture Sampler %s. Was it optimized out?"), *SampleName)
		}

		Dimensions.Bind(ParameterMap, *(UNiagaraDataInterfaceVolumeTexture::DimensionsBaseName + ParameterInfo.DataInterfaceHLSLSymbol));

	}

	void Set(FRHICommandList& RHICmdList, const FNiagaraDataInterfaceSetArgs& Context) const
	{
		check(IsInRenderingThread());

		FRHIComputeShader* ComputeShaderRHI = Context.Shader.GetComputeShader();
		FNiagaraDataInterfaceProxyVolumeTexture* TextureDI = static_cast<FNiagaraDataInterfaceProxyVolumeTexture*>(Context.DataInterface);

		FRHITexture* TextureRHI = (TextureDI && TextureDI->TextureReferenceRHI) ? TextureDI->TextureReferenceRHI->GetReferencedTexture() : nullptr;
		if (TextureRHI)
		{
			FRHISamplerState* SamplerStateRHI = TextureDI->SamplerStateRHI;
			if (!SamplerStateRHI)
			{
				// Fallback required because PostLoad() order affects whether RHI resources 
				// are initalized in UNiagaraDataInterfaceVolumeTexture::PushToRenderThread().
				SamplerStateRHI = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			}
			SetTextureParameter(
				RHICmdList,
				ComputeShaderRHI,
				TextureParam,
				SamplerParam,
				SamplerStateRHI,
				TextureRHI
			);
			SetShaderValue(RHICmdList, ComputeShaderRHI, Dimensions, TextureDI->TexDims);
		}
		else
		{
			SetTextureParameter(
				RHICmdList,
				ComputeShaderRHI,
				TextureParam,
				SamplerParam,
				GBlackTexture->SamplerStateRHI,
				GBlackTexture->TextureRHI
			);
			FVector TexDims(0.0f, 0.0f, 0.0f);
			SetShaderValue(RHICmdList, ComputeShaderRHI, Dimensions, TexDims);
		}
	}
private:
	LAYOUT_FIELD(FShaderResourceParameter, TextureParam);
	LAYOUT_FIELD(FShaderResourceParameter, SamplerParam);
	LAYOUT_FIELD(FShaderParameter, Dimensions);
};

void UNiagaraDataInterfaceVolumeTexture::PushToRenderThread()
{
	FNiagaraDataInterfaceProxyVolumeTexture* RT_Proxy = GetProxyAs<FNiagaraDataInterfaceProxyVolumeTexture>();

	float DimX = 0.0f;
	float DimY = 0.0f;
	float DimZ = 0.0f;
	FTextureReferenceRHIRef RT_TextureReference;
	FSamplerStateRHIRef RT_SamplerState;

	if (Texture && Texture->TextureReference.TextureReferenceRHI.IsValid())
	{
		if (Texture->TextureReference.TextureReferenceRHI)
		{
			RT_TextureReference = Texture->TextureReference.TextureReferenceRHI;
		}
		if (Texture->Resource)
		{
			RT_SamplerState = Texture->Resource->SamplerStateRHI;
		}
		DimX = Texture->GetSizeX();
		DimY = Texture->GetSizeY();
		DimZ = Texture->GetSizeZ();
	}

	ENQUEUE_RENDER_COMMAND(FPushDITextureToRT) (
		[RT_Proxy, RT_TextureReference, RT_SamplerState, DimX, DimY, DimZ](FRHICommandListImmediate& RHICmdList)
	{
		RT_Proxy->TextureReferenceRHI = RT_TextureReference;
		RT_Proxy->SamplerStateRHI = RT_SamplerState;
		RT_Proxy->TexDims.X = DimX;
		RT_Proxy->TexDims.Y = DimY;
		RT_Proxy->TexDims.Z = DimZ;
	}
	);
}

FNiagaraDataInterfaceParametersCS* UNiagaraDataInterfaceVolumeTexture::ConstructComputeParameters()const
{
	return new FNiagaraDataInterfaceParametersCS_VolumeTexture();
}

void UNiagaraDataInterfaceVolumeTexture::SetTexture(UVolumeTexture* InTexture)
{
	if (InTexture)
	{
		Texture = InTexture;
		PushToRenderThread();
	}
}

#undef LOCTEXT_NAMESPACE