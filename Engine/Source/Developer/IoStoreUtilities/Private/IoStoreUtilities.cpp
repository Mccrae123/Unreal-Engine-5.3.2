// Copyright Epic Games, Inc. All Rights Reserved.

#include "IoStoreUtilities.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFilemanager.h"
#include "Hash/CityHash.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Interfaces/ITargetPlatform.h"
#include "IO/IoDispatcher.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/Base64.h"
#include "Modules/ModuleManager.h"
#include "Serialization/Archive.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/BufferWriter.h"
#include "Serialization/LargeMemoryWriter.h"
#include "Serialization/AsyncLoading2.h"
#include "UObject/NameBatchSerialization.h"
#include "UObject/PackageFileSummary.h"
#include "UObject/ObjectResource.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Algo/Find.h"
#include "Misc/FileHelper.h"

//PRAGMA_DISABLE_OPTIMIZATION

IMPLEMENT_MODULE(FDefaultModuleImpl, IoStoreUtilities);

DEFINE_LOG_CATEGORY_STATIC(LogIoStore, Log, All);

#define OUTPUT_CHUNKID_DIRECTORY 0
#define OUTPUT_NAMEMAP_CSV 0
#define OUTPUT_IMPORTMAP_CSV 0
#define SKIP_WRITE_CONTAINER 0
#define SKIP_BULKDATA 0

struct FContainerTarget 
{
	const ITargetPlatform* TargetPlatform;
	const FString CookedDirectory;
	const FString CookedProjectDirectory;
	const FString OutputDirectory;
	const FString ChunkListFile;
};

class FNameMapBuilder
{
public:
	void MarkNameAsReferenced(const FName& Name)
	{
		const FNameEntryId Id = Name.GetComparisonIndex();
		int32& Index = NameIndices.FindOrAdd(Id);
		if (Index == 0)
		{
			Index = NameIndices.Num();
			NameMap.Add(Id);
		}
		// debug counts
		{
			const int32 Number = Name.GetNumber();
			TTuple<int32,int32,int32>& Counts = DebugNameCounts.FindOrAdd(Id);

			if (Number == 0)
			{
				++Counts.Get<0>();
			}
			else
			{
				++Counts.Get<1>();
				if (Number > Counts.Get<2>())
				{
					Counts.Get<2>() = Number;
				}
			}
		}
	}

	int32 MapName(const FName& Name) const
	{
		const FNameEntryId Id = Name.GetComparisonIndex();
		const int32* Index = NameIndices.Find(Id);
		check(Index);
		return Index ? *Index - 1 : INDEX_NONE;
	}

	void SerializeName(FArchive& A, const FName& N) const
	{
		int32 NameIndex = MapName(N);
		int32 NameNumber = N.GetNumber();
		A << NameIndex << NameNumber;
	}


	const TArray<FNameEntryId>& GetNameMap() const
	{
		return NameMap;
	}

#if OUTPUT_NAMEMAP_CSV
	void SaveCsv(const FString& CsvFilePath)
	{
		{
			TUniquePtr<FArchive> CsvArchive(IFileManager::Get().CreateFileWriter(*CsvFilePath));
			if (CsvArchive)
			{
				TCHAR Name[FName::StringBufferSize];
				ANSICHAR Line[MAX_SPRINTF + FName::StringBufferSize];
				ANSICHAR Header[] = "Length\tMaxNumber\tNumberCount\tBaseCount\tTotalCount\tFName\n";
				CsvArchive->Serialize(Header, sizeof(Header) - 1);
				for (auto& Counts : DebugNameCounts)
				{
					const int32 NameLen = FName::CreateFromDisplayId(Counts.Key, 0).ToString(Name);
					FCStringAnsi::Sprintf(Line, "%d\t%d\t%d\t%d\t%d\t",
						NameLen, Counts.Value.Get<2>(), Counts.Value.Get<1>(), Counts.Value.Get<0>(), Counts.Value.Get<0>() + Counts.Value.Get<1>());
					ANSICHAR* L = Line + FCStringAnsi::Strlen(Line);
					const TCHAR* N = Name;
					while (*N)
					{
						*L++ = CharCast<ANSICHAR,TCHAR>(*N++);
					}
					*L++ = '\n';
					CsvArchive.Get()->Serialize(Line, L - Line);
				}
			}
		}
	}
#endif

private:
	TMap<FNameEntryId, int32> NameIndices;
	TArray<FNameEntryId> NameMap;
	TMap<FNameEntryId, TTuple<int32,int32,int32>> DebugNameCounts; // <Number0Count,OtherNumberCount,MaxNumber>
};

#if OUTPUT_CHUNKID_DIRECTORY
class FChunkIdCsv
{
public:

	~FChunkIdCsv()
	{
		if (OutputArchive)
		{
			OutputArchive->Flush();
		}
	}

	void CreateOutputFile(const FString& RootPath)
	{
		const FString OutputFilename = RootPath / TEXT("chunkid_directory.csv");
		OutputArchive.Reset(IFileManager::Get().CreateFileWriter(*OutputFilename));
		if (OutputArchive)
		{
			const ANSICHAR* Output = "NameIndex,NameNumber,ChunkIndex,ChunkType,ChunkIdHash,DebugString\n";
			OutputArchive->Serialize((void*)Output, FPlatformString::Strlen(Output));
		}
	}

	void AddChunk(uint32 NameIndex, uint32 NameNumber, uint16 ChunkIndex, uint8 ChunkType, uint32 ChunkIdHash, const TCHAR* DebugString)
	{
		ANSICHAR Buffer[MAX_SPRINTF + 1] = { 0 };
		int32 NumOfCharacters = FCStringAnsi::Sprintf(Buffer, "%u,%u,%u,%u,%u,%s\n", NameIndex, NameNumber, ChunkIndex, ChunkType, ChunkIdHash, TCHAR_TO_ANSI(DebugString));
		OutputArchive->Serialize(Buffer, NumOfCharacters);
	}	

private:
	TUniquePtr<FArchive> OutputArchive;
};
FChunkIdCsv ChunkIdCsv;

#endif

static FIoChunkId CreateChunkId(int32 GlobalPackageId, uint16 ChunkIndex, EIoChunkType ChunkType, const TCHAR* DebugString)
{
	FIoChunkId ChunkId = CreateIoChunkId(GlobalPackageId, ChunkIndex, ChunkType);
#if OUTPUT_CHUNKID_DIRECTORY
	ChunkIdCsv.AddChunk(GlobalPackageId, 0, ChunkIndex, (uint8)ChunkType, GetTypeHash(ChunkId), DebugString);
#endif
	return ChunkId;
}

static FIoChunkId CreateChunkIdForBulkData(int32 GlobalPackageId, uint64 BulkdataOffset, EIoChunkType ChunkType, const TCHAR* DebugString)
{
	FIoChunkId ChunkId = CreateBulkdataChunkId(GlobalPackageId, BulkdataOffset, ChunkType);
#if OUTPUT_CHUNKID_DIRECTORY
	ChunkIdCsv.AddChunk(GlobalPackageId, 0, 0, (uint8)ChunkType, GetTypeHash(ChunkId), DebugString);
#endif
	return ChunkId;
}

enum EPreloadDependencyType
{
	PreloadDependencyType_Create,
	PreloadDependencyType_Serialize,
};

struct FArc
{
	uint32 FromNodeIndex;
	uint32 ToNodeIndex;

	bool operator==(const FArc& Other) const
	{
		return ToNodeIndex == Other.ToNodeIndex && FromNodeIndex == Other.FromNodeIndex;
	}
};

struct FExportGraphNode;

struct FExportBundle
{
	TArray<FExportGraphNode*> Nodes;
	uint32 LoadOrder;
};

struct FPackage;

struct FPackageGraphNode
{
	FPackage* Package = nullptr;
	bool bTemporaryMark = false;
	bool bPermanentMark = false;
};

class FPackageGraph
{
public:
	FPackageGraph()
	{

	}

	~FPackageGraph()
	{
		for (FPackageGraphNode* Node : Nodes)
		{
			delete Node;
		}
	}

	FPackageGraphNode* AddNode(FPackage* Package)
	{
		FPackageGraphNode* Node = new FPackageGraphNode();
		Node->Package = Package;
		Nodes.Add(Node);
		return Node;
	}

	void AddImportDependency(FPackageGraphNode* FromNode, FPackageGraphNode* ToNode)
	{
		Edges.Add(FromNode, ToNode);
	}

	TArray<FPackage*> TopologicalSort() const;

private:
	TArray<FPackageGraphNode*> Nodes;
	TMultiMap<FPackageGraphNode*, FPackageGraphNode*> Edges;
};

struct FExportGraphNode
{
	FPackage* Package;
	FExportBundleEntry BundleEntry;
	TSet<FExportGraphNode*> ExternalDependencies;
	TSet<uint32> ScriptDependencies;
	uint64 NodeIndex;
};

class FExportGraph
{
public:
	FExportGraph()
	{

	}

	~FExportGraph()
	{
		for (FExportGraphNode* Node : Nodes)
		{
			delete Node;
		}
	}

	FExportGraphNode* AddNode(FPackage* Package, const FExportBundleEntry& BundleEntry)
	{
		FExportGraphNode* Node = new FExportGraphNode();
		Node->Package = Package;
		Node->BundleEntry = BundleEntry;
		Node->NodeIndex = Nodes.Num();
		Nodes.Add(Node);
		return Node;
	}

	void AddInternalDependency(FExportGraphNode* FromNode, FExportGraphNode* ToNode)
	{
		AddEdge(FromNode, ToNode);
	}

	void AddExternalDependency(FExportGraphNode* FromNode, FExportGraphNode* ToNode)
	{
		AddEdge(FromNode, ToNode);
		ToNode->ExternalDependencies.Add(FromNode);
	}

	TArray<FExportGraphNode*> ComputeLoadOrder(const TArray<FPackage*>& Packages) const;

private:
	void AddEdge(FExportGraphNode* FromNode, FExportGraphNode* ToNode)
	{
		Edges.Add(FromNode, ToNode);
	}

	TArray<FExportGraphNode*> TopologicalSort() const;

	TArray<FExportGraphNode*> Nodes;
	TMultiMap<FExportGraphNode*, FExportGraphNode*> Edges;
};

struct FPackage
{
	FName Name;
	FString FileName;
	FString RelativeFileName;
	int32 GlobalPackageId = 0;
	uint32 PackageFlags = 0;
	int32 NameCount = 0;
	int32 ImportCount = 0;
	int32 ImportOffset = 0;
	int32 ExportCount = 0;
	int32 FirstGlobalImport = -1;
	int32 GlobalImportCount = -1;
	int32 ExportIndexOffset = -1;
	int32 PreloadIndexOffset = -1;
	int32 FirstExportBundleMetaEntry = -1;
	int64 BulkDataStartOffset = -1;
	int64 UExpSize = 0;
	int64 UAssetSize = 0;
	int64 SummarySize = 0;
	int64 UGraphSize = 0;
	int64 NameMapSize = 0;
	int64 ImportMapSize = 0;
	int64 ExportMapSize = 0;
	int64 ExportBundlesSize = 0;

	bool bHasCircularImportDependencies = false;

	TArray<FString> ImportedFullNames;

	TArray<FPackage*> ImportedPackages;
	TSet<FPackage*> AllReachablePackages;
	TSet<FPackage*> ImportedPreloadPackages;

	TArray<FNameEntryId> NameMap;
	TArray<int32> NameIndices;

	TArray<int32> Imports;
	TArray<int32> Exports;
	TArray<FArc> InternalArcs;
	TMap<FPackage*, TArray<FArc>> ExternalArcs;
	TArray<FArc> ScriptArcs;
	
	TArray<FExportBundle> ExportBundles;
	TMap<FExportGraphNode*, uint32> ExportBundleMap;

	TArray<FExportGraphNode*> CreateExportNodes;
	TArray<FExportGraphNode*> SerializeExportNodes;

	TArray<FExportGraphNode*> NodesWithNoIncomingEdges;
	FPackageGraphNode* Node = nullptr;
};

struct FCircularImportChain
{
	TArray<FName> SortedNames;
	TArray<FPackage*> Packages;
	uint32 Hash = 0;

	FCircularImportChain()
	{
		Packages.Reserve(128);
	}

	void Add(FPackage* Package)
	{
		Packages.Add(Package);
	}

	void Pop()
	{
		Packages.Pop();
	}

	int32 Num()
	{
		return Packages.Num();
	}

	void SortAndGenerateHash()
	{
		SortedNames.Empty(Packages.Num());
		for (FPackage* Package : Packages)
		{
			SortedNames.Emplace(Package->Name);
		}
		SortedNames.Sort(FNameLexicalLess());
		Hash = CityHash32((char*)SortedNames.GetData(), SortedNames.Num() * SortedNames.GetTypeSize());
	}

	FString ToString()
	{
		FString Result = FString::Printf(TEXT("%d:%u: "), SortedNames.Num(), Hash);
		for (const FName& Name : SortedNames)
		{
			Result.Append(Name.ToString());
			Result.Append(TEXT(" -> "));
		}
		Result.Append(SortedNames[0].ToString());
		return Result;
	}

	bool operator==(const FCircularImportChain& Other) const
	{
		return Hash == Other.Hash && SortedNames == Other.SortedNames;
	}

	friend FORCEINLINE uint32 GetTypeHash(const FCircularImportChain& In)
	{
		return In.Hash;
	}
};

struct FInstallChunk
{
	FString Name;
	int32 ChunkId;
	TArray<FPackage*> Packages;
};

TArray<FPackage*> FPackageGraph::TopologicalSort() const
{
	TMultiMap<FPackageGraphNode*, FPackageGraphNode*> EdgesCopy = Edges;
	TArray<FPackage*> Result;
	Result.Reserve(Nodes.Num());
	
	TSet<FPackageGraphNode*> UnmarkedNodes;
	UnmarkedNodes.Append(Nodes);

	struct
	{
		void Visit(FPackageGraphNode* Node)
		{
			if (Node->bPermanentMark)
			{
				return;
			}
			if (Node->bTemporaryMark)
			{
				return;
			}
			Node->bTemporaryMark = true;
			for (auto EdgeIt = Edges.CreateKeyIterator(Node); EdgeIt; ++EdgeIt)
			{
				FPackageGraphNode* ToNode = EdgeIt.Value();
				Visit(ToNode);
			}
			Node->bTemporaryMark = false;
			Node->bPermanentMark = true;
			UnmarkedNodes.Remove(Node);
			Result.Insert(Node->Package, 0);
		}

		TSet<FPackageGraphNode*>& UnmarkedNodes;
		TMultiMap<FPackageGraphNode*, FPackageGraphNode*>& Edges;
		TArray<FPackage*>& Result;

	} Visitor{ UnmarkedNodes, EdgesCopy, Result };

	while (Result.Num() < Nodes.Num())
	{
		auto It = UnmarkedNodes.CreateIterator();
		FPackageGraphNode* UnmarkedNode = *It;
		It.RemoveCurrent();
		Visitor.Visit(UnmarkedNode);
	}

	return Result;
}

TArray<FExportGraphNode*> FExportGraph::ComputeLoadOrder(const TArray<FPackage*>& Packages) const
{
	FPackageGraph PackageGraph;
	for (FPackage* Package : Packages)
	{
		Package->Node = PackageGraph.AddNode(Package);
	}
	for (FPackage* Package : Packages)
	{
		for (FPackage* ImportedPackage : Package->ImportedPackages)
		{
			PackageGraph.AddImportDependency(ImportedPackage->Node, Package->Node);
		}
	}

	TArray<FPackage*> SortedPackages = PackageGraph.TopologicalSort();
	
	int32 NodeCount = Nodes.Num();
	TArray<uint32> NodesIncomingEdgeCount;
	NodesIncomingEdgeCount.AddZeroed(NodeCount);
	TMultiMap<FExportGraphNode*, FExportGraphNode*> EdgesCopy = Edges;
	for (auto& KV : EdgesCopy)
	{
		FExportGraphNode* ToNode = KV.Value;
		++NodesIncomingEdgeCount[ToNode->NodeIndex];
	}

	TArray<FExportGraphNode*> LoadOrder;
	LoadOrder.Reserve(NodeCount);
	
	for (int32 NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
	{
		if (NodesIncomingEdgeCount[NodeIndex] == 0)
		{
			FExportGraphNode* Node = Nodes[NodeIndex];
			Node->Package->NodesWithNoIncomingEdges.Push(Node);
		}
	}
	while (LoadOrder.Num() < NodeCount)
	{
		for (FPackage* Package : SortedPackages)
		{
			while (Package->NodesWithNoIncomingEdges.Num() > 0)
			{
				FExportGraphNode* RemovedNode = Package->NodesWithNoIncomingEdges.Pop();
				LoadOrder.Add(RemovedNode);
				for (auto EdgeIt = EdgesCopy.CreateKeyIterator(RemovedNode); EdgeIt; ++EdgeIt)
				{
					FExportGraphNode* ToNode = EdgeIt.Value();
					if (--NodesIncomingEdgeCount[ToNode->NodeIndex] == 0)
					{
						ToNode->Package->NodesWithNoIncomingEdges.Push(ToNode);
					}
					EdgeIt.RemoveCurrent();
				}
			}
		}
	}

	return LoadOrder;
}

static void AddInternalExportArc(FExportGraph& ExportGraph, FPackage& Package, uint32 FromExportIndex, EPreloadDependencyType FromPhase, uint32 ToExportIndex, EPreloadDependencyType ToPhase)
{
	FExportGraphNode* FromNode = FromPhase == PreloadDependencyType_Create ? Package.CreateExportNodes[FromExportIndex] : Package.SerializeExportNodes[FromExportIndex];
	FExportGraphNode* ToNode = ToPhase == PreloadDependencyType_Create ? Package.CreateExportNodes[ToExportIndex] : Package.SerializeExportNodes[ToExportIndex];
	ExportGraph.AddInternalDependency(FromNode, ToNode);
}

static void AddExternalExportArc(FExportGraph& ExportGraph, FPackage& FromPackage, uint32 FromExportIndex, EPreloadDependencyType FromPhase, FPackage& ToPackage, uint32 ToExportIndex, EPreloadDependencyType ToPhase)
{
	FExportGraphNode* FromNode = FromPhase == PreloadDependencyType_Create ? FromPackage.CreateExportNodes[FromExportIndex] : FromPackage.SerializeExportNodes[FromExportIndex];
	FExportGraphNode* ToNode = ToPhase == PreloadDependencyType_Create ? ToPackage.CreateExportNodes[ToExportIndex] : ToPackage.SerializeExportNodes[ToExportIndex];
	ExportGraph.AddExternalDependency(FromNode, ToNode);
}

static void AddScriptArc(FPackage& Package, uint32 GlobalImportIndex, uint32 ExportIndex, EPreloadDependencyType Phase)
{
	FExportGraphNode* Node = Phase == PreloadDependencyType_Create ? Package.CreateExportNodes[ExportIndex] : Package.SerializeExportNodes[ExportIndex];
	Node->ScriptDependencies.Add(GlobalImportIndex);
}

static void AddPostLoadArc(FPackage& FromPackage, FPackage& ToPackage)
{
	TArray<FArc>& ExternalArcs = ToPackage.ExternalArcs.FindOrAdd(&FromPackage);
	check(!ExternalArcs.Contains(FArc({EEventLoadNode2::Package_ExportsSerialized, EEventLoadNode2::Package_PostLoad})));
	check(!ExternalArcs.Contains(FArc({EEventLoadNode2::Package_PostLoad, EEventLoadNode2::Package_PostLoad})));
	ExternalArcs.Add({ EEventLoadNode2::Package_PostLoad, EEventLoadNode2::Package_PostLoad });
}

static void AddExportsDoneArc(FPackage& FromPackage, FPackage& ToPackage)
{
	TArray<FArc>& ExternalArcs = ToPackage.ExternalArcs.FindOrAdd(&FromPackage);
	check(!ExternalArcs.Contains(FArc({EEventLoadNode2::Package_ExportsSerialized, EEventLoadNode2::Package_PostLoad})));
	check(!ExternalArcs.Contains(FArc({EEventLoadNode2::Package_PostLoad, EEventLoadNode2::Package_PostLoad})));
	ExternalArcs.Add({ EEventLoadNode2::Package_ExportsSerialized, EEventLoadNode2::Package_PostLoad });
}

static void AddUniqueExternalBundleArc(FPackage& FromPackage, uint32 FromBundleIndex, FPackage& ToPackage, uint32 ToBundleIndex)
{
	uint32 FromNodeIndex = EEventLoadNode2::Package_NumPhases + FromBundleIndex * EEventLoadNode2::ExportBundle_NumPhases + EEventLoadNode2::ExportBundle_Process;
	uint32 ToNodeIndex = EEventLoadNode2::Package_NumPhases + ToBundleIndex * EEventLoadNode2::ExportBundle_NumPhases + EEventLoadNode2::ExportBundle_Process;
	TArray<FArc>& ExternalArcs = ToPackage.ExternalArcs.FindOrAdd(&FromPackage);
	ExternalArcs.AddUnique({ FromNodeIndex, ToNodeIndex });
}

static void AddUniqueScriptBundleArc(FPackage& Package, uint32 GlobalImportIndex, uint32 BundleIndex)
{
	uint32 NodeIndex = EEventLoadNode2::Package_NumPhases + BundleIndex * EEventLoadNode2::ExportBundle_NumPhases + EEventLoadNode2::ExportBundle_Process;
	Package.ScriptArcs.AddUnique({ GlobalImportIndex, NodeIndex });
}

static void AddReachablePackagesRecursive(FPackage& Package, FPackage& PackageWithImports, TSet<FPackage*>& Visited, bool bFirst)
{
	if (!bFirst)
	{
		bool bIsVisited = false;
		Visited.Add(&PackageWithImports, &bIsVisited);
		if (bIsVisited)
		{
			return;
		}

		if (&PackageWithImports == &Package)
		{
			return;
		}
	}

	if (PackageWithImports.AllReachablePackages.Num() > 0)
	{
		Visited.Append(PackageWithImports.AllReachablePackages);
		
	}
	else
	{
		for (FPackage* ImportedPackage : PackageWithImports.ImportedPackages)
		{
			AddReachablePackagesRecursive(Package, *ImportedPackage, Visited, false);
		}
	}
}

static bool FindNewCircularImportChains(
	FPackage& Package,
	FPackage& ImportedPackage,
	TSet<FPackage*>& Visited,
	TSet<FCircularImportChain>& CircularChains,
	FCircularImportChain& CurrentChain)
{
	if (&ImportedPackage == &Package)
	{
		Package.bHasCircularImportDependencies = true;
		CurrentChain.SortAndGenerateHash();
		bool bAlreadyFound = true;
		CircularChains.AddByHash(CurrentChain.Hash, CurrentChain, &bAlreadyFound);

		if (bAlreadyFound)
		{
			// UE_LOG(LogIoStore, Display, TEXT("OLD-IsCircular: %s with %s"), *Package.Name.ToString(), *CurrentChain.ToString());
			return false;
		}
		else
		{
			// UE_LOG(LogIoStore, Display, TEXT("NEW-IsCircular: %s with %s"), *Package.Name.ToString(), *CurrentChain.ToString());
			return true;
		}
	}

	bool bIsVisited = false;
	Visited.Add(&ImportedPackage, &bIsVisited);
	if (bIsVisited)
	{
		return false;
	}

	bool bFoundNew = false;
	for (FPackage* DependentPackage : ImportedPackage.ImportedPackages)
	{
		CurrentChain.Add(DependentPackage);
		bFoundNew |= FindNewCircularImportChains(Package, *DependentPackage, Visited, CircularChains, CurrentChain);
		CurrentChain.Pop();
	}

	return bFoundNew;
}

static void AddPostLoadDependencies(
	FPackage& Package,
	TSet<FPackage*>& Visited,
	TSet<FCircularImportChain>& CircularChains)
{
	TSet<FPackage*> DependentPackages;

	for (FPackage* ImportedPackage : Package.ImportedPackages)
	{
		Visited.Reset();
		FCircularImportChain CurrentChain;
		CurrentChain.Add(ImportedPackage);
		if (FindNewCircularImportChains(Package, *ImportedPackage, Visited, CircularChains, CurrentChain))
		{
			DependentPackages.Append(MoveTemp(Visited));
		}
	}

	if (Package.bHasCircularImportDependencies)
	{
		for (FPackage* ImportedPackage : Package.ImportedPackages)
		{
			if (!DependentPackages.Contains(ImportedPackage))
			{
				AddPostLoadArc(*ImportedPackage, Package);
			}
		}

		DependentPackages.Remove(&Package);
		for (FPackage* DependentPackage : DependentPackages)
		{
			AddExportsDoneArc(*DependentPackage, Package);
		}
	}

	/*
	if (Package.bHasCircularImportDependencies)
	{
		int32 diff = Package.AllReachablePackages.Num() - 1 - DependentPackages.Num();
		if (DependentPackages.Num() == 0)
		{
			UE_LOG(LogIoStore, Display, TEXT("OPT-ALL: %s: Skipping %d/%d arcs"), *Package.Name.ToString(),
				diff,
				Package.AllReachablePackages.Num() - 1);
		}
		else if (diff > 0)
		{
			UE_LOG(LogIoStore, Display, TEXT("OPT: %s: Skipping %d/%d arcs"), *Package.Name.ToString(),
				diff,
				Package.AllReachablePackages.Num() - 1);
		}
		else
		{
			UE_LOG(LogIoStore, Display, TEXT("NOP: %s: Skipping %d/%d arcs"), *Package.Name.ToString(),
				0,
				Package.AllReachablePackages.Num() - 1);
		}
	}
	*/
}

static void BuildBundles(FExportGraph& ExportGraph, const TArray<FPackage*>& Packages)
{
	TArray<FExportGraphNode*> ExportLoadOrder = ExportGraph.ComputeLoadOrder(Packages);
	FPackage* LastPackage = nullptr;
	uint32 BundleLoadOrder = 0;
	for (FExportGraphNode* Node : ExportLoadOrder)
	{
		FPackage* Package = Node->Package;
		check(Package);
		if (!Package)
		{
			continue;
		}

		uint32 BundleIndex;
		FExportBundle* Bundle;
		if (Package != LastPackage)
		{
			BundleIndex = Package->ExportBundles.Num();
			Bundle = &Package->ExportBundles.AddDefaulted_GetRef();
			Bundle->LoadOrder = BundleLoadOrder++;
			LastPackage = Package;
		}
		else
		{
			BundleIndex = Package->ExportBundles.Num() - 1;
			Bundle = &Package->ExportBundles[BundleIndex];
		}
		for (FExportGraphNode* ExternalDependency : Node->ExternalDependencies)
		{
			uint32* FindDependentBundleIndex = ExternalDependency->Package->ExportBundleMap.Find(ExternalDependency);
			check(FindDependentBundleIndex);
			if (BundleIndex > 0)
			{
				AddUniqueExternalBundleArc(*ExternalDependency->Package, *FindDependentBundleIndex, *Package, BundleIndex);
			}
		}
		for (uint32 ScriptDependencyGlobalImportIndex : Node->ScriptDependencies)
		{
			AddUniqueScriptBundleArc(*Package, ScriptDependencyGlobalImportIndex, BundleIndex);
		}
		Bundle->Nodes.Add(Node);
		Package->ExportBundleMap.Add(Node, BundleIndex);
	}
}

static bool WriteBulkData(	const FString& Filename, EIoChunkType Type, const FPackage& Package, const FPackageStoreBulkDataManifest& BulkDataManifest,
							FIoStoreWriter* IoStoreWriter)
{

	const FPackageStoreBulkDataManifest::PackageDesc* PackageDesc = BulkDataManifest.Find(Package.FileName);
	if (PackageDesc != nullptr)
	{
		const FIoChunkId BulkDataChunkId = CreateChunkIdForBulkData(Package.GlobalPackageId, TNumericLimits<uint64>::Max()-1, Type, *Package.FileName);

#if !SKIP_WRITE_CONTAINER		
		FIoBuffer IoBuffer;

		TUniquePtr<FArchive> BulkAr(IFileManager::Get().CreateFileReader(*Filename));
		if (BulkAr != nullptr)
		{
			uint8* BulkBuffer = static_cast<uint8*>(FMemory::Malloc(BulkAr->TotalSize()));
			BulkAr->Serialize(BulkBuffer, BulkAr->TotalSize());
			IoBuffer = FIoBuffer(FIoBuffer::AssumeOwnership, BulkBuffer, BulkAr->TotalSize());

			BulkAr->Close();
		}
		
		const FIoStatus AppendResult = IoStoreWriter->Append(BulkDataChunkId, IoBuffer);
		if (!AppendResult.IsOk())
		{
			UE_LOG(LogIoStore, Error, TEXT("Failed to append bulkdata for '%s' due to: %s"), *Package.FileName, *AppendResult.ToString());
			return false;
		}
#endif

		// Create additional mapping chunks as needed
		for (const FPackageStoreBulkDataManifest::PackageDesc::BulkDataDesc& BulkDataDesc : PackageDesc->GetDataArray())
		{
			if (BulkDataDesc.Type == Type)
			{
				const FIoChunkId AccessChunkId = CreateChunkIdForBulkData(Package.GlobalPackageId, BulkDataDesc.ChunkId, Type, *Package.FileName);
#if !SKIP_WRITE_CONTAINER	
				const FIoStatus PartialResult = IoStoreWriter->MapPartialRange(BulkDataChunkId, BulkDataDesc.Offset, BulkDataDesc.Size, AccessChunkId);
				if (!PartialResult.IsOk())
				{
					UE_LOG(LogIoStore, Warning, TEXT("Failed to map partial range for '%s' due to: %s"), *Package.FileName, *PartialResult.ToString());
				}
#endif
			}
		}
	}
	else if(IFileManager::Get().FileExists(*Filename))
	{
		UE_LOG(LogIoStore, Error, TEXT("Unable to find an entry in the bulkdata manifest for '%s' the file might be out of date!"), *Package.FileName);
		return false;
	}

	return true;
}

struct FImportData
{
	int32 GlobalIndex = -1;
	int32 OuterIndex = -1;
	int32 OutermostIndex = -1;
	int32 GlobalExportIndex = -1;
	int32 RefCount = 0;
	FName ObjectName;
	bool bIsPackage = false;
	bool bIsScript = false;
	FString FullName;
	FPackage* Package = nullptr;

	bool operator<(const FImportData& Other) const
	{
		if (bIsScript != Other.bIsScript)
		{
			return bIsScript;
		}
		if (OutermostIndex != Other.OutermostIndex)
		{
			return OutermostIndex < Other.OutermostIndex;
		}
		if (bIsPackage != Other.bIsPackage)
		{
			return bIsPackage;
		}
		return FullName < Other.FullName;
	}
};

struct FExportData
{
	int32 GlobalIndex = -1;
	FName SourcePackageName;
	FName ObjectName;
	int32 SourceIndex = -1;
	int32 GlobalImportIndex = -1;
	FPackageIndex OuterIndex;
	FPackageIndex ClassIndex;
	FPackageIndex SuperIndex;
	FPackageIndex TemplateIndex;
	FString FullName;

	FExportGraphNode* CreateNode = nullptr;
	FExportGraphNode* SerializeNode = nullptr;
};

static void FindImport(TArray<FImportData>& GlobalImports, TMap<FString, int32>& GlobalImportsByFullName, TArray<FString>& TempFullNames, FObjectImport* ImportMap, int32 LocalImportIndex)
{
	FObjectImport* Import = &ImportMap[LocalImportIndex];
	FString& FullName = TempFullNames[LocalImportIndex];
	if (FullName.Len() == 0)
	{
		if (Import->OuterIndex.IsNull())
		{
			Import->ObjectName.AppendString(FullName);
			int32* FindGlobalImport = GlobalImportsByFullName.Find(FullName);
			if (!FindGlobalImport)
			{
				// first time, assign global index for this root package
				int32 GlobalImportIndex = GlobalImports.Num();
				GlobalImportsByFullName.Add(FullName, GlobalImportIndex);
				FImportData& GlobalImport = GlobalImports.AddDefaulted_GetRef();
				GlobalImport.GlobalIndex = GlobalImportIndex;
				GlobalImport.OutermostIndex = GlobalImportIndex;
				GlobalImport.OuterIndex = -1;
				GlobalImport.ObjectName = Import->ObjectName;
				GlobalImport.bIsPackage = true;
				GlobalImport.bIsScript = FullName.StartsWith(TEXT("/Script/"));
				GlobalImport.FullName = FullName;
				GlobalImport.RefCount = 1;
			}
			else
			{
				++GlobalImports[*FindGlobalImport].RefCount;
			}
		}
		else
		{
			int32 LocalOuterIndex = Import->OuterIndex.ToImport();
			FindImport(GlobalImports, GlobalImportsByFullName, TempFullNames, ImportMap, LocalOuterIndex);
			FString& OuterName = TempFullNames[LocalOuterIndex];
			ensure(OuterName.Len() > 0);

			FullName.Append(OuterName);
			FullName.AppendChar(TEXT('/'));
			Import->ObjectName.AppendString(FullName);

			int32* FindGlobalImport = GlobalImportsByFullName.Find(FullName);
			if (!FindGlobalImport)
			{
				// first time, assign global index for this intermediate import
				int32 GlobalImportIndex = GlobalImports.Num();
				GlobalImportsByFullName.Add(FullName, GlobalImportIndex);
				FImportData& GlobalImport = GlobalImports.AddDefaulted_GetRef();
				int32* FindOuterGlobalImport = GlobalImportsByFullName.Find(OuterName);
				check(FindOuterGlobalImport);
				const FImportData& OuterGlobalImport = GlobalImports[*FindOuterGlobalImport];
				GlobalImport.GlobalIndex = GlobalImportIndex;
				GlobalImport.OutermostIndex = OuterGlobalImport.OutermostIndex;
				GlobalImport.OuterIndex = OuterGlobalImport.GlobalIndex;
				GlobalImport.ObjectName = Import->ObjectName;
				GlobalImport.bIsScript = OuterGlobalImport.bIsScript;
				GlobalImport.FullName = FullName;
				GlobalImport.RefCount = 1;
			}
			else
			{
				++GlobalImports[*FindGlobalImport].RefCount;
			}
		}
	}
};

static void FindExport(TArray<FExportData>& GlobalExports, TMap<FString, int32>& GlobalExportsByFullName, TArray<FString>& TempFullNames, const FObjectExport* ExportMap, int32 LocalExportIndex, const FName& PackageName)
{
	const FObjectExport* Export = ExportMap + LocalExportIndex;
	FString& FullName = TempFullNames[LocalExportIndex];

	if (FullName.Len() == 0)
	{
		if (Export->OuterIndex.IsNull())
		{
			PackageName.AppendString(FullName);
			FullName.AppendChar(TEXT('/'));
			Export->ObjectName.AppendString(FullName);
		}
		else
		{
			check(Export->OuterIndex.IsExport());

			FindExport(GlobalExports, GlobalExportsByFullName, TempFullNames, ExportMap, Export->OuterIndex.ToExport(), PackageName);
			FString& OuterName = TempFullNames[Export->OuterIndex.ToExport()];
			check(OuterName.Len() > 0);

			FullName.Append(OuterName);
			FullName.AppendChar(TEXT('/'));
			Export->ObjectName.AppendString(FullName);
		}
		check(!GlobalExportsByFullName.Contains(FullName));
		int32 GlobalExportIndex = GlobalExports.Num();
		GlobalExportsByFullName.Add(FullName, GlobalExportIndex);
		FExportData& ExportData = GlobalExports.AddDefaulted_GetRef();
		ExportData.GlobalIndex = GlobalExportIndex;
		ExportData.SourcePackageName = PackageName;
		ExportData.ObjectName = Export->ObjectName;
		ExportData.SourceIndex = LocalExportIndex;
		ExportData.FullName = FullName;
	}
};

FPackage* AddPackage(const TCHAR* FileName, const TCHAR* CookedDir, TArray<FPackage*>& Packages, TMap<FName, FPackage*>& PackageMap)
{
	FString RelativeFileName = FileName;
	RelativeFileName.RemoveFromStart(CookedDir);
	RelativeFileName.RemoveFromStart(TEXT("/"));
	RelativeFileName = TEXT("../../../") / RelativeFileName;

	FString PackageName;
	FString ErrorMessage;
	if (!FPackageName::TryConvertFilenameToLongPackageName(RelativeFileName, PackageName, &ErrorMessage))
	{
		UE_LOG(LogIoStore, Warning, TEXT("Failed to convert file name from file name '%s'"), *ErrorMessage);
		return nullptr;
	}

	FName PackageFName = *PackageName;

	FPackage* Package = PackageMap.FindRef(PackageFName);
	if (Package)
	{
		UE_LOG(LogIoStore, Warning, TEXT("Package in multiple pakchunks: '%s'"), *PackageFName.ToString());
	}
	else
	{
		Package = new FPackage();
		Package->Name = PackageFName;
		Package->FileName = FileName;
		Package->RelativeFileName = MoveTemp(RelativeFileName);
		Package->GlobalPackageId = Packages.Num();
		Packages.Add(Package);
		PackageMap.Add(PackageFName, Package);
	}

	return Package;
}

void SerializePackageData(
	FIoStoreWriter* IoStoreWriter,
	int32 TotalPackages,
	const TArray<FPackage*>& Packages,
	const FNameMapBuilder& NameMapBuilder,
	const TArray<FObjectExport>& ObjectExports,
	const TArray<FExportData>& GlobalExports,
	const TMap<FString, int32>& GlobalImportsByFullName,
	FExportBundleMetaEntry* ExportBundleMetaEntries,
	const FPackageStoreBulkDataManifest& BulkDataManifest,
	bool bWithBulkDataManifest)
{
	for (FPackage* Package : Packages)
	{
		UE_CLOG(Package->GlobalPackageId % 1000 == 0, LogIoStore, Display, TEXT("Serializing %d/%d: '%s'"), Package->GlobalPackageId, TotalPackages, *Package->Name.ToString());

		// Temporary Archive for serializing ImportMap
		FBufferWriter ImportMapArchive(nullptr, 0, EBufferWriterFlags::AllowResize | EBufferWriterFlags::TakeOwnership);
		for (int32 GlobalImportIndex : Package->Imports)
		{
			ImportMapArchive << GlobalImportIndex;
		}
		Package->ImportMapSize = ImportMapArchive.Tell();

		// Temporary Archive for serializing EDL graph data
		FBufferWriter GraphArchive(nullptr, 0, EBufferWriterFlags::AllowResize | EBufferWriterFlags::TakeOwnership);

		int32 InternalArcCount = Package->InternalArcs.Num();
		GraphArchive << InternalArcCount;
		for (FArc& InternalArc : Package->InternalArcs)
		{
			GraphArchive << InternalArc.FromNodeIndex;
			GraphArchive << InternalArc.ToNodeIndex;
		}

		int32 ReferencedPackagesCount = Package->ExternalArcs.Num();
		GraphArchive << ReferencedPackagesCount;
		for (auto& KV : Package->ExternalArcs)
		{
			FPackage* ImportedPackage = KV.Key;
			TArray<FArc>& Arcs = KV.Value;
			int32 ExternalArcCount = Arcs.Num();

			GraphArchive << ImportedPackage->GlobalPackageId;
			GraphArchive << ExternalArcCount;
			for (FArc& ExternalArc : Arcs)
			{
				GraphArchive << ExternalArc.FromNodeIndex;
				GraphArchive << ExternalArc.ToNodeIndex;
			}
		}
		Package->UGraphSize = GraphArchive.Tell();

		// Temporary Archive for serializing export map data
		FBufferWriter ExportMapArchive(nullptr, 0, EBufferWriterFlags::AllowResize | EBufferWriterFlags::TakeOwnership);
		for (int32 I = 0; I < Package->ExportCount; ++I)
		{
			const FObjectExport& ObjectExport = ObjectExports[Package->ExportIndexOffset + I];
			const FExportData& ExportData = GlobalExports[Package->Exports[I]];

			int64 SerialSize = ObjectExport.SerialSize;
			ExportMapArchive << SerialSize;
			NameMapBuilder.SerializeName(ExportMapArchive, ObjectExport.ObjectName);
			FPackageIndex OuterIndex = ExportData.OuterIndex;
			ExportMapArchive << OuterIndex;
			FPackageIndex ClassIndex = ExportData.ClassIndex;
			ExportMapArchive << ClassIndex;
			FPackageIndex SuperIndex = ExportData.SuperIndex;
			ExportMapArchive << SuperIndex;
			FPackageIndex TemplateIndex = ExportData.TemplateIndex;
			ExportMapArchive << TemplateIndex;
			int32 GlobalImportIndex = ExportData.GlobalImportIndex;
			ExportMapArchive << GlobalImportIndex;
			uint32 ObjectFlags = ObjectExport.ObjectFlags;
			ExportMapArchive << ObjectFlags;
			uint8 FilterFlags = uint8(EExportFilterFlags::None);
			if (ObjectExport.bNotForClient)
			{
				FilterFlags = uint8(EExportFilterFlags::NotForClient);
			}
			else if (ObjectExport.bNotForServer)
			{
				FilterFlags = uint8(EExportFilterFlags::NotForServer);
			}
			ExportMapArchive << FilterFlags;
			uint8 Pad = 0;
			ExportMapArchive.Serialize(&Pad, 7);
		}
		Package->ExportMapSize = ExportMapArchive.Tell();

		// Temporary archive for serializing export bundle data
		FBufferWriter ExportBundlesArchive(nullptr, 0, EBufferWriterFlags::AllowResize | EBufferWriterFlags::TakeOwnership);
		int32 ExportBundleEntryIndex = 0;
		for (FExportBundle& ExportBundle : Package->ExportBundles)
		{
			ExportBundlesArchive << ExportBundleEntryIndex;
			int32 EntryCount = ExportBundle.Nodes.Num();
			ExportBundlesArchive << EntryCount;
			ExportBundleEntryIndex += ExportBundle.Nodes.Num();
		}
		for (FExportBundle& ExportBundle : Package->ExportBundles)
		{
			for (FExportGraphNode* ExportNode : ExportBundle.Nodes)
			{
				uint32 CommandType = uint32(ExportNode->BundleEntry.CommandType);
				ExportBundlesArchive << ExportNode->BundleEntry.LocalExportIndex;
				ExportBundlesArchive << CommandType;
			}
		}
		Package->ExportBundlesSize = ExportBundlesArchive.Tell();

		Package->NameMapSize = Package->NameIndices.Num() * Package->NameIndices.GetTypeSize();

		{
			const uint64 PackageSummarySize =
				sizeof(FPackageSummary)
				+ Package->NameMapSize
				+ Package->ImportMapSize
				+ Package->ExportMapSize
				+ Package->ExportBundlesSize
				+ Package->UGraphSize;

			uint8* PackageSummaryBuffer = static_cast<uint8*>(FMemory::Malloc(PackageSummarySize));
			FPackageSummary* PackageSummary = reinterpret_cast<FPackageSummary*>(PackageSummaryBuffer);

			PackageSummary->PackageFlags = Package->PackageFlags;
			PackageSummary->GraphDataSize = Package->UGraphSize;
			PackageSummary->BulkDataStartOffset = Package->BulkDataStartOffset;
			const int32* FindGlobalImportIndexForPackage = GlobalImportsByFullName.Find(Package->Name.ToString());
			PackageSummary->GlobalImportIndex = FindGlobalImportIndexForPackage ? *FindGlobalImportIndexForPackage : -1;

			FBufferWriter SummaryArchive(PackageSummaryBuffer, PackageSummarySize);
			SummaryArchive.Seek(sizeof(FPackageSummary));

			// NameMap data
			{
				PackageSummary->NameMapOffset = SummaryArchive.Tell();
				SummaryArchive.Serialize(Package->NameIndices.GetData(), Package->NameMapSize);
			}

			// ImportMap data
			{
				check(ImportMapArchive.Tell() == Package->ImportMapSize);
				PackageSummary->ImportMapOffset = SummaryArchive.Tell();
				SummaryArchive.Serialize(ImportMapArchive.GetWriterData(), ImportMapArchive.Tell());
			}

			// ExportMap data
			{
				check(ExportMapArchive.Tell() == Package->ExportMapSize);
				PackageSummary->ExportMapOffset = SummaryArchive.Tell();
				SummaryArchive.Serialize(ExportMapArchive.GetWriterData(), ExportMapArchive.Tell());
			}

			// ExportBundle data
			{
				check(ExportBundlesArchive.Tell() == Package->ExportBundlesSize);
				PackageSummary->ExportBundlesOffset = SummaryArchive.Tell();
				SummaryArchive.Serialize(ExportBundlesArchive.GetWriterData(), ExportBundlesArchive.Tell());
			}

			// Graph data
			{
				check(GraphArchive.Tell() == Package->UGraphSize);
				PackageSummary->GraphDataOffset = SummaryArchive.Tell();
				SummaryArchive.Serialize(GraphArchive.GetWriterData(), GraphArchive.Tell());
			}

			// Export bundle chunks
			{
				check(Package->ExportBundles.Num());

				FString UExpFileName = FPaths::ChangeExtension(Package->FileName, TEXT(".uexp"));
				TUniquePtr<FArchive> ExpAr(IFileManager::Get().CreateFileReader(*UExpFileName));
				Package->UExpSize = ExpAr->TotalSize();
#if !SKIP_WRITE_CONTAINER
				uint8* ExportsBuffer = static_cast<uint8*>(FMemory::Malloc(ExpAr->TotalSize()));
				ExpAr->Serialize(ExportsBuffer, ExpAr->TotalSize());
#endif
				ExpAr->Close();

				uint64 BundleBufferSize = PackageSummarySize;
				for (int32 ExportBundleIndex = 0; ExportBundleIndex < Package->ExportBundles.Num(); ++ExportBundleIndex)
				{
					FExportBundle& ExportBundle = Package->ExportBundles[ExportBundleIndex];
					FExportBundleMetaEntry* ExportBundleMetaEntry = ExportBundleMetaEntries + Package->FirstExportBundleMetaEntry + ExportBundleIndex;
					ExportBundleMetaEntry->LoadOrder = ExportBundle.LoadOrder;
					for (FExportGraphNode* Node : ExportBundle.Nodes)
					{
						if (Node->BundleEntry.CommandType == FExportBundleEntry::ExportCommandType_Serialize)
						{
							const FObjectExport& ObjectExport = ObjectExports[Package->ExportIndexOffset + Node->BundleEntry.LocalExportIndex];
							BundleBufferSize += ObjectExport.SerialSize;
						}
					}
					if (ExportBundleIndex == 0)
					{
						ExportBundleMetaEntry->PayloadSize = BundleBufferSize;
					}
				}

#if !SKIP_WRITE_CONTAINER
				uint8* BundleBuffer = static_cast<uint8*>(FMemory::Malloc(BundleBufferSize));
				FMemory::Memcpy(BundleBuffer, PackageSummaryBuffer, PackageSummarySize);
#endif
				FMemory::Free(PackageSummaryBuffer);
				uint64 BundleBufferOffset = PackageSummarySize;
				for (FExportBundle& ExportBundle : Package->ExportBundles)
				{
					for (FExportGraphNode* Node : ExportBundle.Nodes)
					{
						if (Node->BundleEntry.CommandType == FExportBundleEntry::ExportCommandType_Serialize)
						{
							const FObjectExport& ObjectExport = ObjectExports[Package->ExportIndexOffset + Node->BundleEntry.LocalExportIndex];
							const int64 Offset = ObjectExport.SerialOffset - Package->UAssetSize;
#if !SKIP_WRITE_CONTAINER
							FMemory::Memcpy(BundleBuffer + BundleBufferOffset, ExportsBuffer + Offset, ObjectExport.SerialSize);
#endif
							BundleBufferOffset += ObjectExport.SerialSize;
						}
					}
				}

#if !SKIP_WRITE_CONTAINER
				FIoBuffer IoBuffer(FIoBuffer::Wrap, BundleBuffer, BundleBufferSize);
				IoStoreWriter->Append(CreateChunkId(Package->GlobalPackageId, 0, EIoChunkType::ExportBundleData, *Package->FileName), IoBuffer);
				FMemory::Free(BundleBuffer);
				FMemory::Free(ExportsBuffer);
#endif
			}

#if !SKIP_BULKDATA
			// Bulk chunks
			if (bWithBulkDataManifest)
			{
				FString BulkFileName = FPaths::ChangeExtension(Package->FileName, TEXT(".ubulk"));
				FPaths::NormalizeFilename(BulkFileName);

				WriteBulkData(BulkFileName, EIoChunkType::BulkData, *Package, BulkDataManifest, IoStoreWriter);

				FString OptionalBulkFileName = FPaths::ChangeExtension(Package->FileName, TEXT(".uptnl"));
				FPaths::NormalizeFilename(OptionalBulkFileName);

				WriteBulkData(OptionalBulkFileName, EIoChunkType::OptionalBulkData, *Package, BulkDataManifest, IoStoreWriter);
			}
#endif
		}
	}
}

template <class T>
class FCookedHeaderVisitor : public IPlatformFile::FDirectoryVisitor
{
	T& FoundFiles;
public:
	FCookedHeaderVisitor(T& InFoundFiles) : FoundFiles(InFoundFiles) {}
	virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory)
	{
		if (bIsDirectory == false)
		{
			FString Filename(FilenameOrDirectory);
			if (Filename.EndsWith(TEXT(".uasset")) || Filename.EndsWith(TEXT(".umap")))
			{
				FoundFiles.Add(MoveTemp(Filename));
			}
		}
		return true;
	}
};

int32 CreateTarget(const FContainerTarget& Target)
{
	TGuardValue<int32> GuardAllowUnversionedContentInEditor(GAllowUnversionedContentInEditor, 1);

	const FString CookedDir = Target.CookedDirectory;
	const FString OutputDir = Target.OutputDirectory;
	
	FPackageStoreBulkDataManifest BulkDataManifest(Target.CookedProjectDirectory);
	const bool bWithBulkDataManifest = BulkDataManifest.Load();
	if (bWithBulkDataManifest)
	{
		UE_LOG(LogIoStore, Display, TEXT("Loaded Bulk Data manifest '%s'"), *BulkDataManifest.GetFilename());
	}

#if OUTPUT_CHUNKID_DIRECTORY
	ChunkIdCsv.CreateOutputFile(CookedDir);
#endif

	FNameMapBuilder NameMapBuilder;

	uint64 NameSize = 0;
	TArray<FObjectImport> ObjectImports;
	TArray<FObjectExport> ObjectExports;
	TArray<FImportData> GlobalImports;
	TArray<FExportData> GlobalExports;
	TMap<FString, int32> GlobalImportsByFullName;
	TMap<FString, int32> GlobalExportsByFullName;
	TArray<FString> TempFullNames;
	TArray<FPackageIndex> PreloadDependencies;
	uint64 UPackageImports = 0;
	TArray<int32> ImportPreloadCounts;
	TArray<int32> ExportPreloadCounts;
	uint64 ImportPreloadCount = 0;
	uint64 ExportPreloadCount = 0;
	FExportGraph ExportGraph;
	TArray<FExportBundleMetaEntry> ExportBundleMetaEntries;

	TArray<FInstallChunk> InstallChunks;
	TArray<FPackage*> Packages;
	TMap<FName, FPackage*> PackageMap;

	if (Target.ChunkListFile.IsEmpty())
	{
		UE_LOG(LogIoStore, Display, TEXT("Searching for .uasset and .umap files..."));
		TArray<FString> FileNames;
		FCookedHeaderVisitor<TArray<FString>> CookedHeaderVistor(FileNames);
		FPlatformFileManager::Get().GetPlatformFile().IterateDirectoryRecursively(*CookedDir, CookedHeaderVistor);
		UE_LOG(LogIoStore, Display, TEXT("Found '%d' files"), FileNames.Num());

		FInstallChunk& InstallChunk = InstallChunks.AddDefaulted_GetRef();
		InstallChunk.Name = TEXT("container0");
		for (FString& FileName : FileNames)
		{
			if (FPackage* Package = AddPackage(*FileName, *CookedDir, Packages, PackageMap))
			{
				InstallChunk.Packages.Add(Package);
			}
		}
	}
	else
	{
		TArray<FString> ChunkFileEntries;
		FString ChunkFilesDirectory = FPaths::GetPath(Target.ChunkListFile);
		if (!FFileHelper::LoadFileToStringArray(ChunkFileEntries, *Target.ChunkListFile))
		{
			UE_LOG(LogIoStore, Error, TEXT("Failed to read chunk list file '%s'."), *Target.ChunkListFile);
			return -1;
		}

		UE_LOG(LogIoStore, Display, TEXT("Searching for .uasset and .umap files..."));
		TSet<FString> FileNames;
		FCookedHeaderVisitor<TSet<FString>> CookedHeaderVistor(FileNames);
		FPlatformFileManager::Get().GetPlatformFile().IterateDirectoryRecursively(*CookedDir, CookedHeaderVistor);
		UE_LOG(LogIoStore, Display, TEXT("Found '%d' files"), FileNames.Num());

		UE_LOG(LogIoStore, Display, TEXT("Parsing chunk list file '%s'"), *Target.ChunkListFile);

		const FString SpaceStr(TEXT(" "));
		for (FString& ChunkFileEntry : ChunkFileEntries)
		{
			FString ChunkFileName;
			FString Remainder;
			FString Option;
			if (!ChunkFileEntry.Split(SpaceStr, &ChunkFileName, &Remainder, ESearchCase::CaseSensitive))
			{
				// no options, only a chunk file name
				ChunkFileName = MoveTemp(ChunkFileEntry);
				UE_LOG(LogIoStore, Log, TEXT("Parsing chunk file entry for '%s' with no options"), *ChunkFileName);
			}
			else
			{
				UE_LOG(LogIoStore, Log, TEXT("Parsing chunk file entry for '%s' with options '%s'"), *ChunkFileName, *Remainder);
			}

			const TCHAR* PakChunkPrefix = TEXT("pakchunk");
			const int32 PakChunkPrefixLength = 8;
			if (!ChunkFileName.StartsWith(PakChunkPrefix))
			{
				UE_LOG(LogIoStore, Error, TEXT("Unexpected file name prefix in '%s'"), *ChunkFileName);
				continue;
			}
			int32 Index = PakChunkPrefixLength;
			int32 DigitCount = 0;
			while (Index < ChunkFileName.Len() && FChar::IsDigit(ChunkFileName[Index]))
			{
				++DigitCount;
				++Index;
			}
			if (DigitCount <= 0)
			{
				UE_LOG(LogIoStore, Error, TEXT("Unexpected file name digits in '%s'"), *ChunkFileName);
				continue;
			}

			while (Remainder.Split(SpaceStr, &Option, &Remainder, ESearchCase::CaseSensitive) || Remainder.Len() > 0)
			{
				if (Option.Len() == 0)
				{
					Option = MoveTemp(Remainder);
				}

				if (FPlatformString::Stricmp(*Option, TEXT("compressed")))
				{
					UE_LOG(LogIoStore, Log, TEXT("Ignored option '%s' for chunk '%s'"), *Option, *ChunkFileName);
				}
				else if (Option.StartsWith(TEXT("encryptionkeyguid=")))
				{
					UE_LOG(LogIoStore, Log, TEXT("Ignored option '%s' for chunk '%s'"), *Option, *ChunkFileName);
				}
				else
				{
					UE_LOG(LogIoStore, Warning, TEXT("Unexpected option '%s' for chunk '%s'"), *Option, *ChunkFileName);
				}
			}

			FString ChunkManifestFullPath = ChunkFilesDirectory / ChunkFileName;
			TArray<FString> ChunkManifest;
			if (!FFileHelper::LoadFileToStringArray(ChunkManifest, *ChunkManifestFullPath))
			{
				UE_LOG(LogIoStore, Error, TEXT("Failed to read chunk manifest file '%s'."), *ChunkManifestFullPath);
				continue;
			}

			if (ChunkManifest.Num() == 0)
			{
				UE_LOG(LogIoStore, Verbose, TEXT("Skipped zero size chunk manifest file '%s'."), *ChunkManifestFullPath);
				continue;
			}

			FString ChunkIdString = ChunkFileName.Mid(PakChunkPrefixLength, DigitCount);
			check(ChunkIdString.IsNumeric());
			int32 ChunkId;
			TTypeFromString<int32>::FromString(ChunkId, *ChunkIdString);
			FInstallChunk& InstallChunk = InstallChunks.AddDefaulted_GetRef();
			InstallChunk.Name = TEXT("container") + FPaths::GetBaseFilename(ChunkFileName.RightChop(8));
			InstallChunk.ChunkId = ChunkId;

			UE_LOG(LogIoStore, Log, TEXT("Parsing chunk manifest file '%s'"), *ChunkManifestFullPath);
			for (const FString& FileNameWithoutExtension : ChunkManifest)
			{
				FString RelativePathWithoutExtension = IFileManager::Get().ConvertToRelativePath(*FileNameWithoutExtension);
				FString FileName = RelativePathWithoutExtension + ".uasset";
				if (!FileNames.Contains(FileName))
				{
					FileName = RelativePathWithoutExtension + ".umap";
					if (!FileNames.Contains(FileName))
					{
						FileName.Empty();
					}
				}
				if (FileName.Len() > 0)
				{
					if (FPackage* Package = AddPackage(*FileName, *CookedDir, Packages, PackageMap))
					{
						InstallChunk.Packages.Add(Package);
					}
				}
				else
				{
					UE_LOG(LogIoStore, Log, TEXT("Ignored file '%s' since it has no corresponding package header file (.map/.uasset) in '%s'."), *FileNameWithoutExtension, *CookedDir);
				}
			}
		}
	}

	ImportPreloadCounts.AddDefaulted(Packages.Num());
	ExportPreloadCounts.AddDefaulted(Packages.Num());

	for (FPackage* Package : Packages)
	{
		UE_CLOG(Package->GlobalPackageId % 1000 == 0, LogIoStore, Display, TEXT("Parsing %d/%d: '%s'"), Package->GlobalPackageId, Packages.Num(), *Package->FileName);

		FPackageFileSummary Summary;
		TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileReader(*Package->FileName));
		check(Ar);
		*Ar << Summary;

		Package->UAssetSize = Ar->TotalSize();
		Package->SummarySize = Ar->Tell();
		Package->NameCount = Summary.NameCount;
		Package->ImportCount = Summary.ImportCount;
		Package->ExportCount = Summary.ExportCount;
		Package->PackageFlags = Summary.PackageFlags;
		Package->BulkDataStartOffset = Summary.BulkDataStartOffset;

		if (Summary.NameCount > 0)
		{
			Ar->Seek(Summary.NameOffset);
			uint64 LastOffset = Summary.NameOffset;

			Package->NameMap.Reserve(Summary.NameCount);
			Package->NameIndices.Reserve(Summary.NameCount);
			FNameEntrySerialized NameEntry(ENAME_LinkerConstructor);

			for (int32 I = 0; I < Summary.NameCount; ++I)
			{
				*Ar << NameEntry;
				FName Name(NameEntry);
				NameMapBuilder.MarkNameAsReferenced(Name);
				Package->NameMap.Emplace(Name.GetDisplayIndex());
				Package->NameIndices.Add(NameMapBuilder.MapName(Name));
			}

			NameSize += Ar->Tell() - Summary.NameOffset;
		}

		auto DeserializeName = [&](FArchive& A, FName& N)
		{
			int32 DisplayIndex, NameNumber;
			A << DisplayIndex << NameNumber;
			FNameEntryId DisplayEntry = Package->NameMap[DisplayIndex];
			N = FName::CreateFromDisplayId(DisplayEntry, NameNumber);
		};

		if (Summary.ImportCount > 0)
		{
			Ar->Seek(Summary.ImportOffset);

			int32 NumPackages = 0;
			int32 BaseIndex = ObjectImports.Num();
			ObjectImports.AddUninitialized(Summary.ImportCount);
			TArray<FString> ImportNames;
			ImportNames.Reserve(Summary.ImportCount);
			for (int32 I = 0; I < Summary.ImportCount; ++I)
			{
				FObjectImport& ObjectImport = ObjectImports[BaseIndex + I];
				DeserializeName(*Ar, ObjectImport.ClassPackage);
				DeserializeName(*Ar, ObjectImport.ClassName);
				*Ar << ObjectImport.OuterIndex;
				DeserializeName(*Ar, ObjectImport.ObjectName);

				if (ObjectImport.OuterIndex.IsNull())
				{
					++NumPackages;
				}

				ImportNames.Emplace(ObjectImport.ObjectName.ToString());
			}

			UPackageImports += NumPackages;

			Package->ImportedFullNames.SetNum(Summary.ImportCount);
			for (int32 I = 0; I < Summary.ImportCount; ++I)
			{
				FindImport(GlobalImports, GlobalImportsByFullName, Package->ImportedFullNames, ObjectImports.GetData() + BaseIndex, I);
			}
		}

		Package->PreloadIndexOffset = PreloadDependencies.Num();
		int32 PreloadDependenciesBaseIndex = -1;
		if (Summary.PreloadDependencyCount > 0)
		{
			Ar->Seek(Summary.PreloadDependencyOffset);
			PreloadDependenciesBaseIndex = PreloadDependencies.Num();
			PreloadDependencies.AddUninitialized(Summary.PreloadDependencyCount);
			for (int32 I = 0; I < Summary.PreloadDependencyCount; ++I)
			{
				FPackageIndex& Index = PreloadDependencies[PreloadDependenciesBaseIndex + I];
				*Ar << Index;
				if (Index.IsImport())
				{
					++ImportPreloadCounts[Package->GlobalPackageId];
					++ImportPreloadCount;
				}
				else
				{
					++ExportPreloadCounts[Package->GlobalPackageId];
					++ExportPreloadCount;
				}
			}
		}

		Package->ExportIndexOffset = ObjectExports.Num();
		if (Summary.ExportCount > 0)
		{
			Ar->Seek(Summary.ExportOffset);

			int32 BaseIndex = ObjectExports.Num();
			ObjectExports.AddUninitialized(Summary.ExportCount);
			for (int32 I = 0; I < Summary.ExportCount; ++I)
			{
				FObjectExport& ObjectExport = ObjectExports[BaseIndex + I];
				*Ar << ObjectExport.ClassIndex;
				*Ar << ObjectExport.SuperIndex;
				*Ar << ObjectExport.TemplateIndex;
				*Ar << ObjectExport.OuterIndex;
				DeserializeName(*Ar, ObjectExport.ObjectName);
				uint32 ObjectFlags;
				*Ar << ObjectFlags;
				ObjectExport.ObjectFlags = (EObjectFlags)ObjectFlags;
				*Ar << ObjectExport.SerialSize;
				*Ar << ObjectExport.SerialOffset;
				*Ar << ObjectExport.bForcedExport;
				*Ar << ObjectExport.bNotForClient;
				*Ar << ObjectExport.bNotForServer;
				*Ar << ObjectExport.PackageGuid;
				*Ar << ObjectExport.PackageFlags;
				*Ar << ObjectExport.bNotAlwaysLoadedForEditorGame;
				*Ar << ObjectExport.bIsAsset;
				*Ar << ObjectExport.FirstExportDependency;
				*Ar << ObjectExport.SerializationBeforeSerializationDependencies;
				*Ar << ObjectExport.CreateBeforeSerializationDependencies;
				*Ar << ObjectExport.SerializationBeforeCreateDependencies;
				*Ar << ObjectExport.CreateBeforeCreateDependencies;
			}

			TempFullNames.Reset();
			TempFullNames.SetNum(Summary.ExportCount, false);
			for (int32 I = 0; I < Summary.ExportCount; ++I)
			{
				FindExport(GlobalExports, GlobalExportsByFullName, TempFullNames, ObjectExports.GetData() + BaseIndex, I, Package->Name);

				FExportData& ExportData = GlobalExports[*GlobalExportsByFullName.Find(TempFullNames[I])];
				Package->Exports.Add(ExportData.GlobalIndex);
				ExportData.CreateNode = ExportGraph.AddNode(Package, { uint32(I), FExportBundleEntry::ExportCommandType_Create });
				ExportData.SerializeNode = ExportGraph.AddNode(Package, { uint32(I), FExportBundleEntry::ExportCommandType_Serialize });
				Package->CreateExportNodes.Add(ExportData.CreateNode);
				Package->SerializeExportNodes.Add(ExportData.SerializeNode);
				ExportGraph.AddInternalDependency(ExportData.CreateNode, ExportData.SerializeNode);
			}
		}
	
		Ar->Close();
	}

	int32 NumScriptImports = 0;
	{
		// Sort imports by script objects first
		GlobalImports.Sort();

		// build remap from old global import index to new sorted global import index
		TMap<int32, int32> Remap;
		Remap.Reserve(GlobalImports.Num() + 1);
		Remap.Add(-1, -1);
		for (int32 I = 0; I < GlobalImports.Num(); ++I)
		{
			FImportData& Import = GlobalImports[I];
			Remap.Add(Import.GlobalIndex, I);
		}

		// remap all global import indices and lookup package pointers and export indices
		FPackage* LastPackage = nullptr; 
		for (int32 I = 0; I < GlobalImports.Num(); ++I)
		{
			FImportData& GlobalImport = GlobalImports[I];

			GlobalImport.GlobalIndex = Remap[GlobalImport.GlobalIndex];
			GlobalImport.OuterIndex = Remap[GlobalImport.OuterIndex];
			GlobalImport.OutermostIndex = Remap[GlobalImport.OutermostIndex];

			if (!GlobalImport.bIsScript)
			{
				if (NumScriptImports == 0)
				{
					NumScriptImports = I;
				}

				if (GlobalImport.bIsPackage)
				{
					if (LastPackage)
					{
						LastPackage->GlobalImportCount = I - LastPackage->FirstGlobalImport;
					}
					FPackage* FindPackage = PackageMap.FindRef(GlobalImport.ObjectName);
					check(FindPackage);
					FindPackage->FirstGlobalImport = I;
					GlobalImport.Package = FindPackage;
					LastPackage = FindPackage;
				}
				else
				{
					int32* FindGlobalExport = GlobalExportsByFullName.Find(GlobalImport.FullName);
					check(FindGlobalExport);
					GlobalImport.GlobalExportIndex = *FindGlobalExport;
				}
			}

			GlobalImportsByFullName[GlobalImport.FullName] = I;
		}
		if (LastPackage)
		{
			LastPackage->GlobalImportCount = GlobalImports.Num() - LastPackage->FirstGlobalImport;
		}
	}

	for (FExportData& GlobalExport : GlobalExports)
	{
		int32* FindGlobalImport = GlobalImportsByFullName.Find(GlobalExport.FullName);
		GlobalExport.GlobalImportIndex = FindGlobalImport ? *FindGlobalImport : -1;
	}

#if OUTPUT_NAMEMAP_CSV
	FString CsvFilePath = OutputDir / TEXT("AllImports.csv");
	TUniquePtr<FArchive> CsvArchive(IFileManager::Get().CreateFileWriter(*CsvFilePath));
	if (CsvArchive)
	{
		ANSICHAR Line[MAX_SPRINTF + FName::StringBufferSize];
		ANSICHAR Header[] = "Count\tOuter\tOutermost\tImportName\n";
		CsvArchive->Serialize(Header, sizeof(Header) - 1);
		for (const FImportData& ImportData : GlobalImports)
		{
			FCStringAnsi::Sprintf(Line, "%d\t%d\t%d\t",
				ImportData.RefCount, ImportData.OuterIndex, ImportData.OutermostIndex);
			ANSICHAR* L = Line + FCStringAnsi::Strlen(Line);
			const TCHAR* N = *ImportData.FullName;
			while (*N)
			{
				*L++ = CharCast<ANSICHAR,TCHAR>(*N++);
			}
			*L++ = '\n';
			CsvArchive.Get()->Serialize(Line, L - Line);
		}
	}
#endif

	TSet<FString> MissingExports;
	TSet<FPackage*> Visited;
	TSet<FCircularImportChain> CircularChains;

	// Lookup global indices and package pointers for all imports before adding preload and postload arcs
	UE_LOG(LogIoStore, Display, TEXT("Looking up import packages..."));
	for (FPackage* Package : Packages)
	{
		Package->ImportedPackages.Reserve(Package->ImportCount);
		for (int32 I = 0; I < Package->ImportCount; ++I)
		{
			int32 GlobalImportIndex = *GlobalImportsByFullName.Find(Package->ImportedFullNames[I]);

			FImportData& ImportData = GlobalImports[GlobalImportIndex];
			Package->Imports.Add(ImportData.GlobalIndex);
			if (ImportData.Package)
			{
				Package->ImportedPackages.Add(ImportData.Package);
			}
		}
	}

	UE_LOG(LogIoStore, Display, TEXT("Converting export map import indices..."));
	for (FPackage* Package : Packages)
	{
		for (int32 I = 0; I < Package->ExportCount; ++I)
		{
			FObjectExport& ObjectExport = ObjectExports[Package->ExportIndexOffset + I];
			FExportData& ExportData = GlobalExports[Package->Exports[I]];

			check(!ObjectExport.OuterIndex.IsImport());
			ExportData.OuterIndex = ObjectExport.OuterIndex;
			ExportData.ClassIndex =
				ObjectExport.ClassIndex.IsImport() ?
				FPackageIndex::FromImport(Package->Imports[ObjectExport.ClassIndex.ToImport()]) :
				ObjectExport.ClassIndex;
			ExportData.SuperIndex =
				ObjectExport.SuperIndex.IsImport() ?
				FPackageIndex::FromImport(Package->Imports[ObjectExport.SuperIndex.ToImport()]) :
				ObjectExport.SuperIndex;
			ExportData.TemplateIndex =
				ObjectExport.TemplateIndex.IsImport() ?
				FPackageIndex::FromImport(Package->Imports[ObjectExport.TemplateIndex.ToImport()]) :
				ObjectExport.TemplateIndex;
		}
	}

	UE_LOG(LogIoStore, Display, TEXT("Adding optimized postload dependencies..."));
	for (FPackage* Package : Packages)
	{
		Visited.Reset();
		Visited.Add(Package);
		AddPostLoadDependencies(*Package, Visited, CircularChains);
	}
		
	UE_LOG(LogIoStore, Display, TEXT("Adding preload dependencies..."));
	for (FPackage* Package : Packages)
	{
		// Convert PreloadDependencies to arcs
		for (int32 I = 0; I < Package->ExportCount; ++I)
		{
			FObjectExport& ObjectExport = ObjectExports[Package->ExportIndexOffset + I];
			FExportData& ExportData = GlobalExports[Package->Exports[I]];
			int32 PreloadDependenciesBaseIndex = Package->PreloadIndexOffset;

			FPackageIndex ExportPackageIndex = FPackageIndex::FromExport(I);

			auto AddPreloadArc = [&](FPackageIndex Dep, EPreloadDependencyType PhaseFrom, EPreloadDependencyType PhaseTo)
			{
				if (Dep.IsExport())
				{
					AddInternalExportArc(ExportGraph, *Package, Dep.ToExport(), PhaseFrom, I, PhaseTo);
				}
				else
				{
					FImportData& Import = GlobalImports[Package->Imports[Dep.ToImport()]];
					check(!Import.bIsPackage);
					if (Import.bIsScript)
					{
						// Add script arc with null package and global import index as node index
						AddScriptArc(*Package, Import.GlobalIndex, I, PhaseTo);
					}
					else
					{
						check(Import.GlobalExportIndex != - 1);
						FExportData& Export = GlobalExports[Import.GlobalExportIndex];
						FPackage* SourcePackage = PackageMap.FindRef(Export.SourcePackageName);
						check(SourcePackage);
						FPackageIndex SourceDep = FPackageIndex::FromExport(Export.SourceIndex);
						AddExternalExportArc(ExportGraph, *SourcePackage, Export.SourceIndex, PhaseFrom, *Package, I, PhaseTo);
						Package->ImportedPreloadPackages.Add(SourcePackage);
					}
				}
			};

			if (PreloadDependenciesBaseIndex >= 0 && ObjectExport.FirstExportDependency >= 0)
			{
				int32 RunningIndex = PreloadDependenciesBaseIndex + ObjectExport.FirstExportDependency;
				for (int32 Index = ObjectExport.SerializationBeforeSerializationDependencies; Index > 0; Index--)
				{
					FPackageIndex Dep = PreloadDependencies[RunningIndex++];
					check(!Dep.IsNull());
					AddPreloadArc(Dep, PreloadDependencyType_Serialize, PreloadDependencyType_Serialize);
				}

				for (int32 Index = ObjectExport.CreateBeforeSerializationDependencies; Index > 0; Index--)
				{
					FPackageIndex Dep = PreloadDependencies[RunningIndex++];
					check(!Dep.IsNull());
					AddPreloadArc(Dep, PreloadDependencyType_Create, PreloadDependencyType_Serialize);
				}

				for (int32 Index = ObjectExport.SerializationBeforeCreateDependencies; Index > 0; Index--)
				{
					FPackageIndex Dep = PreloadDependencies[RunningIndex++];
					check(!Dep.IsNull());
					AddPreloadArc(Dep, PreloadDependencyType_Serialize, PreloadDependencyType_Create);
				}

				for (int32 Index = ObjectExport.CreateBeforeCreateDependencies; Index > 0; Index--)
				{
					FPackageIndex Dep = PreloadDependencies[RunningIndex++];
					check(!Dep.IsNull());
					// can't create this export until these things are created
					AddPreloadArc(Dep, PreloadDependencyType_Create, PreloadDependencyType_Create);
				}
			}
		}
	}

	UE_LOG(LogIoStore, Display, TEXT("Building bundles..."));
	BuildBundles(ExportGraph, Packages);

	TUniquePtr<FLargeMemoryWriter> StoreTocArchive = MakeUnique<FLargeMemoryWriter>(0, true);
	TUniquePtr<FLargeMemoryWriter> ImportedPackagesArchive = MakeUnique<FLargeMemoryWriter>(0, true);
	TUniquePtr<FLargeMemoryWriter> GlobalImportNamesArchive = MakeUnique<FLargeMemoryWriter>(0, true);
	TUniquePtr<FLargeMemoryWriter> InitialLoadArchive = MakeUnique<FLargeMemoryWriter>(0, true);

	UE_LOG(LogIoStore, Display, TEXT("Serializing global import names..."));
	if (GlobalImportNamesArchive)
	{
		for (const FImportData& ImportData : GlobalImports)
		{
			NameMapBuilder.SerializeName(*GlobalImportNamesArchive, ImportData.ObjectName);
		}
	}

	UE_LOG(LogIoStore, Display, TEXT("Serializing initial load..."));
	// Separate file for script arcs that are only required during initial loading
	if (InitialLoadArchive)
	{
		int32 PackageCount = PackageMap.Num();
		*InitialLoadArchive << PackageCount;
		*InitialLoadArchive << NumScriptImports;

		FBufferWriter ScriptArcsArchive(nullptr, 0, EBufferWriterFlags::AllowResize | EBufferWriterFlags::TakeOwnership);

		for (auto& PackageKV : PackageMap)
		{
			FPackage& Package = *PackageKV.Value;

			int32 ScriptArcsOffset = ScriptArcsArchive.Tell();
			int32 ScriptArcsCount = Package.ScriptArcs.Num();

			*InitialLoadArchive << ScriptArcsOffset;
			*InitialLoadArchive << ScriptArcsCount;

			for (FArc& ScriptArc : Package.ScriptArcs)
			{
				ScriptArcsArchive << ScriptArc.FromNodeIndex;
				ScriptArcsArchive << ScriptArc.ToNodeIndex;
			}
		}

		for (int32 I = 0; I < NumScriptImports; ++I)
		{
			FImportData& ImportData = GlobalImports[I];
			FPackageIndex OuterIndex = 
				ImportData.OuterIndex >= 0 ?
				FPackageIndex::FromImport(ImportData.OuterIndex) :
				FPackageIndex();

			*InitialLoadArchive << OuterIndex;
		}

		InitialLoadArchive->Serialize(ScriptArcsArchive.GetWriterData(), ScriptArcsArchive.Tell());
	}

	for (FPackage* Package : Packages)
	{
		Package->FirstExportBundleMetaEntry = ExportBundleMetaEntries.Num();
		ExportBundleMetaEntries.AddDefaulted(Package->ExportBundles.Num());

		NameMapBuilder.MarkNameAsReferenced(Package->Name);
		NameMapBuilder.SerializeName(*StoreTocArchive, Package->Name);
		*StoreTocArchive << Package->ExportCount;
		int32 ExportBundleCount = Package->ExportBundles.Num();
		*StoreTocArchive << ExportBundleCount;
		*StoreTocArchive << Package->FirstExportBundleMetaEntry;
		*StoreTocArchive << Package->FirstGlobalImport;
		*StoreTocArchive << Package->GlobalImportCount;
		int32 ImportedPackagesCount = Package->ImportedPackages.Num();
		*StoreTocArchive << ImportedPackagesCount;
		int32 ImportedPackagesOffset = ImportedPackagesArchive->Tell();
		*StoreTocArchive << ImportedPackagesOffset;

		for (FPackage* ImportedPackage : Package->ImportedPackages)
		{
			*ImportedPackagesArchive << ImportedPackage->GlobalPackageId;
		}
	}

	UE_LOG(LogIoStore, Display, TEXT("Serializing..."));

	TUniquePtr<FIoStoreWriter> GlobalIoStoreWriter;
	FIoStoreEnvironment GlobalIoStoreEnv;
	{
		GlobalIoStoreEnv.InitializeFileEnvironment(OutputDir);
		GlobalIoStoreWriter = MakeUnique<FIoStoreWriter>(GlobalIoStoreEnv);
#if !SKIP_WRITE_CONTAINER
		FIoStatus IoStatus = GlobalIoStoreWriter->Initialize();
		check(IoStatus.IsOk());
#endif
	}

	FIoStoreInstallManifest Manifest;
	for (const FInstallChunk& InstallChunk : InstallChunks)
	{
		TUniquePtr<FIoStoreWriter> IoStoreWriter;
		FIoStoreEnvironment InstallChunkIoStoreEnv(GlobalIoStoreEnv, InstallChunk.Name);
		IoStoreWriter = MakeUnique<FIoStoreWriter>(InstallChunkIoStoreEnv);
#if !SKIP_WRITE_CONTAINER
		FIoStatus IoStatus = IoStoreWriter->Initialize();
		check(IoStatus.IsOk());
#endif
		SerializePackageData(
			IoStoreWriter.Get(),
			PackageMap.Num(),
			InstallChunk.Packages,
			NameMapBuilder,
			ObjectExports,
			GlobalExports,
			GlobalImportsByFullName,
			ExportBundleMetaEntries.GetData(),
			BulkDataManifest,
			bWithBulkDataManifest);
		FIoStoreInstallManifest::FEntry& ManifestEntry = Manifest.EditEntries().AddDefaulted_GetRef();
		ManifestEntry.InstallChunkId = InstallChunk.ChunkId;
		ManifestEntry.PartitionName = InstallChunk.Name;
	}
	TUniquePtr<FLargeMemoryWriter> ManifestArchive = MakeUnique<FLargeMemoryWriter>(0, true);
	*ManifestArchive << Manifest;

	GlobalIoStoreWriter->Append(CreateIoChunkId(0, 0, EIoChunkType::InstallManifest), FIoBuffer(FIoBuffer::Wrap, ManifestArchive->GetData(), ManifestArchive->TotalSize()));

	int32 StoreTocByteCount = StoreTocArchive->TotalSize();
	int32 ImportedPackagesByteCount = ImportedPackagesArchive->TotalSize();
	int32 GlobalImportNamesByteCount = GlobalImportNamesArchive->TotalSize();
	int32 ExportBundleMetaByteCount = ExportBundleMetaEntries.Num() * sizeof(FExportBundleMetaEntry);
	{
		UE_LOG(LogIoStore, Display, TEXT("Saving global meta data to container file"));
		FLargeMemoryWriter GlobalMetaArchive(0, true);

		GlobalMetaArchive << StoreTocByteCount;
		GlobalMetaArchive.Serialize(StoreTocArchive->GetData(), StoreTocByteCount);

		GlobalMetaArchive << ImportedPackagesByteCount;
		GlobalMetaArchive.Serialize(ImportedPackagesArchive->GetData(), ImportedPackagesByteCount);

		GlobalMetaArchive << GlobalImportNamesByteCount;
		GlobalMetaArchive.Serialize(GlobalImportNamesArchive->GetData(), GlobalImportNamesByteCount);

		GlobalMetaArchive << ExportBundleMetaByteCount;
		GlobalMetaArchive.Serialize(ExportBundleMetaEntries.GetData(), ExportBundleMetaByteCount);

#if !SKIP_WRITE_CONTAINER
		const FIoStatus Status = GlobalIoStoreWriter->Append(CreateIoChunkId(0, 0, EIoChunkType::LoaderGlobalMeta), FIoBuffer(FIoBuffer::Wrap, GlobalMetaArchive.GetData(), GlobalMetaArchive.TotalSize()));
		if (!Status.IsOk())
		{
			UE_LOG(LogIoStore, Error, TEXT("Failed to save global meta data to container file"));
		}
#endif
	}

	{
		UE_LOG(LogIoStore, Display, TEXT("Saving initial load meta data to container file"));
#if !SKIP_WRITE_CONTAINER
		const FIoStatus Status = GlobalIoStoreWriter->Append(CreateIoChunkId(0, 0, EIoChunkType::LoaderInitialLoadMeta), FIoBuffer(FIoBuffer::Wrap, InitialLoadArchive->GetData(), InitialLoadArchive->TotalSize()));
		if (!Status.IsOk())
		{
			UE_LOG(LogIoStore, Error, TEXT("Failed to save initial load meta data to container file"));
		}
#endif
	}

	uint64 GlobalNamesMB = 0;
	uint64 GlobalNameHashesMB = 0;
	{
		UE_LOG(LogIoStore, Display, TEXT("Saving global name map to container file"));

		TArray<uint8> Names;
		TArray<uint8> Hashes;
		SaveNameBatch(NameMapBuilder.GetNameMap(), /* out */ Names, /* out */ Hashes);

		GlobalNamesMB = Names.Num() >> 20;
		GlobalNameHashesMB = Hashes.Num() >> 20;

#if !SKIP_WRITE_CONTAINER
		FIoStatus NameStatus = GlobalIoStoreWriter->Append(CreateIoChunkId(0, 0, EIoChunkType::LoaderGlobalNames), 
													 FIoBuffer(FIoBuffer::Wrap, Names.GetData(), Names.Num()));
		FIoStatus HashStatus = GlobalIoStoreWriter->Append(CreateIoChunkId(0, 0, EIoChunkType::LoaderGlobalNameHashes),
													 FIoBuffer(FIoBuffer::Wrap, Hashes.GetData(), Hashes.Num()));
#endif
		
		if (!NameStatus.IsOk() || !HashStatus.IsOk())
		{
			UE_LOG(LogIoStore, Error, TEXT("Failed to save global name map to container file"));
		}

#if OUTPUT_NAMEMAP_CSV
		NameMapBuilder.SaveCsv(OutputDir / TEXT("Container.namemap.csv"));
#endif
	}

	UE_LOG(LogIoStore, Display, TEXT("Calculating stats..."));
	uint64 UExpSize = 0;
	uint64 UAssetSize = 0;
	uint64 SummarySize = 0;
	uint64 UGraphSize = 0;
	uint64 ImportMapSize = 0;
	uint64 ExportMapSize = 0;
	uint64 NameMapSize = 0;
	uint64 NameMapCount = 0;
	uint64 PackageSummarySize = Packages.Num() * sizeof(FPackageSummary);
	uint64 ImportedPackagesCount = 0;
	uint64 InitialLoadSize = InitialLoadArchive->Tell();
	uint64 ScriptArcsCount = 0;
	uint64 CircularPackagesCount = 0;
	uint64 TotalInternalArcCount = 0;
	uint64 TotalExternalArcCount = 0;
	uint64 NameCount = 0;

	uint64 PackagesWithoutImportDependenciesCount = 0;
	uint64 PackagesWithoutPreloadDependenciesCount = 0;
	uint64 BundleCount = 0;
	uint64 BundleEntryCount = 0;

	uint64 PackageHeaderSize = 0;

	uint64 UniqueImportPackages = 0;
	for (const FImportData& ImportData : GlobalImports)
	{
		UniqueImportPackages += (ImportData.OuterIndex == 0);
	}

	for (auto& PackageKV : PackageMap)
	{
		FPackage& Package = *PackageKV.Value;

		UExpSize += Package.UExpSize;
		UAssetSize += Package.UAssetSize;
		SummarySize += Package.SummarySize;
		UGraphSize += Package.UGraphSize;
		ImportMapSize += Package.ImportMapSize;
		ExportMapSize += Package.ExportMapSize;
		NameMapSize += Package.NameMapSize;
		NameMapCount += Package.NameIndices.Num();
		ScriptArcsCount += Package.ScriptArcs.Num();
		CircularPackagesCount += Package.bHasCircularImportDependencies;
		TotalInternalArcCount += Package.InternalArcs.Num();
		ImportedPackagesCount += Package.ImportedPackages.Num();
		NameCount += Package.NameMap.Num();
		PackagesWithoutPreloadDependenciesCount += Package.ImportedPreloadPackages.Num() == 0;
		PackagesWithoutImportDependenciesCount += Package.ImportedPackages.Num() == 0;

		for (auto& KV : Package.ExternalArcs)
		{
			TArray<FArc>& Arcs = KV.Value;
			TotalExternalArcCount += Arcs.Num();
		}

		for (FExportBundle& Bundle : Package.ExportBundles)
		{
			++BundleCount;
			BundleEntryCount += Bundle.Nodes.Num();
		}
	}

	PackageHeaderSize = PackageSummarySize + NameMapSize + ImportMapSize + ExportMapSize + UGraphSize;

	UE_LOG(LogIoStore, Display, TEXT("-------------------- IoStore Summary: %s --------------------"), *Target.TargetPlatform->PlatformName());
	UE_LOG(LogIoStore, Display, TEXT("Packages: %8d total, %d circular dependencies, %d no preload dependencies, %d no import dependencies"),
		PackageMap.Num(), CircularPackagesCount, PackagesWithoutPreloadDependenciesCount, PackagesWithoutImportDependenciesCount);
	UE_LOG(LogIoStore, Display, TEXT("Bundles:  %8d total, %d entries, %d export objects"), BundleCount, BundleEntryCount, GlobalExports.Num());

	UE_LOG(LogIoStore, Display, TEXT("IoStore: %8.2f MB GlobalNames, %d unique names"), (double)GlobalNamesMB, NameMapBuilder.GetNameMap().Num());
	UE_LOG(LogIoStore, Display, TEXT("IoStore: %8.2f MB GlobalNameHashes"), (double)GlobalNameHashesMB);
	UE_LOG(LogIoStore, Display, TEXT("IoStore: %8.2f MB GlobalPackageData"), (double)StoreTocByteCount / 1024.0 / 1024.0);
	UE_LOG(LogIoStore, Display, TEXT("IoStore: %8.2f MB GlobalImportedPackages, %d imported packages"), (double)ImportedPackagesByteCount / 1024.0 / 1024.0, ImportedPackagesCount);
	UE_LOG(LogIoStore, Display, TEXT("IoStore: %8.2f MB GlobalBundleMeta, %d bundles"), (double)ExportBundleMetaByteCount / 1024.0 / 1024.0, ExportBundleMetaEntries.Num());
	UE_LOG(LogIoStore, Display, TEXT("IoStore: %8.2f MB GlobalImportNames, %d total imports, %d script imports, %d UPackage imports"),
		(double)GlobalImportNamesByteCount / 1024.0 / 1024.0, GlobalImportsByFullName.Num(), NumScriptImports, UniqueImportPackages);
	UE_LOG(LogIoStore, Display, TEXT("IoStore: %8.2f MB InitialLoadData, %d script arcs, %d script outers, %d packages"), (double)InitialLoadSize / 1024.0 / 1024.0, ScriptArcsCount, NumScriptImports, Packages.Num());
	UE_LOG(LogIoStore, Display, TEXT("IoStore: %8.2f MB PackageHeader, %d packages"), (double)PackageHeaderSize / 1024.0 / 1024.0, Packages.Num());
	UE_LOG(LogIoStore, Display, TEXT("IoStore: %8.2f MB PackageSummary"), (double)PackageSummarySize / 1024.0 / 1024.0);
	UE_LOG(LogIoStore, Display, TEXT("IoStore: %8.2f MB PackageNameMap, %d indices"), (double)NameMapSize / 1024.0 / 1024.0, NameMapCount);
	UE_LOG(LogIoStore, Display, TEXT("IoStore: %8.2f MB PackageImportMap"), (double)ImportMapSize / 1024.0 / 1024.0);
	UE_LOG(LogIoStore, Display, TEXT("IoStore: %8.2f MB PackageExportMap"), (double)ExportMapSize / 1024.0 / 1024.0);
	UE_LOG(LogIoStore, Display, TEXT("IoStore: %8.2f MB PackageArcs, %d internal arcs, %d external arcs, %d circular packages (%d chains)"),
		(double)UGraphSize / 1024.0 / 1024.0, TotalInternalArcCount, TotalExternalArcCount, CircularPackagesCount, CircularChains.Num());

	return 0;
}

int32 CreateIoStoreContainerFiles(const TCHAR* CmdLine)
{
	UE_LOG(LogIoStore, Display, TEXT("==================== IoStore Utils ===================="));

	const TArray<ITargetPlatform*>& Platforms = GetTargetPlatformManagerRef().GetActiveTargetPlatforms();

	FString OutputDirectory;
	if (FParse::Value(FCommandLine::Get(), TEXT("OutputDirectory="), OutputDirectory))
	{
		UE_LOG(LogIoStore, Display, TEXT("Using output directory: '%s'"), *OutputDirectory);
	}
	else
	{
		UE_LOG(LogIoStore, Display, TEXT("No output directory specified, using project's cooked folder"));
	}

	FString ChunkListFile;
	if (FParse::Value(FCommandLine::Get(), TEXT("ChunkListFile="), ChunkListFile))
	{
		UE_LOG(LogIoStore, Display, TEXT("Using chunk list file: '%s'"), *ChunkListFile);
	}

	for (const ITargetPlatform* TargetPlatform : Platforms)
	{
		const FString TargetCookedDirectory = FPaths::ProjectSavedDir() / TEXT("Cooked") / TargetPlatform->PlatformName();
		const FString TargetCookedProjectDirectory = TargetCookedDirectory / FApp::GetProjectName();

		const FString TargetOutputDirectory = OutputDirectory.Len() > 0
			? OutputDirectory
			: TargetCookedProjectDirectory / TEXT("Content") / TEXT("Containers");

		FContainerTarget Target{ TargetPlatform, TargetCookedDirectory, TargetCookedProjectDirectory, TargetOutputDirectory, ChunkListFile };

		UE_LOG(LogIoStore, Display, TEXT("Creating target: '%s' using output directory: '%s'"), *Target.TargetPlatform->PlatformName(), *Target.OutputDirectory);

		int32 ReturnValue = CreateTarget(Target);
		if (ReturnValue != 0)
		{
			return ReturnValue;
		}
	}

	return 0;
}
