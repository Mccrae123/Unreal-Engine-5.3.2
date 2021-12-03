// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NaniteResources.h"
#include "UnifiedBuffer.h"
#include "RenderGraphResources.h"
#include "RHIGPUReadback.h"

#define SANITY_CHECK_STREAMING_REQUESTS		0		// Performs a number of sanity checks of streaming requests to verify their integrity.
													// Must match define in ClusterCulling.ush

class IFileCacheHandle;

namespace Nanite
{

struct FPageKey
{
	uint32 RuntimeResourceID;
	uint32 PageIndex;
};

FORCEINLINE uint32 GetTypeHash( const FPageKey& Key )
{
	return Key.RuntimeResourceID * 0xFC6014F9u + Key.PageIndex * 0x58399E77u;
}

FORCEINLINE bool operator==( const FPageKey& A, const FPageKey& B )
{
	return A.RuntimeResourceID == B.RuntimeResourceID && A.PageIndex == B.PageIndex;
}

FORCEINLINE bool operator!=(const FPageKey& A, const FPageKey& B)
{
	return !(A == B);
}

FORCEINLINE bool operator<(const FPageKey& A, const FPageKey& B)
{
	return A.RuntimeResourceID != B.RuntimeResourceID ? A.RuntimeResourceID < B.RuntimeResourceID : A.PageIndex < B.PageIndex;
}


// Before deduplication
struct FGPUStreamingRequest
{
	uint32		RuntimeResourceID_Magic;
	uint32		PageIndex_NumPages_Magic;
	uint32		Priority_Magic;
};

// After deduplication
struct FStreamingRequest
{
	FPageKey	Key;
	uint32		Priority;
};

FORCEINLINE bool operator<(const FStreamingRequest& A, const FStreamingRequest& B)
{
	return A.Key != B.Key ? A.Key < B.Key : A.Priority > B.Priority;
}

struct FStreamingPageInfo
{
	FStreamingPageInfo* Next;
	FStreamingPageInfo* Prev;

	FPageKey	RegisteredKey;
	FPageKey	ResidentKey;
	
	uint32		GPUPageIndex;
	uint32		LatestUpdateIndex;
	uint32		RefCount;
};

struct FRootPageInfo
{
	uint32	RuntimeResourceID;
	uint32	NumClusters;
};

struct FPendingPage
{
#if !WITH_EDITOR
	uint8*					MemoryPtr;
	FIoRequest				Request;

	// Legacy compatibility
	// Delete when we can rely on IoStore
	IAsyncReadFileHandle*	AsyncHandle;
	IAsyncReadRequest*		AsyncRequest;
#endif

	uint32					GPUPageIndex;
	FPageKey				InstallKey;
#if !UE_BUILD_SHIPPING
	uint32					BytesLeftToStream;
#endif
};

class FRequestsHashTable;
class FStreamingPageUploader;

struct FAsyncState
{
	FRHIGPUBufferReadback*	LatestReadbackBuffer		= nullptr;
	const uint32*			LatestReadbackBufferPtr		= nullptr;
	uint32					NumReadyPages				= 0;
	bool					bUpdateActive				= false;
	bool					bBuffersTransitionedToWrite = false;
};

/*
 * Streaming manager for Nanite.
 */
class FStreamingManager : public FRenderResource
{
public:
	FStreamingManager();
	
	virtual void InitRHI() override;
	virtual void ReleaseRHI() override;

	void	Add( FResources* Resources );
	void	Remove( FResources* Resources );

	ENGINE_API void BeginAsyncUpdate(FRDGBuilder& GraphBuilder);			// Called once per frame before any Nanite rendering has occurred. Must be called before EndUpdate.
	ENGINE_API void EndAsyncUpdate(FRDGBuilder& GraphBuilder);				// Called once per frame before any Nanite rendering has occurred. Must be called after BeginUpdate.
	ENGINE_API bool IsAsyncUpdateInProgress();
	ENGINE_API void	SubmitFrameStreamingRequests(FRDGBuilder& GraphBuilder);		// Called once per frame after the last request has been added.

	const TRefCountPtr< FRDGPooledBuffer >&	GetStreamingRequestsBuffer()		{ return StreamingRequestsBuffer; }
	uint32									GetStreamingRequestsBufferVersion() { return StreamingRequestsBufferVersion; }

	FRHIShaderResourceView*				GetClusterPageDataSRV() const			{ return ClusterPageData.DataBuffer.SRV; }
	FRHIShaderResourceView*				GetHierarchySRV() const					{ return Hierarchy.DataBuffer.SRV; }
	FRHIShaderResourceView*				GetImposterDataSRV() const				{ return ImposterData.DataBuffer.SRV; }
	uint32								GetMaxStreamingPages() const			{ return MaxStreamingPages; }

	inline bool HasResourceEntries() const
	{
		return !RuntimeResourceMap.IsEmpty();
	}

	ENGINE_API void		RequestNanitePages(TArrayView<uint32> RequestData);
#if WITH_EDITOR
	ENGINE_API uint64	GetRequestRecordBuffer(TArray<uint32>& OutRequestData);
	ENGINE_API void		SetRequestRecordBuffer(uint64 Handle);
#endif
	
private:
	friend class FStreamingUpdateTask;

	struct FHeapBuffer
	{
		int32					TotalUpload = 0;

		FGrowOnlySpanAllocator	Allocator;

		FScatterUploadBuffer	UploadBuffer;
		FRWByteAddressBuffer	DataBuffer;

		void	Release()
		{
			UploadBuffer.Release();
			DataBuffer.Release();
		}
	};

	FHeapBuffer				ClusterPageData;	// FPackedCluster*, GeometryData { Index, Position, TexCoord, TangentX, TangentZ }*
	FScatterUploadBuffer	ClusterFixupUploadBuffer;
	FHeapBuffer				Hierarchy;
	FHeapBuffer				ImposterData;
	TRefCountPtr< FRDGPooledBuffer > StreamingRequestsBuffer;

	uint32					StreamingRequestsBufferVersion;
	uint32					MaxStreamingPages;
	uint32					MaxPendingPages;
	uint32					MaxPageInstallsPerUpdate;
	uint32					MaxStreamingReadbackBuffers;

	uint32					ReadbackBuffersWriteIndex;
	uint32					ReadbackBuffersNumPending;

	TArray<uint32>			NextRootPageVersion;
	uint32					NextUpdateIndex;
	uint32					NumRegisteredStreamingPages;
	uint32					NumPendingPages;
	uint32					NextPendingPageIndex;

	uint32					StatNumRootPages;
	uint32					StatPeakRootPages;
	uint32					StatPeakAllocatedRootPages;

	TArray<FRootPageInfo>	RootPageInfos;

#if !UE_BUILD_SHIPPING
	uint64					PrevUpdateTick;
#endif

	TArray< FRHIGPUBufferReadback* >		StreamingRequestReadbackBuffers;
	TArray< FResources* >					PendingAdds;

	TMap< uint32, FResources* >				RuntimeResourceMap;
	TMultiMap< uint32, FResources* >		PersistentHashResourceMap;			// TODO: MultiMap to handle potential collisions and issues with there temporarily being two meshes with the same hash because of unordered add/remove.
	TMap< FPageKey, FStreamingPageInfo* >	RegisteredStreamingPagesMap;		// This is updated immediately.
	TMap< FPageKey, FStreamingPageInfo* >	CommittedStreamingPageMap;			// This update is deferred to the point where the page has been loaded and committed to memory.
	TArray< FStreamingRequest >				PrioritizedRequestsHeap;
	FStreamingPageInfo						StreamingPageLRU;

	FStreamingPageInfo*						StreamingPageInfoFreeList;
	TArray< FStreamingPageInfo >			StreamingPageInfos;
	TArray< FFixupChunk* >					StreamingPageFixupChunks;			// Fixup information for resident streaming pages. We need to keep this around to be able to uninstall pages.

	TArray< FPendingPage >					PendingPages;
#if !WITH_EDITOR
	TArray< uint8 >							PendingPageStagingMemory;
#endif
	TArray< uint32 >						GPUPageDependencies;

	FRequestsHashTable*						RequestsHashTable = nullptr;
	FStreamingPageUploader*					PageUploader = nullptr;

	FGraphEventArray						AsyncTaskEvents;
	FAsyncState								AsyncState;

#if WITH_EDITOR
	uint64									PageRequestRecordHandle = (uint64)-1;
	TMap<FPageKey, uint32>					PageRequestRecordMap;
#endif
	TArray<uint32>							PendingExplicitRequests;

	void AddPendingExplicitRequests();

	void CollectDependencyPages( FResources* Resources, TSet< FPageKey >& DependencyPages, const FPageKey& Key );
	void SelectStreamingPages( FResources* Resources, TArray< FPageKey >& SelectedPages, TSet<FPageKey>& SelectedPagesSet, uint32 RuntimeResourceID, uint32 PageIndex, uint32 MaxSelectedPages );

	void RegisterStreamingPage( FStreamingPageInfo* Page, const FPageKey& Key );
	void UnregisterPage( const FPageKey& Key );
	void MovePageToFreeList( FStreamingPageInfo* Page );

	void ApplyFixups( const FFixupChunk& FixupChunk, const FResources& Resources, uint32 PageIndex, uint32 GPUPageIndex );

	bool ArePageDependenciesCommitted(uint32 RuntimeResourceID, uint32 PageIndex, uint32 DependencyPageStart, uint32 DependencyPageNum);

	uint32 GPUPageIndexToGPUOffset(uint32 PageIndex) const;

	// Returns whether any work was done and page/hierarchy buffers were transitioned to compute writable state
	bool ProcessNewResources( FRDGBuilder& GraphBuilder);
	
	uint32 DetermineReadyPages();
	void InstallReadyPages( uint32 NumReadyPages );

	void AsyncUpdate();

	void ClearStreamingRequestCount(FRDGBuilder& GraphBuilder, FRDGBufferUAVRef BufferUAVRef);
#if DO_CHECK
	void VerifyPageLRU( FStreamingPageInfo& List, uint32 TargetListLength, bool bCheckUpdateIndex );
#endif

#if SANITY_CHECK_STREAMING_REQUESTS
	void SanityCheckStreamingRequests(const FGPUStreamingRequest* StreamingRequestsPtr, const uint32 NumStreamingRequests);
#endif

#if WITH_EDITOR
	void RecordGPURequests();
#endif
};

extern ENGINE_API TGlobalResource< FStreamingManager > GStreamingManager;

} // namespace Nanite