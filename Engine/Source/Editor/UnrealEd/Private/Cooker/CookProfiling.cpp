// Copyright Epic Games, Inc. All Rights Reserved.

#include "CookProfiling.h"

#include "Algo/GraphConvert.h"
#include "Algo/Sort.h"
#include "Algo/Unique.h"
#include "Containers/Array.h"
#include "Containers/BitArray.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "CookOnTheSide/CookLog.h"
#include "CoreGlobals.h"
#include "DerivedDataBuildRemoteExecutor.h"
#include "Misc/OutputDevice.h"
#include "Misc/StringBuilder.h"
#include "PackageBuildDependencyTracker.h"
#include "Serialization/ArchiveUObject.h"
#include "Templates/Casts.h"
#include "UObject/GCObject.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/Object.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtr.h"

#if OUTPUT_COOKTIMING || ENABLE_COOK_STATS
#include "ProfilingDebugging/ScopedTimers.h"
#endif

#if OUTPUT_COOKTIMING
#include <Containers/AllocatorFixedSizeFreeList.h>
#endif

#if ENABLE_COOK_STATS
#include "AnalyticsET.h"
#include "AnalyticsEventAttribute.h"
#include "IAnalyticsProviderET.h"
#include "StudioAnalytics.h"
#include "Virtualization/VirtualizationSystem.h"
#endif

#if OUTPUT_COOKTIMING
struct FHierarchicalTimerInfo
{
public:
	FHierarchicalTimerInfo(const FHierarchicalTimerInfo& InTimerInfo) = delete;
	FHierarchicalTimerInfo(FHierarchicalTimerInfo&& InTimerInfo) = delete;

	explicit FHierarchicalTimerInfo(const char* InName, uint16 InId)
	:	Id(InId)
	,	Name(InName)
	{
	}

	~FHierarchicalTimerInfo()
	{
		ClearChildren();
	}

	void ClearChildren()
	{
		for (FHierarchicalTimerInfo* Child = FirstChild; Child;)
		{
			FHierarchicalTimerInfo* NextChild = Child->NextSibling;

			DestroyAndFree(Child);

			Child = NextChild;
		}
		FirstChild = nullptr;
	}
	FHierarchicalTimerInfo* GetChild(uint16 InId, const char* InName)
	{
		for (FHierarchicalTimerInfo* Child = FirstChild; Child;)
		{
			if (Child->Id == InId)
				return Child;

			Child = Child->NextSibling;
		}

		FHierarchicalTimerInfo* Child = AllocNew(InName, InId);

		Child->NextSibling	= FirstChild;
		FirstChild			= Child;

		return Child;
	}
	

	uint32							HitCount = 0;
	uint16							Id = 0;
	bool							IncrementDepth = true;
	double							Length = 0;
	const char*						Name;

	FHierarchicalTimerInfo*			FirstChild = nullptr;
	FHierarchicalTimerInfo*			NextSibling = nullptr;

private:
	static FHierarchicalTimerInfo*	AllocNew(const char* InName, uint16 InId);
	static void						DestroyAndFree(FHierarchicalTimerInfo* InPtr);
};

static FHierarchicalTimerInfo RootTimerInfo("Root", 0);
static FHierarchicalTimerInfo* CurrentTimerInfo = &RootTimerInfo;
static TAllocatorFixedSizeFreeList<sizeof(FHierarchicalTimerInfo), 256> TimerInfoAllocator;

FHierarchicalTimerInfo* FHierarchicalTimerInfo::AllocNew(const char* InName, uint16 InId)
{
	return new(TimerInfoAllocator.Allocate()) FHierarchicalTimerInfo(InName, InId);
}

void FHierarchicalTimerInfo::DestroyAndFree(FHierarchicalTimerInfo* InPtr)
{
	InPtr->~FHierarchicalTimerInfo();
	TimerInfoAllocator.Free(InPtr);
}

FScopeTimer::FScopeTimer(int InId, const char* InName, bool IncrementScope /*= false*/ )
{
	checkSlow(IsInGameThread());

	HierarchyTimerInfo = CurrentTimerInfo->GetChild(static_cast<uint16>(InId), InName);

	HierarchyTimerInfo->IncrementDepth = IncrementScope;

	PrevTimerInfo		= CurrentTimerInfo;
	CurrentTimerInfo	= HierarchyTimerInfo;
}

void FScopeTimer::Start()
{
	if (StartTime)
	{
		return;
	}

	StartTime = FPlatformTime::Cycles64();
}

void FScopeTimer::Stop()
{
	if (!StartTime)
	{
		return;
	}

	HierarchyTimerInfo->Length += FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - StartTime);
	++HierarchyTimerInfo->HitCount;

	StartTime = 0;
}

FScopeTimer::~FScopeTimer()
{
	Stop();

	check(CurrentTimerInfo == HierarchyTimerInfo);
	CurrentTimerInfo = PrevTimerInfo;
}

void OutputHierarchyTimers(const FHierarchicalTimerInfo* TimerInfo, int32 Depth)
{
	FString TimerName(TimerInfo->Name);

	static const TCHAR LeftPad[] = TEXT("                                ");
	const SIZE_T PadOffset = FMath::Max<int>(UE_ARRAY_COUNT(LeftPad) - 1 - Depth * 2, 0);

	UE_LOG(LogCook, Display, TEXT("  %s%s: %.3fs (%u)"), &LeftPad[PadOffset], *TimerName, TimerInfo->Length, TimerInfo->HitCount);

	// We need to print in reverse order since the child list begins with the most recently added child

	TArray<const FHierarchicalTimerInfo*> Stack;

	for (const FHierarchicalTimerInfo* Child = TimerInfo->FirstChild; Child; Child = Child->NextSibling)
	{
		Stack.Add(Child);
	}

	const int32 ChildDepth = Depth + TimerInfo->IncrementDepth;

	for (size_t i = Stack.Num(); i > 0; --i)
	{
		OutputHierarchyTimers(Stack[i - 1], ChildDepth);
	}
}

void OutputHierarchyTimers()
{
	UE_LOG(LogCook, Display, TEXT("Hierarchy Timer Information:"));

	OutputHierarchyTimers(&RootTimerInfo, 0);
}

void ClearHierarchyTimers()
{
	RootTimerInfo.ClearChildren();
}

#endif

#if ENABLE_COOK_STATS
namespace DetailedCookStats
{
	FString CookProject;
	FString CookCultures;
	FString CookLabel;
	FString TargetPlatforms;
	double CookStartTime = 0.0;
	double CookWallTimeSec = 0.0;
	double StartupWallTimeSec = 0.0;
	double CookByTheBookTimeSec = 0.0;
	double StartCookByTheBookTimeSec = 0.0;
	double TickCookOnTheSideTimeSec = 0.0;
	double TickCookOnTheSideLoadPackagesTimeSec = 0.0;
	double TickCookOnTheSideResolveRedirectorsTimeSec = 0.0;
	double TickCookOnTheSideSaveCookedPackageTimeSec = 0.0;
	double TickCookOnTheSidePrepareSaveTimeSec = 0.0;
	double BlockOnAssetRegistryTimeSec = 0.0;
	double GameCookModificationDelegateTimeSec = 0.0;
	double TickLoopGCTimeSec = 0.0;
	double TickLoopRecompileShaderRequestsTimeSec = 0.0;
	double TickLoopShaderProcessAsyncResultsTimeSec = 0.0;
	double TickLoopProcessDeferredCommandsTimeSec = 0.0;
	double TickLoopTickCommandletStatsTimeSec = 0.0;
	double TickLoopFlushRenderingCommandsTimeSec = 0.0;
	bool IsCookAll = false;
	bool IsCookOnTheFly = false;
	bool IsIterativeCook = false;
	bool IsFastCook = false;
	bool IsUnversioned = false;


	// Stats tracked through FAutoRegisterCallback
	int32 PeakRequestQueueSize = 0;
	int32 PeakLoadQueueSize = 0;
	int32 PeakSaveQueueSize = 0;
	uint32 NumPreloadedDependencies = 0;
	uint32 NumPackagesIterativelySkipped = 0;
	uint32 NumPackagesSavedForCook = 0;
	FCookStatsManager::FAutoRegisterCallback RegisterCookOnTheFlyServerStats([](FCookStatsManager::AddStatFuncRef AddStat)
		{
			AddStat(TEXT("Package.Load"), FCookStatsManager::CreateKeyValueArray(TEXT("NumPreloadedDependencies"), NumPreloadedDependencies));
			AddStat(TEXT("Package.Save"), FCookStatsManager::CreateKeyValueArray(TEXT("NumPackagesIterativelySkipped"), NumPackagesIterativelySkipped));
			AddStat(TEXT("CookOnTheFlyServer"), FCookStatsManager::CreateKeyValueArray(TEXT("PeakRequestQueueSize"), PeakRequestQueueSize));
			AddStat(TEXT("CookOnTheFlyServer"), FCookStatsManager::CreateKeyValueArray(TEXT("PeakLoadQueueSize"), PeakLoadQueueSize));
			AddStat(TEXT("CookOnTheFlyServer"), FCookStatsManager::CreateKeyValueArray(TEXT("PeakSaveQueueSize"), PeakSaveQueueSize));
		});
}
#endif

namespace UE::Cook
{

/** The various ways objects can be referenced that keeps them in memory. */
enum class EObjectReferencerType : uint8
{
	Unknown = 0,
	Rooted,
	GCObjectRef,
	Referenced,
};

/**
 * Data for how an object is referenced in the DumpObjClassList graph search,
 * including the type of reference and the vertex of the referencer.
 */
struct FObjectReferencer
{
	FObjectReferencer() = default;
	explicit FObjectReferencer(EObjectReferencerType InLinkType, Algo::Graph::FVertex InVertexArgument = Algo::Graph::InvalidVertex)
	{
		Set(InLinkType, InVertexArgument);
	}

	Algo::Graph::FVertex GetVertexArgument() const
	{
		return VertexArgument;
	}
	EObjectReferencerType GetLinkType()
	{
		return LinkType;
	}
	void Set(EObjectReferencerType InLinkType, Algo::Graph::FVertex InVertexArgument = Algo::Graph::InvalidVertex)
	{
		switch (InLinkType)
		{
		case EObjectReferencerType::GCObjectRef:
			check(InVertexArgument != Algo::Graph::InvalidVertex);
			break;
		case EObjectReferencerType::Referenced:
			check(InVertexArgument != Algo::Graph::InvalidVertex);
			break;
		default:
			break;
		}

		VertexArgument = InVertexArgument;
		LinkType = InLinkType;
	}
	void ToString(FStringBuilderBase& Builder, TConstArrayView<UObject*> VertexToObject)
	{
		switch (GetLinkType())
		{
		case EObjectReferencerType::Unknown:
			Builder << TEXT("<Unknown>");
			break;
		case EObjectReferencerType::Rooted:
			Builder << TEXT("<Rooted>");
			break;
		case EObjectReferencerType::GCObjectRef:
		{
			check(VertexArgument != Algo::Graph::InvalidVertex);
			UObject* Object = VertexToObject[VertexArgument];
			FString ReferencerName;
			if (!Object || !FGCObject::GGCObjectReferencer->GetReferencerName(Object, ReferencerName))
			{
				ReferencerName = TEXT("<Unknown>");
			}
			Builder << TEXT("FGCObject ") << ReferencerName;
			break;
		}
		case EObjectReferencerType::Referenced:
		{
			check(VertexArgument != Algo::Graph::InvalidVertex);
			UObject* Object = VertexToObject[VertexArgument];
			if (Object)
			{
				Object->GetPathName(nullptr, Builder);
			}
			else
			{
				Builder << TEXT("<UnknownObject>");
			}
			break;
		}
		default:
			checkNoEntry();
			break;
		}
	}

private:
	Algo::Graph::FVertex VertexArgument = Algo::Graph::InvalidVertex;
	EObjectReferencerType LinkType = EObjectReferencerType::Unknown;
};

/** An ObjectReferenceCollector to pass to Object->Serialize to collect references into an array. */
class FArchiveGetReferences : public FArchiveUObject
{
public:
	FArchiveGetReferences(UObject* Object, TArray<UObject*>& OutReferencedObjects)
		:ReferencedObjects(OutReferencedObjects)
	{
		ArIsObjectReferenceCollector = true;
		ArIgnoreOuterRef = false;
		SetShouldSkipCompilingAssets(false);
		Object->Serialize(*this);
	}

	FArchive& operator<<(UObject*& Object)
	{
		if (Object)
		{
			ReferencedObjects.Add(Object);
		}

		return *this;
	}
private:
	TArray<UObject*>& ReferencedObjects;
};

/**
 * Given the list of AllObjects from e.g. a TObjectIterator, use serialization and other methods from Garbage Collection
 * to find all the dependencies of each Object.
 * Return the dependencies as a normalized graph in the style of GraphConvert.h, with the vertex of each object defined
 * by AllObjects and ObjectToVertex.
 */
void ConstructObjectGraph(TConstArrayView<UObject*> AllObjects,
	const TMap<UObject*, Algo::Graph::FVertex>& ObjectToVertex, TArray64<Algo::Graph::FVertex>& OutGraphBuffer,
	TArray<TConstArrayView<Algo::Graph::FVertex>>& OutGraph)
{
	using namespace Algo::Graph;

	TArray<TArray<FVertex>> LooseEdges;
	int32 NumVertices = AllObjects.Num();
	LooseEdges.SetNum(NumVertices);
	TArray<UObject*> TargetObjects;
	int32 NumEdges = 0;

	for (FVertex SourceVertex = 0; SourceVertex < NumVertices; ++SourceVertex)
	{
		UObject* SourceObject = AllObjects[SourceVertex];
		TargetObjects.Reset();
		{
			FReferenceFinder Collector(TargetObjects);
			if (SourceObject == FGCObject::GGCObjectReferencer)
			{
				UGCObjectReferencer::AddReferencedObjects(FGCObject::GGCObjectReferencer, Collector);
			}
			else
			{
				FArchiveGetReferences Ar(SourceObject, TargetObjects);
				if (SourceObject->GetClass())
				{
					SourceObject->GetClass()->CallAddReferencedObjects(SourceObject, Collector);
				}
				// TODO: Handle elements in the token stream not covered by serialize, such as UPackage's
				// Class->EmitObjectReference(STRUCT_OFFSET(UPackage, MetaData), TEXT("MetaData"));
				// In the meantime we handle MetaData explicitly.
				if (UPackage* AsPackage = Cast<UPackage>(SourceObject))
				{
					TargetObjects.Add(AsPackage->GetMetaData());
				}
			}
		}
		if (TargetObjects.Num())
		{
			Algo::Sort(TargetObjects);
			TargetObjects.SetNum(Algo::Unique(TargetObjects), false /* bAllowShrinking */);
			TArray<FVertex>& TargetVertices = LooseEdges[SourceVertex];
			TargetVertices.Reserve(TargetObjects.Num());
			for (UObject* TargetObject : TargetObjects)
			{
				const FVertex* TargetVertex = ObjectToVertex.Find(TargetObject);
				if (TargetVertex && *TargetVertex != SourceVertex)
				{
					TargetVertices.Add(*TargetVertex);
				}
			}
			NumEdges += TargetVertices.Num();
		}
	}
	OutGraphBuffer.Empty(NumEdges);
	OutGraph.Empty(NumVertices);
	for (FVertex SourceVertex = 0; SourceVertex < NumVertices; ++SourceVertex)
	{
		TArray<FVertex>& InEdges = LooseEdges[SourceVertex];
		TConstArrayView<FVertex>& OutEdges = OutGraph.Emplace_GetRef();
		OutEdges = TConstArrayView<FVertex>(OutGraphBuffer.GetData() + OutGraphBuffer.Num(), InEdges.Num());
		OutGraphBuffer.Append(InEdges);
	}
}

void DumpObjClassList(TConstArrayView<FWeakObjectPtr> InitialObjects)
{
	using namespace Algo::Graph;

	FOutputDevice& LogAr = *(GLog);

	// Get the list of Objects
	TArray<UObject*> AllObjects;
	for (FThreadSafeObjectIterator Iter; Iter; ++Iter)
	{
		UObject* Object = *Iter;
		if (!Object)
		{
			continue;
		}
		AllObjects.Add(Object);
	}

	// Convert Objects to Algo::Graph::FVertex to reduce graph search memory
	int32 NumVertices = AllObjects.Num();
	TMap<UObject*, FVertex> VertexOfObject;
	for (FVertex Vertex = 0; Vertex < NumVertices; ++Vertex)
	{
		VertexOfObject.Add(AllObjects[Vertex], Vertex);
	}

	// Store for each vertex whether the vertex is new - not in InitialObjects
	TBitArray<> IsNew(true, NumVertices);
	for (const FWeakObjectPtr& InitialObjectWeak : InitialObjects)
	{
		UObject* InitialObject = InitialObjectWeak.Get();
		if (InitialObject)
		{
			FVertex* Vertex = VertexOfObject.Find(InitialObject);
			if (Vertex)
			{
				IsNew[*Vertex] = false;
			}
		}
	}

	// Serialize objects to get dependencies and use them to create the ObjectGraph
	TArray64<FVertex> ObjectGraphBuffer;
	TArray<TConstArrayView<FVertex>> ObjectGraph;
	ConstructObjectGraph(AllObjects, VertexOfObject, ObjectGraphBuffer, ObjectGraph);

	// Mark the objects that are rooted by IsRooted, and find any special vertices
	FVertex GCObjectReferencerVertex = InvalidVertex;
	TArray<FObjectReferencer> AliveReason;
	AliveReason.SetNum(NumVertices);
	for (FVertex Vertex = 0; Vertex < NumVertices; ++Vertex)
	{
		UObject* Object = AllObjects[Vertex];
		if (Object->IsRooted())
		{
			AliveReason[Vertex].Set(EObjectReferencerType::Rooted);
		}
		if (Object == FGCObject::GGCObjectReferencer)
		{
			GCObjectReferencerVertex = Vertex;
		}
	}

	// Mark the objects that are rooted by GCObjectReferencerVertex
	for (FVertex Vertex : ObjectGraph[GCObjectReferencerVertex])
	{
		if (AliveReason[Vertex].GetLinkType() == EObjectReferencerType::Unknown)
		{
			AliveReason[Vertex].Set(EObjectReferencerType::GCObjectRef, Vertex);
		}
	}
	check(GCObjectReferencerVertex != InvalidVertex);

	// Do a DFS to mark the referencer and root of all non-rooted objects
	TArray<FVertex> RootOfVertex;
	RootOfVertex.SetNumUninitialized(NumVertices);
	for (FVertex& Root : RootOfVertex)
	{
		Root = InvalidVertex;
	}

	TArray<FVertex> Stack;
	for (FVertex RootedVertex = 0; RootedVertex < NumVertices; ++RootedVertex)
	{
		if (AliveReason[RootedVertex].GetLinkType() == EObjectReferencerType::Unknown ||
			RootedVertex == GCObjectReferencerVertex)
		{
			continue;
		}

		RootOfVertex[RootedVertex] = RootedVertex;
		Stack.Reset();
		Stack.Add(RootedVertex);
		while (!Stack.IsEmpty())
		{
			FVertex SourceVertex = Stack.Pop(false /* bAllowShrinking */);
			for (FVertex TargetVertex : ObjectGraph[SourceVertex])
			{
				if (AliveReason[TargetVertex].GetLinkType() == EObjectReferencerType::Unknown)
				{
					AliveReason[TargetVertex].Set(EObjectReferencerType::Referenced, SourceVertex);
					RootOfVertex[TargetVertex] = RootedVertex;
					Stack.Add(TargetVertex);
				}
			}
		}
	}

	// Count how many new objects of each class there are, and store all root objects that keep them in memory
	struct FClassInfo
	{
		TMap<FVertex, int32> Roots;
		int32 Count = 0;
		UClass* Class = nullptr;
	};
	TMap<UClass*, FClassInfo> ClassInfos;
	for (FVertex Vertex = 0; Vertex < NumVertices; ++Vertex)
	{
		// Ignore non-new objects
		if (!IsNew[Vertex] || Vertex == GCObjectReferencerVertex)
		{
			continue;
		}
		FObjectReferencer Link = AliveReason[Vertex];
		EObjectReferencerType LinkType = Link.GetLinkType();
		// Ignore objects that have AliveReason unknown. This can occur if the objects were rooted during garbage
		// collection but then asynchronous work RemovedThemFromRoot in between GC finishing and our call to IsRooted.
		if (LinkType == EObjectReferencerType::Unknown)
		{
			continue;
		}
		UClass* Class = AllObjects[Vertex]->GetClass();
		if (!Class || !Class->IsNative())
		{
			continue;
		}
		FClassInfo& ClassInfo = ClassInfos.FindOrAdd(Class);
		ClassInfo.Class = Class;
		ClassInfo.Roots.FindOrAdd(RootOfVertex[Vertex], 0)++;
		ClassInfo.Count++;
	}

	TArray<FClassInfo> ClassInfoArray;
	ClassInfoArray.Reserve(ClassInfos.Num());
	for (TPair<UClass*, FClassInfo>& Pair : ClassInfos)
	{
		ClassInfoArray.Add(MoveTemp(Pair.Value));
	}
	Algo::Sort(ClassInfoArray, [](const FClassInfo& A, const FClassInfo& B)
		{
			return FTopLevelAssetPath(A.Class).Compare(FTopLevelAssetPath(B.Class)) < 0;
		});


	LogAr.Logf(TEXT("Memory Analysis: New Objects of each class and the top roots keeping them alive:"));
	LogAr.Logf(TEXT("\t%6s %s"), TEXT("Count"), TEXT("ClassPath"));
	LogAr.Logf(TEXT("\t\t%6s %s"), TEXT("Count"), TEXT("RootObjectAndChain"));
	TStringBuilder<1024> RootObjectString;
	constexpr int32 MaxRootCount = 2;
	TArray<TPair<FVertex, int32>, TInlineAllocator<MaxRootCount + 1>> MaxRoots;
	for (FClassInfo& ClassInfo : ClassInfoArray)
	{
		MaxRoots.Reset();
		for (TPair<FVertex, int32>& RootPair : ClassInfo.Roots)
		{
			for (int32 IndexFromMax = 0; IndexFromMax < MaxRootCount; ++IndexFromMax)
			{
				if (MaxRoots.Num() <= IndexFromMax || MaxRoots[IndexFromMax].Value < RootPair.Value)
				{
					MaxRoots.Insert(RootPair, IndexFromMax);
					break;
				}
			}
			if (MaxRoots.Num() > MaxRootCount)
			{
				MaxRoots.Pop(false /* bAllowShrinking */);
			}
		}
		LogAr.Logf(TEXT("\t%6d %s"), ClassInfo.Count, *ClassInfo.Class->GetPathName());
		for (TPair<FVertex, int32>& RootPair : MaxRoots)
		{
			RootObjectString.Reset();
			RootObjectString.Appendf(TEXT("\t\t%6d: "), RootPair.Value);
			AllObjects[RootPair.Key]->GetFullName(RootObjectString);
			FObjectReferencer Link = AliveReason[RootPair.Key];
			RootObjectString << TEXT(" <- ");
			Link.ToString(RootObjectString, AllObjects);
			while (Link.GetLinkType() == EObjectReferencerType::Referenced)
			{
				Link = AliveReason[Link.GetVertexArgument()];
				RootObjectString << TEXT(" <- ");
				Link.ToString(RootObjectString, AllObjects);
			}
			LogAr.Logf(TEXT("%s"), *RootObjectString);
		}
	}
}

} // namespace UE::Cook

#if ENABLE_COOK_STATS

namespace DetailedCookStats
{

FCookStatsManager::FAutoRegisterCallback RegisterCookStats([](FCookStatsManager::AddStatFuncRef AddStat)
{
	const FString StatName(TEXT("Cook.Profile"));
	#define ADD_COOK_STAT_FLT(Path, Name) AddStat(StatName, FCookStatsManager::CreateKeyValueArray(TEXT("Path"), TEXT(Path), TEXT(#Name), Name))
	ADD_COOK_STAT_FLT(" 0", CookWallTimeSec);
	ADD_COOK_STAT_FLT(" 0. 0", StartupWallTimeSec);
	ADD_COOK_STAT_FLT(" 0. 1", CookByTheBookTimeSec);
	ADD_COOK_STAT_FLT(" 0. 1. 0", StartCookByTheBookTimeSec);
	ADD_COOK_STAT_FLT(" 0. 1. 0. 0", BlockOnAssetRegistryTimeSec);
	ADD_COOK_STAT_FLT(" 0. 1. 0. 1", GameCookModificationDelegateTimeSec);
	ADD_COOK_STAT_FLT(" 0. 1. 1", TickCookOnTheSideTimeSec);
	ADD_COOK_STAT_FLT(" 0. 1. 1. 0", TickCookOnTheSideLoadPackagesTimeSec);
	ADD_COOK_STAT_FLT(" 0. 1. 1. 1", TickCookOnTheSideSaveCookedPackageTimeSec);
	ADD_COOK_STAT_FLT(" 0. 1. 1. 1. 0", TickCookOnTheSideResolveRedirectorsTimeSec);
	ADD_COOK_STAT_FLT(" 0. 1. 1. 2", TickCookOnTheSidePrepareSaveTimeSec);
	ADD_COOK_STAT_FLT(" 0. 1. 2", TickLoopGCTimeSec);
	ADD_COOK_STAT_FLT(" 0. 1. 3", TickLoopRecompileShaderRequestsTimeSec);
	ADD_COOK_STAT_FLT(" 0. 1. 4", TickLoopShaderProcessAsyncResultsTimeSec);
	ADD_COOK_STAT_FLT(" 0. 1. 5", TickLoopProcessDeferredCommandsTimeSec);
	ADD_COOK_STAT_FLT(" 0. 1. 6", TickLoopTickCommandletStatsTimeSec);
	ADD_COOK_STAT_FLT(" 0. 1. 7", TickLoopFlushRenderingCommandsTimeSec);
	ADD_COOK_STAT_FLT(" 0. 1. 8", TargetPlatforms);
	ADD_COOK_STAT_FLT(" 0. 1. 9", CookProject);
	ADD_COOK_STAT_FLT(" 0. 1. 10", CookCultures);
	ADD_COOK_STAT_FLT(" 0. 1. 11", IsCookAll);
	ADD_COOK_STAT_FLT(" 0. 1. 12", IsCookOnTheFly);
	ADD_COOK_STAT_FLT(" 0. 1. 13", IsIterativeCook);
	ADD_COOK_STAT_FLT(" 0. 1. 14", IsUnversioned);
	ADD_COOK_STAT_FLT(" 0. 1. 15", CookLabel);
	ADD_COOK_STAT_FLT(" 0. 1. 16", IsFastCook);
		
	#undef ADD_COOK_STAT_FLT
});

void LogCookStats(ECookMode::Type CookMode)
{
	if (IsCookingInEditor(CookMode))
	{
		return;
	}

	if (FStudioAnalytics::IsAvailable() && IsCookByTheBookMode(CookMode))
	{

		// convert filtered stats directly to an analytics event
		TArray<FAnalyticsEventAttribute> StatAttrs;

		// Sends each cook stat to the studio analytics system.
		auto SendCookStatsToAnalytics = [&StatAttrs](const FString& StatName, const TArray<FCookStatsManager::StringKeyValue>& StatAttributes)
		{
			for (const auto& Attr : StatAttributes)
			{
				FString FormattedAttrName = StatName + "." + Attr.Key;

				StatAttrs.Emplace(FormattedAttrName, Attr.Value);
			}
		};

		// Now actually grab the stats 
		FCookStatsManager::LogCookStats(SendCookStatsToAnalytics);

		// Record them all under cooking event
		FStudioAnalytics::GetProvider().RecordEvent(TEXT("Core.Cooking"), StatAttrs);

		FStudioAnalytics::GetProvider().BlockUntilFlushed(60.0f);
	}

	bool bSendCookAnalytics = false;
	GConfig->GetBool(TEXT("CookAnalytics"), TEXT("SendAnalytics"), bSendCookAnalytics, GEngineIni);

	if (IsCookByTheBookMode(CookMode) &&
		(GIsBuildMachine || FParse::Param(FCommandLine::Get(), TEXT("SendCookAnalytics")) || bSendCookAnalytics))
	{
		FString APIServerET;
		if (GConfig->GetString(TEXT("CookAnalytics"), TEXT("APIServer"), APIServerET, GEngineIni))
		{
			FString AppId(TEXT("Cook"));
			bool bUseLegacyCookProtocol = !GConfig->GetString(TEXT("CookAnalytics"), TEXT("AppId"), AppId, GEngineIni);

			// Optionally create an analytics provider to send stats to for central collection.
			TSharedPtr<IAnalyticsProviderET> CookAnalytics = FAnalyticsET::Get().CreateAnalyticsProvider(FAnalyticsET::Config(AppId, APIServerET, FString(), bUseLegacyCookProtocol));
			if (CookAnalytics.IsValid())
			{
				CookAnalytics->SetUserID(FString::Printf(TEXT("%s\\%s"), FPlatformProcess::ComputerName(), FPlatformProcess::UserName(false)));
				CookAnalytics->StartSession(MakeAnalyticsEventAttributeArray(
					TEXT("Project"), CookProject,
					TEXT("CmdLine"), FString(FCommandLine::Get()),
					TEXT("IsBuildMachine"), GIsBuildMachine,
					TEXT("TargetPlatforms"), TargetPlatforms
				));

				TArray<FString> CookStatsToSend;
				const bool bFilterStats = GConfig->GetArray(TEXT("CookAnalytics"), TEXT("CookStats"), CookStatsToSend, GEngineIni) > 0;
				// Sends each cook stat to the analytics provider.
				auto SendCookStatsToAnalytics = [CookAnalytics, &CookStatsToSend, bFilterStats](const FString& StatName, const TArray<FCookStatsManager::StringKeyValue>& StatAttributes)
				{
					if (!bFilterStats || CookStatsToSend.Contains(StatName))
					{
						// convert filtered stats directly to an analytics event
						TArray<FAnalyticsEventAttribute> StatAttrs;
						StatAttrs.Reset(StatAttributes.Num());
						for (const auto& Attr : StatAttributes)
						{
							StatAttrs.Emplace(Attr.Key, Attr.Value);
						}
						CookAnalytics->RecordEvent(StatName, StatAttrs);
					}
					else
					{
						UE_LOG(LogCook, Verbose, TEXT("[%s] not present in analytics CookStats filter"), *StatName);
					}
				};
				FCookStatsManager::LogCookStats(SendCookStatsToAnalytics);
			}
		}
	}

	/** Used for custom logging of DDC Resource usage stats. */
	struct FDDCResourceUsageStat
	{
	public:
		FDDCResourceUsageStat(FString InAssetType, double InTotalTimeSec, bool bIsGameThreadTime, double InSizeMB, int64 InAssetsBuilt) : AssetType(MoveTemp(InAssetType)), TotalTimeSec(InTotalTimeSec), GameThreadTimeSec(bIsGameThreadTime ? InTotalTimeSec : 0.0), SizeMB(InSizeMB), AssetsBuilt(InAssetsBuilt) {}
		void Accumulate(const FDDCResourceUsageStat& OtherStat)
		{
			TotalTimeSec += OtherStat.TotalTimeSec;
			GameThreadTimeSec += OtherStat.GameThreadTimeSec;
			SizeMB += OtherStat.SizeMB;
			AssetsBuilt += OtherStat.AssetsBuilt;
		}
		FString AssetType;
		double TotalTimeSec;
		double GameThreadTimeSec;
		double SizeMB;
		int64 AssetsBuilt;
	};

	/** Used for custom TSet comparison of DDC Resource usage stats. */
	struct FDDCResourceUsageStatKeyFuncs : BaseKeyFuncs<FDDCResourceUsageStat, FString, false>
	{
		static const FString& GetSetKey(const FDDCResourceUsageStat& Element) { return Element.AssetType; }
		static bool Matches(const FString& A, const FString& B) { return A == B; }
		static uint32 GetKeyHash(const FString& Key) { return GetTypeHash(Key); }
	};

	/** Used to store profile data for custom logging. */
	struct FCookProfileData
	{
	public:
		FCookProfileData(FString InPath, FString InKey, FString InValue) : Path(MoveTemp(InPath)), Key(MoveTemp(InKey)), Value(MoveTemp(InValue)) {}
		FString Path;
		FString Key;
		FString Value;
	};

	// instead of printing the usage stats generically, we capture them so we can log a subset of them in an easy-to-read way.
	TSet<FDDCResourceUsageStat, FDDCResourceUsageStatKeyFuncs> DDCResourceUsageStats;
	TArray<FCookStatsManager::StringKeyValue> DDCSummaryStats;
	TArray<FCookProfileData> CookProfileData;
	TArray<FString> StatCategories;
	TMap<FString, TArray<FCookStatsManager::StringKeyValue>> StatsInCategories;

	/** this functor will take a collected cooker stat and log it out using some custom formatting based on known stats that are collected.. */
	auto LogStatsFunc = [&DDCResourceUsageStats, &DDCSummaryStats, &CookProfileData, &StatCategories, &StatsInCategories]
	(const FString& StatName, const TArray<FCookStatsManager::StringKeyValue>& StatAttributes)
	{
		// Some stats will use custom formatting to make a visibly pleasing summary.
		bool bStatUsedCustomFormatting = false;

		if (StatName == TEXT("DDC.Usage"))
		{
			// Don't even log this detailed DDC data. It's mostly only consumable by ingestion into pivot tools.
			bStatUsedCustomFormatting = true;
		}
		else if (StatName.EndsWith(TEXT(".Usage"), ESearchCase::IgnoreCase))
		{
			// Anything that ends in .Usage is assumed to be an instance of FCookStats.FDDCResourceUsageStats. We'll log that using custom formatting.
			FString AssetType = StatName;
			AssetType.RemoveFromEnd(TEXT(".Usage"), ESearchCase::IgnoreCase);
			// See if the asset has a subtype (found via the "Node" parameter")
			const FCookStatsManager::StringKeyValue* AssetSubType = StatAttributes.FindByPredicate([](const FCookStatsManager::StringKeyValue& Item) { return Item.Key == TEXT("Node"); });
			if (AssetSubType && AssetSubType->Value.Len() > 0)
			{
				AssetType += FString::Printf(TEXT(" (%s)"), *AssetSubType->Value);
			}
			// Pull the Time and Size attributes and AddOrAccumulate them into the set of stats. Ugly string/container manipulation code courtesy of UE/C++.
			const FCookStatsManager::StringKeyValue* AssetTimeSecAttr = StatAttributes.FindByPredicate([](const FCookStatsManager::StringKeyValue& Item) { return Item.Key == TEXT("TimeSec"); });
			double AssetTimeSec = 0.0;
			if (AssetTimeSecAttr)
			{
				LexFromString(AssetTimeSec, *AssetTimeSecAttr->Value);
			}
			const FCookStatsManager::StringKeyValue* AssetSizeMBAttr = StatAttributes.FindByPredicate([](const FCookStatsManager::StringKeyValue& Item) { return Item.Key == TEXT("MB"); });
			double AssetSizeMB = 0.0;
			if (AssetSizeMBAttr)
			{
				LexFromString(AssetSizeMB, *AssetSizeMBAttr->Value);
			}
			const FCookStatsManager::StringKeyValue* ThreadNameAttr = StatAttributes.FindByPredicate([](const FCookStatsManager::StringKeyValue& Item) { return Item.Key == TEXT("ThreadName"); });
			bool bIsGameThreadTime = ThreadNameAttr != nullptr && ThreadNameAttr->Value == TEXT("GameThread");

			const FCookStatsManager::StringKeyValue* HitOrMissAttr = StatAttributes.FindByPredicate([](const FCookStatsManager::StringKeyValue& Item) { return Item.Key == TEXT("HitOrMiss"); });
			bool bWasMiss = HitOrMissAttr != nullptr && HitOrMissAttr->Value == TEXT("Miss");
			int64 AssetsBuilt = 0;
			if (bWasMiss)
			{
				const FCookStatsManager::StringKeyValue* CountAttr = StatAttributes.FindByPredicate([](const FCookStatsManager::StringKeyValue& Item) { return Item.Key == TEXT("Count"); });
				if (CountAttr)
				{
					LexFromString(AssetsBuilt, *CountAttr->Value);
				}
			}


			FDDCResourceUsageStat Stat(AssetType, AssetTimeSec, bIsGameThreadTime, AssetSizeMB, AssetsBuilt);
			FDDCResourceUsageStat* ExistingStat = DDCResourceUsageStats.Find(Stat.AssetType);
			if (ExistingStat)
			{
				ExistingStat->Accumulate(Stat);
			}
			else
			{
				DDCResourceUsageStats.Add(Stat);
			}
			bStatUsedCustomFormatting = true;
		}
		else if (StatName == TEXT("DDC.Summary"))
		{
			DDCSummaryStats.Append(StatAttributes);
			bStatUsedCustomFormatting = true;
		}
		else if (StatName == TEXT("Cook.Profile"))
		{
			if (StatAttributes.Num() >= 2)
			{
				CookProfileData.Emplace(StatAttributes[0].Value, StatAttributes[1].Key, StatAttributes[1].Value);
			}
			bStatUsedCustomFormatting = true;
		}

		// if a stat doesn't use custom formatting, just spit out the raw info.
		if (!bStatUsedCustomFormatting)
		{
			TArray<FCookStatsManager::StringKeyValue>& StatsInCategory = StatsInCategories.FindOrAdd(StatName);
			if (StatsInCategory.Num() == 0)
			{
				StatCategories.Add(StatName);
			}
			StatsInCategory.Append(StatAttributes);
		}
	};

	FCookStatsManager::LogCookStats(LogStatsFunc);

	UE_LOG(LogCook, Display, TEXT("Misc Cook Stats"));
	UE_LOG(LogCook, Display, TEXT("==============="));
	for (FString& StatCategory : StatCategories)
	{
		UE_LOG(LogCook, Display, TEXT("%s"), *StatCategory);
		TArray<FCookStatsManager::StringKeyValue>& StatsInCategory = StatsInCategories.FindOrAdd(StatCategory);

		// log each key/value pair, with the equal signs lined up.
		for (const FCookStatsManager::StringKeyValue& StatKeyValue : StatsInCategory)
		{
			UE_LOG(LogCook, Display, TEXT("    %s=%s"), *StatKeyValue.Key, *StatKeyValue.Value);
		}
	}

	// DDC Usage stats are custom formatted, and the above code just accumulated them into a TSet. Now log it with our special formatting for readability.
	if (CookProfileData.Num() > 0)
	{
		UE_LOG(LogCook, Display, TEXT(""));
		UE_LOG(LogCook, Display, TEXT("Cook Profile"));
		UE_LOG(LogCook, Display, TEXT("============"));
		for (const auto& ProfileEntry : CookProfileData)
		{
			UE_LOG(LogCook, Display, TEXT("%s.%s=%s"), *ProfileEntry.Path, *ProfileEntry.Key, *ProfileEntry.Value);
		}
	}
	if (DDCSummaryStats.Num() > 0)
	{
		UE_LOG(LogCook, Display, TEXT(""));
		UE_LOG(LogCook, Display, TEXT("DDC Summary Stats"));
		UE_LOG(LogCook, Display, TEXT("================="));
		for (const auto& Attr : DDCSummaryStats)
		{
			UE_LOG(LogCook, Display, TEXT("%-16s=%10s"), *Attr.Key, *Attr.Value);
		}
	}

	DumpDerivedDataBuildRemoteExecutorStats();

	if (DDCResourceUsageStats.Num() > 0)
	{
		// sort the list
		TArray<FDDCResourceUsageStat> SortedDDCResourceUsageStats;
		SortedDDCResourceUsageStats.Empty(DDCResourceUsageStats.Num());
		for (const FDDCResourceUsageStat& Stat : DDCResourceUsageStats)
		{
			SortedDDCResourceUsageStats.Emplace(Stat);
		}
		SortedDDCResourceUsageStats.Sort([](const FDDCResourceUsageStat& LHS, const FDDCResourceUsageStat& RHS)
			{
				return LHS.TotalTimeSec > RHS.TotalTimeSec;
			});

		UE_LOG(LogCook, Display, TEXT(""));
		UE_LOG(LogCook, Display, TEXT("DDC Resource Stats"));
		UE_LOG(LogCook, Display, TEXT("======================================================================================================="));
		UE_LOG(LogCook, Display, TEXT("Asset Type                          Total Time (Sec)  GameThread Time (Sec)  Assets Built  MB Processed"));
		UE_LOG(LogCook, Display, TEXT("----------------------------------  ----------------  ---------------------  ------------  ------------"));
		for (const FDDCResourceUsageStat& Stat : SortedDDCResourceUsageStats)
		{
			UE_LOG(LogCook, Display, TEXT("%-34s  %16.2f  %21.2f  %12d  %12.2f"), *Stat.AssetType, Stat.TotalTimeSec, Stat.GameThreadTimeSec, Stat.AssetsBuilt, Stat.SizeMB);
		}
	}

	DumpBuildDependencyTrackerStats();

	if (UE::Virtualization::IVirtualizationSystem::IsInitialized())
	{
		UE::Virtualization::IVirtualizationSystem::Get().DumpStats();
	}

	if (IsCookByTheBookMode(CookMode))
	{
		FStudioAnalytics::FireEvent_Loading(TEXT("CookByTheBook"), DetailedCookStats::CookWallTimeSec);
	}
}

}
#endif