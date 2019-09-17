// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "NiagaraParameterStore.h"
#include "NiagaraCommon.h"
#include "NiagaraDataSet.h"
#include "NiagaraComponent.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraParameterCollection.h"
#include "NiagaraStats.h"

DECLARE_CYCLE_STAT(TEXT("Parameter store bind"), STAT_NiagaraParameterStoreBind, STATGROUP_Niagara);
DECLARE_CYCLE_STAT(TEXT("Parameter store rebind"), STAT_NiagaraParameterStoreRebind, STATGROUP_Niagara);
DECLARE_CYCLE_STAT(TEXT("Parameter store tick"), STAT_NiagaraParameterStoreTick, STATGROUP_Niagara);
DECLARE_CYCLE_STAT(TEXT("Parameter store find var"), STAT_NiagaraParameterStoreFindVar, STATGROUP_Niagara);
DECLARE_MEMORY_STAT(TEXT("Niagara parameter store memory"), STAT_NiagaraParamStoreMemory, STATGROUP_Niagara);

#if WITH_EDITORONLY_DATA
static int32 GbDumpParticleParameterStores = 0;
static FAutoConsoleVariableRef CVarNiagaraDumpParticleParameterStores(
	TEXT("fx.DumpParticleParameterStores"),
	GbDumpParticleParameterStores,
	TEXT("If > 0 current frame particle parameter stores will be dumped when updated. \n"),
	ECVF_Default
);
#endif

//////////////////////////////////////////////////////////////////////////

FNiagaraParameterStore::FNiagaraParameterStore()
	: Owner(nullptr)
	, bParametersDirty(true)
	, bInterfacesDirty(true)
	, bUObjectsDirty(true)
	, LayoutVersion(0)
{
}

FNiagaraParameterStore::FNiagaraParameterStore(UObject* InOwner)
	: Owner(InOwner)
	, bParametersDirty(true)
	, bInterfacesDirty(true)
	, bUObjectsDirty(true)
	, LayoutVersion(0)
{
#if WITH_EDITORONLY_DATA
	if (InOwner != nullptr)
	{
		DebugName = *InOwner->GetFullName();
	}
#endif
}

FNiagaraParameterStore::FNiagaraParameterStore(const FNiagaraParameterStore& Other)
{
	DEC_MEMORY_STAT_BY(STAT_NiagaraParamStoreMemory, ParameterData.GetAllocatedSize());
	*this = Other;
	INC_MEMORY_STAT_BY(STAT_NiagaraParamStoreMemory, ParameterData.GetAllocatedSize());
}

FNiagaraParameterStore& FNiagaraParameterStore::operator=(const FNiagaraParameterStore& Other)
{
	Owner = Other.Owner;
	ParameterOffsets = Other.ParameterOffsets;
	DEC_MEMORY_STAT_BY(STAT_NiagaraParamStoreMemory, ParameterData.GetAllocatedSize());
	ParameterData = Other.ParameterData;
	INC_MEMORY_STAT_BY(STAT_NiagaraParamStoreMemory, ParameterData.GetAllocatedSize());
	DataInterfaces = Other.DataInterfaces;
	UObjects = Other.UObjects;
	++LayoutVersion;
#if WITH_EDITOR
	OnChangedDelegate.Broadcast();
#endif
	//Don't copy bindings. We just want the data.
	return *this;
}

FNiagaraParameterStore::~FNiagaraParameterStore()
{
	//Ensure that any stores bound to drive this one are unbound.
	UnbindFromSourceStores();
	DEC_MEMORY_STAT_BY(STAT_NiagaraParamStoreMemory, ParameterData.GetAllocatedSize());

	//Also unbind from any stores we're feeding.
	for (TPair<FNiagaraParameterStore*, FNiagaraParameterStoreBinding>& Binding : Bindings)
	{
		Binding.Value.Empty(Binding.Key, this);
	}
	Bindings.Empty();
}

void FNiagaraParameterStore::Bind(FNiagaraParameterStore* DestStore)
{
	check(DestStore);
	SCOPE_CYCLE_COUNTER(STAT_NiagaraParameterStoreBind);
	FNiagaraParameterStoreBinding& Binding = Bindings.FindOrAdd(DestStore);
	Binding.Initialize(DestStore, this);
}

void FNiagaraParameterStore::Unbind(FNiagaraParameterStore* DestStore)
{
	FNiagaraParameterStoreBinding* Binding = Bindings.Find(DestStore);
	if (Binding)
	{
		Binding->Empty(DestStore, this);
		Bindings.Remove(DestStore);
	}
}

void FNiagaraParameterStore::Rebind()
{
	SCOPE_CYCLE_COUNTER(STAT_NiagaraParameterStoreRebind);
	for (TPair<FNiagaraParameterStore*, FNiagaraParameterStoreBinding>& Binding : Bindings)
	{
		Binding.Value.Initialize(Binding.Key, this);
	}
}

void FNiagaraParameterStore::TransferBindings(FNiagaraParameterStore& OtherStore)
{
	for (TPair<FNiagaraParameterStore*, FNiagaraParameterStoreBinding>& Binding : Bindings)
	{
		OtherStore.Bind(Binding.Key);
	}

	Bindings.Empty();
}

bool FNiagaraParameterStore::VerifyBinding(const FNiagaraParameterStore* DestStore)const
{
#if WITH_EDITORONLY_DATA
	const FNiagaraParameterStoreBinding* Binding = Bindings.Find(DestStore);
	if (Binding)
	{
		return Binding->VerifyBinding(DestStore, this);
	}
	else
	{
		UE_LOG(LogNiagara, Warning, TEXT("Invalid ParameterStore Binding: % was not bound to %s."), *DebugName, *DestStore->DebugName);
	}

	return false;
#else
	return true;
#endif
}

void FNiagaraParameterStore::CheckForNaNs()const
{
	for (const TPair<FNiagaraVariable, int32>& ParamOffset : ParameterOffsets)
	{
		const FNiagaraVariable& Var = ParamOffset.Key;
		int32 Offset = ParamOffset.Value;
		bool bContainsNans = false;
		if (Var.GetType() == FNiagaraTypeDefinition::GetFloatDef())
		{
			float Val = *(float*)GetParameterData(Offset);
			bContainsNans = FMath::IsNaN(Val) || !FMath::IsFinite(Val);
		}
		else if (Var.GetType() == FNiagaraTypeDefinition::GetVec2Def())
		{
			FVector2D Val = *(FVector2D*)GetParameterData(Offset);
			bContainsNans = Val.ContainsNaN();
		}
		else if (Var.GetType() == FNiagaraTypeDefinition::GetVec3Def())
		{
			FVector Val = *(FVector*)GetParameterData(Offset);
			bContainsNans = Val.ContainsNaN();
		}
		else if (Var.GetType() == FNiagaraTypeDefinition::GetVec4Def())
		{
			FVector4 Val = *(FVector4*)GetParameterData(Offset);
			bContainsNans = Val.ContainsNaN();
		}
		else if (Var.GetType() == FNiagaraTypeDefinition::GetMatrix4Def())
		{
			FMatrix Val;
			FMemory::Memcpy(&Val, GetParameterData(Offset), sizeof(FMatrix));
			bContainsNans = Val.ContainsNaN();
		}

		if (bContainsNans)
		{
			ensureAlwaysMsgf(false, TEXT("Niagara Parameter Store containts Nans!\n"));
			DumpParameters(false);
		}
	}
}

void FNiagaraParameterStore::Tick()
{
#if NIAGARA_NAN_CHECKING
	CheckForNaNs();
#endif
	if (Bindings.Num() > 0 && (bParametersDirty || bInterfacesDirty || bUObjectsDirty))
	{
		SCOPE_CYCLE_COUNTER(STAT_NiagaraParameterStoreTick);
		for (TPair<FNiagaraParameterStore*, FNiagaraParameterStoreBinding>& Binding : Bindings)
		{
			Binding.Value.Tick(Binding.Key, this);
		}
		Dump();
	}

	//We have to have ticked all our source stores before now.
	bParametersDirty = false;
	bInterfacesDirty = false;
	bUObjectsDirty = false;
}

void FNiagaraParameterStore::UnbindFromSourceStores()
{
	//Each source store will remove itself from this array as it is unbound so after N unbinds the array should be empty.
	int32 NumSourceStores = SourceStores.Num();
	while (NumSourceStores--)
	{
		SourceStores[0]->Unbind(this);
	}
	ensureMsgf(SourceStores.Num() == 0, TEXT("Parameter store source array was not empty after unbinding all sources. Something seriously wrong."));
}

void FNiagaraParameterStore::DumpParameters(bool bDumpBindings)const
{
	TArray<FNiagaraVariable> Vars;
	GetParameters(Vars);
	for (FNiagaraVariable& Var : Vars)
	{
		Var.SetData(GetParameterData_Internal(IndexOf(Var))); // Need to keep the data in sync
		UE_LOG(LogNiagara, Log, TEXT("Param: %s Offset: %d Type : %s"), *Var.ToString(), IndexOf(Var), *Var.GetType().GetName());
	}

	if (bDumpBindings)
	{
		for (const TPair<FNiagaraParameterStore*, FNiagaraParameterStoreBinding>& Binding : Bindings)
		{
			Binding.Value.Dump(Binding.Key, this);
		}
	}
}

FString FNiagaraParameterStore::ToString() const
{
	FString Value;
	TArray<FNiagaraVariable> Vars;
	GetParameters(Vars);
	for (FNiagaraVariable& Var : Vars)
	{
		Var.SetData(GetParameterData_Internal(IndexOf(Var))); // Need to keep the data in sync
		Value += FString::Printf(TEXT("Param: %s Offset: %d Type : %s\n"), *Var.ToString(), IndexOf(Var), *Var.GetType().GetName());
	}
	return Value;
}

void FNiagaraParameterStore::Dump()
{
#if WITH_EDITORONLY_DATA
	if (GbDumpParticleParameterStores && GetParametersDirty())
	{
		UE_LOG(LogNiagara, Log, TEXT("\nSource Store: %s\n========================\n"), *DebugName);
		DumpParameters(true);
		
		UE_LOG(LogNiagara, Log, TEXT("\n========================\n"));
	}
#endif
}

/**
Adds the passed parameter to this store.
Does nothing if this parameter is already present.
Returns true if we added a new parameter.
*/
bool FNiagaraParameterStore::AddParameter(const FNiagaraVariable& Param, bool bInitInterfaces, bool bTriggerRebind)
{
	if (int32* Existing = ParameterOffsets.Find(Param))
	{
		return false;
	}

	if (Param.GetType().IsDataInterface())
	{
		int32 Offset = DataInterfaces.AddZeroed();
		ParameterOffsets.Add(Param) = Offset;
		DataInterfaces[Offset] = bInitInterfaces ? NewObject<UNiagaraDataInterface>(Owner, const_cast<UClass*>(Param.GetType().GetClass()), NAME_None, RF_Transactional | RF_Public) : nullptr;
	}
	else if (Param.GetType().IsUObject())
	{
		int32 Offset = UObjects.AddDefaulted();
		ParameterOffsets.Add(Param) = Offset;
		//UObjects[Offset] = nullptr;
	}
	else
	{
		DEC_MEMORY_STAT_BY(STAT_NiagaraParamStoreMemory, ParameterData.GetAllocatedSize());

		int32 ParamSize = Param.GetSizeInBytes();
		int32 ParamAlignment = Param.GetAlignment();
		//int32 Offset = AlignArbitrary(ParameterData.Num(), ParamAlignment);//TODO: We need to handle alignment better here. Need to both satisfy CPU and GPU alignment concerns. VM doesn't care but the VM complier needs to be aware. Probably best to have everything adhere to GPU alignment rules.
		int32 Offset = ParameterData.Num();
		FNiagaraVariable ParamWithNoAllocatedData(Param.GetType(), Param.GetName());
		ParameterOffsets.Add(ParamWithNoAllocatedData) = Offset; // We don't need the default value saved in here.
		ParameterData.AddUninitialized(ParamSize);
				
		INC_MEMORY_STAT_BY(STAT_NiagaraParamStoreMemory, ParameterData.GetAllocatedSize());

		//Temporary to init param data from FNiagaraVariable storage. This will be removed when we change the UNiagaraScript to use a parameter store too.
		if (Param.IsDataAllocated())
		{
			uint8* Dest = GetParameterData_Internal(Offset);
			const uint8* Src = Param.GetData();
			if (Dest != Src)
			{
				FMemory::Memcpy(Dest, Src, ParamSize);
			}
		}
	}

	if (bTriggerRebind)
	{
		OnLayoutChange();
	}
	else
	{
		++LayoutVersion;
	}
	
	return true;
}

bool FNiagaraParameterStore::RemoveParameter(const FNiagaraVariable& ToRemove)
{
	if (int32* ExistingIndex = ParameterOffsets.Find(ToRemove))
	{
		//TODO: Ensure direct bindings are either updated or disallowed here.
		//We have to regenerate the store and the offsets on removal. This shouldn't happen at runtime!
		TMap<FNiagaraVariable, int32> NewOffsets;
		TArray<uint8> NewData;
		TArray<UNiagaraDataInterface*> NewInterfaces;
		TArray<UObject*> NewUObjects;
		for (TPair<FNiagaraVariable, int32>& Existing : ParameterOffsets)
		{
			FNiagaraVariable& ExistingVar = Existing.Key;
			int32& ExistingOffset = Existing.Value;

			//Add all but the one to remove to our
			if (ExistingVar != ToRemove)
			{
				if (ExistingVar.GetType().IsDataInterface())
				{
					int32 Offset = NewInterfaces.AddZeroed();
					NewOffsets.Add(ExistingVar) = Offset;
					NewInterfaces[Offset] = DataInterfaces[ExistingOffset];
				}
				else if (ExistingVar.IsUObject())
				{
					int32 Offset = NewUObjects.AddDefaulted();
					NewOffsets.Add(ExistingVar) = Offset;
					NewUObjects[Offset] = UObjects[ExistingOffset];
				}
				else
				{
					int32 Offset = NewData.Num();
					int32 ParamSize = ExistingVar.GetSizeInBytes();
					NewOffsets.Add(ExistingVar) = Offset;
					NewData.AddUninitialized(ParamSize);
					FMemory::Memcpy(NewData.GetData() + Offset, ParameterData.GetData() + ExistingOffset, ParamSize);
				}
			}
		}

		DEC_MEMORY_STAT_BY(STAT_NiagaraParamStoreMemory, ParameterData.GetAllocatedSize());

		ParameterOffsets = NewOffsets;
		ParameterData = NewData;
		DataInterfaces = NewInterfaces;
		UObjects = NewUObjects;

		INC_MEMORY_STAT_BY(STAT_NiagaraParamStoreMemory, ParameterData.GetAllocatedSize());

		OnLayoutChange();
		return true;
	}

	return false;
}

void FNiagaraParameterStore::RenameParameter(const FNiagaraVariable& Param, FName NewName)
{
	int32 Idx = IndexOf(Param);
	if(Idx != INDEX_NONE)
	{
		FNiagaraVariable NewParam = Param;
		NewParam.SetName(NewName);

		bool bInitInterfaces = false;
		bool bTriggerRebind = false;

		int32 NumBytesBefore = ParameterData.Num();
		AddParameter(NewParam, bInitInterfaces, bTriggerRebind);
		int32 NumBytesAfter = ParameterData.Num();
		int32 DeltaBytes = NumBytesAfter - NumBytesBefore;
		int32 SizeInBytes = Param.GetSizeInBytes();

		check(DeltaBytes == SizeInBytes);

		int32 NewIdx = IndexOf(NewParam);
		if (Param.IsDataInterface())
		{
			SetDataInterface(GetDataInterface(Idx), NewIdx);
		}
		else if (Param.IsUObject())
		{
			SetUObject(GetUObject(Idx), NewIdx);
		}
		else
		{
			SetParameterData(GetParameterData_Internal(Idx), NewIdx, Param.GetSizeInBytes());
		}
		RemoveParameter(Param);

		OnLayoutChange();
	}
}

void FNiagaraParameterStore::SanityCheckData(bool bInitInterfaces)
{
	// This function exists to patch up the issue seen in FORT-208391, where we had entries for DataInterfaces in the offset array but not in the actual DataInterface array entries.
	// Additional protections were added for safety.
	TMap<FNiagaraVariable, int32>::TConstIterator It = ParameterOffsets.CreateConstIterator();
	while (It)
	{
		FNiagaraVariable Parameter = It->Key;
		int32 SrcIndex = It->Value;
		++It;

		if (Parameter.IsValid())
		{
			if (Parameter.IsDataInterface())
			{
				if (DataInterfaces.Num() <= SrcIndex)
				{
					int32 OriginalNum = DataInterfaces.Num();
					int32 NewNum = SrcIndex - DataInterfaces.Num() + 1;
					DataInterfaces.AddZeroed(NewNum);
					UE_LOG(LogNiagara, Warning, TEXT("Missing data interfaces! Had to add %d data interface entries to ParameterStore on %s"), NewNum , Owner != nullptr ? *Owner->GetPathName() : TEXT("Unknown owner"));
				}
				if (DataInterfaces[SrcIndex] == nullptr && bInitInterfaces && Owner)
				{
					DataInterfaces[SrcIndex] = NewObject<UNiagaraDataInterface>(Owner, const_cast<UClass*>(Parameter.GetType().GetClass()), NAME_None, RF_Transactional | RF_Public);
					UE_LOG(LogNiagara, Warning, TEXT("Had to initialize data interface! %s on %s"), *Parameter.GetName().ToString(), Owner != nullptr ? *Owner->GetPathName() : TEXT("Unknown owner"));
				}
			}
			else if (Parameter.IsUObject())
			{
				if (UObjects.Num() <= SrcIndex)
				{
					int32 OriginalNum = UObjects.Num();
					int32 NewNum = SrcIndex - UObjects.Num() + 1;
					UObjects.AddZeroed(NewNum);
					UE_LOG(LogNiagara, Warning, TEXT("Missing UObject interfaces! Had to add %d UObject entries for %s on %s"), NewNum , *Parameter.GetName().ToString(), Owner != nullptr ? *Owner->GetPathName() : TEXT("Unknown owner"));
				}
			}
			else
			{
				int32 Size = Parameter.GetType().GetSize();
				if (ParameterData.Num() < (SrcIndex + Size))
				{
					UE_LOG(LogNiagara, Warning, TEXT("Missing parameter data! %s on %s"), *Parameter.GetName().ToString(), Owner != nullptr ? *Owner->GetPathName() : TEXT("Unknown owner"));
				}
			}
		}
	}
}

void FNiagaraParameterStore::CopyParametersTo(FNiagaraParameterStore& DestStore, bool bOnlyAdd, EDataInterfaceCopyMethod DataInterfaceCopyMethod)
{
	TMap<FNiagaraVariable, int32>::TConstIterator It = ParameterOffsets.CreateConstIterator();
	while (It)
	{
		FNiagaraVariable Parameter = It->Key;
		int32 SrcIndex = It->Value;
		++It;

		if (Parameter.IsValid() == false)
		{
			FString StoreDebugName;
#if WITH_EDITORONLY_DATA
			StoreDebugName = DebugName.IsEmpty() == false ? DebugName : TEXT("Unknown");
#else
			StoreDebugName = TEXT("Unknown");
#endif
			FString StoreName;
			if (Owner != nullptr)
			{
				StoreName = Owner->GetPathName() + TEXT(".") + StoreDebugName;
			}
			else
			{
				StoreName = StoreDebugName;
			}

			UE_LOG(LogNiagara, Error, TEXT("Invalid parameter found while attempting to copy parameters from one parameter store to another.  Parameter Store: %s Parameter Name: %s Parameter Type: %s"), 
				*StoreName, *Parameter.GetName().ToString(), Parameter.GetType().IsValid() ? *Parameter.GetType().GetName() : TEXT("Unknown"));
			continue;
		}

		int32 DestIndex = DestStore.IndexOf(Parameter);
		bool bWrite = false;
		if (DestIndex == INDEX_NONE)
		{
			bool bInitInterfaces = bOnlyAdd == false && Parameter.IsDataInterface() && DataInterfaceCopyMethod == EDataInterfaceCopyMethod::Value;
			bool bTriggerRebind = false;
			DestStore.AddParameter(Parameter, bInitInterfaces, bTriggerRebind);
			bWrite = !bOnlyAdd;
			DestIndex = DestStore.IndexOf(Parameter);
		}
		else if (!bOnlyAdd)
		{
			bWrite = true;
		}

		if (bWrite && DestIndex != INDEX_NONE && SrcIndex != INDEX_NONE)
		{
			if (Parameter.IsDataInterface())
			{
				ensure(DataInterfaces.IsValidIndex(SrcIndex));
				ensure(DestStore.DataInterfaces.IsValidIndex(DestIndex));
				if (DataInterfaceCopyMethod == EDataInterfaceCopyMethod::Reference)
				{
					DestStore.SetDataInterface(DataInterfaces[SrcIndex], DestIndex);
				}
				else if(DataInterfaceCopyMethod == EDataInterfaceCopyMethod::Value)
				{
					UNiagaraDataInterface* SourceInterface = DataInterfaces[SrcIndex];
					SourceInterface->CopyTo(DestStore.GetDataInterface(DestIndex));
				}
				else
				{
					checkf(false, TEXT("A data interface copy method must be specified if the parameter store has data interfaces."));
				}
			}
			else if (Parameter.IsUObject())
			{
				DestStore.SetUObject(GetUObject(SrcIndex), DestIndex);//UObjects are just refs to external objects. They never need to be deep copied.
			}
			else
			{
				if (ParameterData.Num() != 0)
				{
					DestStore.SetParameterData(GetParameterData_Internal(SrcIndex), DestIndex, Parameter.GetSizeInBytes());
				}
			}
		}
	}
	DestStore.OnLayoutChange();
}


FORCEINLINE void FNiagaraParameterStore::SetParameterDataArray(const TArray<uint8>& InParameterDataArray)
{
	DEC_MEMORY_STAT_BY(STAT_NiagaraParamStoreMemory, ParameterData.GetAllocatedSize());
	ParameterData = InParameterDataArray;
	INC_MEMORY_STAT_BY(STAT_NiagaraParamStoreMemory, ParameterData.GetAllocatedSize());

	OnParameterChange();
}

void FNiagaraParameterStore::InitFromSource(const FNiagaraParameterStore* SrcStore, bool bNotifyAsDirty)
{
	Empty(false);
	if (SrcStore == nullptr)
	{
		return;
	}

	ParameterOffsets = SrcStore->ParameterOffsets;

	DEC_MEMORY_STAT_BY(STAT_NiagaraParamStoreMemory, ParameterData.GetAllocatedSize());
	ParameterData = SrcStore->ParameterData;
	INC_MEMORY_STAT_BY(STAT_NiagaraParamStoreMemory, ParameterData.GetAllocatedSize());

	DataInterfaces = SrcStore->DataInterfaces;

	UObjects = SrcStore->UObjects;

	if (bNotifyAsDirty)
	{
		MarkParametersDirty();
		MarkInterfacesDirty();
		MarkUObjectsDirty();
		OnLayoutChange();
	}
}

void FNiagaraParameterStore::RemoveParameters(FNiagaraParameterStore& DestStore)
{
	TMap<FNiagaraVariable, int32>::TConstIterator It = ParameterOffsets.CreateConstIterator();
	while (It)
	{
		FNiagaraVariable Parameter = It->Key;
		DestStore.RemoveParameter(Parameter);
		++It;
	}
}

void FNiagaraParameterStore::Empty(bool bClearBindings)
{
	ParameterOffsets.Empty();
	DEC_MEMORY_STAT_BY(STAT_NiagaraParamStoreMemory, ParameterData.GetAllocatedSize());
	ParameterData.Empty();
	DEC_MEMORY_STAT_BY(STAT_NiagaraParamStoreMemory, ParameterData.GetAllocatedSize());

	DataInterfaces.Empty();

	UObjects.Empty();

	if (bClearBindings)
	{
		UnbindFromSourceStores();
		Bindings.Empty();
	}
}

void FNiagaraParameterStore::Reset(bool bClearBindings)
{
	ParameterOffsets.Reset();
	DEC_MEMORY_STAT_BY(STAT_NiagaraParamStoreMemory, ParameterData.GetAllocatedSize());
	ParameterData.Reset();
	DEC_MEMORY_STAT_BY(STAT_NiagaraParamStoreMemory, ParameterData.GetAllocatedSize());

	DataInterfaces.Reset();

	UObjects.Reset();

	if (bClearBindings)
	{
		UnbindFromSourceStores();
		Bindings.Reset();
	}
}

void FNiagaraParameterStore::OnLayoutChange()
{
	// The VM require that the parameter data we send it in FNiagaraScriptExecutionContext::Execute
	// is aligned to VECTOR_WIDTH_BYTES *and* is padded with an additional VECTOR_WIDTH_BYTES.
	// This is due to possible unaligned reads, e.g. an integer might be stored in the very last byte
	// of the aligned parameter data due to the packing, which will spill 3 bytes outside the bounds
	int32 ExpectedSlack = Align(ParameterData.Num(), VECTOR_WIDTH_BYTES) + VECTOR_WIDTH_BYTES;
	if (ParameterData.Max() < ExpectedSlack)
	{
		ParameterData.Reserve(ExpectedSlack);
	}
	Rebind();
	++LayoutVersion;

#if WITH_EDITOR
	OnChangedDelegate.Broadcast();
#endif
}

const FNiagaraVariable* FNiagaraParameterStore::FindVariable(UNiagaraDataInterface* Interface)const
{
	SCOPE_CYCLE_COUNTER(STAT_NiagaraParameterStoreFindVar);
	int32 Idx = DataInterfaces.IndexOfByKey(Interface);
	if (Idx != INDEX_NONE)
	{
		for (const TPair<FNiagaraVariable, int32>& Existing : ParameterOffsets)
		{
			const FNiagaraVariable& ExistingVar = Existing.Key;
			const int32& ExistingOffset = Existing.Value;

			if (ExistingOffset == Idx && ExistingVar.GetType().GetClass() == Interface->GetClass())
			{
				return &ExistingVar;
			}
		}
	}
	return nullptr;
}

#if WITH_EDITOR
FDelegateHandle FNiagaraParameterStore::AddOnChangedHandler(FOnChanged::FDelegate InOnChanged)
{
	return OnChangedDelegate.Add(InOnChanged);
}

void FNiagaraParameterStore::RemoveOnChangedHandler(FDelegateHandle DelegateHandle)
{
	OnChangedDelegate.Remove(DelegateHandle);
}

void FNiagaraParameterStore::RemoveAllOnChangedHandlers(const void* InUserObject)
{
	OnChangedDelegate.RemoveAll(InUserObject);
}
#endif

//////////////////////////////////////////////////////////////////////////