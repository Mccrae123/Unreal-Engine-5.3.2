// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraEmitterInstanceBatcher.h"
#include "NiagaraScriptExecutionContext.h"
#include "RHI.h"
#include "RHIGPUReadback.h"
#include "NiagaraStats.h"
#include "NiagaraShader.h"
#include "NiagaraSortingGPU.h"
#include "NiagaraWorldManager.h"
#include "NiagaraShaderParticleID.h"
#include "NiagaraRenderer.h"
#include "ShaderParameterUtils.h"
#include "SceneUtils.h"
#include "ShaderParameterUtils.h"
#include "ClearQuad.h"
#include "Runtime/Engine/Private/GPUSort.h"

DECLARE_CYCLE_STAT(TEXT("Niagara Dispatch Setup"), STAT_NiagaraGPUDispatchSetup_RT, STATGROUP_Niagara);
DECLARE_CYCLE_STAT(TEXT("GPU Emitter Dispatch [RT]"), STAT_NiagaraGPUSimTick_RT, STATGROUP_Niagara);
DECLARE_CYCLE_STAT(TEXT("GPU Data Readback [RT]"), STAT_NiagaraGPUReadback_RT, STATGROUP_Niagara);
DECLARE_FLOAT_COUNTER_STAT(TEXT("Niagara GPU Sim"), STAT_GPU_NiagaraSim, STATGROUP_GPU);
DECLARE_DWORD_COUNTER_STAT(TEXT("# GPU Particles"), STAT_NiagaraGPUParticles, STATGROUP_Niagara);
DECLARE_DWORD_COUNTER_STAT(TEXT("# GPU Sorted Particles"), STAT_NiagaraGPUSortedParticles, STATGROUP_Niagara);
DECLARE_DWORD_COUNTER_STAT(TEXT("# GPU Sorted Buffers"), STAT_NiagaraGPUSortedBuffers, STATGROUP_Niagara);
DECLARE_DWORD_COUNTER_STAT(TEXT("Readback latency (frames)"), STAT_NiagaraReadbackLatency, STATGROUP_Niagara);

DECLARE_GPU_STAT_NAMED(NiagaraGPU, TEXT("Niagara"));
DECLARE_GPU_STAT_NAMED(NiagaraGPUSimulation, TEXT("Niagara GPU Simulation"));
DECLARE_GPU_STAT_NAMED(NiagaraGPUClearIDTables, TEXT("NiagaraGPU Clear ID Tables"));
DECLARE_GPU_STAT_NAMED(NiagaraGPUComputeFreeIDs, TEXT("Niagara GPU Compute All Free IDs"));
DECLARE_GPU_STAT_NAMED(NiagaraGPUComputeFreeIDsEmitter, TEXT("Niagara GPU Compute Emitter Free IDs"));
DECLARE_GPU_STAT_NAMED(NiagaraGPUSorting, TEXT("Niagara GPU sorting"));

uint32 FNiagaraComputeExecutionContext::TickCounter = 0;

int32 GNiagaraAllowTickBeforeRender = 1;
static FAutoConsoleVariableRef CVarNiagaraAllowTickBeforeRender(
	TEXT("fx.NiagaraAllowTickBeforeRender"),
	GNiagaraAllowTickBeforeRender,
	TEXT("If 1, Niagara GPU systems that don't rely on view data will be rendered in sync\n")
	TEXT("with the current frame simulation instead of the last frame one. (default=1)\n"),
	ECVF_Default
);

int32 GNiagaraOverlapCompute = 1;
static FAutoConsoleVariableRef CVarNiagaraUseAsyncCompute(
	TEXT("fx.NiagaraOverlapCompute"),
	GNiagaraOverlapCompute,
	TEXT("0 - Disable compute dispatch overlap, this will result in poor performance due to resource barriers between each dispatch call, but can be used to debug resource transition issues.\n")
	TEXT("1 - (Default) Enable compute dispatch overlap where possible, this increases GPU utilization.\n"),
	ECVF_Default
);

int32 GNiagaraSubmitCommands = 0;
static FAutoConsoleVariableRef CVarNiagaraSubmitCommands(
	TEXT("fx.NiagaraSubmitCommands"),
	GNiagaraSubmitCommands,
	TEXT("1 - (Default) Submit commands to the GPU once we have finished dispatching.\n"),
	ECVF_Default
);

// @todo REMOVE THIS HACK
int32 GNiagaraGpuMaxQueuedRenderFrames = 10;
static FAutoConsoleVariableRef CVarNiagaraGpuMaxQueuedRenderFrames(
	TEXT("fx.NiagaraGpuMaxQueuedRenderFrames"),
	GNiagaraGpuMaxQueuedRenderFrames,
	TEXT("Number of frames we all to pass before we start to discard GPU ticks.\n"),
	ECVF_Default
);

const FName NiagaraEmitterInstanceBatcher::Name(TEXT("NiagaraEmitterInstanceBatcher"));

FFXSystemInterface* NiagaraEmitterInstanceBatcher::GetInterface(const FName& InName)
{
	return InName == Name ? this : nullptr;
}

NiagaraEmitterInstanceBatcher::NiagaraEmitterInstanceBatcher(ERHIFeatureLevel::Type InFeatureLevel, EShaderPlatform InShaderPlatform, FGPUSortManager* InGPUSortManager)
	: FeatureLevel(InFeatureLevel)
	, ShaderPlatform(InShaderPlatform)
	, GPUSortManager(InGPUSortManager)
	// @todo REMOVE THIS HACK
	, LastFrameThatDrainedData(GFrameNumberRenderThread)
	, NumAllocatedFreeIDListSizes(0)
	, bFreeIDListSizesBufferCleared(false)
{
	// Register the batcher callback in the GPUSortManager. 
	// The callback is used to generate the initial keys and values for the GPU sort tasks, 
	// the values being the sorted particle indices used by the Niagara renderers.
	// The registration also involves defining the list of flags possibly used in GPUSortManager::AddTask()
	if (GPUSortManager)
	{
		GPUSortManager->Register(FGPUSortKeyGenDelegate::CreateLambda([this](FRHICommandListImmediate& RHICmdList, int32 BatchId, int32 NumElementsInBatch, EGPUSortFlags Flags, FRHIUnorderedAccessView* KeysUAV, FRHIUnorderedAccessView* ValuesUAV)
		{ 
			GenerateSortKeys(RHICmdList, BatchId, NumElementsInBatch, Flags, KeysUAV, ValuesUAV);
		}), 
		EGPUSortFlags::AnyKeyPrecision | EGPUSortFlags::KeyGenAfterPreRender | EGPUSortFlags::AnySortLocation | EGPUSortFlags::ValuesAsInt32,
		Name);
	}
}

NiagaraEmitterInstanceBatcher::~NiagaraEmitterInstanceBatcher()
{
	FinishDispatches();
}

void NiagaraEmitterInstanceBatcher::InstanceDeallocated_RenderThread(const FNiagaraSystemInstanceID InstanceID)
{
	int iTick = 0;
	while ( iTick < Ticks_RT.Num() )
	{
		FNiagaraGPUSystemTick& Tick = Ticks_RT[iTick];
		if (Tick.SystemInstanceID == InstanceID)
		{
			//-OPT: Since we can't RemoveAtSwap (due to ordering issues) if may be better to not remove and flag as dead
			Tick.Destroy();
			Ticks_RT.RemoveAt(iTick);
		}
		else
		{
			++iTick;
		}
	}
}

void NiagaraEmitterInstanceBatcher::GiveSystemTick_RenderThread(FNiagaraGPUSystemTick& Tick)
{
	check(IsInRenderingThread());

	// @todo REMOVE THIS HACK
	if (GFrameNumberRenderThread > LastFrameThatDrainedData + GNiagaraGpuMaxQueuedRenderFrames)
	{
		Tick.Destroy();
		return;
	}

	// Now we consume DataInterface instance data.
	if (Tick.DIInstanceData)
	{
		uint8* BasePointer = (uint8*) Tick.DIInstanceData->PerInstanceDataForRT;

		//UE_LOG(LogNiagara, Log, TEXT("RT Give DI (dipacket) %p (baseptr) %p"), Tick.DIInstanceData, BasePointer);
		for(auto& Pair : Tick.DIInstanceData->InterfaceProxiesToOffsets)
		{
			FNiagaraDataInterfaceProxy* Proxy = Pair.Key;
			uint8* InstanceDataPtr = BasePointer + Pair.Value;

			//UE_LOG(LogNiagara, Log, TEXT("\tRT DI (proxy) %p (instancebase) %p"), Proxy, InstanceDataPtr);
			Proxy->ConsumePerInstanceDataFromGameThread(InstanceDataPtr, Tick.SystemInstanceID);
		}
	}

	// A note:
	// This is making a copy of Tick. That structure is small now and we take a copy to avoid
	// making a bunch of small allocations on the game thread. We may need to revisit this.
	Ticks_RT.Add(Tick);
}

void NiagaraEmitterInstanceBatcher::ReleaseInstanceCounts_RenderThread(FNiagaraComputeExecutionContext* ExecContext, FNiagaraDataSet* DataSet)
{
	LLM_SCOPE(ELLMTag::Niagara);

	if ( ExecContext != nullptr )
	{
		GPUInstanceCounterManager.FreeEntry(ExecContext->EmitterInstanceReadback.GPUCountOffset);
	}
	if ( DataSet != nullptr )
	{
		DataSet->ReleaseGPUInstanceCounts(GPUInstanceCounterManager);
	}
}

void NiagaraEmitterInstanceBatcher::FinishDispatches()
{
	ReleaseTicks();
}

void NiagaraEmitterInstanceBatcher::ReleaseTicks()
{
	check(IsInRenderingThread());

	for (FNiagaraGPUSystemTick& Tick : Ticks_RT)
	{
		Tick.Destroy();
	}

	Ticks_RT.Empty(0);
}

bool NiagaraEmitterInstanceBatcher::ResetDataInterfaces(const FNiagaraGPUSystemTick& Tick, FNiagaraComputeInstanceData *Instance, FRHICommandList &RHICmdList, FNiagaraShader* ComputeShader ) const
{
	bool ValidSpawnStage = true;
	FNiagaraComputeExecutionContext* Context = Instance->Context;

	// Reset all rw data interface data
	if (Tick.bNeedsReset)
	{
		uint32 InterfaceIndex = 0;
		for (FNiagaraDataInterfaceProxy* Interface : Instance->DataInterfaceProxies)
		{
			FNiagaraDataInterfaceParamRef& DIParam = ComputeShader->GetDIParameters()[InterfaceIndex];
			if (DIParam.Parameters)
			{
				FNiagaraDataInterfaceSetArgs TmpContext;
				TmpContext.Shader = ComputeShader;
				TmpContext.DataInterface = Interface;
				TmpContext.SystemInstance = Tick.SystemInstanceID;
				TmpContext.Batcher = this;		
				Interface->ResetData(RHICmdList, TmpContext);
			}			
			InterfaceIndex++;
		}
	}
	return ValidSpawnStage;
}

FNiagaraDataInterfaceProxy* NiagaraEmitterInstanceBatcher::FindIterationInterface( FNiagaraComputeInstanceData *Instance, const uint32 ShaderStageIndex) const
{
	// Determine if the iteration is outputting to a custom data size
	FNiagaraDataInterfaceProxy* IterationInterface = nullptr;

	for (FNiagaraDataInterfaceProxy* Interface : Instance->DataInterfaceProxies)
	{
		if (Interface->IsIterationStage(ShaderStageIndex))
		{
			if (IterationInterface)
			{
				UE_LOG(LogNiagara, Error, TEXT("Multiple output Data Interfaces found for current stage"));
			}
			else
			{
				IterationInterface = Interface;
			}
		}
	}
	return IterationInterface;
}

void NiagaraEmitterInstanceBatcher::PreStageInterface(const FNiagaraGPUSystemTick& Tick, FNiagaraComputeInstanceData *Instance, FRHICommandList &RHICmdList, FNiagaraShader* ComputeShader, const uint32 ShaderStageIndex) const
{
	uint32 InterfaceIndex = 0;
	for (FNiagaraDataInterfaceProxy* Interface : Instance->DataInterfaceProxies)
	{
		FNiagaraDataInterfaceParamRef& DIParam = ComputeShader->GetDIParameters()[InterfaceIndex];
		if (DIParam.Parameters)
		{
			FNiagaraDataInterfaceSetArgs TmpContext;
			TmpContext.Shader = ComputeShader;
			TmpContext.DataInterface = Interface;
			TmpContext.SystemInstance = Tick.SystemInstanceID;
			TmpContext.Batcher = this;
			TmpContext.ShaderStageIndex = ShaderStageIndex;
			TmpContext.IsOutputStage = Interface->IsOutputStage(ShaderStageIndex);
			TmpContext.IsIterationStage = Interface->IsIterationStage(ShaderStageIndex);
			Interface->PreStage(RHICmdList, TmpContext);
		}
		InterfaceIndex++;
	}
}

void NiagaraEmitterInstanceBatcher::PostStageInterface(const FNiagaraGPUSystemTick& Tick, FNiagaraComputeInstanceData *Instance, FRHICommandList &RHICmdList, FNiagaraShader* ComputeShader, const uint32 ShaderStageIndex) const
{
	uint32 InterfaceIndex = 0;
	for (FNiagaraDataInterfaceProxy* Interface : Instance->DataInterfaceProxies)
	{
		FNiagaraDataInterfaceParamRef& DIParam = ComputeShader->GetDIParameters()[InterfaceIndex];
		if (DIParam.Parameters)
		{
			FNiagaraDataInterfaceSetArgs TmpContext;
			TmpContext.Shader = ComputeShader;
			TmpContext.DataInterface = Interface;
			TmpContext.SystemInstance = Tick.SystemInstanceID;
			TmpContext.Batcher = this;
			TmpContext.ShaderStageIndex = ShaderStageIndex;
			TmpContext.IsOutputStage = Interface->IsOutputStage(ShaderStageIndex);
			TmpContext.IsIterationStage = Interface->IsIterationStage(ShaderStageIndex);
			Interface->PostStage(RHICmdList, TmpContext);
		}
		InterfaceIndex++;
	}
}

void NiagaraEmitterInstanceBatcher::DispatchMultipleStages(const FNiagaraGPUSystemTick& Tick, FNiagaraComputeInstanceData *Instance, FRHICommandList &RHICmdList, FRHIUniformBuffer* ViewUniformBuffer, FNiagaraShader* ComputeShader)
{
	FNiagaraComputeExecutionContext* Context = Instance->Context;

	if (!ResetDataInterfaces(Tick, Instance, RHICmdList, ComputeShader)) return;

	static const auto UseShaderStagesCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("fx.UseShaderStages"));
	if (UseShaderStagesCVar->GetInt() == 1)
	{

		bool HasRunParticleStage = false;

		FNiagaraDataBuffer* CurrentData = Instance->CurrentData;
		FNiagaraDataBuffer* DestinationData = Instance->DestinationData;

		const uint32 NumStages = Instance->Context->MaxUpdateIterations;
		const uint32 DefaultShaderStageIndex = Instance->Context->DefaultShaderStageIndex;
		bool bTransitionCurrentBuffer = false;

		for (uint32 ShaderStageIndex = 0; ShaderStageIndex < NumStages; ++ShaderStageIndex)
		{
			// Determine if the iteration is outputting to a custom data size
			FNiagaraDataInterfaceProxy *IterationInterface = FindIterationInterface(Instance, ShaderStageIndex);

			if (IterationInterface && Context->SpawnStages.Num() > 0 &&
				((Tick.bNeedsReset && !Context->SpawnStages.Contains(ShaderStageIndex)) ||
				(!Tick.bNeedsReset && Context->SpawnStages.Contains(ShaderStageIndex))))
			{
				continue;
			}

			PreStageInterface(Tick, Instance, RHICmdList, ComputeShader, ShaderStageIndex);

			// If we are reading from current data we need to transition the resource if it was previously written to
			if (bTransitionCurrentBuffer && (ComputeShader->FloatInputBufferParam.IsBound() || ComputeShader->IntInputBufferParam.IsBound()))
			{
				bTransitionCurrentBuffer = false;

				TArray<FRHIUnorderedAccessView*, TInlineAllocator<2>> Resources;
				if (Instance->CurrentData->GetGPUBufferFloat().UAV.IsValid())
				{
					Resources.Add(Instance->CurrentData->GetGPUBufferFloat().UAV);
				}
				if (Instance->CurrentData->GetGPUBufferInt().UAV.IsValid())
				{
					Resources.Add(Instance->CurrentData->GetGPUBufferInt().UAV);
				}
				if (Resources.Num() > 0)
				{
					RHICmdList.TransitionResources(EResourceTransitionAccess::ERWBarrier, EResourceTransitionPipeline::EComputeToCompute, Resources.GetData(), Resources.Num());
				}
			}

			if (!IterationInterface)
			{
				Run(Tick, Instance, 0, Instance->DestinationData->GetNumInstances(), ComputeShader, RHICmdList, ViewUniformBuffer, Instance->SpawnInfo, false, DefaultShaderStageIndex, ShaderStageIndex,  nullptr, HasRunParticleStage);
				HasRunParticleStage = true;
				bTransitionCurrentBuffer = true;

				FNiagaraDataBuffer* StoreData = Instance->CurrentData;
				Instance->CurrentData = Instance->DestinationData;
				Instance->DestinationData = StoreData;
			}
			else
			{
				// run with correct number of instances.  This will make curr data junk or empty
				Run(Tick, Instance, 0, IterationInterface->ElementCount, ComputeShader, RHICmdList, ViewUniformBuffer, Instance->SpawnInfo, false, DefaultShaderStageIndex, ShaderStageIndex, IterationInterface);
			}
			PostStageInterface(Tick, Instance, RHICmdList, ComputeShader, ShaderStageIndex);
		}
		Instance->CurrentData = CurrentData;
		Instance->DestinationData = DestinationData;
	}
	else
	{
		// run shader, sim and spawn in a single dispatch
		Run(Tick, Instance, 0, Instance->DestinationData->GetNumInstances(), ComputeShader, RHICmdList, ViewUniformBuffer, Instance->SpawnInfo);
	}
}

void NiagaraEmitterInstanceBatcher::ResizeBuffersAndGatherResources(FOverlappableTicks& OverlappableTick, FRHICommandList& RHICmdList, FNiagaraBufferArray& ReadBuffers, FNiagaraBufferArray& WriteBuffers, FNiagaraBufferArray& OutputGraphicsBuffers, FEmitterInstanceList& InstancesWithPersistentIDs)
{
	SCOPE_CYCLE_COUNTER(STAT_NiagaraGPUDispatchSetup_RT);

	//UE_LOG(LogNiagara, Warning, TEXT("NiagaraEmitterInstanceBatcher::ResizeBuffersAndGatherResources:  %0xP"), this);
	for (FNiagaraGPUSystemTick* Tick : OverlappableTick)
	{
		const uint32 DispatchCount = Tick->Count;
		const bool bIsFinalTick = Tick->bIsFinalTick;
		const bool bNeedsReset = Tick->bNeedsReset;

		FNiagaraComputeInstanceData* Instances = Tick->GetInstanceData();
		for (uint32 Index = 0; Index < DispatchCount; Index++)
		{
			FNiagaraComputeInstanceData& Instance = Instances[Index];
			FNiagaraComputeExecutionContext* Context = Instance.Context;
			if ( Context == nullptr )
			{
				continue;
			}

			FNiagaraShader* Shader = Context->GPUScript_RT->GetShader();
			if ( Shader == nullptr )
			{
				continue;
			}

			const bool bNeedsPersistentIDs = Context->MainDataSet->GetNeedsPersistentIDs();

			//The buffer containing current simulation state.
			Instance.CurrentData = Context->MainDataSet->GetCurrentData();
			//The buffer we're going to write simulation results to.
			Instance.DestinationData = &Context->MainDataSet->BeginSimulate();

			check(Instance.CurrentData && Instance.DestinationData);
			FNiagaraDataBuffer& CurrentData = *Instance.CurrentData;
			FNiagaraDataBuffer& DestinationData = *Instance.DestinationData;

			const uint32 PrevNumInstances = bNeedsReset ? 0 : CurrentData.GetNumInstances();
			const uint32 NewNumInstances = Instance.SpawnInfo.SpawnRateInstances + Instance.SpawnInfo.EventSpawnTotal + PrevNumInstances;

			//We must assume all particles survive when allocating here. 
			//If this is not true, the read back in ResolveDatasetWrites will shrink the buffers.
			const uint32 RequiredInstances = FMath::Max(PrevNumInstances, NewNumInstances);
			const uint32 AllocatedInstances = FMath::Max(RequiredInstances, Instance.SpawnInfo.MaxParticleCount);

			if (bNeedsPersistentIDs)
			{
				Context->MainDataSet->AllocateGPUFreeIDs(AllocatedInstances + 1, RHICmdList, FeatureLevel, Context->GetDebugSimName());
				ReadBuffers.Add(Context->MainDataSet->GetGPUFreeIDs().UAV);
				InstancesWithPersistentIDs.Add(&Instance);
			}

			DestinationData.AllocateGPU(AllocatedInstances + 1, GPUInstanceCounterManager, RHICmdList, FeatureLevel, Context->GetDebugSimName());
			DestinationData.SetNumInstances(RequiredInstances);

			if ( Shader->FloatInputBufferParam.IsBound() )
			{
				ReadBuffers.Add(CurrentData.GetGPUBufferFloat().UAV);
			}
			if ( Shader->IntInputBufferParam.IsBound() )
			{
				ReadBuffers.Add(CurrentData.GetGPUBufferInt().UAV);
			}

			if ( Shader->FloatOutputBufferParam.IsBound() )
			{
				WriteBuffers.Add(DestinationData.GetGPUBufferFloat().UAV);
				if (bIsFinalTick)
				{
					OutputGraphicsBuffers.Add(DestinationData.GetGPUBufferFloat().UAV);
				}
			}
			if ( Shader->IntOutputBufferParam.IsBound() )
			{
				WriteBuffers.Add(DestinationData.GetGPUBufferInt().UAV);
				if (bIsFinalTick)
				{
					OutputGraphicsBuffers.Add(DestinationData.GetGPUBufferInt().UAV);
				}
			}

			if (bNeedsPersistentIDs)
			{
				WriteBuffers.Add(DestinationData.GetGPUIDToIndexTable().UAV);
			}

			Context->MainDataSet->EndSimulate();
			if (bIsFinalTick)
			{
				Context->SetDataToRender(Instance.DestinationData);
			}
		}
	}

	uint32 NumInstancesWithPersistentIDs = (uint32)InstancesWithPersistentIDs.Num();
	if (NumInstancesWithPersistentIDs > 0)
	{
		// These buffers will be needed by the simulation dispatches which come immediately after, so there will be a stall, but
		// moving this step to a different place is difficult, and the stall is not large, so we'll live with it for now.
		SCOPED_DRAW_EVENT(RHICmdList, NiagaraGPUClearIDTables);
		SCOPED_GPU_STAT(RHICmdList, NiagaraGPUClearIDTables);

		FNiagaraBufferArray IDToIndexTables;
		IDToIndexTables.SetNum(NumInstancesWithPersistentIDs);
		for (uint32 i = 0; i < NumInstancesWithPersistentIDs; ++i)
		{
			FNiagaraComputeInstanceData* Instance = InstancesWithPersistentIDs[i];
			IDToIndexTables[i] = Instance->DestinationData->GetGPUIDToIndexTable().UAV;
		}
		// TODO: is it sufficient to do a CS cache flush before all this and get rid of these explicit barriers?
		RHICmdList.TransitionResources(EResourceTransitionAccess::EWritable, EResourceTransitionPipeline::EComputeToCompute, IDToIndexTables.GetData(), IDToIndexTables.Num());

		for (uint32 i = 0; i < NumInstancesWithPersistentIDs; ++i)
		{
			FNiagaraComputeInstanceData* Instance = InstancesWithPersistentIDs[i];
			SCOPED_DRAW_EVENTF(RHICmdList, NiagaraGPUComputeClearIDToIndexBuffer, TEXT("Clear ID To Index Table - %s"), Instance->Context->GetDebugSimName());
			NiagaraFillGPUIntBuffer(RHICmdList, FeatureLevel, Instance->DestinationData->GetGPUIDToIndexTable(), INDEX_NONE);
		}
	}
}

void NiagaraEmitterInstanceBatcher::DispatchAllOnCompute(FOverlappableTicks& OverlappableTick, FRHICommandList& RHICmdList, FRHIUniformBuffer* ViewUniformBuffer, FNiagaraBufferArray& ReadBuffers, FNiagaraBufferArray& WriteBuffers, bool bSetReadback)
{
	FRHICommandListImmediate& RHICmdListImmediate = FRHICommandListExecutor::GetImmediateCommandList();

	//UE_LOG(LogNiagara, Warning, TEXT("NiagaraEmitterInstanceBatcher::DispatchAllOnCompute:  %0xP"), this);

#if WITH_EDITORONLY_DATA
	{
		for (FNiagaraGPUSystemTick* Tick : OverlappableTick)
		{
			uint32 DispatchCount = Tick->Count;
			FNiagaraComputeInstanceData* Instances = Tick->GetInstanceData();
			for (uint32 Index = 0; Index < DispatchCount; Index++)
			{
				FNiagaraComputeInstanceData& Instance = Instances[Index];
				FNiagaraComputeExecutionContext* Context = Instance.Context;
				if (Context && Context->GPUScript_RT->GetShader())
				{
					if (Context->DebugInfo.IsValid())
					{
						ProcessDebugInfo(RHICmdList, Context);
					}
				}
			}
		}
	}
#endif // WITH_EDITORONLY_DATA

	//
	RHICmdList.TransitionResources(EResourceTransitionAccess::EReadable, EResourceTransitionPipeline::EComputeToCompute, ReadBuffers.GetData(), ReadBuffers.Num());
	RHICmdList.TransitionResources(EResourceTransitionAccess::ERWBarrier, EResourceTransitionPipeline::EComputeToCompute, WriteBuffers.GetData(), WriteBuffers.Num());

	for (FNiagaraGPUSystemTick* Tick : OverlappableTick)
	{
		uint32 DispatchCount = Tick->Count;
		FNiagaraComputeInstanceData* Instances = Tick->GetInstanceData();
		for (uint32 Index = 0; Index < DispatchCount; Index++)
		{
			FNiagaraComputeInstanceData& Instance = Instances[Index];
			FNiagaraComputeExecutionContext* Context = Instance.Context;
			if (Context && Context->GPUScript_RT->GetShader())
			{
				FNiagaraComputeExecutionContext::TickCounter++;

				// run shader, sim and spawn in a single dispatch
				DispatchMultipleStages(*Tick, &Instance, RHICmdList, ViewUniformBuffer, Context->GPUScript_RT->GetShader());

				FNiagaraDataBuffer* CurrentData = Instance.CurrentData;
				if (bSetReadback && Tick->bIsFinalTick)
				{
					// Now that the current data is not required anymore, stage it for readback.
					if (CurrentData->GetNumInstances() && Context->EmitterInstanceReadback.GPUCountOffset == INDEX_NONE && CurrentData->GetGPUInstanceCountBufferOffset() != INDEX_NONE)
					{
						// Transfer the GPU instance counter ownership to the context. Note that when bSetReadback is true, a readback request will be performed later in the tick update.
						Context->EmitterInstanceReadback.GPUCountOffset = CurrentData->GetGPUInstanceCountBufferOffset();
						Context->EmitterInstanceReadback.CPUCount = CurrentData->GetNumInstances();
						CurrentData->ClearGPUInstanceCountBufferOffset();
					}
				}
			}
		}
	}

	if (GNiagaraSubmitCommands)
	{
		RHICmdList.SubmitCommandsHint();
	}
}

void NiagaraEmitterInstanceBatcher::PostRenderOpaque(FRHICommandListImmediate& RHICmdList, FRHIUniformBuffer* ViewUniformBuffer, const class FShaderParametersMetadata* SceneTexturesUniformBufferStruct, FRHIUniformBuffer* SceneTexturesUniformBuffer, bool bAllowGPUParticleUpdate)
{
	LLM_SCOPE(ELLMTag::Niagara);

	if (bAllowGPUParticleUpdate)
	{
		// Setup new readback since if there is no pending request, there is no risk of having invalid data read (offset being allocated after the readback was sent).
		ExecuteAll(RHICmdList, ViewUniformBuffer, !GPUInstanceCounterManager.HasPendingGPUReadback(), ETickStage::PostOpaqueRender);

		RHICmdList.AutomaticCacheFlushAfterComputeShader(false);
		UpdateFreeIDBuffers(RHICmdList, DeferredIDBufferUpdates);
		RHICmdList.AutomaticCacheFlushAfterComputeShader(true);

		DeferredIDBufferUpdates.SetNum(0, false);

		FinishDispatches();
	}

	if (!GPUInstanceCounterManager.HasPendingGPUReadback())
	{
		GPUInstanceCounterManager.EnqueueGPUReadback(RHICmdList);
	}
}

bool NiagaraEmitterInstanceBatcher::ShouldTickForStage(const FNiagaraGPUSystemTick& Tick, ETickStage TickStage) const
{
	if (!GNiagaraAllowTickBeforeRender || Tick.bRequiresDistanceFieldData || Tick.bRequiresDepthBuffer)
	{
		return TickStage == ETickStage::PostOpaqueRender;
	}

	if (Tick.bRequiresEarlyViewData)
	{
		return TickStage == ETickStage::PostInitViews;
	}

	FNiagaraShader* ComputeShader = Tick.GetInstanceData()->Context->GPUScript_RT->GetShader();
	if (ComputeShader->ViewUniformBufferParam.IsBound())
	{
		return TickStage == ETickStage::PostOpaqueRender;
	}
	return TickStage == ETickStage::PreInitViews;
}

void NiagaraEmitterInstanceBatcher::ResizeFreeIDsListSizesBuffer(uint32 NumInstances)
{
	if (NumInstances <= NumAllocatedFreeIDListSizes)
	{
		return;
	}

	constexpr uint32 ALLOC_CHUNK_SIZE = 128;
	NumAllocatedFreeIDListSizes = Align(NumInstances, ALLOC_CHUNK_SIZE);
	if (FreeIDListSizesBuffer.Buffer)
	{
		FreeIDListSizesBuffer.Release();
	}
	FreeIDListSizesBuffer.Initialize(sizeof(uint32), NumAllocatedFreeIDListSizes, EPixelFormat::PF_R32_SINT, BUF_Static, TEXT("NiagaraFreeIDListSizes"));
	bFreeIDListSizesBufferCleared = false;
}

void NiagaraEmitterInstanceBatcher::ClearFreeIDsListSizesBuffer(FRHICommandList& RHICmdList)
{
	if (bFreeIDListSizesBufferCleared)
	{
		return;
	}

	SCOPED_DRAW_EVENT(RHICmdList, NiagaraGPUComputeClearFreeIDListSizes);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, EResourceTransitionPipeline::EComputeToCompute, FreeIDListSizesBuffer.UAV);
	NiagaraFillGPUIntBuffer(RHICmdList, FeatureLevel, FreeIDListSizesBuffer, 0);
	bFreeIDListSizesBufferCleared = true;
}

void NiagaraEmitterInstanceBatcher::UpdateFreeIDBuffers(FRHICommandList& RHICmdList, FEmitterInstanceList& Instances)
{
	if (Instances.Num() == 0)
	{
		return;
	}

	SCOPED_DRAW_EVENT(RHICmdList, NiagaraGPUComputeFreeIDs);
	SCOPED_GPU_STAT(RHICmdList, NiagaraGPUComputeFreeIDs);

	FNiagaraBufferArray ReadBuffers, WriteBuffers;
	for(FNiagaraComputeInstanceData* Instance : Instances)
	{
		ReadBuffers.Add(Instance->DestinationData->GetGPUIDToIndexTable().UAV);
		WriteBuffers.Add(Instance->Context->MainDataSet->GetGPUFreeIDs().UAV);
	}

	RHICmdList.TransitionResource(EResourceTransitionAccess::ERWNoBarrier, EResourceTransitionPipeline::EComputeToCompute, FreeIDListSizesBuffer.UAV);
	RHICmdList.TransitionResources(EResourceTransitionAccess::EReadable, EResourceTransitionPipeline::EComputeToCompute, ReadBuffers.GetData(), ReadBuffers.Num());
	RHICmdList.TransitionResources(EResourceTransitionAccess::EWritable, EResourceTransitionPipeline::EComputeToCompute, WriteBuffers.GetData(), WriteBuffers.Num());

	check((uint32)Instances.Num() <= NumAllocatedFreeIDListSizes);

	for (uint32 InstanceIdx = 0; InstanceIdx < (uint32)Instances.Num(); ++InstanceIdx)
	{
		FNiagaraComputeInstanceData* Instance = Instances[InstanceIdx];
		FNiagaraDataSet* MainDataSet = Instance->Context->MainDataSet;
		FNiagaraDataBuffer* DestinationData = Instance->DestinationData;

		SCOPED_DRAW_EVENTF(RHICmdList, NiagaraGPUComputeFreeIDsEmitter, TEXT("Update Free ID Buffer - %s"), Instance->Context->GetDebugSimName());
		NiagaraComputeGPUFreeIDs(RHICmdList, FeatureLevel, MainDataSet->GetGPUNumAllocatedIDs(), DestinationData->GetGPUIDToIndexTable().SRV, MainDataSet->GetGPUFreeIDs(), FreeIDListSizesBuffer, InstanceIdx);
	}

	bFreeIDListSizesBufferCleared = false;
}

void NiagaraEmitterInstanceBatcher::ExecuteAll(FRHICommandList& RHICmdList, FRHIUniformBuffer* ViewUniformBuffer, bool bSetReadback, ETickStage TickStage)
{
	SCOPE_CYCLE_COUNTER(STAT_NiagaraGPUSimTick_RT);

	// This is always called by the renderer so early out if we have no work.
	if (Ticks_RT.Num() == 0)
	{
		return;
	}

	SCOPED_DRAW_EVENT(RHICmdList, NiagaraEmitterInstanceBatcher_ExecuteAll);

	FMemMark Mark(FMemStack::Get());
	TArray<FOverlappableTicks, TMemStackAllocator<> > SimPasses;
	{
		TArray< FNiagaraComputeExecutionContext* , TMemStackAllocator<> > RelevantContexts;
		TArray< FNiagaraGPUSystemTick* , TMemStackAllocator<> > RelevantTicks;
		for (FNiagaraGPUSystemTick& Tick : Ticks_RT)
		{
			FNiagaraComputeInstanceData* Data = Tick.GetInstanceData();
			FNiagaraComputeExecutionContext* Context = Data->Context;
			// This assumes all emitters fallback to the same FNiagaraShaderScript*.
			FNiagaraShader* ComputeShader = Context->GPUScript_RT->GetShader();
			if (!ComputeShader || !ShouldTickForStage(Tick, TickStage))
			{
				continue;
			}

			Tick.bIsFinalTick = false; // @todo : this is true sometimes, needs investigation
			if (Context->ScratchIndex == INDEX_NONE)
			{
				RelevantContexts.Add(Context);
			}
			// Here scratch index represent the index of the last tick
			Context->ScratchIndex = RelevantTicks.Add(&Tick);
		}

		// Set bIsFinalTick for the last tick of each context and reset the scratch index.
		const int32 ScrachIndexReset = GNiagaraOverlapCompute ? 0 : INDEX_NONE;
		for (FNiagaraComputeExecutionContext* Context : RelevantContexts)
		{
			RelevantTicks[Context->ScratchIndex]->bIsFinalTick = true;
			Context->ScratchIndex = ScrachIndexReset;
		}

		if (GNiagaraOverlapCompute)
		{
			// Transpose now only once the data to get all independent tick per pass
			SimPasses.Reserve(2); // Safe bet!

			for (FNiagaraGPUSystemTick* Tick : RelevantTicks)
			{
				FNiagaraComputeExecutionContext* Context = Tick->GetInstanceData()->Context;
				const int32 ScratchIndex = Context->ScratchIndex;
				check(ScratchIndex != INDEX_NONE);

				if (ScratchIndex >= SimPasses.Num())
				{
					SimPasses.AddDefaulted(SimPasses.Num() - ScratchIndex + 1);
					if (ScratchIndex == 0)
					{
						SimPasses[0].Reserve(RelevantContexts.Num()); // Guarantied!
					}
				}
				SimPasses[ScratchIndex].Add(Tick);
				// Scratch index is now the number of passes for this context.
				if (Tick->bIsFinalTick)
				{
					// Reset to default as it will no longer be used.
					Context->ScratchIndex = INDEX_NONE;
				}
				else
				{
					Context->ScratchIndex += 1;
				}
			}
		}
		else
		{
			// Force dispatches to run individually, this should only be used for debugging as it is highly inefficient on the GPU
			SimPasses.Reserve(RelevantTicks.Num()); // Guarantied!
			for (FNiagaraGPUSystemTick* Tick : RelevantTicks)
			{
				SimPasses.AddDefaulted_GetRef().Add(Tick);
			}
		}
	}

	RHICmdList.AutomaticCacheFlushAfterComputeShader(false);

	FEmitterInstanceList InstancesWithPersistentIDs;
	FNiagaraBufferArray ReadBuffers, WriteBuffers, OutputGraphicsBuffers;

	for (int32 SimPassIdx = 0; SimPassIdx < SimPasses.Num(); ++SimPassIdx)
	{
		FOverlappableTicks& SimPass = SimPasses[SimPassIdx];
		ReadBuffers.SetNum(0, false);
		WriteBuffers.SetNum(0, false);
		InstancesWithPersistentIDs.SetNum(0, false);

		// This initial pass gathers all the buffers that are read from and written to so we can do batch resource transitions.
		// It also ensures the GPU buffers are large enough to hold everything.
		ResizeBuffersAndGatherResources(SimPass, RHICmdList, ReadBuffers, WriteBuffers, OutputGraphicsBuffers, InstancesWithPersistentIDs);

		{
			SCOPED_DRAW_EVENT(RHICmdList, NiagaraGPUSimulation);
			SCOPED_GPU_STAT(RHICmdList, NiagaraGPUSimulation);
			DispatchAllOnCompute(SimPass, RHICmdList, ViewUniformBuffer, ReadBuffers, WriteBuffers, bSetReadback);
		}

		if (InstancesWithPersistentIDs.Num() == 0)
		{
			continue;
		}

		// If we're doing multiple ticks (e.g. when scrubbing the timeline in the editor), we must update the free ID buffers before running
		// the next tick, which will cause stalls (because the ID to index buffer is written by DispatchAllOnCompute and read by UpdateFreeIDBuffers).
		// However, when we're at the last tick, we can postpone the update until later in the frame and avoid the stall. This will be the case when
		// running normally, with one tick per frame.
		if (SimPassIdx < SimPasses.Num() - 1)
		{
			ResizeFreeIDsListSizesBuffer(InstancesWithPersistentIDs.Num());
			ClearFreeIDsListSizesBuffer(RHICmdList);
			UpdateFreeIDBuffers(RHICmdList, InstancesWithPersistentIDs);
		}
		else
		{
			DeferredIDBufferUpdates.Append(InstancesWithPersistentIDs);
			ResizeFreeIDsListSizesBuffer(DeferredIDBufferUpdates.Num());

			// Speculatively clear the list sizes buffer here. Under normal circumstances, this happens in the first stage which finds instances with persistent IDs
			// (usually PreInitViews) and it's finished by the time the deferred updates need to be processed. If a subsequent tick stage runs multiple time ticks,
			// the first step will find the buffer already cleared and will not clear again. The only time when this clear is superfluous is when a following stage
			// reallocates the buffer, but that's unlikely (and amortized) because we allocate in chunks.
			ClearFreeIDsListSizesBuffer(RHICmdList);
		}
	}

	OutputGraphicsBuffers.Add(GPUInstanceCounterManager.GetInstanceCountBuffer().UAV);
	RHICmdList.TransitionResources(EResourceTransitionAccess::EReadable, EResourceTransitionPipeline::EComputeToGfx, OutputGraphicsBuffers.GetData(), OutputGraphicsBuffers.Num());

	RHICmdList.AutomaticCacheFlushAfterComputeShader(true);
}

void NiagaraEmitterInstanceBatcher::PreInitViews(FRHICommandListImmediate& RHICmdList, bool bAllowGPUParticleUpdate)
{
	LLM_SCOPE(ELLMTag::Niagara);

	// Reset the list of GPUSort tasks and release any resources they hold on to.
	// It might be worth considering doing so at the end of the render to free the resources immediately.
	// (note that currently there are no callback appropriate to do it)
	SimulationsToSort.Reset();

	// Update draw indirect buffer to max possible size.
	if (bAllowGPUParticleUpdate)
	{
		int32 TotalDispatchCount = 0;
		for (FNiagaraGPUSystemTick& Tick : Ticks_RT)
		{
			TotalDispatchCount += (int32)Tick.Count;

			// Cancel any pending readback if the emitter is resetting.
			if (Tick.bNeedsReset)
			{
				FNiagaraComputeInstanceData* Instances = Tick.GetInstanceData();
				for (uint32 InstanceIndex = 0; InstanceIndex < Tick.Count; ++InstanceIndex)
				{
					FNiagaraComputeExecutionContext* Context = Instances[InstanceIndex].Context;
					if (Context)
					{
						GPUInstanceCounterManager.FreeEntry(Context->EmitterInstanceReadback.GPUCountOffset);
					}
				}
			}
		}
		GPUInstanceCounterManager.ResizeBuffers(RHICmdList, FeatureLevel, TotalDispatchCount);

		// Update the instance counts from the GPU readback.
		{
			SCOPE_CYCLE_COUNTER(STAT_NiagaraGPUReadback_RT);
			const uint32* Counts = GPUInstanceCounterManager.GetGPUReadback();
			if (Counts)
			{
				for (FNiagaraGPUSystemTick& Tick : Ticks_RT)
				{
					FNiagaraComputeInstanceData* Instances = Tick.GetInstanceData();
					for (uint32 InstanceIndex = 0; InstanceIndex < Tick.Count; ++InstanceIndex)
					{
						FNiagaraComputeExecutionContext* Context = Instances[InstanceIndex].Context;
						if (Context && Context->EmitterInstanceReadback.GPUCountOffset != INDEX_NONE)
						{
							check(Context->MainDataSet);
							FNiagaraDataBuffer* CurrentData = Context->MainDataSet->GetCurrentData();
							if (CurrentData)
							{
								const uint32 DeadInstanceCount = Context->EmitterInstanceReadback.CPUCount - Counts[Context->EmitterInstanceReadback.GPUCountOffset];

								// This will communicate the particle counts to the game thread. If DeadInstanceCount equals CurrentData->GetNumInstances() the game thread will know that the emitter has completed.
								if (DeadInstanceCount <= CurrentData->GetNumInstances())
								{
									CurrentData->SetNumInstances(CurrentData->GetNumInstances() - DeadInstanceCount); 
								}
							}

							// Now release the readback since another one will be enqueued in the tick.
							// Also prevents processing the same data again.
							GPUInstanceCounterManager.FreeEntry(Context->EmitterInstanceReadback.GPUCountOffset);
						}
					}
				}
				// Readback is only valid for one frame, so that any newly allocated instance count
				// are guarantied to be in the next valid readback data.
				GPUInstanceCounterManager.ReleaseGPUReadback();
			}
		}

		// @todo REMOVE THIS HACK
		LastFrameThatDrainedData = GFrameNumberRenderThread;

		if (GNiagaraAllowTickBeforeRender)
		{
			ExecuteAll(RHICmdList, nullptr, !GPUInstanceCounterManager.HasPendingGPUReadback(), ETickStage::PreInitViews);
		}
	}
	else
	{
		GPUInstanceCounterManager.ResizeBuffers(RHICmdList, FeatureLevel,  0);
	}
}

void NiagaraEmitterInstanceBatcher::PostInitViews(FRHICommandListImmediate& RHICmdList, FRHIUniformBuffer* ViewUniformBuffer, bool bAllowGPUParticleUpdate)
{
	LLM_SCOPE(ELLMTag::Niagara);

	if (bAllowGPUParticleUpdate)
	{
		ExecuteAll(RHICmdList, ViewUniformBuffer, false, ETickStage::PostInitViews);
	}
}

bool NiagaraEmitterInstanceBatcher::UsesGlobalDistanceField() const
{
	for (const FNiagaraGPUSystemTick& Tick : Ticks_RT)
	{
		if (Tick.bRequiresDistanceFieldData)
		{
			return true;
		}
	}

	return false;
}

bool NiagaraEmitterInstanceBatcher::UsesDepthBuffer() const
{
	for (const FNiagaraGPUSystemTick& Tick : Ticks_RT)
	{
		if (Tick.bRequiresDepthBuffer)
		{
			return true;
		}
	}

	return false;
}

bool NiagaraEmitterInstanceBatcher::RequiresEarlyViewUniformBuffer() const
{
	for (const FNiagaraGPUSystemTick& Tick : Ticks_RT)
	{
		if (Tick.bRequiresEarlyViewData)
		{
			return true;
		}
	}

	return false;
}

void NiagaraEmitterInstanceBatcher::PreRender(FRHICommandListImmediate& RHICmdList, const class FGlobalDistanceFieldParameterData* GlobalDistanceFieldParameterData, bool bAllowGPUParticleUpdate)
{
	LLM_SCOPE(ELLMTag::Niagara);

	GlobalDistanceFieldParams = GlobalDistanceFieldParameterData ? *GlobalDistanceFieldParameterData : FGlobalDistanceFieldParameterData();

	// Update draw indirect args from the simulation results.
	GPUInstanceCounterManager.UpdateDrawIndirectBuffer(RHICmdList, FeatureLevel);
}

void NiagaraEmitterInstanceBatcher::OnDestroy()
{
	FNiagaraWorldManager::OnBatcherDestroyed(this);
	FFXSystemInterface::OnDestroy();
}

bool NiagaraEmitterInstanceBatcher::AddSortedGPUSimulation(FNiagaraGPUSortInfo& SortInfo)
{
	if (GPUSortManager && GPUSortManager->AddTask(SortInfo.AllocationInfo, SortInfo.ParticleCount, SortInfo.SortFlags))
	{
		// It's not worth currently to have a map between SortInfo.AllocationInfo.SortBatchId and the relevant indices in SimulationsToSort
		// because the number of batches is expect to be very small (1 or 2). If this change, it might be worth reconsidering.
		SimulationsToSort.Add(SortInfo);
		return true;
	}
	else
	{
		return false;
	}
}

void NiagaraEmitterInstanceBatcher::GenerateSortKeys(FRHICommandListImmediate& RHICmdList, int32 BatchId, int32 NumElementsInBatch, EGPUSortFlags Flags, FRHIUnorderedAccessView* KeysUAV, FRHIUnorderedAccessView* ValuesUAV)
{
	// Currently all Niagara KeyGen must execute after PreRender() - in between PreInitViews() and PostRenderOpaque(), when the GPU simulation are possibly ticked.
	check(EnumHasAnyFlags(Flags, EGPUSortFlags::KeyGenAfterPreRender));

	const FGPUSortManager::FKeyGenInfo KeyGenInfo((uint32)NumElementsInBatch, EnumHasAnyFlags(Flags, EGPUSortFlags::HighPrecisionKeys));

	FNiagaraSortKeyGenCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FNiagaraSortKeyGenCS::FSortUsingMaxPrecision>(EnumHasAnyFlags(Flags, EGPUSortFlags::HighPrecisionKeys));
	TShaderMapRef<FNiagaraSortKeyGenCS> KeyGenCS(GetGlobalShaderMap(FeatureLevel), PermutationVector);
	RHICmdList.SetComputeShader(KeyGenCS->GetComputeShader());
	KeyGenCS->SetOutput(RHICmdList, KeysUAV, ValuesUAV);

	FRHIUnorderedAccessView* OutputUAVs[] = { KeysUAV, ValuesUAV };
	for (const FNiagaraGPUSortInfo& SortInfo : SimulationsToSort)
	{
		if (SortInfo.AllocationInfo.SortBatchId == BatchId)
		{
			KeyGenCS->SetParameters(RHICmdList, SortInfo, (uint32)SortInfo.AllocationInfo.ElementIndex << KeyGenInfo.ElementKeyShift, SortInfo.AllocationInfo.BufferOffset, KeyGenInfo.SortKeyParams);
			DispatchComputeShader(RHICmdList, *KeyGenCS, FMath::DivideAndRoundUp(SortInfo.ParticleCount, NIAGARA_KEY_GEN_THREAD_COUNT), 1, 1);
			// TR-KeyGen : No sync needed between tasks since they update different parts of the data (assuming it's ok if cache lines overlap).
			RHICmdList.TransitionResources(EResourceTransitionAccess::ERWNoBarrier, EResourceTransitionPipeline::EComputeToCompute, OutputUAVs, UE_ARRAY_COUNT(OutputUAVs));
		}
	}
	KeyGenCS->UnbindBuffers(RHICmdList);
}

void NiagaraEmitterInstanceBatcher::ProcessDebugInfo(FRHICommandList &RHICmdList, const FNiagaraComputeExecutionContext* Context) const
{
#if WITH_EDITORONLY_DATA
	// This method may be called from one of two places: in the tick or as part of a paused frame looking for the debug info that was submitted previously...
	// Note that PrevData is where we expect the data to be for rendering
	if (Context && Context->DebugInfo.IsValid())
	{
		// Fire off the readback if not already doing so
		if (!Context->GPUDebugDataReadbackFloat && !Context->GPUDebugDataReadbackInt && !Context->GPUDebugDataReadbackCounts)
		{
			// Do nothing.., handled in Run
		}
		// We may not have floats or ints, but we should have at least one of the two
		else if ((Context->GPUDebugDataReadbackFloat == nullptr || Context->GPUDebugDataReadbackFloat->IsReady()) 
				&& (Context->GPUDebugDataReadbackInt == nullptr || Context->GPUDebugDataReadbackInt->IsReady())
				&& Context->GPUDebugDataReadbackCounts->IsReady()
			)
		{
			//UE_LOG(LogNiagara, Warning, TEXT("Read back!"));

			int32 NewExistingDataCount =  static_cast<int32*>(Context->GPUDebugDataReadbackCounts->Lock((Context->GPUDebugDataCountOffset + 1) * sizeof(int32)))[Context->GPUDebugDataCountOffset];
			{
				float* FloatDataBuffer = nullptr;
				if (Context->GPUDebugDataReadbackFloat)
				{
					FloatDataBuffer = static_cast<float*>(Context->GPUDebugDataReadbackFloat->Lock(Context->GPUDebugDataFloatSize));
				}
				int* IntDataBuffer = nullptr;
				if (Context->GPUDebugDataReadbackInt)
				{
					IntDataBuffer = static_cast<int*>(Context->GPUDebugDataReadbackInt->Lock(Context->GPUDebugDataIntSize));
				}

				Context->DebugInfo->Frame.CopyFromGPUReadback(FloatDataBuffer, IntDataBuffer, 0, NewExistingDataCount, Context->GPUDebugDataFloatStride, Context->GPUDebugDataIntStride);

				Context->DebugInfo->bWritten = true;

				if (Context->GPUDebugDataReadbackFloat)
				{
					Context->GPUDebugDataReadbackFloat->Unlock();
				}
				if (Context->GPUDebugDataReadbackInt)
				{
					Context->GPUDebugDataReadbackInt->Unlock();
				}
				Context->GPUDebugDataReadbackCounts->Unlock();
			}
			{
				// The following code seems to take significant time on d3d12
				// Clear out the readback buffers...
				if (Context->GPUDebugDataReadbackFloat)
				{
					delete Context->GPUDebugDataReadbackFloat;
					Context->GPUDebugDataReadbackFloat = nullptr;
				}
				if (Context->GPUDebugDataReadbackInt)
				{
					delete Context->GPUDebugDataReadbackInt;
					Context->GPUDebugDataReadbackInt = nullptr;
				}
				delete Context->GPUDebugDataReadbackCounts;
				Context->GPUDebugDataReadbackCounts = nullptr;	
				Context->GPUDebugDataFloatSize = 0;
				Context->GPUDebugDataIntSize = 0;
				Context->GPUDebugDataFloatStride = 0;
				Context->GPUDebugDataIntStride = 0;
				Context->GPUDebugDataCountOffset = INDEX_NONE;
			}

			// We've updated the debug info directly, now we need to no longer keep asking and querying because this frame is done!
			Context->DebugInfo.Reset();
		}
	}
#endif // WITH_EDITORONLY_DATA
}

/* Set shader parameters for data interfaces
 */
void NiagaraEmitterInstanceBatcher::SetDataInterfaceParameters(const TArray<FNiagaraDataInterfaceProxy*>& DataInterfaceProxies, FNiagaraShader* Shader, FRHICommandList &RHICmdList, const FNiagaraComputeInstanceData* Instance, const FNiagaraGPUSystemTick& Tick, uint32 ShaderStageIndex) const
{
	// set up data interface buffers, as defined by the DIs during compilation
	//

	// @todo-threadsafety This is a bit gross. Need to rethink this api.
	const FNiagaraSystemInstanceID& SystemInstance = Tick.SystemInstanceID;

	uint32 InterfaceIndex = 0;
	for (FNiagaraDataInterfaceProxy* Interface : DataInterfaceProxies)
	{
		FNiagaraDataInterfaceParamRef& DIParam = Shader->GetDIParameters()[InterfaceIndex];
		if (DIParam.Parameters)
		{
			FNiagaraDataInterfaceSetArgs Context;
			Context.Shader = Shader;
			Context.DataInterface = Interface;
			Context.SystemInstance = SystemInstance;
			Context.Batcher = this;
			Context.ComputeInstanceData = Instance;
			Context.ShaderStageIndex = ShaderStageIndex;
			Context.IsOutputStage = Interface->IsOutputStage(ShaderStageIndex);
			DIParam.Parameters->Set(RHICmdList, Context);
		}

		InterfaceIndex++;
	}
}

void NiagaraEmitterInstanceBatcher::UnsetDataInterfaceParameters(const TArray<FNiagaraDataInterfaceProxy*> &DataInterfaceProxies, FNiagaraShader* Shader, FRHICommandList &RHICmdList, const FNiagaraComputeInstanceData* Instance, const FNiagaraGPUSystemTick& Tick) const
{
	// set up data interface buffers, as defined by the DIs during compilation
	//

	// @todo-threadsafety This is a bit gross. Need to rethink this api.
	const FNiagaraSystemInstanceID& SystemInstance = Tick.SystemInstanceID;

	uint32 InterfaceIndex = 0;
	for (FNiagaraDataInterfaceProxy* Interface : DataInterfaceProxies)
	{
		FNiagaraDataInterfaceParamRef& DIParam = Shader->GetDIParameters()[InterfaceIndex];
		if (DIParam.Parameters)
		{
			void* PerInstanceData = nullptr;
			int32* OffsetFound = nullptr;
			if (Tick.DIInstanceData && Tick.DIInstanceData->PerInstanceDataSize != 0 && Tick.DIInstanceData->InterfaceProxiesToOffsets.Num() != 0)
			{
				OffsetFound = Tick.DIInstanceData->InterfaceProxiesToOffsets.Find(Interface);
				if (OffsetFound != nullptr)
				{
					PerInstanceData = (*OffsetFound) + (uint8*)Tick.DIInstanceData->PerInstanceDataForRT;
				}
			}
			FNiagaraDataInterfaceSetArgs Context;
			Context.Shader = Shader;
			Context.DataInterface = Interface;
			Context.SystemInstance = SystemInstance;
			Context.Batcher = this;
			DIParam.Parameters->Unset(RHICmdList, Context);
		}

		InterfaceIndex++;
	}
}

static void SetConstantBuffer(FRHICommandList &RHICmdList, FRHIComputeShader* ComputeShader, const FShaderUniformBufferParameter& BufferParam, const FRHIUniformBufferLayout& Layout, const uint8* ParamData)
{
	if (!BufferParam.IsBound())
		return;

	if (Layout.ConstantBufferSize)
	{
		check(Layout.Resources.Num() == 0);
		FUniformBufferRHIRef CBuffer = RHICreateUniformBuffer(ParamData, Layout, EUniformBufferUsage::UniformBuffer_SingleDraw);
		RHICmdList.SetShaderUniformBuffer(ComputeShader, BufferParam.GetBaseIndex(), CBuffer);
	}
}

/* Kick off a simulation/spawn run
 */
void NiagaraEmitterInstanceBatcher::Run(const FNiagaraGPUSystemTick& Tick, const FNiagaraComputeInstanceData* Instance, uint32 UpdateStartInstance, const uint32 TotalNumInstances, FNiagaraShader* Shader,
	FRHICommandList &RHICmdList, FRHIUniformBuffer* ViewUniformBuffer, const FNiagaraGpuSpawnInfo& SpawnInfo, bool bCopyBeforeStart, uint32 DefaultShaderStageIndex, uint32 ShaderStageIndex, FNiagaraDataInterfaceProxy *IterationInterface, bool HasRunParticleStage)
{
	FNiagaraComputeExecutionContext* Context = Instance->Context;

	SCOPED_DRAW_EVENTF(RHICmdList, NiagaraGPUSimulationCS, TEXT("Niagara Gpu Sim - %s - NumInstances: %u - StageNumber: %u"),
		Context->GetDebugSimName(),
		TotalNumInstances,
		ShaderStageIndex);

	if (TotalNumInstances == 0)
	{
		return;
	}

	//UE_LOG(LogNiagara, Warning, TEXT("Run"));
	
	const TArray<FNiagaraDataInterfaceProxy*>& DataInterfaceProxies = Instance->DataInterfaceProxies;
	check(Instance->CurrentData && Instance->DestinationData);
	FNiagaraDataBuffer& DestinationData = *Instance->DestinationData;
	FNiagaraDataBuffer& CurrentData = *Instance->CurrentData;

	int32 InstancesToSpawnThisFrame = Instance->SpawnInfo.SpawnRateInstances + Instance->SpawnInfo.EventSpawnTotal;

	// Only spawn particles on the first stage
	if (HasRunParticleStage)
	{
		InstancesToSpawnThisFrame = 0;
	}

	FRHIComputeShader* ComputeShader = Shader->GetComputeShader();
	DestinationData.SetNumSpawnedInstances(InstancesToSpawnThisFrame);
	DestinationData.SetIDAcquireTag(FNiagaraComputeExecutionContext::TickCounter);

	RHICmdList.SetComputeShader(Shader->GetComputeShader());

	// #todo(dmp): clean up this logic for shader stages on first frame
	SetShaderValue(RHICmdList, ComputeShader, Shader->SimStartParam, Tick.bNeedsReset ? 1U : 0U);

	// set the view uniform buffer param
	if (Shader->ViewUniformBufferParam.IsBound() && ViewUniformBuffer)
	{
		RHICmdList.SetShaderUniformBuffer(ComputeShader, Shader->ViewUniformBufferParam.GetBaseIndex(), ViewUniformBuffer);
	}

	SetDataInterfaceParameters(DataInterfaceProxies, Shader, RHICmdList, Instance, Tick, ShaderStageIndex);

	// set the shader and data set params 
	//
	const bool bNeedsPersistentIDs = Context->MainDataSet->GetNeedsPersistentIDs();
	SetSRVParameter(RHICmdList, Shader->GetComputeShader(), Shader->FreeIDBufferParam, bNeedsPersistentIDs ? Context->MainDataSet->GetGPUFreeIDs().SRV : FNiagaraRenderer::GetDummyIntBuffer().SRV);
	CurrentData.SetShaderParams(Shader, RHICmdList, true);
	DestinationData.SetShaderParams(Shader, RHICmdList, false);

	// set the index buffer uav
	//
	if (Shader->InstanceCountsParam.IsBound() && !IterationInterface )
	{
		RHICmdList.TransitionResource(EResourceTransitionAccess::ERWNoBarrier, EResourceTransitionPipeline::EComputeToCompute, GPUInstanceCounterManager.GetInstanceCountBuffer().UAV);
		Shader->InstanceCountsParam.SetBuffer(RHICmdList, ComputeShader, GPUInstanceCounterManager.GetInstanceCountBuffer());
		const uint32 ReadOffset = Tick.bNeedsReset ? INDEX_NONE : CurrentData.GetGPUInstanceCountBufferOffset();
		const uint32 WriteOffset = DestinationData.GetGPUInstanceCountBufferOffset();
		SetShaderValue(RHICmdList, ComputeShader, Shader->ReadInstanceCountOffsetParam, ReadOffset);
		SetShaderValue(RHICmdList, ComputeShader, Shader->WriteInstanceCountOffsetParam, WriteOffset);
	}

	// set the execution parameters
	//
	SetShaderValue(RHICmdList, ComputeShader, Shader->EmitterTickCounterParam, FNiagaraComputeExecutionContext::TickCounter);

	// set spawn info
	//
	static_assert((sizeof(SpawnInfo.SpawnInfoStartOffsets) % SHADER_PARAMETER_ARRAY_ELEMENT_ALIGNMENT) == 0, "sizeof SpawnInfoStartOffsets should be a multiple of SHADER_PARAMETER_ARRAY_ELEMENT_ALIGNMENT");
	static_assert((sizeof(SpawnInfo.SpawnInfoParams) % SHADER_PARAMETER_ARRAY_ELEMENT_ALIGNMENT) == 0, "sizeof SpawnInfoParams should be a multiple of SHADER_PARAMETER_ARRAY_ELEMENT_ALIGNMENT");
	SetShaderValueArray(RHICmdList, ComputeShader, Shader->EmitterSpawnInfoOffsetsParam, SpawnInfo.SpawnInfoStartOffsets, NIAGARA_MAX_GPU_SPAWN_INFOS_V4);
	SetShaderValueArray(RHICmdList, ComputeShader, Shader->EmitterSpawnInfoParamsParam, SpawnInfo.SpawnInfoParams, NIAGARA_MAX_GPU_SPAWN_INFOS);

	SetShaderValue(RHICmdList, ComputeShader, Shader->UpdateStartInstanceParam, UpdateStartInstance);					// 0, except for event handler runs
	SetShaderValue(RHICmdList, ComputeShader, Shader->NumSpawnedInstancesParam, InstancesToSpawnThisFrame);				// number of instances in the spawn run
	SetShaderValue(RHICmdList, ComputeShader, Shader->DefaultShaderStageIndexParam, DefaultShaderStageIndex);					// 0, except if several stages are defined
	SetShaderValue(RHICmdList, ComputeShader, Shader->ShaderStageIndexParam, ShaderStageIndex);					// 0, except if several stages are defined
	const int32 DefaultIterationCount = -1;
	SetShaderValue(RHICmdList, ComputeShader, Shader->IterationInterfaceCount, DefaultIterationCount);					// 0, except if several stages are defined

	const uint32 ShaderThreadGroupSize = FNiagaraShader::GetGroupSize(ShaderPlatform);
	if (IterationInterface)
	{
		if (TotalNumInstances > ShaderThreadGroupSize)
		{
			SetShaderValue(RHICmdList, ComputeShader, Shader->IterationInterfaceCount, TotalNumInstances);					// 0, except if several stages are defined
		}
	}

	uint32 NumThreadGroups = 1;
	if (TotalNumInstances > ShaderThreadGroupSize)
	{
		NumThreadGroups = FMath::Min(NIAGARA_MAX_COMPUTE_THREADGROUPS, FMath::DivideAndRoundUp(TotalNumInstances, ShaderThreadGroupSize));
	}

	SetConstantBuffer(RHICmdList, ComputeShader, Shader->GlobalConstantBufferParam[0], Context->GlobalCBufferLayout, Instance->GlobalParamData);
	SetConstantBuffer(RHICmdList, ComputeShader, Shader->SystemConstantBufferParam[0], Context->SystemCBufferLayout, Instance->SystemParamData);
	SetConstantBuffer(RHICmdList, ComputeShader, Shader->OwnerConstantBufferParam[0], Context->OwnerCBufferLayout, Instance->OwnerParamData);
	SetConstantBuffer(RHICmdList, ComputeShader, Shader->EmitterConstantBufferParam[0], Context->EmitterCBufferLayout, Instance->EmitterParamData);
	SetConstantBuffer(RHICmdList, ComputeShader, Shader->ExternalConstantBufferParam[0], Context->ExternalCBufferLayout, Instance->ExternalParamData);
	// setup script parameters
	if (Context->HasInterpolationParameters)
	{
		SetConstantBuffer(RHICmdList, ComputeShader, Shader->GlobalConstantBufferParam[1], Context->GlobalCBufferLayout, Instance->GlobalParamData + sizeof(FNiagaraGlobalParameters));
		SetConstantBuffer(RHICmdList, ComputeShader, Shader->SystemConstantBufferParam[1], Context->SystemCBufferLayout, Instance->SystemParamData + sizeof(FNiagaraSystemParameters));
		SetConstantBuffer(RHICmdList, ComputeShader, Shader->OwnerConstantBufferParam[1], Context->OwnerCBufferLayout, Instance->OwnerParamData + sizeof(FNiagaraOwnerParameters));
		SetConstantBuffer(RHICmdList, ComputeShader, Shader->EmitterConstantBufferParam[1], Context->EmitterCBufferLayout, Instance->EmitterParamData + sizeof(FNiagaraEmitterParameters));
		SetConstantBuffer(RHICmdList, ComputeShader, Shader->ExternalConstantBufferParam[1], Context->ExternalCBufferLayout, Instance->ExternalParamData + Context->ExternalCBufferLayout.ConstantBufferSize);
	}

	// setup script parameters

	// #todo(dmp): temporary hack -- unbind UAVs if we have a valid iteration DI.  This way, when we are outputting with a different iteration count, we don't
	// mess up particle state
	if (IterationInterface)
	{
		CurrentData.UnsetShaderParams(Shader, RHICmdList);
		DestinationData.UnsetShaderParams(Shader, RHICmdList);
	}

	//UE_LOG(LogNiagara, Log, TEXT("Num Instance : %d | Num Group : %d | Spawned Istance : %d | Start Instance : %d | Num Indices : %d | Stage Index : %d"), 
		//TotalNumInstances, NumThreadGroups, InstancesToSpawnThisFrame, UpdateStartInstance, Context->NumIndicesPerInstance, ShaderStageIndex);

	// Dispatch, if anything needs to be done
	if (TotalNumInstances)
	{
		DispatchComputeShader(RHICmdList, Shader, NumThreadGroups, 1, 1);
	}

	// reset iteration count
	if (IterationInterface)
	{
		SetShaderValue(RHICmdList, ComputeShader, Shader->IterationInterfaceCount, DefaultIterationCount);					// 0, except if several stages are defined
	}

#if WITH_EDITORONLY_DATA
	// Check to see if we need to queue up a debug dump..
	if (Context->DebugInfo.IsValid())
	{
		//UE_LOG(LogNiagara, Warning, TEXT("Queued up!"));

		if (!Context->GPUDebugDataReadbackFloat && !Context->GPUDebugDataReadbackInt && !Context->GPUDebugDataReadbackCounts && DestinationData.GetGPUInstanceCountBufferOffset() != INDEX_NONE && ShaderStageIndex == Context->MaxUpdateIterations - 1)
		{
			Context->GPUDebugDataFloatSize = 0;
			Context->GPUDebugDataIntSize = 0;
			Context->GPUDebugDataFloatStride = 0;
			Context->GPUDebugDataIntStride = 0;

			if (DestinationData.GetGPUBufferFloat().NumBytes > 0)
			{
				static const FName ReadbackFloatName(TEXT("Niagara GPU Debug Info Float Emitter Readback"));
				Context->GPUDebugDataReadbackFloat = new FRHIGPUBufferReadback(ReadbackFloatName);
				Context->GPUDebugDataReadbackFloat->EnqueueCopy(RHICmdList, DestinationData.GetGPUBufferFloat().Buffer);
				Context->GPUDebugDataFloatSize = DestinationData.GetGPUBufferFloat().NumBytes;
				Context->GPUDebugDataFloatStride = DestinationData.GetFloatStride();
			}

			if (DestinationData.GetGPUBufferInt().NumBytes > 0)
			{
				static const FName ReadbackIntName(TEXT("Niagara GPU Debug Info Int Emitter Readback"));
				Context->GPUDebugDataReadbackInt = new FRHIGPUBufferReadback(ReadbackIntName);
				Context->GPUDebugDataReadbackInt->EnqueueCopy(RHICmdList, DestinationData.GetGPUBufferInt().Buffer);
				Context->GPUDebugDataIntSize = DestinationData.GetGPUBufferInt().NumBytes;
				Context->GPUDebugDataIntStride = DestinationData.GetInt32Stride();
			}

			static const FName ReadbackCountsName(TEXT("Niagara GPU Emitter Readback"));
			Context->GPUDebugDataReadbackCounts = new FRHIGPUBufferReadback(ReadbackCountsName);
			Context->GPUDebugDataReadbackCounts->EnqueueCopy(RHICmdList, GPUInstanceCounterManager.GetInstanceCountBuffer().Buffer);
			Context->GPUDebugDataCountOffset = DestinationData.GetGPUInstanceCountBufferOffset();
		}
	}
#endif // WITH_EDITORONLY_DATA

	// Unset UAV parameters and transition resources (TODO: resource transition should be moved to the renderer)
	// 
	UnsetDataInterfaceParameters(DataInterfaceProxies, Shader, RHICmdList, Instance, Tick);
	CurrentData.UnsetShaderParams(Shader, RHICmdList);
	DestinationData.UnsetShaderParams(Shader, RHICmdList);
	Shader->InstanceCountsParam.UnsetUAV(RHICmdList, ComputeShader);
}

FGPUSortManager* NiagaraEmitterInstanceBatcher::GetGPUSortManager() const
{
	return GPUSortManager;
}
