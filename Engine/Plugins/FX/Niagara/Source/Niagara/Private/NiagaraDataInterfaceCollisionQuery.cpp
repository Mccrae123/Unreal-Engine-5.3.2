// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraDataInterfaceCollisionQuery.h"

#include "GlobalDistanceFieldParameters.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitterInstanceBatcher.h"
#include "NiagaraRayTracingHelper.h"
#include "NiagaraStats.h"
#include "NiagaraTypes.h"
#include "NiagaraWorldManager.h"
#include "RayTracingInstanceUtils.h"
#include "RenderResource.h"
#include "Shader.h"
#include "ShaderCore.h"
#include "ShaderParameterUtils.h"

FCriticalSection UNiagaraDataInterfaceCollisionQuery::CriticalSection;

struct FNiagaraCollisionDIFunctionVersion
{
	enum Type
	{
		InitialVersion = 0,
		AddedTraceSkip = 1,
		AddedCustomDepthCollision = 2,
		ReturnCollisionMaterialIdx = 3,

		VersionPlusOne,
		LatestVersion = VersionPlusOne - 1
	};
};

struct FNiagaraDataIntefaceProxyCollisionQuery : public FNiagaraDataInterfaceProxy
{
#if RHI_RAYTRACING
	FRWBufferStructured RayTraceRequests;
	FRWBufferStructured RayTraceIntersections;

	int32 MaxRayTraceCount = 0;
#endif

	virtual ~FNiagaraDataIntefaceProxyCollisionQuery()
	{
#if RHI_RAYTRACING
		DEC_MEMORY_STAT_BY(STAT_NiagaraGPUDataInterfaceMemory, RayTraceRequests.NumBytes);
		RayTraceRequests.Release();

		DEC_MEMORY_STAT_BY(STAT_NiagaraGPUDataInterfaceMemory, RayTraceIntersections.NumBytes);
		RayTraceIntersections.Release();
#endif
	}

	virtual int32 PerInstanceDataPassedToRenderThreadSize() const override
	{
		return 0;
	}

	virtual void PostSimulate(FRHICommandList& RHICmdList, const FNiagaraDataInterfaceArgs& Context) override
	{
#if RHI_RAYTRACING
		if (MaxRayTraceCount > 0 && Context.Batcher->HasRayTracingScene())
		{
			{
				FRHITransitionInfo PreTransitions[] =
				{
					FRHITransitionInfo(RayTraceRequests.UAV, ERHIAccess::UAVMask, ERHIAccess::SRVMask),
					FRHITransitionInfo(RayTraceIntersections.UAV, ERHIAccess::SRVMask, ERHIAccess::UAVMask)
				};
				RHICmdList.Transition(PreTransitions);
			}

			Context.Batcher->IssueRayTraces(RHICmdList, FIntPoint(MaxRayTraceCount, 1), RayTraceRequests.SRV, RayTraceIntersections.UAV);

			{
				FRHITransitionInfo PostTransitions[] =
				{
					FRHITransitionInfo(RayTraceRequests.UAV, ERHIAccess::SRVMask, ERHIAccess::UAVMask),
					FRHITransitionInfo(RayTraceIntersections.UAV, ERHIAccess::UAVMask, ERHIAccess::SRVMask)
				};
				RHICmdList.Transition(PostTransitions);
			}
		}
#endif
	}

	void RenderThreadInitialize(int32 InMaxRayTraceRequests)
	{
#if RHI_RAYTRACING
		MaxRayTraceCount = 0;

		DEC_MEMORY_STAT_BY(STAT_NiagaraGPUDataInterfaceMemory, RayTraceRequests.NumBytes);
		RayTraceRequests.Release();

		DEC_MEMORY_STAT_BY(STAT_NiagaraGPUDataInterfaceMemory, RayTraceIntersections.NumBytes);
		RayTraceIntersections.Release();

		if (IsRayTracingEnabled() && InMaxRayTraceRequests > 0)
		{
			MaxRayTraceCount = 16 * FMath::DivideAndRoundUp(InMaxRayTraceRequests, 16);

			RayTraceRequests.Initialize(
				TEXT("NiagaraRayTraceRequests"),
				sizeof(FBasicRayData),
				MaxRayTraceCount,
				BUF_Static);
			INC_MEMORY_STAT_BY(STAT_NiagaraGPUDataInterfaceMemory, RayTraceRequests.NumBytes);

			RayTraceIntersections.Initialize(
				TEXT("NiagaraRayTraceIntersections"),
				sizeof(FNiagaraRayTracingPayload),
				MaxRayTraceCount,
				BUF_Static,
				false /*bUseUavCounter*/,
				false /*bAppendBuffer*/,
				ERHIAccess::SRVMask);
			INC_MEMORY_STAT_BY(STAT_NiagaraGPUDataInterfaceMemory, RayTraceIntersections.NumBytes);
		}
#endif
	}
};

const FName UNiagaraDataInterfaceCollisionQuery::SceneDepthName(TEXT("QuerySceneDepthGPU"));
const FName UNiagaraDataInterfaceCollisionQuery::CustomDepthName(TEXT("QueryCustomDepthGPU"));
const FName UNiagaraDataInterfaceCollisionQuery::DistanceFieldName(TEXT("QueryMeshDistanceFieldGPU"));
const FName UNiagaraDataInterfaceCollisionQuery::IssueAsyncRayTraceName(TEXT("IssueAsyncRayTraceGpu"));
const FName UNiagaraDataInterfaceCollisionQuery::ReadAsyncRayTraceName(TEXT("ReadAsyncRayTraceGpu"));
const FName UNiagaraDataInterfaceCollisionQuery::SyncTraceName(TEXT("PerformCollisionQuerySyncCPU"));
const FName UNiagaraDataInterfaceCollisionQuery::AsyncTraceName(TEXT("PerformCollisionQueryAsyncCPU"));

UNiagaraDataInterfaceCollisionQuery::UNiagaraDataInterfaceCollisionQuery(FObjectInitializer const& ObjectInitializer)
	: Super(ObjectInitializer)
{
	TraceChannelEnum = StaticEnum<ECollisionChannel>();
	SystemInstance = nullptr;

    Proxy.Reset(new FNiagaraDataIntefaceProxyCollisionQuery());
}

bool UNiagaraDataInterfaceCollisionQuery::InitPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* InSystemInstance)
{
	CQDIPerInstanceData *PIData = new (PerInstanceData) CQDIPerInstanceData;
	PIData->SystemInstance = InSystemInstance;
	if (InSystemInstance)
	{
		PIData->CollisionBatch.Init(InSystemInstance->GetId(), InSystemInstance->GetWorld());
	}
	return true;
}

void UNiagaraDataInterfaceCollisionQuery::DestroyPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* InSystemInstance)
{
	CQDIPerInstanceData* InstData = (CQDIPerInstanceData*)PerInstanceData;
	InstData->~CQDIPerInstanceData();
}

void UNiagaraDataInterfaceCollisionQuery::PostInitProperties()
{
	Super::PostInitProperties();

	//Can we register data interfaces as regular types and fold them into the FNiagaraVariable framework for UI and function calls etc?
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		ENiagaraTypeRegistryFlags Flags = ENiagaraTypeRegistryFlags::AllowAnyVariable | ENiagaraTypeRegistryFlags::AllowParameter;
		FNiagaraTypeRegistry::Register(FNiagaraTypeDefinition(GetClass()), Flags);
		FNiagaraTypeRegistry::Register(FNiagaraTypeDefinition(TraceChannelEnum), Flags);
	}
}

void UNiagaraDataInterfaceCollisionQuery::PostLoad()
{
	Super::PostLoad();

	if (MaxRayTraceCount)
	{
		MarkRenderDataDirty();
	}
}

void UNiagaraDataInterfaceCollisionQuery::GetAssetTagsForContext(const UObject* InAsset, const TArray<const UNiagaraDataInterface*>& InProperties, TMap<FName, uint32>& NumericKeys, TMap<FName, FString>& StringKeys) const
{
#if WITH_EDITOR
	const UNiagaraSystem* System = Cast<UNiagaraSystem>(InAsset);
	const UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(InAsset);

	// We need to check if the DI is used to access collisions in a cpu context so that artists can surface potential perf problems
	// through the content browser.

	TArray<const UNiagaraScript*> Scripts;
	if (System)
	{
		Scripts.Add(System->GetSystemSpawnScript());
		Scripts.Add(System->GetSystemUpdateScript());
		for (auto&& EmitterHandle : System->GetEmitterHandles())
		{
			const UNiagaraEmitter* HandleEmitter = EmitterHandle.GetInstance();
			if (HandleEmitter)
			{
				if (HandleEmitter->SimTarget == ENiagaraSimTarget::GPUComputeSim)
				{
					// Ignore gpu emitters
					continue;
				}
				TArray<UNiagaraScript*> OutScripts;
				HandleEmitter->GetScripts(OutScripts, false);
				Scripts.Append(OutScripts);
			}
		}
	}
	if (Emitter)
	{
		if (Emitter->SimTarget != ENiagaraSimTarget::GPUComputeSim)
		{
			TArray<UNiagaraScript*> OutScripts;
			Emitter->GetScripts(OutScripts, false);
			Scripts.Append(OutScripts);
		}
	}

	// Check if any CPU script uses Collsion query CPU functions
	//TODO: This is the same as in the skel mesh DI for GetFeedback, it doesn't guarantee that the DI used by these functions are THIS DI.
	// Has a possibility of false positives
	bool bHaCPUQueriesWarning = [this, &Scripts]()
	{
		for (const auto Script : Scripts)
		{
			if (Script)
			{
				for (const auto& Info : Script->GetVMExecutableData().DataInterfaceInfo)
				{
					if (Info.MatchesClass(GetClass()))
					{
						for (const auto& Func : Info.RegisteredFunctions)
						{
							if (Func.Name == SyncTraceName || Func.Name == AsyncTraceName)
							{
								return true;
							}
						}
					}
				}
			}
		}
		return false;
	}();

	// Note that in order for these tags to be registered, we always have to put them in place for the CDO of the object, but 
	// for readability's sake, we leave them out of non-CDO assets.
	if (bHaCPUQueriesWarning || (InAsset && InAsset->HasAnyFlags(EObjectFlags::RF_ClassDefaultObject)))
	{
		StringKeys.Add("CPUCollision") = bHaCPUQueriesWarning ? TEXT("True") : TEXT("False");

	}

#endif
	
	// Make sure and get the base implementation tags
	Super::GetAssetTagsForContext(InAsset, InProperties, NumericKeys, StringKeys);
	
}

void UNiagaraDataInterfaceCollisionQuery::GetFunctions(TArray<FNiagaraFunctionSignature>& OutFunctions)
{
	FNiagaraFunctionSignature SigDepth;
	SigDepth.Name = UNiagaraDataInterfaceCollisionQuery::SceneDepthName;
	SigDepth.bMemberFunction = true;
	SigDepth.bRequiresContext = false;
	SigDepth.bSupportsCPU = false;
#if WITH_EDITORONLY_DATA
	SigDepth.FunctionVersion = FNiagaraCollisionDIFunctionVersion::LatestVersion;
#endif
	SigDepth.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("CollisionQuery")));
	SigDepth.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("DepthSamplePosWorld")));
	SigDepth.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("SceneDepth")));
	SigDepth.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("CameraPosWorld")));
	SigDepth.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("IsInsideView")));
	SigDepth.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("SamplePosWorld")));
	SigDepth.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("SampleWorldNormal")));
	OutFunctions.Add(SigDepth);

	FNiagaraFunctionSignature SigCustomDepth;
	SigCustomDepth.Name = UNiagaraDataInterfaceCollisionQuery::CustomDepthName;
	SigCustomDepth.bMemberFunction = true;
	SigCustomDepth.bRequiresContext = false;
	SigCustomDepth.bSupportsCPU = false;
#if WITH_EDITORONLY_DATA
	SigCustomDepth.FunctionVersion = FNiagaraCollisionDIFunctionVersion::LatestVersion;
#endif
	SigCustomDepth.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("CollisionQuery")));
	SigCustomDepth.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("DepthSamplePosWorld")));
	SigCustomDepth.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("SceneDepth")));
	SigCustomDepth.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("CameraPosWorld")));
	SigCustomDepth.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("IsInsideView")));
	SigCustomDepth.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("SamplePosWorld")));
	SigCustomDepth.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("SampleWorldNormal")));
	OutFunctions.Add(SigCustomDepth);

	FNiagaraFunctionSignature SigMeshField;
	SigMeshField.Name = UNiagaraDataInterfaceCollisionQuery::DistanceFieldName;
	SigMeshField.bMemberFunction = true;
	SigMeshField.bRequiresContext = false;
	SigMeshField.bSupportsCPU = false;
#if WITH_EDITORONLY_DATA
	SigMeshField.FunctionVersion = FNiagaraCollisionDIFunctionVersion::LatestVersion;
#endif
	SigMeshField.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("CollisionQuery")));
	SigMeshField.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("FieldSamplePosWorld")));
	SigMeshField.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("DistanceToNearestSurface")));
	SigMeshField.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("FieldGradient")));
	SigMeshField.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("IsDistanceFieldValid")));
    OutFunctions.Add(SigMeshField);

	{
		FNiagaraFunctionSignature& IssueRayTrace = OutFunctions.AddDefaulted_GetRef();
		IssueRayTrace.Name = UNiagaraDataInterfaceCollisionQuery::IssueAsyncRayTraceName;
		IssueRayTrace.bRequiresExecPin = true;
		IssueRayTrace.bMemberFunction = true;
		IssueRayTrace.bRequiresContext = false;
		IssueRayTrace.bSupportsCPU = false;
#if WITH_EDITORONLY_DATA
		IssueRayTrace.FunctionVersion = FNiagaraCollisionDIFunctionVersion::LatestVersion;
#endif
		IssueRayTrace.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("CollisionQuery")));
		IssueRayTrace.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("QueryID")));
		IssueRayTrace.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("TraceStartWorld")));
		IssueRayTrace.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("TraceEndWorld")));
		IssueRayTrace.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("TraceChannel")));
		IssueRayTrace.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("IsQueryValid")));
	}

	{
		FNiagaraFunctionSignature& ReadRayTrace = OutFunctions.AddDefaulted_GetRef();
		ReadRayTrace.Name = UNiagaraDataInterfaceCollisionQuery::ReadAsyncRayTraceName;
		ReadRayTrace.bMemberFunction = true;
		ReadRayTrace.bRequiresContext = false;
		ReadRayTrace.bSupportsCPU = false;
#if WITH_EDITORONLY_DATA
		ReadRayTrace.FunctionVersion = FNiagaraCollisionDIFunctionVersion::LatestVersion;
#endif
		ReadRayTrace.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("CollisionQuery")));
		ReadRayTrace.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("PreviousFrameQueryID")));
		ReadRayTrace.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("CollisionValid")));
		ReadRayTrace.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("CollisionDistance")));
		ReadRayTrace.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("CollisionPosWorld")));
		ReadRayTrace.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("CollisionNormal")));
	}

	FNiagaraFunctionSignature SigCpuSync;
	SigCpuSync.Name = UNiagaraDataInterfaceCollisionQuery::SyncTraceName;
	SigCpuSync.bMemberFunction = true;
	SigCpuSync.bRequiresContext = false;
	SigCpuSync.bSupportsGPU = false;
#if WITH_EDITORONLY_DATA
	SigCpuSync.FunctionVersion = FNiagaraCollisionDIFunctionVersion::LatestVersion;
#endif
	SigCpuSync.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("CollisionQuery")));
	SigCpuSync.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("TraceStartWorld")));
	SigCpuSync.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("TraceEndWorld")));
	SigCpuSync.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(TraceChannelEnum), TEXT("TraceChannel")));
	SigCpuSync.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("SkipTrace")));
	SigCpuSync.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("CollisionValid")));
	SigCpuSync.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("IsTraceInsideMesh")));
	SigCpuSync.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("CollisionPosWorld")));
	SigCpuSync.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("CollisionNormal")));
	SigCpuSync.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("CollisionMaterialFriction")));
	SigCpuSync.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("CollisionMaterialRestitution")));
	SigCpuSync.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("CollisionMaterialIndex")));
	OutFunctions.Add(SigCpuSync);

	FNiagaraFunctionSignature SigCpuAsync;
	SigCpuAsync.Name = UNiagaraDataInterfaceCollisionQuery::AsyncTraceName;
	SigCpuAsync.bMemberFunction = true;
	SigCpuAsync.bRequiresContext = false;
	SigCpuAsync.bSupportsGPU = false;
#if WITH_EDITORONLY_DATA
	SigCpuAsync.FunctionVersion = FNiagaraCollisionDIFunctionVersion::LatestVersion;
#endif
	SigCpuAsync.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("CollisionQuery")));
	SigCpuAsync.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("PreviousFrameQueryID")));
	SigCpuAsync.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("TraceStartWorld")));
	SigCpuAsync.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("TraceEndWorld")));
	SigCpuAsync.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(TraceChannelEnum), TEXT("TraceChannel")));
	SigCpuAsync.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("SkipTrace")));
	SigCpuAsync.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("NextFrameQueryID")));
	SigCpuAsync.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("CollisionValid")));
	SigCpuAsync.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("IsTraceInsideMesh")));
	SigCpuAsync.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("CollisionPosWorld")));
	SigCpuAsync.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("CollisionNormal")));
	SigCpuAsync.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("CollisionMaterialFriction")));
	SigCpuAsync.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("CollisionMaterialRestitution")));
	SigCpuAsync.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("CollisionMaterialIndex")));
	OutFunctions.Add(SigCpuAsync);
}

// build the shader function HLSL; function name is passed in, as it's defined per-DI; that way, configuration could change
// the HLSL in the spirit of a static switch
// TODO: need a way to identify each specific function here

// 
bool UNiagaraDataInterfaceCollisionQuery::GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, const FNiagaraDataInterfaceGeneratedFunction& FunctionInfo, int FunctionInstanceIndex, FString& OutHLSL)
{
	TMap<FString, FStringFormatArg> Args;
	Args.Add(TEXT("FunctionName"), FunctionInfo.InstanceName);

	if (FunctionInfo.DefinitionName == SceneDepthName)
	{
		static const TCHAR* FormatSample = TEXT(R"(
			void {FunctionName}(in float3 In_SamplePos, out float Out_SceneDepth, out float3 Out_CameraPosWorld, out bool Out_IsInsideView, out float3 Out_WorldPos, out float3 Out_WorldNormal)
			{
				DICollisionQuery_SceneDepth(In_SamplePos, Out_SceneDepth, Out_CameraPosWorld, Out_IsInsideView, Out_WorldPos, Out_WorldNormal);
			}
		)");

		OutHLSL += FString::Format(FormatSample, Args);
	}
	else if (FunctionInfo.DefinitionName == CustomDepthName)
	{
		static const TCHAR* FormatSample = TEXT(R"(
			void {FunctionName}(in float3 In_SamplePos, out float Out_SceneDepth, out float3 Out_CameraPosWorld, out bool Out_IsInsideView, out float3 Out_WorldPos, out float3 Out_WorldNormal)
			{
				DICollisionQuery_CustomDepth(In_SamplePos, Out_SceneDepth, Out_CameraPosWorld, Out_IsInsideView, Out_WorldPos, Out_WorldNormal);
			}
		)");

		OutHLSL += FString::Format(FormatSample, Args);
	}
	else if (FunctionInfo.DefinitionName == DistanceFieldName)
	{
		static const TCHAR* FormatSample = TEXT(R"(
			void {FunctionName}(in float3 In_SamplePos, out float Out_DistanceToNearestSurface, out float3 Out_FieldGradient, out bool Out_IsDistanceFieldValid)
			{
				DICollisionQuery_DistanceField(In_SamplePos, Out_DistanceToNearestSurface, Out_FieldGradient, Out_IsDistanceFieldValid);
			}
		)");

		OutHLSL += FString::Format(FormatSample, Args);
	}
	else if (FunctionInfo.DefinitionName == IssueAsyncRayTraceName)
	{
		static const TCHAR* FormatSample = TEXT(R"(
			void {FunctionName}(int In_QueryID, float3 In_TraceStart, float3 In_TraceEnd, int In_TraceChannel, out bool Out_IsQueryValid)
			{
				DICollisionQuery_IssueAsyncRayTrace(In_QueryID, In_TraceStart, In_TraceEnd, In_TraceChannel, Out_IsQueryValid);
			}
		)");

		OutHLSL += FString::Format(FormatSample, Args);
	}
	else if (FunctionInfo.DefinitionName == ReadAsyncRayTraceName)
	{
		static const TCHAR* FormatSample = TEXT(R"(
			void {FunctionName}(int In_PreviousFrameQueryID, out bool Out_CollisionValid, out float Out_CollisionDistance, out float3 Out_CollisionPosWorld, out float3 Out_CollisionNormal)
			{
				DICollisionQuery_ReadAsyncRayTrace(In_PreviousFrameQueryID, Out_CollisionValid, Out_CollisionDistance, Out_CollisionPosWorld, Out_CollisionNormal);
			}
		)");

		OutHLSL += FString::Format(FormatSample, Args);
	}
	else
	{
		return false;
	}

	return true;
}

#if WITH_EDITORONLY_DATA
bool UNiagaraDataInterfaceCollisionQuery::UpgradeFunctionCall(FNiagaraFunctionSignature& FunctionSignature)
{
	bool bWasChanged = false;

	// The distance field query got a new output at some point, but there exists no custom version for it
	if (FunctionSignature.Name == UNiagaraDataInterfaceCollisionQuery::DistanceFieldName && FunctionSignature.Outputs.Num() == 2)
	{
		FunctionSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("IsDistanceFieldValid")));
		bWasChanged = true;
	}

	// Early out for version matching
	if (FunctionSignature.FunctionVersion == FNiagaraCollisionDIFunctionVersion::LatestVersion)
	{
		return bWasChanged;
	}

	// Added the possibility to skip a line trace to increase performance when only a fraction of particles wants to do a line trace
	if (FunctionSignature.FunctionVersion < FNiagaraCollisionDIFunctionVersion::AddedTraceSkip)
	{
		if (FunctionSignature.Name == UNiagaraDataInterfaceCollisionQuery::SyncTraceName || FunctionSignature.Name == UNiagaraDataInterfaceCollisionQuery::AsyncTraceName)
		{
			FunctionSignature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("SkipTrace")));
			bWasChanged = true;
		}
	}

	// Added the physical material ID as a result for line traces
	if (FunctionSignature.FunctionVersion < FNiagaraCollisionDIFunctionVersion::ReturnCollisionMaterialIdx)
	{
		if (FunctionSignature.Name == SyncTraceName || FunctionSignature.Name == AsyncTraceName)
		{
			FunctionSignature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("CollisionMaterialIndex")));
			bWasChanged = true;
		}
	}

	// Set latest version
	FunctionSignature.FunctionVersion = FNiagaraCollisionDIFunctionVersion::LatestVersion;

	return bWasChanged;
}
#endif

bool IsDistanceFieldEnabled()
{
	static const auto* CVarGenerateMeshDistanceFields = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.GenerateMeshDistanceFields"));
	return CVarGenerateMeshDistanceFields != nullptr && CVarGenerateMeshDistanceFields->GetValueOnAnyThread() > 0;
}

#if WITH_EDITOR
void UNiagaraDataInterfaceCollisionQuery::ValidateFunction(const FNiagaraFunctionSignature& Function, TArray<FText>& OutValidationErrors)
{
	if (Function.Name == DistanceFieldName)
	{
		if (!IsDistanceFieldEnabled())
		{
			OutValidationErrors.Add(NSLOCTEXT("NiagaraDataInterfaceCollisionQuery", "NiagaraDistanceFieldNotEnabledMsg", "The mesh distance field generation is currently not enabled, please check the project settings.\nNiagara cannot query the distance field otherwise."));
		}
	}
}
#endif

bool UNiagaraDataInterfaceCollisionQuery::RequiresRayTracingScene() const
{
	return IsRayTracingEnabled() && MaxRayTraceCount > 0;
}

void UNiagaraDataInterfaceCollisionQuery::GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, FString& OutHLSL)
{
	// we don't need to add these to hlsl, as they're already in common.ush
}

DEFINE_NDI_DIRECT_FUNC_BINDER(UNiagaraDataInterfaceCollisionQuery, PerformQuerySyncCPU);
DEFINE_NDI_DIRECT_FUNC_BINDER(UNiagaraDataInterfaceCollisionQuery, PerformQueryAsyncCPU);
DEFINE_NDI_DIRECT_FUNC_BINDER(UNiagaraDataInterfaceCollisionQuery, QuerySceneDepth);
DEFINE_NDI_DIRECT_FUNC_BINDER(UNiagaraDataInterfaceCollisionQuery, QueryMeshDistanceField);

void UNiagaraDataInterfaceCollisionQuery::GetVMExternalFunction(const FVMExternalFunctionBindingInfo& BindingInfo, void* InstanceData, FVMExternalFunction &OutFunc)
{
	if (BindingInfo.Name == UNiagaraDataInterfaceCollisionQuery::SyncTraceName)
	{
		NDI_FUNC_BINDER(UNiagaraDataInterfaceCollisionQuery, PerformQuerySyncCPU)::Bind(this, OutFunc);
	}
	else if (BindingInfo.Name == UNiagaraDataInterfaceCollisionQuery::AsyncTraceName)
	{
		NDI_FUNC_BINDER(UNiagaraDataInterfaceCollisionQuery, PerformQueryAsyncCPU)::Bind(this, OutFunc);
	}
	else if (BindingInfo.Name == UNiagaraDataInterfaceCollisionQuery::SceneDepthName ||
			 BindingInfo.Name == UNiagaraDataInterfaceCollisionQuery::CustomDepthName)
	{
		NDI_FUNC_BINDER(UNiagaraDataInterfaceCollisionQuery, QuerySceneDepth)::Bind(this, OutFunc);
	}
	else if (BindingInfo.Name == UNiagaraDataInterfaceCollisionQuery::DistanceFieldName)
	{
		NDI_FUNC_BINDER(UNiagaraDataInterfaceCollisionQuery, QueryMeshDistanceField)::Bind(this, OutFunc);
	}
	else
	{
		UE_LOG(LogNiagara, Error, TEXT("Could not find data interface external function. %s\n"),
			*BindingInfo.Name.ToString());
	}
}

void UNiagaraDataInterfaceCollisionQuery::GetCommonHLSL(FString& OutHlsl)
{
	OutHlsl += TEXT("#include \"/Plugin/FX/Niagara/Private/NiagaraDataInterfaceCollisionQuery.ush\"\n");
}

bool UNiagaraDataInterfaceCollisionQuery::AppendCompileHash(FNiagaraCompileHashVisitor* InVisitor) const
{
	if (!Super::AppendCompileHash(InVisitor))
	{
		return false;
	}
	bool bDistanceFieldEnabled = IsDistanceFieldEnabled();
	InVisitor->UpdatePOD(TEXT("NiagaraCollisionDI_DistanceField"), bDistanceFieldEnabled);

	FSHAHash Hash = GetShaderFileHash(TEXT("/Plugin/FX/Niagara/Private/NiagaraDataInterfaceCollisionQuery.ush"), EShaderPlatform::SP_PCD3D_SM5);
	InVisitor->UpdateString(TEXT("NiagaraDataInterfaceCollisionQueryHlslSource"), Hash.ToString());
	return true;
}

void UNiagaraDataInterfaceCollisionQuery::PerformQuerySyncCPU(FVectorVMContext & Context)
{
	VectorVM::FUserPtrHandler<CQDIPerInstanceData> InstanceData(Context);

	VectorVM::FExternalFuncInputHandler<float> StartPosParamX(Context);
	VectorVM::FExternalFuncInputHandler<float> StartPosParamY(Context);
	VectorVM::FExternalFuncInputHandler<float> StartPosParamZ(Context);

	VectorVM::FExternalFuncInputHandler<float> EndPosParamX(Context);
	VectorVM::FExternalFuncInputHandler<float> EndPosParamY(Context);
	VectorVM::FExternalFuncInputHandler<float> EndPosParamZ(Context);

	VectorVM::FExternalFuncInputHandler<ECollisionChannel> TraceChannelParam(Context);

	VectorVM::FExternalFuncInputHandler<FNiagaraBool> IsSkipTrace(Context);

	VectorVM::FExternalFuncRegisterHandler<FNiagaraBool> OutQueryValid(Context);
	VectorVM::FExternalFuncRegisterHandler<FNiagaraBool> OutInsideMesh(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutCollisionPosX(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutCollisionPosY(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutCollisionPosZ(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutCollisionNormX(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutCollisionNormY(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutCollisionNormZ(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutFriction(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutRestitution(Context);
	VectorVM::FExternalFuncRegisterHandler<int32> OutPhysicalMaterialIdx(Context);

	FScopeLock ScopeLock(&CriticalSection);
	for (int32 i = 0; i < Context.NumInstances; ++i)
	{
		FVector Pos(StartPosParamX.GetAndAdvance(), StartPosParamY.GetAndAdvance(), StartPosParamZ.GetAndAdvance());
		FVector Dir(EndPosParamX.GetAndAdvance(), EndPosParamY.GetAndAdvance(), EndPosParamZ.GetAndAdvance());
		ECollisionChannel TraceChannel = TraceChannelParam.GetAndAdvance();
		bool Skip = IsSkipTrace.GetAndAdvance().GetValue();
		ensure(!Pos.ContainsNaN());
		FNiagaraDICollsionQueryResult Res;

		if (!Skip && InstanceData->CollisionBatch.PerformQuery(Pos, Dir, Res, TraceChannel))
		{
			*OutQueryValid.GetDestAndAdvance() = FNiagaraBool(true);
			*OutInsideMesh.GetDestAndAdvance() = FNiagaraBool(Res.IsInsideMesh);
			*OutCollisionPosX.GetDestAndAdvance() = Res.CollisionPos.X;
			*OutCollisionPosY.GetDestAndAdvance() = Res.CollisionPos.Y;
			*OutCollisionPosZ.GetDestAndAdvance() = Res.CollisionPos.Z;
			*OutCollisionNormX.GetDestAndAdvance() = Res.CollisionNormal.X;
			*OutCollisionNormY.GetDestAndAdvance() = Res.CollisionNormal.Y;
			*OutCollisionNormZ.GetDestAndAdvance() = Res.CollisionNormal.Z;
			*OutFriction.GetDestAndAdvance() = Res.Friction;
			*OutRestitution.GetDestAndAdvance() = Res.Restitution;
			*OutPhysicalMaterialIdx.GetDestAndAdvance() = Res.PhysicalMaterialIdx;
		}
		else
		{
			*OutQueryValid.GetDestAndAdvance() = FNiagaraBool();
			*OutInsideMesh.GetDestAndAdvance() = FNiagaraBool();
			*OutCollisionPosX.GetDestAndAdvance() = 0.0f;
			*OutCollisionPosY.GetDestAndAdvance() = 0.0f;
			*OutCollisionPosZ.GetDestAndAdvance() = 0.0f;
			*OutCollisionNormX.GetDestAndAdvance() = 0.0f;
			*OutCollisionNormY.GetDestAndAdvance() = 0.0f;
			*OutCollisionNormZ.GetDestAndAdvance() = 0.0f;
			*OutFriction.GetDestAndAdvance() = 0.0f;
			*OutRestitution.GetDestAndAdvance() = 0.0f;
			*OutPhysicalMaterialIdx.GetDestAndAdvance() = 0;
		}
	}
}

void UNiagaraDataInterfaceCollisionQuery::PerformQueryAsyncCPU(FVectorVMContext & Context)
{
	VectorVM::FUserPtrHandler<CQDIPerInstanceData> InstanceData(Context);

	VectorVM::FExternalFuncInputHandler<int32> InIDParam(Context);
	VectorVM::FExternalFuncInputHandler<float> StartPosParamX(Context);
	VectorVM::FExternalFuncInputHandler<float> StartPosParamY(Context);
	VectorVM::FExternalFuncInputHandler<float> StartPosParamZ(Context);

	VectorVM::FExternalFuncInputHandler<float> EndPosParamX(Context);
	VectorVM::FExternalFuncInputHandler<float> EndPosParamY(Context);
	VectorVM::FExternalFuncInputHandler<float> EndPosParamZ(Context);

	VectorVM::FExternalFuncInputHandler<ECollisionChannel> TraceChannelParam(Context);

	VectorVM::FExternalFuncInputHandler<FNiagaraBool> IsSkipTrace(Context);

	VectorVM::FExternalFuncRegisterHandler<int32> OutQueryID(Context);

	VectorVM::FExternalFuncRegisterHandler<FNiagaraBool> OutQueryValid(Context);
	VectorVM::FExternalFuncRegisterHandler<FNiagaraBool> OutInsideMesh(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutCollisionPosX(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutCollisionPosY(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutCollisionPosZ(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutCollisionNormX(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutCollisionNormY(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutCollisionNormZ(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutFriction(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutRestitution(Context);
	VectorVM::FExternalFuncRegisterHandler<int32> OutPhysicalMaterialIdx(Context);

	FScopeLock ScopeLock(&CriticalSection);
	for (int32 i = 0; i < Context.NumInstances; ++i)
	{
		FVector Pos(StartPosParamX.GetAndAdvance(), StartPosParamY.GetAndAdvance(), StartPosParamZ.GetAndAdvance());
		FVector End(EndPosParamX.GetAndAdvance(), EndPosParamY.GetAndAdvance(), EndPosParamZ.GetAndAdvance());
		ECollisionChannel TraceChannel = TraceChannelParam.GetAndAdvance();
		bool Skip = IsSkipTrace.GetAndAdvance().GetValue();
		ensure(!Pos.ContainsNaN());

		*OutQueryID.GetDestAndAdvance() = Skip ? INDEX_NONE : InstanceData->CollisionBatch.SubmitQuery(Pos, End, TraceChannel);

		// try to retrieve a query with the supplied query ID
		FNiagaraDICollsionQueryResult Res;
		int32 ID = InIDParam.GetAndAdvance();
		if (ID != INDEX_NONE && InstanceData->CollisionBatch.GetQueryResult(ID, Res))
		{
			*OutQueryValid.GetDestAndAdvance() = FNiagaraBool(true);
			*OutInsideMesh.GetDestAndAdvance() = FNiagaraBool(Res.IsInsideMesh);
			*OutCollisionPosX.GetDestAndAdvance() = Res.CollisionPos.X;
			*OutCollisionPosY.GetDestAndAdvance() = Res.CollisionPos.Y;
			*OutCollisionPosZ.GetDestAndAdvance() = Res.CollisionPos.Z;
			*OutCollisionNormX.GetDestAndAdvance() = Res.CollisionNormal.X;
			*OutCollisionNormY.GetDestAndAdvance() = Res.CollisionNormal.Y;
			*OutCollisionNormZ.GetDestAndAdvance() = Res.CollisionNormal.Z;
			*OutFriction.GetDestAndAdvance() = Res.Friction;
			*OutRestitution.GetDestAndAdvance() = Res.Restitution;
			*OutPhysicalMaterialIdx.GetDestAndAdvance() = Res.PhysicalMaterialIdx;
		}
		else
		{
			*OutQueryValid.GetDestAndAdvance() = FNiagaraBool();
			*OutInsideMesh.GetDestAndAdvance() = FNiagaraBool();
			*OutCollisionPosX.GetDestAndAdvance() = 0.0f;
			*OutCollisionPosY.GetDestAndAdvance() = 0.0f;
			*OutCollisionPosZ.GetDestAndAdvance() = 0.0f;
			*OutCollisionNormX.GetDestAndAdvance() = 0.0f;
			*OutCollisionNormY.GetDestAndAdvance() = 0.0f;
			*OutCollisionNormZ.GetDestAndAdvance() = 0.0f;
			*OutFriction.GetDestAndAdvance() = 0.0f;
			*OutRestitution.GetDestAndAdvance() = 0.0f;
			*OutPhysicalMaterialIdx.GetDestAndAdvance() = 0;
		}
	}
}

void UNiagaraDataInterfaceCollisionQuery::QuerySceneDepth(FVectorVMContext & Context)
{
	UE_LOG(LogNiagara, Error, TEXT("GPU only function 'QuerySceneDepthGPU' called on CPU VM, check your module code to fix."));

	VectorVM::FUserPtrHandler<CQDIPerInstanceData> InstanceData(Context);

	VectorVM::FExternalFuncInputHandler<float> SamplePosParamX(Context);
	VectorVM::FExternalFuncInputHandler<float> SamplePosParamY(Context);
	VectorVM::FExternalFuncInputHandler<float> SamplePosParamZ(Context);
	
	VectorVM::FExternalFuncRegisterHandler<float> OutSceneDepth(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutCameraPosX(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutCameraPosY(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutCameraPosZ(Context);
	VectorVM::FExternalFuncRegisterHandler<int32> OutIsInsideView(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutWorldPosX(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutWorldPosY(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutWorldPosZ(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutWorldNormX(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutWorldNormY(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutWorldNormZ(Context);

	FScopeLock ScopeLock(&CriticalSection);
	for (int32 i = 0; i < Context.NumInstances; ++i)
	{
		*OutSceneDepth.GetDestAndAdvance() = -1;
		*OutIsInsideView.GetDestAndAdvance() = 0;
		*OutWorldPosX.GetDestAndAdvance() = 0.0f;
		*OutWorldPosY.GetDestAndAdvance() = 0.0f;
		*OutWorldPosZ.GetDestAndAdvance() = 0.0f;
		*OutWorldNormX.GetDestAndAdvance() = 0.0f;
		*OutWorldNormY.GetDestAndAdvance() = 0.0f;
		*OutWorldNormZ.GetDestAndAdvance() = 1.0f;
		*OutCameraPosX.GetDestAndAdvance() = 0.0f;
		*OutCameraPosY.GetDestAndAdvance() = 0.0f;
		*OutCameraPosZ.GetDestAndAdvance() = 0.0f;
	}
}

void UNiagaraDataInterfaceCollisionQuery::QueryMeshDistanceField(FVectorVMContext& Context)
{
	UE_LOG(LogNiagara, Error, TEXT("GPU only function 'QueryMeshDistanceFieldGPU' called on CPU VM, check your module code to fix."));

	VectorVM::FUserPtrHandler<CQDIPerInstanceData> InstanceData(Context);

	VectorVM::FExternalFuncInputHandler<float> SamplePosParamX(Context);
	VectorVM::FExternalFuncInputHandler<float> SamplePosParamY(Context);
	VectorVM::FExternalFuncInputHandler<float> SamplePosParamZ(Context);

	VectorVM::FExternalFuncRegisterHandler<float> OutSurfaceDistance(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutFieldGradientX(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutFieldGradientY(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutFieldGradientZ(Context);
	FNDIOutputParam<FNiagaraBool> OutIsFieldValid(Context);

	FScopeLock ScopeLock(&CriticalSection);
	for (int32 i = 0; i < Context.NumInstances; ++i)
	{
		*OutSurfaceDistance.GetDestAndAdvance() = -1;
		*OutFieldGradientX.GetDestAndAdvance() = 0.0f;
		*OutFieldGradientY.GetDestAndAdvance() = 0.0f;
		*OutFieldGradientZ.GetDestAndAdvance() = 1.0f;
		OutIsFieldValid.SetAndAdvance(FNiagaraBool());
	}
}

bool UNiagaraDataInterfaceCollisionQuery::PerInstanceTick(void* PerInstanceData, FNiagaraSystemInstance* InSystemInstance, float DeltaSeconds)
{
	CQDIPerInstanceData* PIData = static_cast<CQDIPerInstanceData*>(PerInstanceData);
	PIData->CollisionBatch.CollectResults();

	return false;
}

bool UNiagaraDataInterfaceCollisionQuery::PerInstanceTickPostSimulate(void* PerInstanceData, FNiagaraSystemInstance* InSystemInstance, float DeltaSeconds)
{
	CQDIPerInstanceData* PIData = static_cast<CQDIPerInstanceData*>(PerInstanceData);
	PIData->CollisionBatch.DispatchQueries();
	PIData->CollisionBatch.ClearWrite();
	return false;
}

bool UNiagaraDataInterfaceCollisionQuery::Equals(const UNiagaraDataInterface* Other) const
{
	if (!Super::Equals(Other))
	{
		return false;
	}

	const UNiagaraDataInterfaceCollisionQuery* OtherTyped = CastChecked<const UNiagaraDataInterfaceCollisionQuery>(Other);
	return OtherTyped->MaxRayTraceCount == MaxRayTraceCount;
}

bool UNiagaraDataInterfaceCollisionQuery::CopyToInternal(UNiagaraDataInterface* Destination) const
{
	if (!Super::CopyToInternal(Destination))
	{
		return false;
	}

	UNiagaraDataInterfaceCollisionQuery* OtherTyped = CastChecked<UNiagaraDataInterfaceCollisionQuery>(Destination);
	OtherTyped->MaxRayTraceCount = MaxRayTraceCount;
	OtherTyped->MarkRenderDataDirty();
	return true;
}

void UNiagaraDataInterfaceCollisionQuery::PushToRenderThreadImpl()
{
	if (!GSupportsResourceView)
	{
		return;
	}

	FNiagaraDataIntefaceProxyCollisionQuery* RT_Proxy = GetProxyAs<FNiagaraDataIntefaceProxyCollisionQuery>();

	// Push Updates to Proxy, first release any resources
	ENQUEUE_RENDER_COMMAND(FUpdateDI)(
		[RT_Proxy, RT_MaxRayTraceRequests = MaxRayTraceCount](FRHICommandListImmediate& RHICmdList)
		{
			RT_Proxy->RenderThreadInitialize(RT_MaxRayTraceRequests);
		});
}

#if WITH_EDITOR
void UNiagaraDataInterfaceCollisionQuery::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.Property && PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(UNiagaraDataInterfaceCollisionQuery, MaxRayTraceCount))
	{
		MarkRenderDataDirty();
	}
}
#endif

//////////////////////////////////////////////////////////////////////////

struct FNiagaraDataInterfaceParametersCS_CollisionQuery : public FNiagaraDataInterfaceParametersCS
{
	DECLARE_TYPE_LAYOUT(FNiagaraDataInterfaceParametersCS_CollisionQuery, NonVirtual);
public:
	void Bind(const FNiagaraDataInterfaceGPUParamInfo& ParameterInfo, const class FShaderParameterMap& ParameterMap)
	{
		GlobalDistanceFieldParameters.Bind(ParameterMap);
#if RHI_RAYTRACING
		MaxRayTraceCountParam.Bind(ParameterMap, TEXT("MaxRayTraceCount"));
		RayRequestsParam.Bind(ParameterMap, TEXT("RayRequests"));
		IntersectionResultsParam.Bind(ParameterMap, TEXT("IntersectionResults"));
#endif
	}

	void Set(FRHICommandList& RHICmdList, const FNiagaraDataInterfaceSetArgs& Context) const
	{
		check(IsInRenderingThread());

		FNiagaraDataIntefaceProxyCollisionQuery* QueryDI = (FNiagaraDataIntefaceProxyCollisionQuery*)Context.DataInterface;
		FRHIComputeShader* ComputeShaderRHI = Context.Shader.GetComputeShader();

		if (GlobalDistanceFieldParameters.IsBound() && Context.Batcher)
		{
			GlobalDistanceFieldParameters.Set(RHICmdList, ComputeShaderRHI, Context.Batcher->GetGlobalDistanceFieldParameters());
		}

#if RHI_RAYTRACING
		SetShaderValue(RHICmdList, ComputeShaderRHI, MaxRayTraceCountParam, QueryDI->MaxRayTraceCount);

		if (RayRequestsParam.IsUAVBound())
		{
			RHICmdList.SetUAVParameter(ComputeShaderRHI, RayRequestsParam.GetUAVIndex(), QueryDI->RayTraceRequests.UAV);
		}

		if (IntersectionResultsParam.IsBound())
		{
			SetSRVParameter(RHICmdList, ComputeShaderRHI, IntersectionResultsParam, QueryDI->RayTraceIntersections.SRV);
		}
#endif
	}

#if RHI_RAYTRACING
	void Unset(FRHICommandList& RHICmdList, const FNiagaraDataInterfaceSetArgs& Context) const
	{
		FRHIComputeShader* ComputeShaderRHI = Context.Shader.GetComputeShader();
		FNiagaraDataIntefaceProxyCollisionQuery* QueryDI = (FNiagaraDataIntefaceProxyCollisionQuery*)Context.DataInterface;

		if (RayRequestsParam.IsUAVBound())
		{
			RayRequestsParam.UnsetUAV(RHICmdList, ComputeShaderRHI);
		}
	}
#endif

private:
	LAYOUT_FIELD(FGlobalDistanceFieldParameters, GlobalDistanceFieldParameters);

#if RHI_RAYTRACING
	LAYOUT_FIELD(FShaderParameter, MaxRayTraceCountParam);
	LAYOUT_FIELD(FRWShaderParameter, RayRequestsParam);
	LAYOUT_FIELD(FShaderResourceParameter, IntersectionResultsParam);
#endif
};

IMPLEMENT_TYPE_LAYOUT(FNiagaraDataInterfaceParametersCS_CollisionQuery);

IMPLEMENT_NIAGARA_DI_PARAMETER(UNiagaraDataInterfaceCollisionQuery, FNiagaraDataInterfaceParametersCS_CollisionQuery);
