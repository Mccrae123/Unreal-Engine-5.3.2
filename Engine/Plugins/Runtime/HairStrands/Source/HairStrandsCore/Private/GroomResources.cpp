// Copyright Epic Games, Inc. All Rights Reserved.

#include "GroomResources.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "GroomAssetImportData.h"
#include "GroomBuilder.h"
#include "GroomImportOptions.h"
#include "GroomSettings.h"
#include "RenderingThread.h"
#include "Engine/AssetUserData.h"
#include "HairStrandsVertexFactory.h"
#include "Misc/Paths.h"
#include "RHIStaticStates.h"
#include "Serialization/LargeMemoryReader.h"
#include "Serialization/LargeMemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/BulkData.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/PhysicsObjectVersion.h"
#include "UObject/ReleaseObjectVersion.h"
#include "NiagaraSystem.h"
#include "Async/ParallelFor.h"
#include "RenderGraph.h"
#include "RenderGraphUtils.h"
#include "RenderTargetPool.h"
#include "GroomBindingBuilder.h"
#if WITH_EDITORONLY_DATA
#include "Containers/StringView.h"
#include "DerivedDataCache.h"
#include "DerivedDataRequestOwner.h"
#endif

static int32 GHairStrandsBulkData_ReleaseAfterUse = 0;
static FAutoConsoleVariableRef CVarHairStrandsBulkData_ReleaseAfterUse(TEXT("r.HairStrands.Strands.BulkData.ReleaseAfterUse"), GHairStrandsBulkData_ReleaseAfterUse, TEXT("Release CPU bulk data once hair groom/groom binding asset GPU resources are created. This saves memory"));

static int32 GHairStrandsBulkData_AsyncLoading = -1;
static int32 GHairCardsBulkData_AsyncLoading = -1;

static FAutoConsoleVariableRef CVarHairStrandsBulkData_AsyncLoading(TEXT("r.HairStrands.Strands.BulkData.AsyncLoading"), GHairStrandsBulkData_AsyncLoading, TEXT("Load hair strands data with async loading so that it is not blocking the rendering thread. This value define the MinLOD at which this happen. Default disabled (-1)"));
static FAutoConsoleVariableRef CVarHairCardsBulkData_AsyncLoading(TEXT("r.HairStrands.Cards.BulkData.AsyncLoading"), GHairCardsBulkData_AsyncLoading, TEXT("Load hair cards/meshes data with async loading so that it is not blocking the rendering thread. This value define the MinLOD at which this happen. Default disabled (-1)"));

static int32 GHairStrandsBulkData_Validation = 1;
static FAutoConsoleVariableRef CVarHairStrandsBulkData_Validation(TEXT("r.HairStrands.Strands.BulkData.Validation"), GHairStrandsBulkData_Validation, TEXT("Validate some hair strands data at serialization/loading time."));

static float GHairStrandsDebugVoxel_WorldSize = 0.3f;
static int32 GHairStrandsDebugVoxel_MaxSegmentPerVoxel = 2048;
static FAutoConsoleVariableRef CVarHairStrandsDebugVoxel_WorldSize(TEXT("r.HairStrands.DebugData.VoxelSize"), GHairStrandsDebugVoxel_WorldSize, TEXT("Voxel size use for creating debug data."));
static FAutoConsoleVariableRef CVarHairStrandsDebugVoxel_MaxSegmentPerVoxel(TEXT("r.HairStrands.DebugData.MaxSegmentPerVoxel"), GHairStrandsDebugVoxel_MaxSegmentPerVoxel, TEXT("Max number of segments per Voxel size when creating debug data."));

bool IsHairStrandsContinousLODEnabled();

bool ValidateHairBulkData()
{
	return GHairStrandsBulkData_Validation > 0;
}

////////////////////////////////////////////////////////////////////////////////////

EHairResourceLoadingType GetHairResourceLoadingType(EHairGeometryType InGeometryType, int32 InLODIndex)
{
#if !WITH_EDITORONLY_DATA
	switch (InGeometryType)
	{
	case EHairGeometryType::Strands: return InLODIndex <= GHairStrandsBulkData_AsyncLoading ? EHairResourceLoadingType::Async : EHairResourceLoadingType::Sync;
	case EHairGeometryType::Cards:
	case EHairGeometryType::Meshes: return InLODIndex <= GHairCardsBulkData_AsyncLoading ? EHairResourceLoadingType::Async : EHairResourceLoadingType::Sync;
	}
	return EHairResourceLoadingType::Sync;
#else
	return EHairResourceLoadingType::Sync;
#endif
}

enum class EHairResourceUsageType : uint8
{
	Static,
	Dynamic
};

#define HAIRSTRANDS_RESOUCE_NAME(Type, Name) (Type == EHairStrandsResourcesType::Guides  ? TEXT(#Name "(Guides)") : (Type == EHairStrandsResourcesType::Strands ? TEXT(#Name "(Strands)") : TEXT(#Name "(Cards)")))

const TCHAR* ToHairResourceDebugName(const TCHAR* In, FHairResourceName& InDebugNames)
{
#if HAIR_RESOURCE_DEBUG_NAME
	FString& TempDebugName = InDebugNames.Names.AddDefaulted_GetRef();
	TempDebugName = In;
	if (InDebugNames.GroupIndex >= 0)
	{
		TempDebugName += TEXT("_GROUP") + FString::FromInt(InDebugNames.GroupIndex);
	}
	if (InDebugNames.LODIndex >= 0)
	{
		TempDebugName += TEXT("_LOD") + FString::FromInt(InDebugNames.LODIndex);
	}
	TempDebugName += TEXT("_") + InDebugNames.AssetName.ToString();
	return *TempDebugName;
#else
	return In;
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////
// FRWBuffer utils 
void UploadDataToBuffer(FReadBuffer& OutBuffer, uint32 DataSizeInBytes, const void* InCpuData)
{
	void* BufferData = RHILockBuffer(OutBuffer.Buffer, 0, DataSizeInBytes, RLM_WriteOnly);
	FMemory::Memcpy(BufferData, InCpuData, DataSizeInBytes);
	RHIUnlockBuffer(OutBuffer.Buffer);
}

void UploadDataToBuffer(FRWBufferStructured& OutBuffer, uint32 DataSizeInBytes, const void* InCpuData)
{
	void* BufferData = RHILockBuffer(OutBuffer.Buffer, 0, DataSizeInBytes, RLM_WriteOnly);
	FMemory::Memcpy(BufferData, InCpuData, DataSizeInBytes);
	RHIUnlockBuffer(OutBuffer.Buffer);
}

template<typename FormatType>
void CreateBuffer(const TArray<typename FormatType::Type>& InData, FRWBuffer& OutBuffer, const TCHAR* DebugName, ERHIAccess InitialAccess = ERHIAccess::SRVMask)
{
	const uint32 DataCount = InData.Num();
	const uint32 DataSizeInBytes = FormatType::SizeInByte*DataCount;

	if (DataSizeInBytes == 0) return;

	OutBuffer.Initialize(FormatType::SizeInByte, DataCount, FormatType::Format, InitialAccess, BUF_Static, DebugName);
	void* BufferData = RHILockBuffer(OutBuffer.Buffer, 0, DataSizeInBytes, RLM_WriteOnly);

	FMemory::Memcpy(BufferData, InData.GetData(), DataSizeInBytes);
	RHIUnlockBuffer(OutBuffer.Buffer);
}

template<typename FormatType>
void CreateBuffer(uint32 InVertexCount, FRWBuffer& OutBuffer, const TCHAR* DebugName)
{
	const uint32 DataCount = InVertexCount;
	const uint32 DataSizeInBytes = FormatType::SizeInByte*DataCount;

	if (DataSizeInBytes == 0) return;

	OutBuffer.Initialize(FormatType::SizeInByte, DataCount, FormatType::Format, ERHIAccess::UAVCompute, BUF_Static, DebugName);
	void* BufferData = RHILockBuffer(OutBuffer.Buffer, 0, DataSizeInBytes, RLM_WriteOnly);
	FMemory::Memset(BufferData, 0, DataSizeInBytes);
	RHIUnlockBuffer(OutBuffer.Buffer);
}

template<typename FormatType>
void CreateBuffer(const TArray<typename FormatType::Type>& InData, FHairCardsVertexBuffer& OutBuffer, const TCHAR* DebugName, const FName& OwnerName, ERHIAccess InitialAccess = ERHIAccess::SRVMask)
{
	const uint32 DataCount = InData.Num();
	const uint32 DataSizeInBytes = FormatType::SizeInByte * DataCount;

	if (DataSizeInBytes == 0) return;

	FRHIResourceCreateInfo CreateInfo(DebugName);
	CreateInfo.ResourceArray = nullptr;

	OutBuffer.VertexBufferRHI = RHICreateVertexBuffer(DataSizeInBytes, BUF_Static | BUF_ShaderResource, InitialAccess, CreateInfo);
	OutBuffer.VertexBufferRHI->SetOwnerName(OwnerName);

	void* BufferData = RHILockBuffer(OutBuffer.VertexBufferRHI, 0, DataSizeInBytes, RLM_WriteOnly);
	FMemory::Memcpy(BufferData, InData.GetData(), DataSizeInBytes);
	RHIUnlockBuffer(OutBuffer.VertexBufferRHI);
	OutBuffer.ShaderResourceViewRHI = RHICreateShaderResourceView(OutBuffer.VertexBufferRHI, FormatType::SizeInByte, FormatType::Format);
}

/////////////////////////////////////////////////////////////////////////////////////////
// RDG buffers utils 

static FRDGBufferDesc ApplyUsage(FRDGBufferDesc In, EHairResourceUsageType InUsage)
{
	if (InUsage != EHairResourceUsageType::Dynamic)
	{
		In.Usage = In.Usage & (~BUF_UnorderedAccess);
	}
	return In;
}

inline bool ReleaseAfterUse() { return GHairStrandsBulkData_ReleaseAfterUse > 0; }

inline void InternalSetBulkDataFlags(FByteBulkData& In)
{
	// Unloading of the bulk data is only supported on cooked build, as we can reload the data from the file/archieve
#if !WITH_EDITORONLY_DATA
	const bool bReleaseCPUData = ReleaseAfterUse();
	if (bReleaseCPUData)
	{
		In.SetBulkDataFlags(BULKDATA_SingleUse);
	}
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Regular loading from BulkData

void InternalCreateBufferRDG_FromBulkData(FRDGBuilder& GraphBuilder, FByteBulkData& InBulkData, FRDGExternalBuffer& Out, const EPixelFormat OutFormat, const FRDGBufferDesc Desc, const TCHAR* DebugName, const FName& OwnerName)
{
	InternalSetBulkDataFlags(InBulkData);

	const uint32 DataSizeInBytes = Desc.GetSize();
	check(InBulkData.GetBulkDataSize() >= DataSizeInBytes);
	if (DataSizeInBytes == 0)
	{
		Out.Buffer = nullptr;
		return;
	}

	const uint8* Data = (const uint8*)InBulkData.Lock(LOCK_READ_ONLY);
	FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(Desc, DebugName, ERDGBufferFlags::MultiFrame);
	Buffer->SetOwnerName(OwnerName);
	if (Data && DataSizeInBytes)
	{
		#if !WITH_EDITORONLY_DATA
		if (ReleaseAfterUse())
		{
			GraphBuilder.QueueBufferUpload(Buffer, Data, DataSizeInBytes, [&InBulkData](const void* Ptr) { InBulkData.Unlock(); });
		}
		else
		#endif
		{
			GraphBuilder.QueueBufferUpload(Buffer, Data, DataSizeInBytes, ERDGInitialDataFlags::None);  // Copy data internally
			InBulkData.Unlock();
		}
	}
	ConvertToExternalBufferWithViews(GraphBuilder, Buffer, Out, OutFormat);
}

template<typename FormatType>
void InternalCreateVertexBufferRDG_FromBulkData(FRDGBuilder& GraphBuilder, FByteBulkData& InBulkData, uint32 InDataCount, FRDGExternalBuffer& Out, const TCHAR* DebugName, const FName& OwnerName, EHairResourceUsageType UsageType)
{
	const FRDGBufferDesc Desc = ApplyUsage(FRDGBufferDesc::CreateBufferDesc(FormatType::SizeInByte, InDataCount), UsageType);
	InternalCreateBufferRDG_FromBulkData(GraphBuilder, InBulkData, Out, FormatType::Format, Desc, DebugName, OwnerName);
}

template<typename FormatType>
void InternalCreateStructuredBufferRDG_FromBulkData(FRDGBuilder& GraphBuilder, FByteBulkData& InBulkData, uint32 InDataCount, FRDGExternalBuffer& Out, const TCHAR* DebugName, const FName& OwnerName, EHairResourceUsageType UsageType)
{
	const FRDGBufferDesc Desc = ApplyUsage(FRDGBufferDesc::CreateStructuredDesc(FormatType::SizeInByte, InDataCount), UsageType);
	InternalCreateBufferRDG_FromBulkData(GraphBuilder, InBulkData, Out, PF_Unknown, Desc, DebugName, OwnerName);
}

void InternalCreateByteAddressBufferRDG_FromBulkData(FRDGBuilder& GraphBuilder, FByteBulkData& InBulkData, FRDGExternalBuffer& Out, const TCHAR* DebugName, const FName& OwnerName, EHairResourceUsageType UsageType)
{
	const FRDGBufferDesc Desc = ApplyUsage(FRDGBufferDesc::CreateByteAddressDesc(InBulkData.GetBulkDataSize()), UsageType);
	InternalCreateBufferRDG_FromBulkData(GraphBuilder, InBulkData, Out, PF_Unknown, Desc, DebugName, OwnerName);
}


///////////////////////////////////////////////////////////////////////////////////////////////////
// HairBulkData loading

static FRDGBufferRef InternalCreateBufferRDG_FromHairBulkData(FRDGBuilder& GraphBuilder, FHairBulkContainer& InData, const FRDGBufferRef& In, const FRDGBufferDesc& BufferDesc, const FRDGBufferDesc& UploadDesc, const TCHAR* DebugName, const FName& OwnerName)
{
	check(InData.ChunkRequest);
	FHairStreamingRequest::FChunk& InChunk = *InData.ChunkRequest;

	const bool bCreate 		= In == nullptr || BufferDesc.GetSize() == UploadDesc.GetSize();
	const bool bCopy 		= In != nullptr && InChunk.Size > 0;
	const bool bReallocate 	= In != nullptr && In->Desc.GetSize() < InChunk.TotalSize;

	check(InChunk.Size > 0);
	// Either create a new buffer or append new data to existing buffer
	if (bCreate)
	{	
		check(BufferDesc.GetSize() >= UploadDesc.GetSize());

		FRDGBufferRef Out = GraphBuilder.CreateBuffer(BufferDesc, DebugName, ERDGBufferFlags::MultiFrame);
		GraphBuilder.QueueBufferUpload(Out, InChunk.GetData(), InChunk.Size, ERDGInitialDataFlags::None);
		InChunk.Release();
		return Out;
	}
	else if (bCopy)
	{
		// 1. If the current buffer is too small for storing the new data, reallocate it
		FRDGBufferRef Out = In;
		if (bReallocate)
		{
			// 1.1 Create new buffer
			FRDGBufferDesc NewBufferDesc = BufferDesc;
			NewBufferDesc.Usage |= EBufferUsageFlags::UnorderedAccess;
			Out = GraphBuilder.CreateBuffer(NewBufferDesc, DebugName, ERDGBufferFlags::MultiFrame);
	
			// 1.2 Copy existing data from the old buffer to the new buffer
			AddCopyBufferPass(GraphBuilder, Out, 0, In, 0, In->Desc.GetSize());
		}

		// 2. Upload missing data
		FRDGBufferRef UploadBuffer = GraphBuilder.CreateBuffer(UploadDesc, DebugName, ERDGBufferFlags::MultiFrame);
		GraphBuilder.QueueBufferUpload(UploadBuffer, InChunk.GetData(), InChunk.Size, ERDGInitialDataFlags::None);
		InChunk.Release();

		// 4. Append new data to the new/existing buffer
		AddCopyBufferPass(GraphBuilder, Out, InChunk.Offset, UploadBuffer, 0, InChunk.Size);

		// Return the new buffer if it needs to be extracted
		return Out;
		//return bReallocate ? Out : nullptr;
	}
	else
	{
		return nullptr;
	}
}

template<typename FormatType>
void InternalCreateVertexBufferRDG_FromHairBulkData(FRDGBuilder& GraphBuilder, FHairBulkContainer& InChunk, uint32 InDataCount, FRDGExternalBuffer& Out, const TCHAR* DebugName, const FName& OwnerName, EHairResourceUsageType UsageType)
{
	// Fallback for non-streamble resources (e.g. guides)
	if (InChunk.ChunkRequest == nullptr)
	{
		InternalCreateVertexBufferRDG_FromBulkData<FormatType>(GraphBuilder, InChunk.Data, InDataCount, Out, DebugName, OwnerName, UsageType);
		return;
	}

	const FRDGBufferRef In = Out.Buffer ? Register(GraphBuilder, Out, ERDGImportedBufferFlags::None).Buffer : nullptr;
	const FRDGBufferDesc BufferDesc = ApplyUsage(FRDGBufferDesc::CreateBufferDesc(FormatType::SizeInByte, FMath::DivideAndRoundUp(InChunk.ChunkRequest->TotalSize, FormatType::SizeInByte)), UsageType);
	const FRDGBufferDesc UploadDesc = ApplyUsage(FRDGBufferDesc::CreateBufferDesc(FormatType::SizeInByte, FMath::DivideAndRoundUp(InChunk.ChunkRequest->Size, FormatType::SizeInByte)), UsageType);
	if (BufferDesc.GetSize() == 0) 	{ Out.Buffer = nullptr; return; }
	if (UploadDesc.GetSize()==0) 	{ return; }
	if (FRDGBufferRef Buffer = InternalCreateBufferRDG_FromHairBulkData(GraphBuilder, InChunk, In, BufferDesc, UploadDesc, DebugName, OwnerName))
	{
		ConvertToExternalBufferWithViews(GraphBuilder, Buffer, Out, FormatType::Format);
	}
}

template<typename FormatType>
void InternalCreateStructuredBufferRDG_FromHairBulkData(FRDGBuilder& GraphBuilder, FHairBulkContainer& InChunk, uint32 InDataCount, FRDGExternalBuffer& Out, const TCHAR* DebugName, const FName& OwnerName, EHairResourceUsageType UsageType)
{
	// Fallback for non-streamble resources (e.g. guides)
	if (InChunk.ChunkRequest == nullptr)
	{
		InternalCreateStructuredBufferRDG_FromHairBulkData<FormatType>(GraphBuilder, InChunk.Data, InDataCount, Out, DebugName, OwnerName, UsageType);
		return;
	}

	const FRDGBufferRef In = Out.Buffer ? Register(GraphBuilder, Out, ERDGImportedBufferFlags::None).Buffer : nullptr;
	const FRDGBufferDesc BufferDesc = ApplyUsage(FRDGBufferDesc::CreateStructuredDesc(FormatType::SizeInByte, FMath::DivideAndRoundUp(InChunk.ChunkRequest->TotalSize, FormatType::SizeInByte)), UsageType);
	const FRDGBufferDesc UploadDesc = ApplyUsage(FRDGBufferDesc::CreateStructuredDesc(FormatType::SizeInByte, FMath::DivideAndRoundUp(InChunk.ChunkRequest->Size, FormatType::SizeInByte)), UsageType);
	if (BufferDesc.GetSize() == 0) 	{ Out.Buffer = nullptr; return; }
	if (UploadDesc.GetSize()==0) 	{ return; }
	if (FRDGBufferRef Buffer = InternalCreateBufferRDG_FromHairBulkData(GraphBuilder, InChunk, In, BufferDesc, UploadDesc, DebugName, OwnerName))
	{
		ConvertToExternalBufferWithViews(GraphBuilder, Buffer, Out);
	}
}

void InternalCreateByteAddressBufferRDG_FromHairBulkData(FRDGBuilder& GraphBuilder, FHairBulkContainer& InChunk, FRDGExternalBuffer& Out, const TCHAR* DebugName, const FName& OwnerName, EHairResourceUsageType UsageType)
{
	// Fallback for non-streamble resources (e.g. guides)
	if (InChunk.ChunkRequest == nullptr)
	{
		InternalCreateByteAddressBufferRDG_FromBulkData(GraphBuilder, InChunk.Data, Out, DebugName, OwnerName, UsageType);
		return;
	}

	const FRDGBufferRef In = Out.Buffer ? Register(GraphBuilder, Out, ERDGImportedBufferFlags::None).Buffer : nullptr;
	const FRDGBufferDesc BufferDesc = ApplyUsage(FRDGBufferDesc::CreateByteAddressDesc(InChunk.ChunkRequest->TotalSize), UsageType);
	const FRDGBufferDesc UploadDesc = ApplyUsage(FRDGBufferDesc::CreateByteAddressDesc(InChunk.ChunkRequest->Size), UsageType);
	if (BufferDesc.GetSize() == 0) 	{ Out.Buffer = nullptr; return; }
	if (UploadDesc.GetSize()==0) 	{ return; }
	if (FRDGBufferRef Buffer = InternalCreateBufferRDG_FromHairBulkData(GraphBuilder, InChunk, In, BufferDesc, UploadDesc, DebugName, OwnerName))
	{
		ConvertToExternalBufferWithViews(GraphBuilder, Buffer, Out);
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Regular data loading

static FRDGBufferRef InternalCreateVertexBuffer(
	FRDGBuilder& GraphBuilder,
	const TCHAR* Name,
	const FRDGBufferDesc& Desc,
	const void* InitialData,
	uint64 InitialDataSize,
	ERDGInitialDataFlags InitialDataFlags,
	const FName& OwnerName)
{
	checkf(EnumHasAnyFlags(Desc.Usage, EBufferUsageFlags::VertexBuffer), TEXT("CreateVertexBuffer called with an FRDGBufferDesc underlying type that is not 'VertexBuffer'. Buffer: %s"), Name);
	FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(Desc, Name, ERDGBufferFlags::MultiFrame);
	Buffer->SetOwnerName(OwnerName);
	if (InitialData && InitialDataSize)
	{
		GraphBuilder.QueueBufferUpload(Buffer, InitialData, InitialDataSize, InitialDataFlags);
	}
	return Buffer;
}

template<typename FormatType>
void InternalCreateVertexBufferRDG(FRDGBuilder& GraphBuilder, const typename FormatType::Type* InData, uint32 InDataCount, FRDGExternalBuffer& Out, const TCHAR* DebugName, const FName& OwnerName, EHairResourceUsageType UsageType, ERDGInitialDataFlags InitialDataFlags)
{
	FRDGBufferRef Buffer = nullptr;

	const uint32 DataSizeInBytes = FormatType::SizeInByte * InDataCount;
	if (DataSizeInBytes == 0)
	{
		Out.Buffer = nullptr;
		return;
	}

	const FRDGBufferDesc Desc = ApplyUsage(FRDGBufferDesc::CreateBufferDesc(FormatType::SizeInByte, InDataCount), UsageType);
	Buffer = InternalCreateVertexBuffer(
		GraphBuilder,
		DebugName,
		Desc,
		InData,
		DataSizeInBytes,
		InitialDataFlags,
		OwnerName);

	ConvertToExternalBufferWithViews(GraphBuilder, Buffer, Out, FormatType::Format);
}

template<typename FormatType>
void InternalCreateVertexBufferRDG(FRDGBuilder& GraphBuilder, const TArray<typename FormatType::Type>& InData, FRDGExternalBuffer& Out, const TCHAR* DebugName, const FName& OwnerName, EHairResourceUsageType UsageType, ERDGInitialDataFlags InitialDataFlags=ERDGInitialDataFlags::NoCopy)
{
	FRDGBufferRef Buffer = nullptr;

	const uint32 DataCount = InData.Num();
	const uint32 DataSizeInBytes = FormatType::SizeInByte * DataCount;
	if (DataSizeInBytes == 0)
	{
		Out.Buffer = nullptr;
		return;
	}

	const FRDGBufferDesc Desc = ApplyUsage(FRDGBufferDesc::CreateBufferDesc(FormatType::SizeInByte, InData.Num()), UsageType);
	Buffer = InternalCreateVertexBuffer(
		GraphBuilder,
		DebugName,
		Desc,
		InData.GetData(),
		DataSizeInBytes,
		InitialDataFlags,
		OwnerName);

	ConvertToExternalBufferWithViews(GraphBuilder, Buffer, Out, FormatType::Format);
}

template<typename DataType>
void InternalCreateVertexBufferRDG(FRDGBuilder& GraphBuilder, const TArray<DataType>& InData, EPixelFormat Format, FRDGExternalBuffer& Out, const TCHAR* DebugName, const FName& OwnerName, EHairResourceUsageType UsageType, ERDGInitialDataFlags InitialDataFlags=ERDGInitialDataFlags::NoCopy)
{
	FRDGBufferRef Buffer = nullptr;

	const uint32 DataCount = InData.Num();
	const uint32 DataSizeInBytes = sizeof(DataType) * DataCount;
	if (DataSizeInBytes == 0)
	{
		Out.Buffer = nullptr;
		return;
	}

	const FRDGBufferDesc Desc = ApplyUsage(FRDGBufferDesc::CreateBufferDesc(sizeof(DataType), InData.Num()), UsageType);

	Buffer = InternalCreateVertexBuffer(
		GraphBuilder,
		DebugName,
		Desc,
		InData.GetData(),
		DataSizeInBytes,
		InitialDataFlags,
		OwnerName);

	ConvertToExternalBufferWithViews(GraphBuilder, Buffer, Out, Format);
}

template<typename FormatType>
void InternalCreateVertexBufferRDG(FRDGBuilder& GraphBuilder, uint32 InVertexCount, FRDGExternalBuffer& Out, const TCHAR* DebugName, const FName& OwnerName, EHairResourceUsageType Usage)
{
	// Sanity check
	check(Usage == EHairResourceUsageType::Dynamic);

	const uint32 DataCount = InVertexCount;
	const uint32 DataSizeInBytes = FormatType::SizeInByte * DataCount;
	if (DataSizeInBytes == 0)
	{
		Out.Buffer = nullptr;
		return;
	}

	FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(FormatType::SizeInByte, InVertexCount);
	FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(Desc, DebugName, ERDGBufferFlags::MultiFrame);
	Buffer->SetOwnerName(OwnerName);

	if (IsFloatFormat(FormatType::Format) || IsUnormFormat(FormatType::Format) || IsSnormFormat(FormatType::Format))
	{
		AddClearUAVFloatPass(GraphBuilder, GraphBuilder.CreateUAV(Buffer, FormatType::Format), 0.0f);
	}
	else
	{
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Buffer, FormatType::Format), 0u);
	}
	
	ConvertToExternalBufferWithViews(GraphBuilder, Buffer, Out, FormatType::Format);
}

template<typename FormatType>
void InternalCreateStructuredBufferRDG(FRDGBuilder& GraphBuilder, uint32 DataCount, FRDGExternalBuffer& Out, const TCHAR* DebugName, const FName& OwnerName, EHairResourceUsageType Usage)
{
	// Sanity check
	check(Usage == EHairResourceUsageType::Dynamic);

	const uint32 DataSizeInBytes = FormatType::SizeInByte * DataCount;
	if (DataSizeInBytes == 0)
	{
		Out.Buffer = nullptr;
		return;
	}

	FRDGBufferDesc Desc = ApplyUsage(FRDGBufferDesc::CreateStructuredDesc(FormatType::SizeInByte, DataCount), Usage);
	FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(Desc, DebugName, ERDGBufferFlags::MultiFrame);
	Buffer->SetOwnerName(OwnerName);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Buffer, FormatType::Format), 0u);
	ConvertToExternalBufferWithViews(GraphBuilder, Buffer, Out, FormatType::Format);
}

template<typename DataType>
void InternalCreateByteAddressBufferRDG(FRDGBuilder& GraphBuilder, const TArray<DataType>& InData, EPixelFormat Format, FRDGExternalBuffer& Out, const TCHAR* DebugName, const FName& OwnerName, EHairResourceUsageType UsageType)
{
	const uint32 DataCount = InData.Num();
	const uint32 DataSizeInBytes = sizeof(DataType) * DataCount;
	Out.Buffer = nullptr;
	if (DataSizeInBytes != 0)
	{
		const FRDGBufferDesc Desc = ApplyUsage(FRDGBufferDesc::CreateByteAddressDesc(DataSizeInBytes), UsageType);
		FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(Desc, DebugName, ERDGBufferFlags::MultiFrame);
		Buffer->SetOwnerName(OwnerName);
		if (InData.GetData() && DataSizeInBytes)
		{
			GraphBuilder.QueueBufferUpload(Buffer, InData.GetData(), DataSizeInBytes, ERDGInitialDataFlags::None);
		}
		
		ConvertToExternalBufferWithViews(GraphBuilder, Buffer, Out, Format);
	}
}

void InternalCreateByteAddressBufferRDG(FRDGBuilder& GraphBuilder, uint64 DataSizeInBytes, FRDGExternalBuffer& Out, const TCHAR* DebugName, const FName& OwnerName, EHairResourceUsageType UsageType)
{
	Out.Buffer = nullptr;
	if (DataSizeInBytes != 0)
	{
		const FRDGBufferDesc Desc = ApplyUsage(FRDGBufferDesc::CreateByteAddressDesc(DataSizeInBytes), UsageType);
		FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(Desc, DebugName, ERDGBufferFlags::MultiFrame);
		Buffer->SetOwnerName(OwnerName);
		ConvertToExternalBufferWithViews(GraphBuilder, Buffer, Out, PF_Unknown);
	}
}
/////////////////////////////////////////////////////////////////////////////////////////

static UTexture2D* CreateCardTexture(FIntPoint Resolution)
{
	UTexture2D* Out = nullptr;

	// Pass NAME_None as name to ensure an unique name is picked, so GC dont delete the new texture when it wants to delete the old one 
	Out = NewObject<UTexture2D>(GetTransientPackage(), NAME_None /*TEXT("ProceduralFollicleMaskTexture")*/, RF_Transient);
	Out->AddToRoot();
	Out->SetPlatformData(new FTexturePlatformData());
	Out->GetPlatformData()->SizeX = Resolution.X;
	Out->GetPlatformData()->SizeY = Resolution.Y;
	Out->GetPlatformData()->PixelFormat = PF_R32_FLOAT;
	Out->SRGB = false;

	const uint32 MipCount = 1; // FMath::Min(FMath::FloorLog2(Resolution), 5u);// Don't need the full chain
	for (uint32 MipIt = 0; MipIt < MipCount; ++MipIt)
	{
		const uint32 MipResolutionX = Resolution.X >> MipIt;
		const uint32 MipResolutionY = Resolution.Y >> MipIt;
		const uint32 SizeInBytes = sizeof(float) * MipResolutionX * MipResolutionY;

		FTexture2DMipMap* MipMap = new FTexture2DMipMap(MipResolutionX, MipResolutionY);
		Out->GetPlatformData()->Mips.Add(MipMap);
		MipMap->BulkData.Lock(LOCK_READ_WRITE);
		float* MipMemory = (float*)MipMap->BulkData.Realloc(SizeInBytes);
		for (uint32 Y = 0; Y < MipResolutionY; Y++)
			for (uint32 X = 0; X < MipResolutionX; X++)
			{
				MipMemory[X + Y * MipResolutionY] = X / float(MipResolutionX);
			}
		//FMemory::Memzero(MipMemory, SizeInBytes);
		MipMap->BulkData.Unlock();
	}
	Out->UpdateResource();

	return Out;
}

/////////////////////////////////////////////////////////////////////////////////////////
void CreateHairStrandsDebugAttributeBuffer(FRDGBuilder& GraphBuilder, FRDGExternalBuffer* DebugAttributeBuffer, uint32 SizeInBytes, const FName& OwnerName)
{
	if (SizeInBytes == 0 || !DebugAttributeBuffer)
	{
		return;
	}

	FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(SizeInBytes), TEXT("Hair.Strands_DebugAttributeBuffer"), ERDGBufferFlags::MultiFrame);
	Buffer->SetOwnerName(OwnerName);
	ConvertToExternalBufferWithViews(GraphBuilder, Buffer, *DebugAttributeBuffer, PF_R32_UINT);
}

/////////////////////////////////////////////////////////////////////////////////////////

FHairCommonResource::FHairCommonResource(EHairStrandsAllocationType InAllocationType, const FHairResourceName& InResourceName, const FName& InOwnerName, bool bInUseRenderGraph):
bUseRenderGraph(bInUseRenderGraph),
bIsInitialized(false),
AllocationType(InAllocationType),
ResourceName(InResourceName),
OwnerName(InOwnerName)
{
}

void FHairCommonResource::InitRHI()
{
	if (bIsInitialized || AllocationType == EHairStrandsAllocationType::Deferred || GUsingNullRHI) { return; }

	check(InternalIsDataLoaded(HAIR_MAX_NUM_CURVE_PER_GROUP, HAIR_MAX_NUM_POINT_PER_GROUP));

	if (bUseRenderGraph)
	{
		FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
		FRDGBuilder GraphBuilder(RHICmdList);
		InternalAllocate(GraphBuilder);
		GraphBuilder.Execute();
	}
	else
	{
		InternalAllocate();
	}
	bIsInitialized = true;
}

void FHairCommonResource::ReleaseRHI()
{
	InternalRelease();
	bIsInitialized = false;
}

void FHairCommonResource::Allocate(FRDGBuilder& GraphBuilder, EHairResourceLoadingType LoadingType)
{
	EHairResourceStatus Status;
	Status.Status = EHairResourceStatus::EStatus::None;
	Status.AvailableCurveCount = HAIR_MAX_NUM_CURVE_PER_GROUP;
	Allocate(GraphBuilder, LoadingType, Status, HAIR_MAX_NUM_CURVE_PER_GROUP, HAIR_MAX_NUM_POINT_PER_GROUP);
}

void FHairCommonResource::Allocate(FRDGBuilder& GraphBuilder, EHairResourceLoadingType LoadingType, EHairResourceStatus& Status)
{
	Allocate(GraphBuilder, LoadingType, Status, HAIR_MAX_NUM_CURVE_PER_GROUP, HAIR_MAX_NUM_POINT_PER_GROUP);
}

void FHairCommonResource::Allocate(FRDGBuilder& GraphBuilder, EHairResourceLoadingType LoadingType, EHairResourceStatus& Status, uint32 InRequestedCurveCount, uint32 InRequestedPointCount)
{
	check(AllocationType == EHairStrandsAllocationType::Deferred);

	// Check if there is already a request in flight	

	if (LoadingType == EHairResourceLoadingType::Sync)
	{
		if (!bIsInitialized)
		{
			FRenderResource::InitResource(); // Call RenderResource InitResource() so that the resources is marked as initialized
			InternalAllocate(GraphBuilder);
			bIsInitialized = true;

			MaxAvailableCurveCount = InRequestedCurveCount;
		}
		Status |= EHairResourceStatus::EStatus::Valid;

	}
	else if (LoadingType == EHairResourceLoadingType::Async)
	{
		// 1. If all requested curve are already loaded, nothing to do
		if (bIsInitialized && MaxAvailableCurveCount >= InRequestedCurveCount) 
		{ 
			Status |= EHairResourceStatus::EStatus::Valid; 
		}
		// 2. If more curves are requested, issue a streaming request
		else if (InternalIsDataLoaded(InRequestedCurveCount, InRequestedPointCount))
		{ 
			// 2.1 Curve data are availble, and update GPU resources
			if (!bIsInitialized)
			{
				FRenderResource::InitResource(); // Call RenderResource InitResource() so that the resources is marked as initialized
			}
			InternalAllocate(GraphBuilder);
			bIsInitialized = true;

			// Update the max curve count available
			MaxAvailableCurveCount = StreamingRequest.CurveCount;

			// Reset streaming request. When the request is delete, the DDC request becomes cancelled. 
			StreamingRequest = FHairStreamingRequest();

			Status |= EHairResourceStatus::EStatus::Valid;
		}
		else
		{
			// 2.2 Curve data are not availble yet, postpone (new) resources creation
			Status |= EHairResourceStatus::EStatus::Loading; 
		}
	}

	Status.AddAvailableCurve(MaxAvailableCurveCount);
}

void FHairCommonResource::AllocateLOD(FRDGBuilder& GraphBuilder, int32 LODIndex, EHairResourceLoadingType LoadingType, EHairResourceStatus& Status)
{
	// When using a async loading, sub-resources allocation requires the common/main resource to be already initialized.
	if (LoadingType == EHairResourceLoadingType::Async && (!InternalIsLODDataLoaded(LODIndex) || !bIsInitialized)) { Status |= EHairResourceStatus::EStatus::Loading; return; }

	// Sanity check. 
	check(AllocationType == EHairStrandsAllocationType::Deferred);

	InternalAllocateLOD(GraphBuilder, LODIndex);
	Status |= EHairResourceStatus::EStatus::Valid;
}

void FHairCommonResource::StreamInData()
{
	if (!bIsInitialized)
	{
		// TODO
		InternalIsDataLoaded(HAIR_MAX_NUM_CURVE_PER_GROUP, HAIR_MAX_NUM_POINT_PER_GROUP);
	}
}

void FHairCommonResource::StreamInLODData(int32 LODIndex)
{
	if (!bIsInitialized)
	{
		// TODO
		InternalIsLODDataLoaded(LODIndex);
	}
}

/////////////////////////////////////////////////////////////////////////////////////////

FHairCardIndexBuffer::FHairCardIndexBuffer(const TArray<FHairCardsIndexFormat::Type>& InIndices, const FName& InOwnerName)
	:Indices(InIndices) 
{ 
	SetOwnerName(InOwnerName); 
}

void FHairCardIndexBuffer::InitRHI()
{
	const uint32 DataSizeInBytes = FHairCardsIndexFormat::SizeInByte * Indices.Num();

	FRHIResourceCreateInfo CreateInfo(TEXT("FHairCardIndexBuffer"));
	IndexBufferRHI = RHICreateBuffer(DataSizeInBytes, BUF_Static | BUF_IndexBuffer, FHairCardsIndexFormat::SizeInByte, ERHIAccess::VertexOrIndexBuffer, CreateInfo);
	void* Buffer = RHILockBuffer(IndexBufferRHI, 0, DataSizeInBytes, RLM_WriteOnly);
	FMemory::Memcpy(Buffer, Indices.GetData(), DataSizeInBytes);
	RHIUnlockBuffer(IndexBufferRHI);
	IndexBufferRHI->SetOwnerName(GetOwnerName());
}

FHairCardsRestResource::FHairCardsRestResource(const FHairCardsBulkData& InBulkData, const FHairResourceName& InResourceName, const FName& InOwnerName) :
	FHairCommonResource(EHairStrandsAllocationType::Immediate, InResourceName, InOwnerName, false),
	RestPositionBuffer(),
	RestIndexBuffer(InBulkData.Indices, InOwnerName),
	NormalsBuffer(),
	UVsBuffer(),
	MaterialsBuffer(),
	BulkData(InBulkData)
{

}

void FHairCardsRestResource::InternalAllocate()
{
	// These resources are kept as regular (i.e., non-RDG resources) as they need to be bound at the input assembly stage by the Vertex declaraction which requires FVertexBuffer type
	CreateBuffer<FHairCardsPositionFormat>(BulkData.Positions, RestPositionBuffer, ToHairResourceDebugName(TEXT("Hair.CardsRest_PositionBuffer"), ResourceName), OwnerName);
	CreateBuffer<FHairCardsNormalFormat>(BulkData.Normals, NormalsBuffer, ToHairResourceDebugName(TEXT("Hair.CardsRest_NormalBuffer"), ResourceName), OwnerName);
	CreateBuffer<FHairCardsUVFormat>(BulkData.UVs, UVsBuffer, ToHairResourceDebugName(TEXT("Hair.CardsRest_UVBuffer"), ResourceName), OwnerName);
	CreateBuffer<FHairCardsMaterialFormat>(BulkData.Materials, MaterialsBuffer, ToHairResourceDebugName(TEXT("Hair.CardsRest_MaterialBuffer"), ResourceName), OwnerName);

	FSamplerStateRHIRef DefaultSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	DepthSampler = DefaultSampler;
	TangentSampler = DefaultSampler;
	CoverageSampler = DefaultSampler;
	AttributeSampler = DefaultSampler;
	MaterialSampler = DefaultSampler;
}

void FHairCardsRestResource::InternalRelease()
{
}

void FHairCardsRestResource::InitResource()
{
	FRenderResource::InitResource();
	RestIndexBuffer.InitResource();
	RestPositionBuffer.InitResource();
	NormalsBuffer.InitResource();
	UVsBuffer.InitResource();
	MaterialsBuffer.InitResource();
}

void FHairCardsRestResource::ReleaseResource()
{
	FRenderResource::ReleaseResource();
	RestIndexBuffer.ReleaseResource();
	RestPositionBuffer.ReleaseResource();
	NormalsBuffer.ReleaseResource();
	UVsBuffer.ReleaseResource();
	MaterialsBuffer.ReleaseResource();
}

/////////////////////////////////////////////////////////////////////////////////////////
FHairCardsProceduralResource::FHairCardsProceduralResource(const FHairCardsProceduralDatas::FRenderData& InRenderData, const FIntPoint& InAtlasResolution, const FHairCardsVoxel& InVoxel, const FName& InOwnerName):
	FHairCommonResource(EHairStrandsAllocationType::Immediate, FHairResourceName(), InOwnerName),
	CardBoundCount(InRenderData.ClusterBounds.Num()),
	AtlasResolution(InAtlasResolution),
	AtlasRectBuffer(),
	LengthBuffer(),
	CardItToClusterBuffer(),
	ClusterIdToVerticesBuffer(),
	ClusterBoundBuffer(),
	CardsStrandsPositions(),
	CardsStrandsAttributes(),
	RenderData(InRenderData)
{
	CardVoxel = InVoxel;
}

void FHairCardsProceduralResource::InternalAllocate(FRDGBuilder& GraphBuilder)
{
	InternalCreateVertexBufferRDG<FHairCardsAtlasRectFormat>(GraphBuilder, RenderData.CardsRect, AtlasRectBuffer, ToHairResourceDebugName(TEXT("Hair.CardsProcedural_AtlasRectBuffer"), ResourceName), OwnerName, EHairResourceUsageType::Static);
	InternalCreateVertexBufferRDG<FHairCardsDimensionFormat>(GraphBuilder, RenderData.CardsLengths, LengthBuffer, ToHairResourceDebugName(TEXT("Hair.CardsProcedural_LengthBuffer"), ResourceName), OwnerName, EHairResourceUsageType::Static);

	InternalCreateVertexBufferRDG<FHairCardsOffsetAndCount>(GraphBuilder, RenderData.CardItToCluster, CardItToClusterBuffer, ToHairResourceDebugName(TEXT("Hair.CardsProcedural_CardItToClusterBuffer"), ResourceName), OwnerName, EHairResourceUsageType::Static);
	InternalCreateVertexBufferRDG<FHairCardsOffsetAndCount>(GraphBuilder, RenderData.ClusterIdToVertices, ClusterIdToVerticesBuffer, ToHairResourceDebugName(TEXT("Hair.CardsProcedural_ClusterIdToVerticesBuffer"), ResourceName), OwnerName, EHairResourceUsageType::Static);
	InternalCreateVertexBufferRDG<FHairCardsBoundsFormat>(GraphBuilder, RenderData.ClusterBounds, ClusterBoundBuffer, ToHairResourceDebugName(TEXT("Hair.CardsProcedural_ClusterBoundBuffer"), ResourceName), OwnerName, EHairResourceUsageType::Static);

	InternalCreateVertexBufferRDG<FHairCardsVoxelDensityFormat>(GraphBuilder, RenderData.VoxelDensity, CardVoxel.DensityBuffer, ToHairResourceDebugName(TEXT("Hair.CardsProcedural_VoxelDensityBuffer"), ResourceName), OwnerName, EHairResourceUsageType::Static);
	InternalCreateVertexBufferRDG<FHairCardsVoxelTangentFormat>(GraphBuilder, RenderData.VoxelTangent, CardVoxel.TangentBuffer, ToHairResourceDebugName(TEXT("Hair.CardsProcedural_VoxelTangentBuffer"), ResourceName), OwnerName, EHairResourceUsageType::Static);
	InternalCreateVertexBufferRDG<FHairCardsVoxelTangentFormat>(GraphBuilder, RenderData.VoxelNormal, CardVoxel.NormalBuffer, ToHairResourceDebugName(TEXT("Hair.CardsProcedural_VoxelNormalBuffer"), ResourceName), OwnerName, EHairResourceUsageType::Static);

	InternalCreateVertexBufferRDG<FHairCardsStrandsPositionFormat>(GraphBuilder, RenderData.CardsStrandsPositions, CardsStrandsPositions, ToHairResourceDebugName(TEXT("Hair.CardsProcedural_CardsStrandsPositions"), ResourceName), OwnerName, EHairResourceUsageType::Static);
	InternalCreateVertexBufferRDG<FHairCardsStrandsAttributeFormat>(GraphBuilder, RenderData.CardsStrandsAttributes, CardsStrandsAttributes, ToHairResourceDebugName(TEXT("Hair.CardsProcedural_CardsStrandsAttributes"), ResourceName), OwnerName, EHairResourceUsageType::Static);
}

void FHairCardsProceduralResource::InternalRelease()
{
	AtlasRectBuffer.Release();
	LengthBuffer.Release();

	CardItToClusterBuffer.Release();
	ClusterIdToVerticesBuffer.Release();
	ClusterBoundBuffer.Release();
	CardsStrandsPositions.Release();
	CardsStrandsAttributes.Release();

	CardVoxel.DensityBuffer.Release();
	CardVoxel.TangentBuffer.Release();
	CardVoxel.NormalBuffer.Release();
}

/////////////////////////////////////////////////////////////////////////////////////////

FHairCardsDeformedResource::FHairCardsDeformedResource(const FHairCardsBulkData& InBulkData, bool bInInitializedData, const FHairResourceName& InResourceName, const FName& InOwnerName) :
	FHairCommonResource(EHairStrandsAllocationType::Deferred, InResourceName, InOwnerName),
	BulkData(InBulkData), bInitializedData(bInInitializedData)
{}

void FHairCardsDeformedResource::InternalAllocate(FRDGBuilder& GraphBuilder)
{
	if (bInitializedData)
	{
		InternalCreateVertexBufferRDG<FHairCardsPositionFormat>(GraphBuilder, BulkData.Positions, DeformedPositionBuffer[0], ToHairResourceDebugName(TEXT("Hair.CardsDeformedPosition(Current)"), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
		InternalCreateVertexBufferRDG<FHairCardsPositionFormat>(GraphBuilder, BulkData.Positions, DeformedPositionBuffer[1], ToHairResourceDebugName(TEXT("Hair.CardsDeformedPosition(Previous)"), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);

		InternalCreateVertexBufferRDG<FHairCardsNormalFormat>(GraphBuilder, BulkData.Normals, DeformedNormalBuffer, ToHairResourceDebugName(TEXT("Hair.CardsDeformedNormal"), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
	}
	else
	{
		InternalCreateVertexBufferRDG<FHairCardsPositionFormat>(GraphBuilder, BulkData.GetNumVertices(), DeformedPositionBuffer[0], ToHairResourceDebugName(TEXT("Hair.CardsDeformedPosition(Current)"), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
		InternalCreateVertexBufferRDG<FHairCardsPositionFormat>(GraphBuilder, BulkData.GetNumVertices(), DeformedPositionBuffer[1], ToHairResourceDebugName(TEXT("Hair.CardsDeformedPosition(Previous)"), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);

		InternalCreateVertexBufferRDG<FHairCardsNormalFormat>(GraphBuilder, BulkData.GetNumVertices() * FHairCardsNormalFormat::ComponentCount, DeformedNormalBuffer, ToHairResourceDebugName(TEXT("Hair.CardsDeformedNormal"), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);

		// Manually transit to SRVs, in case of the cards are rendered not visible, but still rendered (in shadows for instance). In such a case, the cards deformation pass is not called, and thus the 
		// buffers are never transit from UAV (clear) to SRV for rasterization. 
		GraphBuilder.UseExternalAccessMode(Register(GraphBuilder, DeformedPositionBuffer[0], ERDGImportedBufferFlags::CreateSRV).Buffer, ERHIAccess::SRVMask);
		GraphBuilder.UseExternalAccessMode(Register(GraphBuilder, DeformedPositionBuffer[1], ERDGImportedBufferFlags::CreateSRV).Buffer, ERHIAccess::SRVMask);
	}

	FRDGImportedBuffer CardsDeformedNormalRDGBuffer = Register(GraphBuilder, DeformedNormalBuffer, ERDGImportedBufferFlags::CreateSRV);
	GraphBuilder.UseExternalAccessMode(CardsDeformedNormalRDGBuffer.Buffer, ERHIAccess::SRVMask);
}

void FHairCardsDeformedResource::InternalRelease()
{
	DeformedPositionBuffer[0].Release();
	DeformedPositionBuffer[1].Release();
	DeformedNormalBuffer.Release();
}

/////////////////////////////////////////////////////////////////////////////////////////

FHairMeshesRestResource::FHairMeshesRestResource(const FHairMeshesBulkData& InBulkData, const FHairResourceName& InResourceName, const FName& InOwnerName) :
	FHairCommonResource(EHairStrandsAllocationType::Immediate, InResourceName, InOwnerName, false),
	RestPositionBuffer(),
	IndexBuffer(InBulkData.Indices, InOwnerName),
	NormalsBuffer(),
	UVsBuffer(),
	BulkData(InBulkData)
{
	check(BulkData.GetNumVertices() > 0);
	check(IndexBuffer.Indices.Num() > 0);
}

void FHairMeshesRestResource::InternalAllocate()
{
	// These resources are kept as regular (i.e., non-RDG resources) as they need to be bound at the input assembly stage by the Vertex declaraction which requires FVertexBuffer type
	CreateBuffer<FHairCardsPositionFormat>(BulkData.Positions, RestPositionBuffer, ToHairResourceDebugName(TEXT("Hair.MeshesRest_Positions"), ResourceName), OwnerName);
	CreateBuffer<FHairCardsNormalFormat>(BulkData.Normals, NormalsBuffer, ToHairResourceDebugName(TEXT("Hair.MeshesRest_Normals"), ResourceName), OwnerName);
	CreateBuffer<FHairCardsUVFormat>(BulkData.UVs, UVsBuffer, ToHairResourceDebugName(TEXT("Hair.MeshesRest_UVs"), ResourceName), OwnerName);
}

void FHairMeshesRestResource::InternalRelease()
{

}

void FHairMeshesRestResource::InitResource()
{
	FRenderResource::InitResource();
	IndexBuffer.InitResource();
	RestPositionBuffer.InitResource();
	NormalsBuffer.InitResource();
	UVsBuffer.InitResource();
}

void FHairMeshesRestResource::ReleaseResource()
{
	FRenderResource::ReleaseResource();
	IndexBuffer.ReleaseResource();
	RestPositionBuffer.ReleaseResource();
	NormalsBuffer.ReleaseResource();
	UVsBuffer.ReleaseResource();

}

/////////////////////////////////////////////////////////////////////////////////////////

FHairMeshesDeformedResource::FHairMeshesDeformedResource(const FHairMeshesBulkData& InBulkData, bool bInInitializedData, const FHairResourceName& InResourceName, const FName& InOwnerName) :
	FHairCommonResource(EHairStrandsAllocationType::Deferred, InResourceName, InOwnerName),
	BulkData(InBulkData), bInitializedData(bInInitializedData)
{}

void FHairMeshesDeformedResource::InternalAllocate(FRDGBuilder& GraphBuilder)
{
	if (bInitializedData)
	{
		InternalCreateVertexBufferRDG<FHairCardsPositionFormat>(GraphBuilder, BulkData.Positions, DeformedPositionBuffer[0], ToHairResourceDebugName(TEXT("Hair.MeshesDeformed(Current)"), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
		InternalCreateVertexBufferRDG<FHairCardsPositionFormat>(GraphBuilder, BulkData.Positions, DeformedPositionBuffer[1], ToHairResourceDebugName(TEXT("Hair.MeshesDeformed(Previous)"), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
	}
	else
	{
		InternalCreateVertexBufferRDG<FHairCardsPositionFormat>(GraphBuilder, BulkData.GetNumVertices(), DeformedPositionBuffer[0], ToHairResourceDebugName(TEXT("Hair.MeshesDeformed(Current)"), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
		InternalCreateVertexBufferRDG<FHairCardsPositionFormat>(GraphBuilder, BulkData.GetNumVertices(), DeformedPositionBuffer[1], ToHairResourceDebugName(TEXT("Hair.MeshesDeformed(Previous)"), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
	}
}

void FHairMeshesDeformedResource::InternalRelease()
{
	DeformedPositionBuffer[0].Release();
	DeformedPositionBuffer[1].Release();
}

/////////////////////////////////////////////////////////////////////////////////////////

FHairStrandsRestResource::FHairStrandsRestResource(FHairStrandsBulkData& InBulkData, EHairStrandsResourcesType InCurveType, const FHairResourceName& InResourceName, const FName& InOwnerName) :
	FHairCommonResource(EHairStrandsAllocationType::Deferred, InResourceName, InOwnerName),
	PositionBuffer(), PointAttributeBuffer(), CurveAttributeBuffer(), PointToCurveBuffer(), BulkData(InBulkData), CurveType(InCurveType)
{
	MaxAvailableCurveCount = 0;

	// Sanity check
	check(!!(BulkData.Header.Flags & FHairStrandsBulkData::DataFlags_HasData));
}

bool FHairStrandsRestResource::InternalIsDataLoaded(uint32 InRequestedCurveCount, uint32 InRequestedPointCount)
{
	if (StreamingRequest.IsNone())
	{
		StreamingRequest.Request(InRequestedCurveCount, InRequestedPointCount, BulkData, false, false, OwnerName);
	}
	return StreamingRequest.IsCompleted();
}

void FHairStrandsRestResource::InternalAllocate(FRDGBuilder& GraphBuilder)
{
	// If we enter this function, the request need to be completed
	check(StreamingRequest.IsCompleted());

	const uint32 PointCount = BulkData.GetNumPoints();
	const uint32 CurveCount = BulkData.GetNumCurves();

	// 1. Lock data, which force the loading data from files (on non-editor build/cooked data). These data are then uploaded to the GPU
	// 2. A local copy is done by the buffer uploader. This copy is discarded once the uploading is done.
	InternalCreateVertexBufferRDG_FromHairBulkData<FHairStrandsPositionFormat>(GraphBuilder, BulkData.Data.Positions, PointCount, PositionBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRest_PositionBuffer), ResourceName), OwnerName, EHairResourceUsageType::Static);
	InternalCreateByteAddressBufferRDG_FromHairBulkData(GraphBuilder, BulkData.Data.CurveAttributes, CurveAttributeBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRest_CurveAttributeBuffer), ResourceName), OwnerName, EHairResourceUsageType::Static);
	if (!!(BulkData.Header.Flags & FHairStrandsBulkData::DataFlags_HasPointAttribute))
	{
		InternalCreateByteAddressBufferRDG_FromHairBulkData(GraphBuilder, BulkData.Data.PointAttributes, PointAttributeBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRest_PointAttributeBuffer), ResourceName), OwnerName, EHairResourceUsageType::Static);
	}
	else if (PointAttributeBuffer.Buffer == nullptr)
	{
		TArray<uint32> DummyAttribute;
		DummyAttribute.Add(0u);
		InternalCreateByteAddressBufferRDG(GraphBuilder, DummyAttribute, EPixelFormat::PF_R32_UINT, PointAttributeBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRest_PointAttributeBuffer), ResourceName), OwnerName, EHairResourceUsageType::Static);
		GraphBuilder.UseExternalAccessMode(Register(GraphBuilder, PointAttributeBuffer, ERDGImportedBufferFlags::CreateSRV).Buffer, ERHIAccess::SRVMask);
	}
	InternalCreateVertexBufferRDG_FromHairBulkData<FHairStrandsCurveFormat>(GraphBuilder, BulkData.Data.Curves, CurveCount, CurveBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRest_CurveBuffer), ResourceName), OwnerName, EHairResourceUsageType::Static);
	if (!!(BulkData.Header.Flags & FHairStrandsBulkData::DataFlags_Has16bitsCurveIndex))
	{
		InternalCreateVertexBufferRDG_FromHairBulkData<FHairStrandsPointToCurveFormat16>(GraphBuilder, BulkData.Data.PointToCurve, PointCount, PointToCurveBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRest_PointToCurveBuffer), ResourceName), OwnerName, EHairResourceUsageType::Static);
	}
	else
	{
		InternalCreateVertexBufferRDG_FromHairBulkData<FHairStrandsPointToCurveFormat32>(GraphBuilder, BulkData.Data.PointToCurve, PointCount, PointToCurveBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRest_PointToCurveBuffer), ResourceName), OwnerName, EHairResourceUsageType::Static);
	}

	if (PositionOffsetBuffer.Buffer == nullptr)
	{	
		TArray<FVector4f> RestOffset;
		RestOffset.Add((FVector3f)BulkData.GetPositionOffset());// LWC_TODO: precision loss
		InternalCreateVertexBufferRDG<FHairStrandsPositionOffsetFormat>(GraphBuilder, RestOffset, PositionOffsetBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRest_PositionOffsetBuffer), ResourceName), OwnerName, EHairResourceUsageType::Static, ERDGInitialDataFlags::None);
		GraphBuilder.UseExternalAccessMode(Register(GraphBuilder, PositionOffsetBuffer, ERDGImportedBufferFlags::CreateSRV).Buffer, ERHIAccess::SRVMask);
	}
}

void AddHairTangentPass(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	uint32 VertexCount,
	FHairGroupPublicData* HairGroupPublicData,
	FRDGBufferSRVRef PositionBuffer,
	FRDGImportedBuffer OutTangentBuffer);

FRDGExternalBuffer FHairStrandsRestResource::GetTangentBuffer(FRDGBuilder& GraphBuilder, FGlobalShaderMap* ShaderMap)
{
	// Lazy allocation and update
	if (TangentBuffer.Buffer == nullptr)
	{
		InternalCreateVertexBufferRDG<FHairStrandsTangentFormat>(GraphBuilder, BulkData.GetNumPoints() * FHairStrandsTangentFormat::ComponentCount, TangentBuffer, ToHairResourceDebugName(TEXT("Hair.StrandsRest_TangentBuffer"), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);

		AddHairTangentPass(
			GraphBuilder,
			ShaderMap,
			BulkData.GetNumPoints(),
			nullptr,
			RegisterAsSRV(GraphBuilder, PositionBuffer),
			Register(GraphBuilder, TangentBuffer, ERDGImportedBufferFlags::CreateUAV));
	}

	return TangentBuffer;
}

void FHairStrandsRestResource::InternalRelease()
{
	PositionBuffer.Release();
	PositionOffsetBuffer.Release();
	CurveAttributeBuffer.Release();
	PointAttributeBuffer.Release();
	PointToCurveBuffer.Release();
	TangentBuffer.Release();
	CurveBuffer.Release();
	MaxAvailableCurveCount = 0;
}

/////////////////////////////////////////////////////////////////////////////////////////

FHairStrandsDeformedResource::FHairStrandsDeformedResource(FHairStrandsBulkData& InBulkData, EHairStrandsResourcesType InCurveType, const FHairResourceName& InResourceName, const FName& InOwnerName) :
	FHairCommonResource(EHairStrandsAllocationType::Deferred, InResourceName, InOwnerName),
	BulkData(InBulkData), CurveType(InCurveType)
{
	GetPositionOffset(FHairStrandsDeformedResource::EFrameType::Current)  = BulkData.GetPositionOffset();
	GetPositionOffset(FHairStrandsDeformedResource::EFrameType::Previous) = BulkData.GetPositionOffset();
}

void FHairStrandsDeformedResource::InternalAllocate(FRDGBuilder& GraphBuilder)
{
	const uint32 PointCount = BulkData.GetNumPoints();

	InternalCreateVertexBufferRDG<FHairStrandsPositionFormat>(GraphBuilder, PointCount, DeformedPositionBuffer[0], ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsDeformed_DeformedPositionBuffer0), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
	InternalCreateVertexBufferRDG<FHairStrandsPositionFormat>(GraphBuilder, PointCount, DeformedPositionBuffer[1], ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsDeformed_DeformedPositionBuffer1), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
	InternalCreateVertexBufferRDG<FHairStrandsTangentFormat>(GraphBuilder,  PointCount * FHairStrandsTangentFormat::ComponentCount, TangentBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsDeformed_TangentBuffer), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);

	TArray<FVector4f> DefaultOffsets;
	DefaultOffsets.Add((FVector3f)BulkData.GetPositionOffset()); // LWC_TODO: precision loss
	InternalCreateVertexBufferRDG<FHairStrandsPositionOffsetFormat>(GraphBuilder, DefaultOffsets, DeformedOffsetBuffer[0], ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsDeformed_DeformedOffsetBuffer0), ResourceName), OwnerName, EHairResourceUsageType::Dynamic, ERDGInitialDataFlags::None);
	InternalCreateVertexBufferRDG<FHairStrandsPositionOffsetFormat>(GraphBuilder, DefaultOffsets, DeformedOffsetBuffer[1], ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsDeformed_DeformedOffsetBuffer1), ResourceName), OwnerName, EHairResourceUsageType::Dynamic, ERDGInitialDataFlags::None);

	// Note: DeformerBuffer is optionally/lazily allocated by a mesh-deformer graph
}

FRDGExternalBuffer& FHairStrandsDeformedResource::GetDeformerBuffer(FRDGBuilder& GraphBuilder)
{
	// Lazy allocation and update
	if (DeformerBuffer.Buffer == nullptr)
	{
		InternalCreateVertexBufferRDG<FHairStrandsPositionFormat>(GraphBuilder, BulkData.GetNumPoints(), DeformerBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsDeformed_DeformerBuffer), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
	}
	return DeformerBuffer;
}

FRDGExternalBuffer& FHairStrandsDeformedResource::GetDeformerCurveAttributeBuffer(FRDGBuilder& GraphBuilder)
{
	// Deformer curve attributes
	if (DeformerCurveAttributeBuffer.Buffer == nullptr)
	{
		const uint32 AllocSize = FMath::DivideAndRoundUp(BulkData.Header.CurveCount, BulkData.Header.Strides.CurveAttributeChunkElementCount) * BulkData.Header.Strides.CurveAttributeChunkStride;
		check(AllocSize > 0);
		InternalCreateByteAddressBufferRDG(GraphBuilder, AllocSize, DeformerCurveAttributeBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsDeformed_DeformerCurveAttributeBuffer), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
	}
	return DeformerCurveAttributeBuffer;
}

FRDGExternalBuffer& FHairStrandsDeformedResource::GetDeformerPointAttributeBuffer(FRDGBuilder& GraphBuilder)
{
	// Deformer point attributes
	if (DeformerPointAttributeBuffer.Buffer == nullptr && (BulkData.Header.Flags & FHairStrandsBulkData::DataFlags_HasPointAttribute) && BulkData.Data.PointAttributes.Data.GetBulkDataSize() > 0)
	{
		const uint32 AllocSize = FMath::DivideAndRoundUp(BulkData.Header.PointCount, BulkData.Header.Strides.PointAttributeChunkElementCount) * BulkData.Header.Strides.PointAttributeChunkStride;
		check(AllocSize > 0);
		InternalCreateByteAddressBufferRDG(GraphBuilder, AllocSize, DeformerPointAttributeBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsDeformedt_DeformerPointAttributeBuffer), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
	}
	return DeformerPointAttributeBuffer;
}

void FHairStrandsDeformedResource::InternalRelease()
{
	DeformedPositionBuffer[0].Release();
	DeformedPositionBuffer[1].Release();
	TangentBuffer.Release();
	DeformerBuffer.Release();
	DeformerPointAttributeBuffer.Release();
	DeformerCurveAttributeBuffer.Release();

	DeformedOffsetBuffer[0].Release();
	DeformedOffsetBuffer[1].Release();
}

/////////////////////////////////////////////////////////////////////////////////////////
// Cluster culling resources
FHairStrandsClusterCullingResource::FHairStrandsClusterCullingResource(FHairStrandsClusterCullingBulkData& InBulkData, const FHairResourceName& InResourceName, const FName& InOwnerName):
	FHairCommonResource(EHairStrandsAllocationType::Deferred, InResourceName, InOwnerName),
	BulkData(InBulkData)
{
	MaxAvailableCurveCount = 0;
}

bool FHairStrandsClusterCullingResource::InternalIsDataLoaded(uint32 InRequestedCurveCount, uint32 InRequestedPointCount)
{
	if (StreamingRequest.IsNone())
	{
		StreamingRequest.Request(InRequestedCurveCount, InRequestedPointCount, BulkData, true, true, OwnerName);
	}
	return StreamingRequest.IsCompleted();
}

void FHairStrandsClusterCullingResource::InternalAllocate(FRDGBuilder& GraphBuilder)
{
	check(StreamingRequest.IsCompleted());

	if (ValidateHairBulkData())
	{
		BulkData.Validate(false);
	}

	InternalCreateStructuredBufferRDG_FromBulkData<FHairClusterInfoFormat>(GraphBuilder, BulkData.Data.PackedClusterInfos.Data, BulkData.Header.ClusterCount, ClusterInfoBuffer, ToHairResourceDebugName(TEXT("Hair.StrandsClusterCulling_ClusterInfoBuffer"), ResourceName), OwnerName, EHairResourceUsageType::Static);
	InternalCreateStructuredBufferRDG_FromBulkData<FHairClusterLODInfoFormat>(GraphBuilder, BulkData.Data.ClusterLODInfos.Data, BulkData.Header.ClusterLODCount, ClusterLODInfoBuffer, ToHairResourceDebugName(TEXT("Hair.StrandsClusterCulling_ClusterLODInfoBuffer"), ResourceName), OwnerName, EHairResourceUsageType::Static);

	InternalCreateVertexBufferRDG_FromBulkData<FHairClusterIndexFormat>(GraphBuilder, BulkData.Data.VertexToClusterIds.Data, BulkData.Header.VertexCount, VertexToClusterIdBuffer, ToHairResourceDebugName(TEXT("Hair.StrandsClusterCulling_VertexToClusterIds"), ResourceName), OwnerName, EHairResourceUsageType::Static);
	InternalCreateVertexBufferRDG_FromBulkData<FHairClusterIndexFormat>(GraphBuilder, BulkData.Data.ClusterVertexIds.Data, BulkData.Header.VertexLODCount, ClusterVertexIdBuffer, ToHairResourceDebugName(TEXT("Hair.StrandsClusterCulling_ClusterVertexIds"), ResourceName), OwnerName, EHairResourceUsageType::Static);
}

void FHairStrandsClusterCullingResource::InternalRelease()
{
	ClusterInfoBuffer.Release();
	ClusterLODInfoBuffer.Release();
	ClusterVertexIdBuffer.Release();
	VertexToClusterIdBuffer.Release();
	MaxAvailableCurveCount = 0;
}

/////////////////////////////////////////////////////////////////////////////////////////

FHairStrandsRestRootResource::FHairStrandsRestRootResource(FHairStrandsRootBulkData& InBulkData, EHairStrandsResourcesType InCurveType, const FHairResourceName& InResourceName, const FName& InOwnerName) :
	FHairCommonResource(EHairStrandsAllocationType::Deferred, InResourceName, InOwnerName),
	BulkData(InBulkData), CurveType(InCurveType)
{
	PopulateFromRootData();
}

void FHairStrandsRestRootResource::PopulateFromRootData()
{
	uint32 LODIndex = 0;
	LODs.Reserve(BulkData.Header.LODs.Num());
	for (const FHairStrandsRootBulkData::FHeader::FLOD& InLOD : BulkData.Header.LODs)
	{
		FLOD& OutLOD = LODs.AddDefaulted_GetRef();

		OutLOD.LODIndex 	= InLOD.LODIndex;
		OutLOD.Status 		= FLOD::EStatus::Invalid;
		OutLOD.SampleCount 	= InLOD.SampleCount;
	}
	LODRequests.SetNum(BulkData.Header.LODs.Num());
}

bool FHairStrandsRestRootResource::InternalIsLODDataLoaded(int32 LODIndex)
{
	bool bIsLoading = false;
	
	check(LODs.Num() == BulkData.Header.LODs.Num());
	if (LODIndex >= 0 && LODIndex < LODs.Num())
	{
		FBulkDataBatchRequest& LODRequest = LODRequests[LODIndex];
		if (LODRequest.IsNone())
		{
			FBulkDataBatchRequest::FBatchBuilder Batch = FBulkDataBatchRequest::NewBatch(9);
			FHairStrandsRootBulkData::FData::FLOD& CPUData = BulkData.Data.LODs[LODIndex];
			const bool bHasValidCPUData = CPUData.RootBarycentricBuffer.GetBulkDataSize() > 0;
			if (bHasValidCPUData)
			{
				Batch.Read(CPUData.RootBarycentricBuffer);
				Batch.Read(CPUData.RootToUniqueTriangleIndexBuffer);
				Batch.Read(CPUData.UniqueTriangleIndexBuffer);
				Batch.Read(CPUData.RestUniqueTrianglePositionBuffer);
			}
			
			const bool bHasValidCPUWeights = CPUData.MeshSampleIndicesBuffer.GetBulkDataSize() > 0;
			if (bHasValidCPUWeights)
			{
				Batch.Read(CPUData.MeshInterpolationWeightsBuffer);
				Batch.Read(CPUData.MeshSampleIndicesBuffer);
				Batch.Read(CPUData.RestSamplePositionsBuffer);
			}
			
			if (bHasValidCPUData || bHasValidCPUWeights)
			{
				Batch.Issue(LODRequest);
			}
		}

		bIsLoading = LODRequest.IsCompleted() == false;
	}

	return !bIsLoading;
}

void FHairStrandsRestRootResource::InternalAllocate(FRDGBuilder& GraphBuilder)
{
	// Once empty, the MeshProjectionLODsneeds to be repopulate as it might be re-initialized. 
	// E.g., when a resource is updated, it is first released, then re-init. 
	if (LODs.IsEmpty())
	{
		PopulateFromRootData();
	}
}

void FHairStrandsRestRootResource::InternalAllocateLOD(FRDGBuilder& GraphBuilder, int32 LODIndex)
{
	// Sanity check ti insure that the 'common' part of FHairStrandsRestRootResource is already initialized
	check(bIsInitialized);
	check(BulkData.Header.PointCount > 0);
	check(LODs.Num() == BulkData.Header.LODs.Num());
	if (LODIndex >= 0 && LODIndex < LODs.Num())
	{
		FLOD& GPUData = LODs[LODIndex];
		const bool bIsLODInitialized = GPUData.Status == FLOD::EStatus::Completed || GPUData.Status == FLOD::EStatus::Initialized;
		if (bIsLODInitialized)
		{
			return;
		}
		
		LODRequests[LODIndex] = FBulkDataBatchRequest();

		const FHairStrandsRootBulkData::FHeader::FLOD& LODHeader = BulkData.Header.LODs[LODIndex];
		FHairStrandsRootBulkData::FData::FLOD& CPUData = BulkData.Data.LODs[LODIndex];
		const bool bHasValidCPUData = CPUData.RootBarycentricBuffer.GetBulkDataSize() > 0;
		if (bHasValidCPUData)
		{
			GPUData.Status = FLOD::EStatus::Completed;
			
			InternalCreateVertexBufferRDG_FromBulkData<FHairStrandsRootBarycentricFormat>(GraphBuilder, CPUData.RootBarycentricBuffer, BulkData.Header.RootCount, GPUData.RootBarycentricBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRestRoot_RootTriangleBarycentricBuffer), ResourceName), OwnerName, EHairResourceUsageType::Static);
			InternalCreateVertexBufferRDG_FromBulkData<FHairStrandsRootToUniqueTriangleIndexFormat>(GraphBuilder, CPUData.RootToUniqueTriangleIndexBuffer, BulkData.Header.RootCount, GPUData.RootToUniqueTriangleIndexBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRestRoot_RootToUniqueTriangleIndexBuffer), ResourceName), OwnerName, EHairResourceUsageType::Static);
			InternalCreateVertexBufferRDG_FromBulkData<FHairStrandsUniqueTriangleIndexFormat>(GraphBuilder, CPUData.UniqueTriangleIndexBuffer, LODHeader.UniqueTriangleCount, GPUData.UniqueTriangleIndexBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRestRoot_UniqueTriangleIndexBuffer), ResourceName), OwnerName, EHairResourceUsageType::Static);
			InternalCreateVertexBufferRDG_FromBulkData<FHairStrandsMeshTrianglePositionFormat>(GraphBuilder, CPUData.RestUniqueTrianglePositionBuffer, LODHeader.UniqueTriangleCount * 3, GPUData.RestUniqueTrianglePositionBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRestRoot_RestUniqueTrianglePosition0Buffer), ResourceName), OwnerName, EHairResourceUsageType::Static);
		}
		else
		{
			GPUData.Status = FLOD::EStatus::Initialized;
			
			InternalCreateVertexBufferRDG<FHairStrandsRootBarycentricFormat>(GraphBuilder, BulkData.Header.RootCount, GPUData.RootBarycentricBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRestRoot_RootBarycentricBuffer), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
			InternalCreateVertexBufferRDG<FHairStrandsRootToUniqueTriangleIndexFormat>(GraphBuilder, BulkData.Header.RootCount, GPUData.RootToUniqueTriangleIndexBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRestRoot_RootToUniqueTriangleIndexBuffer), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
			InternalCreateVertexBufferRDG<FHairStrandsUniqueTriangleIndexFormat>(GraphBuilder, LODHeader.UniqueTriangleCount, GPUData.UniqueTriangleIndexBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRestRoot_UniqueTriangleIndexBuffer), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
			
			// Create buffers. Initialization will be done by render passes
			InternalCreateVertexBufferRDG<FHairStrandsMeshTrianglePositionFormat>(GraphBuilder, LODHeader.UniqueTriangleCount * 3, GPUData.RestUniqueTrianglePositionBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRestRoot_RestUniqueTrianglePosition0Buffer), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
		}

		GPUData.SampleCount = LODHeader.SampleCount;
		const bool bHasValidCPUWeights = CPUData.MeshSampleIndicesBuffer.GetBulkDataSize() > 0;
		if (bHasValidCPUWeights)
		{
			const uint32 InteroplationWeightCount = CPUData.MeshInterpolationWeightsBuffer.GetBulkDataSize() / sizeof(FHairStrandsWeightFormat::Type);
			InternalCreateVertexBufferRDG_FromBulkData<FHairStrandsWeightFormat>(GraphBuilder, CPUData.MeshInterpolationWeightsBuffer, InteroplationWeightCount, GPUData.MeshInterpolationWeightsBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRestRoot_MeshInterpolationWeightsBuffer), ResourceName), OwnerName, EHairResourceUsageType::Static);
			InternalCreateVertexBufferRDG_FromBulkData<FHairStrandsIndexFormat>(GraphBuilder, CPUData.MeshSampleIndicesBuffer, LODHeader.SampleCount, GPUData.MeshSampleIndicesBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRestRoot_MeshSampleIndicesBuffer), ResourceName), OwnerName, EHairResourceUsageType::Static);
			InternalCreateVertexBufferRDG_FromBulkData<FHairStrandsMeshTrianglePositionFormat>(GraphBuilder, CPUData.RestSamplePositionsBuffer, LODHeader.SampleCount, GPUData.RestSamplePositionsBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRestRoot_RestSamplePositionsBuffer), ResourceName), OwnerName, EHairResourceUsageType::Static);
		}
		else
		{
			// TODO: do not allocate these resources, since they won't be used
			InternalCreateVertexBufferRDG<FHairStrandsWeightFormat>(GraphBuilder, (LODHeader.SampleCount+4) * (LODHeader.SampleCount+4), GPUData.MeshInterpolationWeightsBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRestRoot_MeshInterpolationWeightsBuffer), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
			InternalCreateVertexBufferRDG<FHairStrandsIndexFormat>(GraphBuilder, LODHeader.SampleCount, GPUData.MeshSampleIndicesBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRestRoot_MeshSampleIndicesBuffer), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
			InternalCreateVertexBufferRDG<FHairStrandsMeshTrianglePositionFormat>(GraphBuilder, LODHeader.SampleCount, GPUData.RestSamplePositionsBuffer, ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRestRoot_RestSamplePositionsBuffer), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
		}
	}
}

void FHairStrandsRestRootResource::InternalRelease()
{
	for (FLOD& GPUData : LODs)
	{
		GPUData.Status = FLOD::EStatus::Invalid;
		GPUData.RootBarycentricBuffer.Release();
		GPUData.RootToUniqueTriangleIndexBuffer.Release();
		GPUData.UniqueTriangleIndexBuffer.Release();
		GPUData.RestUniqueTrianglePositionBuffer.Release();
		GPUData.SampleCount = 0;
		GPUData.MeshInterpolationWeightsBuffer.Release();
		GPUData.MeshSampleIndicesBuffer.Release();
		GPUData.RestSamplePositionsBuffer.Release();
	}
	LODs.Empty();
	LODRequests.Empty();
}

/////////////////////////////////////////////////////////////////////////////////////////

FHairStrandsDeformedRootResource::FHairStrandsDeformedRootResource(EHairStrandsResourcesType InCurveType, const FHairResourceName& InResourceName, const FName& InOwnerName):
	FHairCommonResource(EHairStrandsAllocationType::Deferred, InResourceName, InOwnerName),
	CurveType(InCurveType)
{

}

FHairStrandsDeformedRootResource::FHairStrandsDeformedRootResource(const FHairStrandsRestRootResource* InRestResources, EHairStrandsResourcesType InCurveType, const FHairResourceName& InResourceName, const FName& InOwnerName):
	FHairCommonResource(EHairStrandsAllocationType::Deferred, InResourceName, InOwnerName),
	CurveType(InCurveType)
{
	check(InRestResources);
	uint32 LODIndex = 0;
	RootCount = InRestResources->BulkData.Header.RootCount;
	LODs.Reserve(InRestResources->LODs.Num());
	for (const FHairStrandsRestRootResource::FLOD& InLOD : InRestResources->LODs)
	{
		FLOD& LOD = LODs.AddDefaulted_GetRef();

		LOD.Status = FLOD::EStatus::Invalid;
		LOD.LODIndex = InLOD.LODIndex;
		LOD.SampleCount = InLOD.SampleCount;
	}
}

void FHairStrandsDeformedRootResource::InternalAllocateLOD(FRDGBuilder& GraphBuilder, int32 LODIndex)
{
	if (RootCount > 0 && LODIndex >= 0 && LODIndex < LODs.Num())
	{
		FLOD& LOD = LODs[LODIndex];
		if (LOD.Status == FLOD::EStatus::Invalid)
		{		
			LOD.Status = FLOD::EStatus::Initialized;
			if (LOD.SampleCount > 0)
			{
				InternalCreateVertexBufferRDG<FHairStrandsMeshTrianglePositionFormat>(GraphBuilder, LOD.SampleCount, LOD.DeformedSamplePositionsBuffer[0], ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRootDeformed_DeformedSamplePositionsBuffer0), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
				InternalCreateVertexBufferRDG<FHairStrandsMeshTrianglePositionFormat>(GraphBuilder, LOD.SampleCount + 4, LOD.MeshSampleWeightsBuffer[0], ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRootDeformed_MeshSampleWeightsBuffer0), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);

				// Double buffering is disabled by default unless the read-only cvar r.HairStrands.ContinuousDecimationReordering is set
				if (IsHairStrandContinuousDecimationReorderingEnabled())
				{
					InternalCreateVertexBufferRDG<FHairStrandsMeshTrianglePositionFormat>(GraphBuilder, LOD.SampleCount, LOD.DeformedSamplePositionsBuffer[1], ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRootDeformed_DeformedSamplePositionsBuffer1), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
					InternalCreateVertexBufferRDG<FHairStrandsMeshTrianglePositionFormat>(GraphBuilder, LOD.SampleCount + 4, LOD.MeshSampleWeightsBuffer[1], ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRootDeformed_MeshSampleWeightsBuffer1), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
				}
			}

			InternalCreateVertexBufferRDG<FHairStrandsMeshTrianglePositionFormat>(GraphBuilder, RootCount * 3, LOD.DeformedUniqueTrianglePositionBuffer[0], ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRootDeformed_DeformedUniqueTrianglePosition0Buffer0), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);

			// Double buffering is disabled by default unless the read-only cvar r.HairStrands.ContinuousDecimationReordering is set
			if (IsHairStrandContinuousDecimationReorderingEnabled())
			{
				InternalCreateVertexBufferRDG<FHairStrandsMeshTrianglePositionFormat>(GraphBuilder, RootCount * 3, LOD.DeformedUniqueTrianglePositionBuffer[1], ToHairResourceDebugName(HAIRSTRANDS_RESOUCE_NAME(CurveType, Hair.StrandsRootDeformed_DeformedUniqueTrianglePosition0Buffer1), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
			}
		}
	}
}

void FHairStrandsDeformedRootResource::InternalRelease()
{
	for (FLOD& GPUData : LODs)
	{
		GPUData.Status = FLOD::EStatus::Invalid;
		GPUData.DeformedUniqueTrianglePositionBuffer[0].Release();
		GPUData.DeformedSamplePositionsBuffer[0].Release();
		GPUData.MeshSampleWeightsBuffer[0].Release();
	
		// Double buffering is disabled by default unless the read-only cvar r.HairStrands.ContinuousDecimationReordering is set
		if (IsHairStrandContinuousDecimationReorderingEnabled())
		{
			GPUData.DeformedUniqueTrianglePositionBuffer[1].Release();
			GPUData.DeformedSamplePositionsBuffer[1].Release();
			GPUData.MeshSampleWeightsBuffer[1].Release();
		}
	}
	LODs.Empty();
}

/////////////////////////////////////////////////////////////////////////////////////////

FHairStrandsInterpolationResource::FHairStrandsInterpolationResource(FHairStrandsInterpolationBulkData& InBulkData, const FHairResourceName& InResourceName, const FName& InOwnerName) :
	FHairCommonResource(EHairStrandsAllocationType::Deferred, InResourceName, InOwnerName),
	InterpolationBuffer(), BulkData(InBulkData)
{
	MaxAvailableCurveCount = 0;

	// Sanity check
	check(!!(BulkData.Header.Flags & FHairStrandsInterpolationBulkData::DataFlags_HasData));
}

bool FHairStrandsInterpolationResource::InternalIsDataLoaded(uint32 InRequestedCurveCount, uint32 InRequestedPointCount)
{
	if (StreamingRequest.IsNone())
	{
		StreamingRequest.Request(InRequestedCurveCount, InRequestedPointCount, BulkData, false, false, OwnerName);
	}
	return StreamingRequest.IsCompleted();
}

void FHairStrandsInterpolationResource::InternalAllocate(FRDGBuilder& GraphBuilder)
{
	// If we enter this function, the request need to be completed
	check(StreamingRequest.IsCompleted());

	InternalCreateByteAddressBufferRDG_FromHairBulkData(GraphBuilder, BulkData.Data.Interpolation, InterpolationBuffer, ToHairResourceDebugName(TEXT("Hair.StrandsInterpolation_InterpolationBuffer"), ResourceName), OwnerName, EHairResourceUsageType::Static);
	InternalCreateVertexBufferRDG_FromHairBulkData<FHairStrandsRootIndexFormat>(GraphBuilder, BulkData.Data.SimRootPointIndex, BulkData.Header.SimPointCount, SimRootPointIndexBuffer, ToHairResourceDebugName(TEXT("Hair.StrandsInterpolation_SimRootPointIndex"), ResourceName), OwnerName, EHairResourceUsageType::Static);
}

void FHairStrandsInterpolationResource::InternalRelease()
{
	InterpolationBuffer.Release();
	SimRootPointIndexBuffer.Release();
	MaxAvailableCurveCount = 0;
}

/////////////////////////////////////////////////////////////////////////////////////////

FHairCardsInterpolationResource::FHairCardsInterpolationResource(FHairCardsInterpolationBulkData& InBulkData, const FHairResourceName& InResourceName, const FName& InOwnerName) :
	FHairCommonResource(EHairStrandsAllocationType::Deferred, InResourceName, InOwnerName),
	InterpolationBuffer(), BulkData(InBulkData)
{
}

void FHairCardsInterpolationResource::InternalAllocate(FRDGBuilder& GraphBuilder)
{
	InternalCreateVertexBufferRDG<FHairCardsInterpolationFormat>(GraphBuilder, BulkData.Interpolation, InterpolationBuffer, ToHairResourceDebugName(TEXT("Hair.CardsInterpolation_InterpolationBuffer"), ResourceName), OwnerName, EHairResourceUsageType::Static);
}

void FHairCardsInterpolationResource::InternalRelease()
{
	InterpolationBuffer.Release();
}

/////////////////////////////////////////////////////////////////////////////////////////

#if RHI_RAYTRACING
// RT geometry for strands is built as a 4 sided cylinder
//   each vertex of the curve becomes 4 points
//   each curve segment turns into 2*4=8 triangles (3 indices for each)
// there is some waste due to the degenerate triangles emitted from the end points of each curve
// total memory usage is: 4*float4 + 8*uint3 = 40 bytes per vertex
// The previous implementation used a "cross" layout without an index buffer
// which used 6*float4 = 48 bytes per vertex
// NOTE: the vertex buffer is a float4 because it is registered as a UAV for the compute shader to work
// TODO: use a plain float vertex buffer with 3x the entries instead to save memory? (float3 UAVs are not allowed)
bool GetSupportHairStrandsProceduralPrimitive(EShaderPlatform InShaderPlatform);
FHairStrandsRaytracingResource::FHairStrandsRaytracingResource(const FHairStrandsBulkData& InData, const FHairResourceName& ResourceName, const FName& InOwnerName) :
	FHairCommonResource(EHairStrandsAllocationType::Deferred, ResourceName, InOwnerName), bOwnBuffers(true)
{
	bProceduralPrimitive = GetSupportHairStrandsProceduralPrimitive(GMaxRHIShaderPlatform);
	if (bProceduralPrimitive)
	{
		// only allocate space for primitive AABBs
		VertexCount = InData.GetNumPoints() * 2 * STRANDS_PROCEDURAL_INTERSECTOR_MAX_SPLITS;
	}
	else
	{
		VertexCount = InData.GetNumPoints() * 4;
		IndexCount = InData.GetNumPoints() * 8 * 3;
	}
}

FHairStrandsRaytracingResource::FHairStrandsRaytracingResource(const FHairCardsBulkData& InData, const FHairResourceName& ResourceName, const FName& InOwnerName) :
	FHairCommonResource(EHairStrandsAllocationType::Deferred, ResourceName, InOwnerName),
	VertexCount(InData.GetNumVertices()), bOwnBuffers(false)
{}

FHairStrandsRaytracingResource::FHairStrandsRaytracingResource(const FHairMeshesBulkData& InData, const FHairResourceName& ResourceName, const FName& InOwnerName) :
	FHairCommonResource(EHairStrandsAllocationType::Deferred, ResourceName, InOwnerName),
	VertexCount(InData.GetNumVertices()), bOwnBuffers(false)
{}

void FHairStrandsRaytracingResource::InternalAllocate(FRDGBuilder& GraphBuilder)
{
	if (bOwnBuffers)
	{
		InternalCreateVertexBufferRDG<FHairStrandsRaytracingFormat>(GraphBuilder, VertexCount, PositionBuffer, ToHairResourceDebugName(TEXT("Hair.StrandsRaytracing_PositionBuffer"), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
		InternalCreateStructuredBufferRDG<FHairStrandsIndexFormat>(GraphBuilder, IndexCount, IndexBuffer, ToHairResourceDebugName(TEXT("Hair.StrandsRaytracing_IndexBuffer"), ResourceName), OwnerName, EHairResourceUsageType::Dynamic);
	}
}

void FHairStrandsRaytracingResource::InternalRelease()
{
	PositionBuffer.Release();
	IndexBuffer.Release();
	RayTracingGeometry.ReleaseResource();
	bIsRTGeometryInitialized = false;
}
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Debug data

static uint32 ToLinearCoord(const FIntVector& T, const FIntVector& Resolution)
{
	// Morton instead for better locality?
	return T.X + T.Y * Resolution.X + T.Z * Resolution.X * Resolution.Y;
}

static FIntVector ToCoord(const FVector3f& T, const FIntVector& Resolution, const FVector3f& MinBound, const float VoxelSize)
{
	const FVector3f C = (T - MinBound) / VoxelSize;
	return FIntVector(
		FMath::Clamp(FMath::FloorToInt(C.X), 0, Resolution.X - 1),
		FMath::Clamp(FMath::FloorToInt(C.Y), 0, Resolution.Y - 1),
		FMath::Clamp(FMath::FloorToInt(C.Z), 0, Resolution.Z - 1));
}

void CreateHairStrandsDebugDatas(
	const FHairStrandsDatas& InData,
	FHairStrandsDebugDatas& Out)
{
	const FVector3f BoundSize = FVector3f(InData.BoundingBox.Max) - FVector3f(InData.BoundingBox.Min);
	Out.VoxelDescription.VoxelSize = FMath::Clamp(GHairStrandsDebugVoxel_WorldSize, 0.1f, 10.f);
	Out.VoxelDescription.VoxelResolution = FIntVector(FMath::CeilToInt(BoundSize.X / Out.VoxelDescription.VoxelSize), FMath::CeilToInt(BoundSize.Y / Out.VoxelDescription.VoxelSize), FMath::CeilToInt(BoundSize.Z / Out.VoxelDescription.VoxelSize));
	Out.VoxelDescription.VoxelMinBound = FVector3f(InData.BoundingBox.Min);
	Out.VoxelDescription.VoxelMaxBound = FVector3f(Out.VoxelDescription.VoxelResolution) * Out.VoxelDescription.VoxelSize + FVector3f(InData.BoundingBox.Min);
	Out.VoxelOffsetAndCount.Init(FHairStrandsDebugDatas::FOffsetAndCount(), Out.VoxelDescription.VoxelResolution.X * Out.VoxelDescription.VoxelResolution.Y * Out.VoxelDescription.VoxelResolution.Z);
	Out.VoxelDescription.MaxSegmentPerVoxel = 0;

	uint32 AllocationCount = 0;
	TArray<TArray<FHairStrandsDebugDatas::FVoxel>> TempVoxelData;

	const uint32 MaxNumberOfSegmentPerVoxel = uint32(FMath::Clamp(GHairStrandsDebugVoxel_MaxSegmentPerVoxel, 16, 64000));

	// Fill in voxel (TODO: make it parallel)
	const uint32 CurveCount = InData.StrandsCurves.Num();
	for (uint32 CurveIndex = 0; CurveIndex < CurveCount; ++CurveIndex)
	{
		const uint32 PointOffset = InData.StrandsCurves.CurvesOffset[CurveIndex];
		const uint32 PointCount = InData.StrandsCurves.CurvesCount[CurveIndex];

		for (uint32 PointIndex = 0; PointIndex < PointCount - 1; ++PointIndex)
		{
			const uint32 Index0 = PointOffset + PointIndex;
			const uint32 Index1 = PointOffset + PointIndex + 1;
			const FVector3f& P0 = InData.StrandsPoints.PointsPosition[Index0];
			const FVector3f& P1 = InData.StrandsPoints.PointsPosition[Index1];
			const FVector3f Segment = P1 - P0;

			const float Length = Segment.Size();
			const uint32 StepCount = FMath::CeilToInt(Length / (0.25f * Out.VoxelDescription.VoxelSize));
			uint32 PrevLinearCoord = ~0;
			for (uint32 StepIt = 0; StepIt < StepCount + 1; ++StepIt)
			{
				const FVector3f P = P0 + Segment * StepIt / float(StepCount);
				const FIntVector Coord = ToCoord(P, Out.VoxelDescription.VoxelResolution, Out.VoxelDescription.VoxelMinBound, Out.VoxelDescription.VoxelSize);
				const uint32 LinearCoord = ToLinearCoord(Coord, Out.VoxelDescription.VoxelResolution);
				if (LinearCoord != PrevLinearCoord)
				{
					if (Out.VoxelOffsetAndCount[LinearCoord].Count == 0)
					{
						Out.VoxelOffsetAndCount[LinearCoord].Offset = TempVoxelData.Num();
						TempVoxelData.Add(TArray<FHairStrandsDebugDatas::FVoxel>());
					}

					if (Out.VoxelOffsetAndCount[LinearCoord].Count+1 < MaxNumberOfSegmentPerVoxel)
					{
						const uint32 Offset = Out.VoxelOffsetAndCount[LinearCoord].Offset;
						Out.VoxelOffsetAndCount[LinearCoord].Count += 1;
						TempVoxelData[Offset].Add({Index0, Index1});
					}

					Out.VoxelDescription.MaxSegmentPerVoxel = FMath::Max(Out.VoxelDescription.MaxSegmentPerVoxel, Out.VoxelOffsetAndCount[LinearCoord].Count);

					PrevLinearCoord = LinearCoord;

					++AllocationCount;
				}
			}
		}
	}

	check(Out.VoxelDescription.MaxSegmentPerVoxel < MaxNumberOfSegmentPerVoxel);
	Out.VoxelData.Reserve(AllocationCount);

	for (int32 Index = 0, Count = Out.VoxelOffsetAndCount.Num(); Index < Count; ++Index)
	{
		if (Out.VoxelOffsetAndCount[Index].Count > 0)
		{
			const uint32 ArrayIndex = Out.VoxelOffsetAndCount[Index].Offset;
			const uint32 NewOffset = Out.VoxelData.Num();
			Out.VoxelOffsetAndCount[Index].Offset = NewOffset;

			for (FHairStrandsDebugDatas::FVoxel& Voxel : TempVoxelData[ArrayIndex])
			{
				Voxel.Index1 = NewOffset;
				Out.VoxelData.Add(Voxel);
			}
		}
		else
		{
			Out.VoxelOffsetAndCount[Index].Offset = 0;
		}

		// Sanity check
		//check(Out.VoxelOffsetAndCount[Index].Offset + Out.VoxelOffsetAndCount[Index].Count == Out.VoxelData.Num());
	}

	check(Out.VoxelData.Num()>0);
}

void CreateHairStrandsDebugResources(FRDGBuilder& GraphBuilder, const FHairStrandsDebugDatas* In, FHairStrandsDebugDatas::FResources* Out)
{
	check(In);
	check(Out);

	Out->VoxelDescription = In->VoxelDescription;

	FRDGBufferRef VoxelOffsetAndCount = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("HairStrandsDebug_VoxelOffsetAndCount"),
		sizeof(FHairStrandsDebugDatas::FOffsetAndCount),
		In->VoxelOffsetAndCount.Num(),
		In->VoxelOffsetAndCount.GetData(),
		sizeof(FHairStrandsDebugDatas::FOffsetAndCount) * In->VoxelOffsetAndCount.Num());

	FRDGBufferRef VoxelData = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("HairStrandsDebug_VoxelData"),
		sizeof(FHairStrandsDebugDatas::FVoxel),
		In->VoxelData.Num(),
		In->VoxelData.GetData(),
		sizeof(FHairStrandsDebugDatas::FVoxel) * In->VoxelData.Num());

	Out->VoxelOffsetAndCount = ConvertToExternalAccessBuffer(GraphBuilder, VoxelOffsetAndCount);
	Out->VoxelData = ConvertToExternalAccessBuffer(GraphBuilder, VoxelData);
}
