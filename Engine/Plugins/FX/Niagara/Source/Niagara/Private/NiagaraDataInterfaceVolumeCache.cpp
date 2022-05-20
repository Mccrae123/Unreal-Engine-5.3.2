// Copyright Epic Games, Inc. All Rights Reserved.
#include "NiagaraDataInterfaceVolumeCache.h"
#include "NiagaraShader.h"
#include "ShaderParameterUtils.h"
#include "ClearQuad.h"
#include "TextureResource.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraRenderer.h"
#include "Engine/VolumeTexture.h"
#include "Engine/TextureRenderTargetVolume.h"
#include "NiagaraSettings.h"
#include "NiagaraConstants.h"
#include "NiagaraComputeExecutionContext.h"
#include "NiagaraShaderParametersBuilder.h"
#if WITH_EDITOR
#include "NiagaraGpuComputeDebug.h"
#endif

#define LOCTEXT_NAMESPACE "NiagaraDataInterfaceVolumeCache"

const TCHAR* UNiagaraDataInterfaceVolumeCache::TemplateShaderFilePath = TEXT("/Plugin/FX/Niagara/Private/NiagaraDataInterfaceVolumeCache.ush");
const FName UNiagaraDataInterfaceVolumeCache::SetFrameName("SetFrame");
const FName UNiagaraDataInterfaceVolumeCache::ReadFileName("ReadFile");
const FName UNiagaraDataInterfaceVolumeCache::SampleCurrentFrameValueName(TEXT("SampleCurrentFrameValue"));
const FName UNiagaraDataInterfaceVolumeCache::GetCurrentFrameValue(TEXT("GetCurrentFrameValue"));
const FName UNiagaraDataInterfaceVolumeCache::GetCurrentFrameNumCells(TEXT("GetCurrentFrameNumCells"));

struct FVolumeCacheInstanceData_RenderThread
{
	int						CurrFrame = 0;

	FSamplerStateRHIRef		SamplerStateRHI = nullptr;
	FTextureReferenceRHIRef	TextureReferenceRHI = nullptr;
	FTextureRHIRef			ResolvedTextureRHI = nullptr;
	FVector3f				TextureSize = FVector3f::ZeroVector;
};

struct FVolumeCacheInstanceData_GameThread
{
	int CurrFrame = 0;
	int PrevFrame = -1;

	bool ReadFile = false;		
};

struct FNiagaraDataInterfaceVolumeCacheProxy : public FNiagaraDataInterfaceProxy
{
	virtual void ConsumePerInstanceDataFromGameThread(void* PerInstanceData, const FNiagaraSystemInstanceID& Instance) override { check(false); }
	virtual int32 PerInstanceDataPassedToRenderThreadSize() const override { return 0; }

	TMap<FNiagaraSystemInstanceID, FVolumeCacheInstanceData_RenderThread> InstanceData_RT;
};



UNiagaraDataInterfaceVolumeCache::UNiagaraDataInterfaceVolumeCache(FObjectInitializer const& ObjectInitializer)
	: Super(ObjectInitializer)
{
	Proxy.Reset(new FNiagaraDataInterfaceVolumeCacheProxy());

}

void UNiagaraDataInterfaceVolumeCache::PostInitProperties()
{
	Super::PostInitProperties();

	//Can we register data interfaces as regular types and fold them into the FNiagaraVariable framework for UI and function calls etc?
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		ENiagaraTypeRegistryFlags Flags = ENiagaraTypeRegistryFlags::AllowAnyVariable | ENiagaraTypeRegistryFlags::AllowParameter;
		FNiagaraTypeRegistry::Register(FNiagaraTypeDefinition(GetClass()), Flags);		
	}
}


bool UNiagaraDataInterfaceVolumeCache::Equals(const UNiagaraDataInterface* Other) const
{
	if (!Super::Equals(Other))
	{
		return false;
	}
	const UNiagaraDataInterfaceVolumeCache* OtherTyped = CastChecked<const UNiagaraDataInterfaceVolumeCache>(Other);

	return OtherTyped != nullptr && VolumeCache == OtherTyped->VolumeCache;
}

inline int32 UNiagaraDataInterfaceVolumeCache::PerInstanceDataSize() const 
{ 
	return sizeof(FVolumeCacheInstanceData_GameThread); 
}

bool UNiagaraDataInterfaceVolumeCache::CopyToInternal(UNiagaraDataInterface* Destination) const
{
	if (!Super::CopyToInternal(Destination))
	{
		return false;
	}

	UNiagaraDataInterfaceVolumeCache* OtherTyped = CastChecked<UNiagaraDataInterfaceVolumeCache>(Destination);

	OtherTyped->VolumeCache = VolumeCache;
	return true;
}

bool UNiagaraDataInterfaceVolumeCache::InitPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance)
{
	check(Proxy);
	
	FVolumeCacheInstanceData_GameThread* InstanceData = new (PerInstanceData) FVolumeCacheInstanceData_GameThread();
	SystemInstancesToProxyData_GT.Emplace(SystemInstance->GetId(), InstanceData);	

	// Push Updates to Proxy.
	FNiagaraDataInterfaceVolumeCacheProxy* TheProxy = GetProxyAs<FNiagaraDataInterfaceVolumeCacheProxy>();
	ENQUEUE_RENDER_COMMAND(FUpdateData)(
		[TheProxy, InstanceID = SystemInstance->GetId()](FRHICommandListImmediate& RHICmdList)
	{
		check(!TheProxy->InstanceData_RT.Contains(InstanceID));
		FVolumeCacheInstanceData_RenderThread* TargetData = &TheProxy->InstanceData_RT.Add(InstanceID);

	});

	return true;
}

void UNiagaraDataInterfaceVolumeCache::DestroyPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance)
{
	FVolumeCacheInstanceData_GameThread* InstanceData = SystemInstancesToProxyData_GT.FindRef(SystemInstance->GetId());	
	
	InstanceData->~FVolumeCacheInstanceData_GameThread();
	SystemInstancesToProxyData_GT.Remove(SystemInstance->GetId());

	ENQUEUE_RENDER_COMMAND(RemoveInstance)
		(
			[RT_Proxy = GetProxyAs<FNiagaraDataInterfaceVolumeCacheProxy>(), RT_InstanceID = SystemInstance->GetId()](FRHICommandListImmediate&)
	{
		RT_Proxy->InstanceData_RT.Remove(RT_InstanceID);
	}
	);
}

void UNiagaraDataInterfaceVolumeCache::GetFunctions(TArray<FNiagaraFunctionSignature>& OutFunctions)
{	

	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = SetFrameName;
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("Grid")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Frame")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Success")));

		Sig.ModuleUsageBitmask = ENiagaraScriptUsageMask::Emitter | ENiagaraScriptUsageMask::System;
		Sig.bExperimental = true;
		Sig.bMemberFunction = true;
		Sig.bRequiresExecPin = true;
		Sig.bRequiresContext = false;
		Sig.bSupportsCPU = true;
		Sig.bSupportsGPU = false;
		OutFunctions.Add(Sig);
	}

	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = ReadFileName;
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("Grid")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("ReadFile")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Success")));

		Sig.ModuleUsageBitmask = ENiagaraScriptUsageMask::Emitter | ENiagaraScriptUsageMask::System;
		Sig.bExperimental = true;
		Sig.bMemberFunction = true;
		Sig.bRequiresExecPin = true;
		Sig.bRequiresContext = false;
		Sig.bSupportsCPU = true;
		Sig.bSupportsGPU = false;
		OutFunctions.Add(Sig);
	}

	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = SampleCurrentFrameValueName;
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		Sig.bSupportsCPU = false;
		Sig.bSupportsGPU = true;
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
		Sig.Name = GetCurrentFrameValue;
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		Sig.bSupportsCPU = false;
		Sig.bSupportsGPU = true;
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("Texture")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("x")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("y")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("z")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("MipLevel")));
		Sig.SetDescription(LOCTEXT("TextureLoadVolumeTextureDesc", "load the specified mip level of the input 3d texture at the specified x, y, z voxel coordinates."));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("Value")));
		//Sig.Owner = *GetName();

		OutFunctions.Add(Sig);
	}

	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = GetCurrentFrameNumCells;
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		Sig.bSupportsCPU = false;
		Sig.bSupportsGPU = true;
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("Texture")));
		Sig.SetDescription(LOCTEXT("TextureDimsDesc", "Get the dimensions of mip 0 of the texture."));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Dimensions3D")));
		//Sig.Owner = *GetName();

		OutFunctions.Add(Sig);
	}
}

void UNiagaraDataInterfaceVolumeCache::SetFrame(FVectorVMExternalFunctionContext& Context)
{
	// This should only be called from a system or emitter script due to a need for only setting up initially.
	VectorVM::FUserPtrHandler<FVolumeCacheInstanceData_GameThread> InstData(Context);
	VectorVM::FExternalFuncInputHandler<int> InFrame(Context);	
	VectorVM::FExternalFuncRegisterHandler<FNiagaraBool> OutSuccess(Context);

	InstData->CurrFrame = InFrame.GetAndAdvance();

	*OutSuccess.GetDestAndAdvance() = true;
}

void UNiagaraDataInterfaceVolumeCache::ReadFile(FVectorVMExternalFunctionContext& Context)
{
	// This should only be called from a system or emitter script due to a need for only setting up initially.
	VectorVM::FUserPtrHandler<FVolumeCacheInstanceData_GameThread> InstData(Context);
	VectorVM::FExternalFuncInputHandler<bool> Read(Context);
	VectorVM::FExternalFuncRegisterHandler<FNiagaraBool> OutSuccess(Context);

	InstData->ReadFile = Read.GetAndAdvance();

	*OutSuccess.GetDestAndAdvance() = true;
}

FString UNiagaraDataInterfaceVolumeCache::GetAssetPath(FString PathFormat, int32 FrameIndex) const
{
	UNiagaraSystem* NiagaraSystem = GetTypedOuter<UNiagaraSystem>();
	check(NiagaraSystem);

	const TMap<FString, FStringFormatArg> PathFormatArgs =
	{		
		{TEXT("SavedDir"),		FPaths::ProjectSavedDir()},
		{TEXT("FrameIndex"),	FString::Printf(TEXT("%03d"), FrameIndex)},
	};
	FString AssetPath = FString::Format(*PathFormat, PathFormatArgs);
	AssetPath.ReplaceInline(TEXT("//"), TEXT("/"));
	return AssetPath;
}

DEFINE_NDI_DIRECT_FUNC_BINDER(UNiagaraDataInterfaceVolumeCache, SetFrame);
DEFINE_NDI_DIRECT_FUNC_BINDER(UNiagaraDataInterfaceVolumeCache, ReadFile);
void UNiagaraDataInterfaceVolumeCache::GetVMExternalFunction(const FVMExternalFunctionBindingInfo& BindingInfo, void* InstanceData, FVMExternalFunction& OutFunc)
{
	if (BindingInfo.Name == SetFrameName)
	{
		check(BindingInfo.GetNumInputs() == 2 && BindingInfo.GetNumOutputs() == 1);
		NDI_FUNC_BINDER(UNiagaraDataInterfaceVolumeCache, SetFrame)::Bind(this, OutFunc);
	}
	if (BindingInfo.Name == ReadFileName)
	{
		check(BindingInfo.GetNumInputs() == 2 && BindingInfo.GetNumOutputs() == 1);
		NDI_FUNC_BINDER(UNiagaraDataInterfaceVolumeCache, ReadFile)::Bind(this, OutFunc);
	}
}

bool UNiagaraDataInterfaceVolumeCache::PerInstanceTick(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance, float DeltaSeconds)
{
	FVolumeCacheInstanceData_GameThread* InstanceData = SystemInstancesToProxyData_GT.FindRef(SystemInstance->GetId());	

	// we can run into the case where depending on the ordering of DI initialization, we might have not been able to grab the other grid's InstanceData
	// in InitPerInstanceData.  If this is the case, we ensure it is correct here.
	if (InstanceData && InstanceData->ReadFile && VolumeCache != nullptr)
	{
		FNiagaraDataInterfaceVolumeCacheProxy* TextureProxy = GetProxyAs<FNiagaraDataInterfaceVolumeCacheProxy>();

		EPixelFormat Format = PF_A32B32G32R32F;
		const int32 FormatSize = GPixelFormats[Format].BlockBytes;

		// cannot read from cache...spew errors or let it go?
		if (!VolumeCache->LoadFile(InstanceData->CurrFrame))
		{
			UE_LOG(LogNiagara, Warning, TEXT("Cache Read failed: %s"), *VolumeCache->GetName());
			return false;
		}

		ENQUEUE_RENDER_COMMAND(FVolumeCacheFillTexture)(
			[RT_Frame = InstanceData->CurrFrame, RT_VolumeCacheData = VolumeCache->GetData(), Format, TextureProxy, SystemID = SystemInstance->GetId()](FRHICommandListImmediate& RHICmdList)
		{
			FVolumeCacheInstanceData_RenderThread* InstanceData = TextureProxy->InstanceData_RT.Find(SystemID);
			FIntVector Size = RT_VolumeCacheData->GetDenseResolution();

			if (!InstanceData->ResolvedTextureRHI.IsValid())
			{

				const FRHITextureCreateDesc Desc =
					FRHITextureCreateDesc::Create3D(TEXT("stuff"), Size.X, Size.Y, Size.Z, Format)
					.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::NoTiling);

				InstanceData->ResolvedTextureRHI = RHICreateTexture(Desc);
				InstanceData->TextureSize = FVector3f(Size.X, Size.Y, Size.Z);
				InstanceData->SamplerStateRHI = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			}
				
			RT_VolumeCacheData->Fill3DTexture_RenderThread(RT_Frame, InstanceData->ResolvedTextureRHI, RHICmdList);			
		});
	}

	return false;

}

#if WITH_EDITORONLY_DATA
bool UNiagaraDataInterfaceVolumeCache::AppendCompileHash(FNiagaraCompileHashVisitor* InVisitor) const
{
	bool bSuccess = Super::AppendCompileHash(InVisitor);
	InVisitor->UpdateString(TEXT("UNiagaraDataInterfaceVolumeTextureHLSLSource"), GetShaderFileHash(TemplateShaderFilePath, EShaderPlatform::SP_PCD3D_SM5).ToString());
	bSuccess &= InVisitor->UpdateShaderParameters<FShaderParameters>();
	return bSuccess;
}

void UNiagaraDataInterfaceVolumeCache::GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, FString& OutHLSL)
{
	TMap<FString, FStringFormatArg> TemplateArgs =
	{
		{TEXT("ParameterName"),	ParamInfo.DataInterfaceHLSLSymbol},
	};

	FString TemplateFile;
	LoadShaderSourceFile(TemplateShaderFilePath, EShaderPlatform::SP_PCD3D_SM5, &TemplateFile, nullptr);
	OutHLSL += FString::Format(*TemplateFile, TemplateArgs);
}

bool UNiagaraDataInterfaceVolumeCache::GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, const FNiagaraDataInterfaceGeneratedFunction& FunctionInfo, int FunctionInstanceIndex, FString& OutHLSL)
{
	if ((FunctionInfo.DefinitionName == SampleCurrentFrameValueName) ||
		(FunctionInfo.DefinitionName == GetCurrentFrameNumCells) ||
		(FunctionInfo.DefinitionName == GetCurrentFrameValue))
	{
		return true;
	}
	return false;
}
#endif

void UNiagaraDataInterfaceVolumeCache::BuildShaderParameters(FNiagaraShaderParametersBuilder& ShaderParametersBuilder) const
{
	ShaderParametersBuilder.AddNestedStruct<FShaderParameters>();
}

void UNiagaraDataInterfaceVolumeCache::SetShaderParameters(const FNiagaraDataInterfaceSetShaderParametersContext& Context) const
{
	const FNiagaraDataInterfaceVolumeCacheProxy& TextureProxy = Context.GetProxy<FNiagaraDataInterfaceVolumeCacheProxy>();
	const FVolumeCacheInstanceData_RenderThread* InstanceData = TextureProxy.InstanceData_RT.Find(Context.GetSystemInstanceID());

	FShaderParameters* Parameters = Context.GetParameterNestedStruct<FShaderParameters>();
	if (InstanceData && InstanceData->ResolvedTextureRHI.IsValid())
	{
		Parameters->TextureSize = InstanceData->TextureSize;
		Parameters->Texture = InstanceData->ResolvedTextureRHI;
		Parameters->TextureSampler = InstanceData->SamplerStateRHI ? InstanceData->SamplerStateRHI : GBlackVolumeTexture->SamplerStateRHI;
	}
	else
	{
		Parameters->TextureSize = FVector3f::ZeroVector;
		Parameters->Texture = GBlackVolumeTexture->TextureRHI;
		Parameters->TextureSampler = GBlackVolumeTexture->SamplerStateRHI;
	}
}

#undef LOCTEXT_NAMESPACE
