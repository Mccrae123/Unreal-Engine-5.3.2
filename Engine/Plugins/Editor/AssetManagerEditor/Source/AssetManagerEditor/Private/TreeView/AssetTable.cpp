// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetTable.h"

#include "Styling/StyleColors.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SToolTip.h"
#include "Widgets/Text/STextBlock.h"

// AssetManagerEditor
#include "AssetTreeNode.h"
#include "Insights/Table/ViewModels/TableCellValueFormatter.h"
#include "Insights/Table/ViewModels/TableCellValueGetter.h"
#include "Insights/Table/ViewModels/TableCellValueSorter.h"
#include "Insights/Table/ViewModels/TableColumn.h"

#define LOCTEXT_NAMESPACE "FAssetTable"

////////////////////////////////////////////////////////////////////////////////////////////////////
// Column identifiers

const FName FAssetTableColumns::CountColumnId(TEXT("Count"));
const FName FAssetTableColumns::NameColumnId(TEXT("Name"));
const FName FAssetTableColumns::TypeColumnId(TEXT("Type"));
const FName FAssetTableColumns::PathColumnId(TEXT("Path"));
const FName FAssetTableColumns::PrimaryTypeColumnId(TEXT("PrimaryType"));
const FName FAssetTableColumns::PrimaryNameColumnId(TEXT("PrimaryName"));
const FName FAssetTableColumns::StagedCompressedSizeColumnId(TEXT("StagedCompressedSize"));
const FName FAssetTableColumns::TotalSizeUniqueDependenciesColumnId(TEXT("TotalSizeUniqueDependencies"));
const FName FAssetTableColumns::TotalSizeOtherDependenciesColumnId(TEXT("TotalSizeOtherDependencies"));
const FName FAssetTableColumns::TotalUsageCountColumnId(TEXT("TotalUsageCount"));
const FName FAssetTableColumns::ChunksColumnId(TEXT("Chunks"));
const FName FAssetTableColumns::NativeClassColumnId(TEXT("NativeClass"));
const FName FAssetTableColumns::GameFeaturePluginColumnId(TEXT("GameFeaturePlugin"));

////////////////////////////////////////////////////////////////////////////////////////////////////
// FAssetTableStringStore
////////////////////////////////////////////////////////////////////////////////////////////////////

FAssetTableStringStore::FAssetTableStringStore()
	: Chunks()
	, Cache()
	, TotalInputStringSize(0)
	, TotalStoredStringSize(0)
	, NumInputStrings(0)
	, NumStoredStrings(0)
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////

FAssetTableStringStore::~FAssetTableStringStore()
{
	Reset();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void FAssetTableStringStore::Reset()
{
	for (FChunk& Chunk : Chunks)
	{
		delete[] Chunk.Buffer;
	}
	Chunks.Reset();
	Cache.Reset();
	TotalInputStringSize = 0;
	TotalStoredStringSize = 0;
	NumInputStrings = 0;
	NumStoredStrings = 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

const FStringView FAssetTableStringStore::Store(const TCHAR* InStr)
{
	if (!InStr)
	{
		return FStringView();
	}
	return Store(FStringView(InStr));
}

////////////////////////////////////////////////////////////////////////////////////////////////////

const FStringView FAssetTableStringStore::Store(const FStringView InStr)
{
	if (InStr.IsEmpty())
	{
		return FStringView();
	}

	check(InStr.Len() <= GetMaxStringLength());

	TotalInputStringSize += (InStr.Len() + 1) * sizeof(TCHAR);
	++NumInputStrings;

	uint32 Hash = GetTypeHash(InStr);

	TArray<FStringView> CachedStrings;
	Cache.MultiFind(Hash, CachedStrings);
	for (const FStringView& CachedString : CachedStrings)
	{
		if (CachedString.Equals(InStr, SearchCase))
		{
			return CachedString;
		}
	}

	if (Chunks.Num() == 0 ||
		Chunks.Last().Used + InStr.Len() + 1 > ChunkBufferLen)
	{
		AddChunk();
	}

	TotalStoredStringSize += (InStr.Len() + 1) * sizeof(TCHAR);
	++NumStoredStrings;

	FChunk& Chunk = Chunks.Last();
	FStringView StoredStr(Chunk.Buffer + Chunk.Used, InStr.Len());
	FMemory::Memcpy((void*)(Chunk.Buffer + Chunk.Used), (const void*)InStr.GetData(), InStr.Len() * sizeof(TCHAR));
	Chunk.Used += InStr.Len();
	Chunk.Buffer[Chunk.Used] = TEXT('\0');
	Chunk.Used ++;
	Cache.Add(Hash, StoredStr);
	return StoredStr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void FAssetTableStringStore::AddChunk()
{
	FChunk& Chunk = Chunks.AddDefaulted_GetRef();
	Chunk.Buffer = new TCHAR[ChunkBufferLen];
	Chunk.Used = 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void FAssetTableStringStore::EnumerateStrings(TFunction<void(const FStringView Str)> Callback) const
{
	for (const auto& Pair : Cache)
	{
		Callback(Pair.Value);
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// FAssetTable
////////////////////////////////////////////////////////////////////////////////////////////////////

FAssetTable::FAssetTable()
{
//#define RUN_ASSET_TRAVERSAL_TESTS
#ifdef RUN_ASSET_TRAVERSAL_TESTS
	for (int32 i = 0; i < 9; i++)
	{
		FAssetTableRow& Asset = Assets.AddDefaulted_GetRef();
		int32 Id = i;
		int32 Id2 = 10 * i;

		Asset.Type = StoreStr(FString::Printf(TEXT("Type%02d"), Id % 10));
		Asset.Name = StoreStr(FString::Printf(TEXT("Name%d"), Id));
		Asset.Path = StoreStr(FString::Printf(TEXT("A%02d/B%02d/C%02d/D%02d"), Id % 11, Id % 13, Id % 17, Id % 19));
		Asset.PrimaryType = StoreStr(FString::Printf(TEXT("PT_%02d"), Id2 % 10));
		Asset.PrimaryName = StoreStr(FString::Printf(TEXT("PN%d"), Id2));
		Asset.TotalUsageCount = 10;
		Asset.StagedCompressedSize = 1;
		Asset.NativeClass = StoreStr(FString::Printf(TEXT("NativeClass%02d"), (Id * Id * Id) % 8));
		Asset.GameFeaturePlugin = StoreStr(TEXT("MockGFP"));
	}

	for (int32 TestIndex = 0; TestIndex < 5; TestIndex++)
	{
		TArray<TSet<int32>> UniqueDependencies;
		UniqueDependencies.SetNum(Assets.Num());
		TArray<TSet<int32>> SharedDependencies;
		SharedDependencies.SetNum(Assets.Num());
		// Reset dependencies for next test
		for (int32 AssetIndex = 0; AssetIndex < Assets.Num(); AssetIndex++)
		{
			Assets[AssetIndex].Dependencies.Empty();
			Assets[AssetIndex].Referencers.Empty();
		}

		if (TestIndex == 0)
		{
			// 0-->1-->2-->3, 4-->3
			Assets[0].Dependencies.Add(1);
			Assets[1].Dependencies.Add(2);
			Assets[2].Dependencies.Add(3);
			Assets[4].Dependencies.Add(3);
			Assets[1].Referencers.Add(0);
			Assets[2].Referencers.Add(1);
			Assets[3].Referencers.Add(2);
			Assets[3].Referencers.Add(4);

			// Add the expected results in order 0-->4
			UniqueDependencies[0] = TSet<int32>{1, 2};
			UniqueDependencies[1] = TSet<int32>{2};
			UniqueDependencies[2] = TSet<int32>{};
			UniqueDependencies[3] = TSet<int32>{};
			UniqueDependencies[4] = TSet<int32>{};

			SharedDependencies[0] = TSet<int32>{3};
			SharedDependencies[1] = TSet<int32>{3};
			SharedDependencies[2] = TSet<int32>{3};
			SharedDependencies[3] = TSet<int32>{};
			SharedDependencies[4] = TSet<int32>{3};
		}
		else if (TestIndex == 1)
		{
			// 0-->1-->2-->3, 4-->0
			Assets[0].Dependencies.Add(1);
			Assets[1].Dependencies.Add(2);
			Assets[2].Dependencies.Add(3);
			Assets[4].Dependencies.Add(0);
			Assets[1].Referencers.Add(0);
			Assets[2].Referencers.Add(1);
			Assets[3].Referencers.Add(2);
			Assets[0].Referencers.Add(4);

			// Add the expected results in order 0-->4
			UniqueDependencies[0] = TSet<int32>{1, 2, 3};
			UniqueDependencies[1] = TSet<int32>{2, 3};
			UniqueDependencies[2] = TSet<int32>{3};
			UniqueDependencies[3] = TSet<int32>{};
			UniqueDependencies[4] = TSet<int32>{0, 1, 2, 3};
   
			SharedDependencies[0] = TSet<int32>{};
			SharedDependencies[1] = TSet<int32>{};
			SharedDependencies[2] = TSet<int32>{};
			SharedDependencies[3] = TSet<int32>{};
			SharedDependencies[4] = TSet<int32>{};
		}
		else if (TestIndex == 2)
		{
			// 0-->1-->2-->3-->0, 4-->2
			Assets[0].Dependencies.Add(1);
			Assets[1].Dependencies.Add(2);
			Assets[2].Dependencies.Add(3);
			Assets[3].Dependencies.Add(0);
			Assets[4].Dependencies.Add(2);
			Assets[1].Referencers.Add(0);
			Assets[2].Referencers.Add(1);
			Assets[2].Referencers.Add(4);
			Assets[3].Referencers.Add(2);
			Assets[0].Referencers.Add(3);

			// Add the expected results in order 0-->4
			UniqueDependencies[0] = TSet<int32>{1};
			UniqueDependencies[1] = TSet<int32>{};
			UniqueDependencies[2] = TSet<int32>{3, 0, 1};
			UniqueDependencies[3] = TSet<int32>{0, 1};

			// This is an interesting result. When we traverse the graph for Element 4
			// we find 4-->2-->3-->0-->1-->[terminate loop]. From that point of view,
			// '2' is not shared because its other referencer is part of 4's dependency chain
			// therefore everything is treated as a unique dependency of 4.
			UniqueDependencies[4] = TSet<int32>{2, 3, 0, 1};

			SharedDependencies[0] = TSet<int32>{2, 3};
			SharedDependencies[1] = TSet<int32>{2, 3, 0};
			SharedDependencies[2] = TSet<int32>{};
			SharedDependencies[3] = TSet<int32>{2};
			SharedDependencies[4] = TSet<int32>{};
		}
		else if (TestIndex == 3)
		{
			// 0-->1-->2-->3-->0, 4-->2, 5-->2
			Assets[0].Dependencies.Add(1);
			Assets[1].Dependencies.Add(2);
			Assets[2].Dependencies.Add(3);
			Assets[3].Dependencies.Add(0);
			Assets[4].Dependencies.Add(2);
			Assets[5].Dependencies.Add(2);
			Assets[1].Referencers.Add(0);
			Assets[2].Referencers.Add(1);
			Assets[2].Referencers.Add(4);
			Assets[2].Referencers.Add(5);
			Assets[3].Referencers.Add(2);
			Assets[0].Referencers.Add(3);

			// Add the expected results in order 0-->4
			UniqueDependencies[0] = TSet<int32>{ 1 };
			UniqueDependencies[1] = TSet<int32>{};
			UniqueDependencies[2] = TSet<int32>{ 3, 0, 1 };
			UniqueDependencies[3] = TSet<int32>{ 0, 1 };

			// Unlike the previous test, since 5 also references 2, 
			// 4 will see itself as having no unique dependencies
			UniqueDependencies[4] = TSet<int32>{};
			UniqueDependencies[5] = TSet<int32>{};

			SharedDependencies[0] = TSet<int32>{ 2, 3 };
			SharedDependencies[1] = TSet<int32>{ 2, 3, 0 };
			SharedDependencies[2] = TSet<int32>{};
			SharedDependencies[3] = TSet<int32>{ 2 };
			SharedDependencies[4] = TSet<int32>{2, 3, 0, 1};
			SharedDependencies[5] = TSet<int32>{2, 3, 0, 1};
		}
		else if (TestIndex == 4)
		{
			// 0 --> 1 --> 6
			// 0 --> 2 --> 6
			// 0 --> 7
			// 0 --> 5 --> 8
			// 3 --> 2 --> 6
			// 3 --> 5 --> 8
			// 2 --> 5 --> 8
			// 3 --> 4
			// 2 --> 8

			Assets[0].Dependencies.Add(1);
			Assets[0].Dependencies.Add(2);
			Assets[1].Dependencies.Add(6);
			Assets[0].Dependencies.Add(7);
			Assets[3].Dependencies.Add(2);
			Assets[2].Dependencies.Add(6);
			Assets[3].Dependencies.Add(5);
			Assets[2].Dependencies.Add(5);
			Assets[0].Dependencies.Add(5);
			Assets[3].Dependencies.Add(4);
			Assets[5].Dependencies.Add(8);
			Assets[2].Dependencies.Add(8);

			Assets[1].Referencers.Add(0);
			Assets[2].Referencers.Add(0);
			Assets[6].Referencers.Add(1);
			Assets[7].Referencers.Add(0);
			Assets[2].Referencers.Add(3);
			Assets[6].Referencers.Add(2);
			Assets[5].Referencers.Add(3);
			Assets[5].Referencers.Add(2);
			Assets[5].Referencers.Add(0);
			Assets[4].Referencers.Add(3);
			Assets[8].Referencers.Add(2);
			Assets[8].Referencers.Add(5);

			UniqueDependencies[0] = TSet<int32>{ 1, 7 };
			UniqueDependencies[1] = TSet<int32>{};
			UniqueDependencies[2] = TSet<int32>{};
			UniqueDependencies[3] = TSet<int32>{ 4 };
			UniqueDependencies[4] = TSet<int32>{};
			UniqueDependencies[5] = TSet<int32>{};
			UniqueDependencies[6] = TSet<int32>{};
			UniqueDependencies[7] = TSet<int32>{};
			UniqueDependencies[8] = TSet<int32>{};

			SharedDependencies[0] = TSet<int32>{ 2, 6, 5, 8 };
			SharedDependencies[1] = TSet<int32>{ 6 };
			SharedDependencies[2] = TSet<int32>{ 5, 6, 8 };
			SharedDependencies[3] = TSet<int32>{ 2, 5, 8, 6 };
			SharedDependencies[4] = TSet<int32>{};
			SharedDependencies[5] = TSet<int32>{ 8 };
			SharedDependencies[6] = TSet<int32>{};
			SharedDependencies[7] = TSet<int32>{};
			SharedDependencies[8] = TSet<int32>{};

		}

		for (int32 AssetIndex = 0; AssetIndex < Assets.Num(); AssetIndex++)
		{
			TSet<int32> DiscoveredUniqueDependencies;
			TSet<int32> DiscoveredSharedDependencies;
			Assets[AssetIndex].ComputeDependencySizes(this, AssetIndex, &DiscoveredUniqueDependencies, &DiscoveredSharedDependencies);
			ensureAlways(DiscoveredUniqueDependencies.Num() == UniqueDependencies[AssetIndex].Num()
				&& DiscoveredUniqueDependencies.Intersect(UniqueDependencies[AssetIndex]).Num() == DiscoveredUniqueDependencies.Num());
			ensureAlways(DiscoveredSharedDependencies.Num() == SharedDependencies[AssetIndex].Num()
				&& DiscoveredSharedDependencies.Intersect(SharedDependencies[AssetIndex]).Num() == DiscoveredSharedDependencies.Num());
		}
	}
	Assets.Empty();
#endif
#if 0 // debug, mock data

	VisibleAssetCount = 100;
	constexpr int32 HiddenAssetCount = 50;
	const int32 TotalAssetCount = VisibleAssetCount + HiddenAssetCount;
	
	// Create assets.
	Assets.Reserve(TotalAssetCount);
	for (int32 AssetIndex = 0; AssetIndex < TotalAssetCount; ++AssetIndex)
	{
		FAssetTableRow& Asset = Assets.AddDefaulted_GetRef();

		int32 Id = FMath::Rand();
		int32 Id2 = FMath::Rand();

		Asset.Type = FString::Printf(TEXT("Type%02d"), Id % 10);
		Asset.Name = FString::Printf(TEXT("Name%d"), Id);
		Asset.Path = FString::Printf(TEXT("A%02d/B%02d/C%02d/D%02d"), Id % 11, Id % 13, Id % 17, Id % 19);
		Asset.PrimaryType = FString::Printf(TEXT("PT_%02d"), Id2 % 10);
		Asset.PrimaryName = FString::Printf(TEXT("PN%d"), Id2);
		//Asset.ManagedDiskSize = FMath::Abs(Id * Id);
		//Asset.DiskSize = FMath::Abs(Id * Id * Id);
		Asset.StagedCompressedSize = Asset.DiskSize / 2;
		Asset.TotalUsageCount = Id % 1000;
		//Asset.CookRule = FString::Printf(TEXT("CookRule%02d"), (Id * Id) % 8);
		//Asset.Chunks = FString::Printf(TEXT("Chunks%02d"), (Id * Id + 1) % 41);
		Asset.NativeClass = FString::Printf(TEXT("NativeClass%02d"), (Id * Id * Id) % 8);
		Asset.GameFeaturePlugin = FString::Printf(TEXT("GFP_%02d"), (Id * Id) % 7);
	}

	// Set dependencies (only for visible assets)
	for (int32 AssetIndex = 0; AssetIndex < VisibleAssetCount; ++AssetIndex)
	{
		if (FMath::Rand() % 100 > 60) // 60% chance to have dependencies
		{
			continue;
		}

		FAssetTableRow& Asset = Assets[AssetIndex];

		int32 NumDependents = FMath::Rand() % 30; // max 30 dependent assets
		while (--NumDependents >= 0)
		{
			int32 DepIndex = -1;
			if (FMath::Rand() % 100 <= 10) // 10% chance to be another asset that is visible by default
			{
				DepIndex = FMath::Rand() % VisibleAssetCount;
			}
			else // 90% chance to be a dependet only asset (not visible by default)
			{
				DepIndex = VisibleAssetCount + FMath::Rand() % HiddenAssetCount;
			}
			if (!Asset.Dependencies.Contains(DepIndex))
			{
				Asset.Dependencies.Add(DepIndex);
			}
		}
	}

#endif
}

////////////////////////////////////////////////////////////////////////////////////////////////////

FAssetTable::~FAssetTable()
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void FAssetTable::Reset()
{
	//...

	FTable::Reset();

	AddDefaultColumns();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void FAssetTable::AddDefaultColumns()
{
	using namespace UE::Insights;

	//////////////////////////////////////////////////
	// Hierarchy Column
	{
		const int32 HierarchyColumnIndex = -1;
		const TCHAR* HierarchyColumnName = nullptr;
		AddHierarchyColumn(HierarchyColumnIndex, HierarchyColumnName);

		const TSharedRef<FTableColumn>& ColumnRef = GetColumns()[0];
		ColumnRef->SetInitialWidth(200.0f);
		ColumnRef->SetShortName(LOCTEXT("HierarchyColumnName", "Hierarchy"));
		ColumnRef->SetTitleName(LOCTEXT("HierarchyColumnTitle", "Asset Hierarchy"));
		ColumnRef->SetDescription(LOCTEXT("HierarchyColumnDesc", "Hierarchy of the asset tree"));
	}

	int32 ColumnIndex = 0;

	//////////////////////////////////////////////////
	// Count Column
	{
		TSharedRef<FTableColumn> ColumnRef = MakeShared<FTableColumn>(FAssetTableColumns::CountColumnId);
		FTableColumn& Column = *ColumnRef;

		Column.SetIndex(ColumnIndex++);

		Column.SetShortName(LOCTEXT("CountColumnName", "Count"));
		Column.SetTitleName(LOCTEXT("CountColumnTitle", "Asset Count"));
		Column.SetDescription(LOCTEXT("CountColumnDesc", "Number of assets"));

		Column.SetFlags(ETableColumnFlags::ShouldBeVisible | ETableColumnFlags::CanBeHidden | ETableColumnFlags::CanBeFiltered);

		Column.SetHorizontalAlignment(HAlign_Right);
		Column.SetInitialWidth(100.0f);

		Column.SetDataType(ETableCellDataType::Int64);

		class FAssetCountValueGetter : public FTableCellValueGetter
		{
		public:
			virtual const TOptional<FTableCellValue> GetValue(const FTableColumn& Column, const FBaseTreeNode& Node) const override
			{
				if (Node.IsGroup())
				{
					const FTableTreeNode& NodePtr = static_cast<const FTableTreeNode&>(Node);
					if (NodePtr.HasAggregatedValue(Column.GetId()))
					{
						return NodePtr.GetAggregatedValue(Column.GetId());
					}
				}
				else //if (Node->Is<FAssetTreeNode>())
				{
					//const FAssetTreeNode& TreeNode = static_cast<const FAssetTreeNode&>(Node);
					//const FAssetTableRow& Asset = TreeNode.GetAssetChecked();
					return FTableCellValue((int64)1);
				}

				return TOptional<FTableCellValue>();
			}
		};
		TSharedRef<ITableCellValueGetter> Getter = MakeShared<FAssetCountValueGetter>();
		Column.SetValueGetter(Getter);

		TSharedRef<ITableCellValueFormatter> Formatter = MakeShared<FInt64ValueFormatterAsNumber>();
		Column.SetValueFormatter(Formatter);

		TSharedRef<ITableCellValueSorter> Sorter = MakeShared<FSorterByInt64Value>(ColumnRef);
		Column.SetValueSorter(Sorter);
		Column.SetInitialSortMode(EColumnSortMode::Descending);

		Column.SetAggregation(ETableColumnAggregation::Sum);

		AddColumn(ColumnRef);
	}
	//////////////////////////////////////////////////
	// Name Column
	{
		TSharedRef<FTableColumn> ColumnRef = MakeShared<FTableColumn>(FAssetTableColumns::NameColumnId);
		FTableColumn& Column = *ColumnRef;

		Column.SetIndex(ColumnIndex++);

		Column.SetShortName(LOCTEXT("NameColumnName", "Name"));
		Column.SetTitleName(LOCTEXT("NameColumnTitle", "Name"));
		Column.SetDescription(LOCTEXT("NameColumnDesc", "Asset's name"));

		Column.SetFlags(ETableColumnFlags::ShouldBeVisible | ETableColumnFlags::CanBeHidden | ETableColumnFlags::CanBeFiltered);

		Column.SetHorizontalAlignment(HAlign_Left);
		Column.SetInitialWidth(120.0f);

		Column.SetDataType(ETableCellDataType::CString);

		class FAssetNameValueGetter : public FTableCellValueGetter
		{
		public:
			virtual const TOptional<FTableCellValue> GetValue(const FTableColumn& Column, const FBaseTreeNode& Node) const
			{
				if (Node.IsGroup())
				{
					const FTableTreeNode& NodePtr = static_cast<const FTableTreeNode&>(Node);
					if (NodePtr.HasAggregatedValue(Column.GetId()))
					{
						return NodePtr.GetAggregatedValue(Column.GetId());
					}
				}
				else //if (Node->Is<FAssetTreeNode>())
				{
					const FAssetTreeNode& TreeNode = static_cast<const FAssetTreeNode&>(Node);
					const FAssetTableRow& Asset = TreeNode.GetAssetChecked();
					return FTableCellValue(Asset.GetName());
				}

				return TOptional<FTableCellValue>();
			}
		};
		TSharedRef<ITableCellValueGetter> Getter = MakeShared<FAssetNameValueGetter>();
		Column.SetValueGetter(Getter);

		TSharedRef<ITableCellValueFormatter> Formatter = MakeShared<FCStringValueFormatterAsText>();
		Column.SetValueFormatter(Formatter);

		TSharedRef<ITableCellValueSorter> Sorter = MakeShared<FSorterByCStringValue>(ColumnRef);
		Column.SetValueSorter(Sorter);

		Column.SetAggregation(ETableColumnAggregation::SameValue);

		AddColumn(ColumnRef);
	}
	//////////////////////////////////////////////////
	// Type Column
	{
		TSharedRef<FTableColumn> ColumnRef = MakeShared<FTableColumn>(FAssetTableColumns::TypeColumnId);
		FTableColumn& Column = *ColumnRef;

		Column.SetIndex(ColumnIndex++);

		Column.SetShortName(LOCTEXT("TypeColumnName", "Type"));
		Column.SetTitleName(LOCTEXT("TypeColumnTitle", "Type"));
		Column.SetDescription(LOCTEXT("TypeColumnDesc", "Asset's type"));

		Column.SetFlags(ETableColumnFlags::ShouldBeVisible | ETableColumnFlags::CanBeHidden | ETableColumnFlags::CanBeFiltered);

		Column.SetHorizontalAlignment(HAlign_Left);
		Column.SetInitialWidth(120.0f);

		Column.SetDataType(ETableCellDataType::CString);

		class FAssetTypeValueGetter : public FTableCellValueGetter
		{
		public:
			virtual const TOptional<FTableCellValue> GetValue(const FTableColumn& Column, const FBaseTreeNode& Node) const
			{
				if (Node.IsGroup())
				{
					const FTableTreeNode& NodePtr = static_cast<const FTableTreeNode&>(Node);
					if (NodePtr.HasAggregatedValue(Column.GetId()))
					{
						return NodePtr.GetAggregatedValue(Column.GetId());
					}
				}
				else //if (Node->Is<FAssetTreeNode>())
				{
					const FAssetTreeNode& TreeNode = static_cast<const FAssetTreeNode&>(Node);
					const FAssetTableRow& Asset = TreeNode.GetAssetChecked();
					return FTableCellValue(Asset.GetType());
				}

				return TOptional<FTableCellValue>();
			}
		};
		TSharedRef<ITableCellValueGetter> Getter = MakeShared<FAssetTypeValueGetter>();
		Column.SetValueGetter(Getter);

		TSharedRef<ITableCellValueFormatter> Formatter = MakeShared<FCStringValueFormatterAsText>();
		Column.SetValueFormatter(Formatter);

		TSharedRef<ITableCellValueSorter> Sorter = MakeShared<FSorterByCStringValue>(ColumnRef);
		Column.SetValueSorter(Sorter);

		Column.SetAggregation(ETableColumnAggregation::SameValue);

		AddColumn(ColumnRef);
	}
	//////////////////////////////////////////////////
	// Path Column
	{
		TSharedRef<FTableColumn> ColumnRef = MakeShared<FTableColumn>(FAssetTableColumns::PathColumnId);
		FTableColumn& Column = *ColumnRef;

		Column.SetIndex(ColumnIndex++);

		Column.SetShortName(LOCTEXT("PathColumnName", "Path"));
		Column.SetTitleName(LOCTEXT("PathColumnTitle", "Path"));
		Column.SetDescription(LOCTEXT("PathColumnDesc", "Asset's path"));

		Column.SetFlags(ETableColumnFlags::ShouldBeVisible | ETableColumnFlags::CanBeHidden | ETableColumnFlags::CanBeFiltered);

		Column.SetHorizontalAlignment(HAlign_Left);
		Column.SetInitialWidth(120.0f);

		Column.SetDataType(ETableCellDataType::CString);

		class FAssetPathValueGetter : public FTableCellValueGetter
		{
		public:
			virtual const TOptional<FTableCellValue> GetValue(const FTableColumn& Column, const FBaseTreeNode& Node) const
			{
				if (Node.IsGroup())
				{
					const FTableTreeNode& NodePtr = static_cast<const FTableTreeNode&>(Node);
					if (NodePtr.HasAggregatedValue(Column.GetId()))
					{
						return NodePtr.GetAggregatedValue(Column.GetId());
					}
				}
				else //if (Node->Is<FAssetTreeNode>())
				{
					const FAssetTreeNode& TreeNode = static_cast<const FAssetTreeNode&>(Node);
					const FAssetTableRow& Asset = TreeNode.GetAssetChecked();
					return FTableCellValue(Asset.GetPath());
				}

				return TOptional<FTableCellValue>();
			}
		};
		TSharedRef<ITableCellValueGetter> Getter = MakeShared<FAssetPathValueGetter>();
		Column.SetValueGetter(Getter);

		TSharedRef<ITableCellValueFormatter> Formatter = MakeShared<FCStringValueFormatterAsText>();
		Column.SetValueFormatter(Formatter);

		TSharedRef<ITableCellValueSorter> Sorter = MakeShared<FSorterByCStringValue>(ColumnRef);
		Column.SetValueSorter(Sorter);

		Column.SetAggregation(ETableColumnAggregation::SameValue);

		AddColumn(ColumnRef);
	}
	//////////////////////////////////////////////////
	// Primary Type Column
	{
		TSharedRef<FTableColumn> ColumnRef = MakeShared<FTableColumn>(FAssetTableColumns::PrimaryTypeColumnId);
		FTableColumn& Column = *ColumnRef;

		Column.SetIndex(ColumnIndex++);

		Column.SetShortName(LOCTEXT("PrimaryTypeColumnName", "Primary Type"));
		Column.SetTitleName(LOCTEXT("PrimaryTypeColumnTitle", "Primary Type"));
		Column.SetDescription(LOCTEXT("PrimaryTypeColumnDesc", "Primary Asset Type of this asset, if set"));

		Column.SetFlags(ETableColumnFlags::ShouldBeVisible | ETableColumnFlags::CanBeHidden | ETableColumnFlags::CanBeFiltered);

		Column.SetHorizontalAlignment(HAlign_Left);
		Column.SetInitialWidth(120.0f);

		Column.SetDataType(ETableCellDataType::CString);

		class FAssetPrimaryTypeValueGetter : public FTableCellValueGetter
		{
		public:
			virtual const TOptional<FTableCellValue> GetValue(const FTableColumn& Column, const FBaseTreeNode& Node) const
			{
				if (Node.IsGroup())
				{
					const FTableTreeNode& NodePtr = static_cast<const FTableTreeNode&>(Node);
					if (NodePtr.HasAggregatedValue(Column.GetId()))
					{
						return NodePtr.GetAggregatedValue(Column.GetId());
					}
				}
				else //if (Node->Is<FAssetTreeNode>())
				{
					const FAssetTreeNode& TreeNode = static_cast<const FAssetTreeNode&>(Node);
					const FAssetTableRow& Asset = TreeNode.GetAssetChecked();
					return FTableCellValue(Asset.GetPrimaryType());
				}

				return TOptional<FTableCellValue>();
			}
		};
		TSharedRef<ITableCellValueGetter> Getter = MakeShared<FAssetPrimaryTypeValueGetter>();
		Column.SetValueGetter(Getter);

		TSharedRef<ITableCellValueFormatter> Formatter = MakeShared<FCStringValueFormatterAsText>();
		Column.SetValueFormatter(Formatter);

		TSharedRef<ITableCellValueSorter> Sorter = MakeShared<FSorterByCStringValue>(ColumnRef);
		Column.SetValueSorter(Sorter);

		Column.SetAggregation(ETableColumnAggregation::SameValue);

		AddColumn(ColumnRef);
	}
	//////////////////////////////////////////////////
	// Primary Name Column
	{
		TSharedRef<FTableColumn> ColumnRef = MakeShared<FTableColumn>(FAssetTableColumns::PrimaryNameColumnId);
		FTableColumn& Column = *ColumnRef;

		Column.SetIndex(ColumnIndex++);

		Column.SetShortName(LOCTEXT("PrimaryNameColumnName", "Primary Name"));
		Column.SetTitleName(LOCTEXT("PrimaryNameColumnTitle", "Primary Name"));
		Column.SetDescription(LOCTEXT("PrimaryNameColumnDesc", "Primary Asset Name of this asset, if set"));

		Column.SetFlags(ETableColumnFlags::ShouldBeVisible | ETableColumnFlags::CanBeHidden | ETableColumnFlags::CanBeFiltered);

		Column.SetHorizontalAlignment(HAlign_Left);
		Column.SetInitialWidth(120.0f);

		Column.SetDataType(ETableCellDataType::CString);

		class FAssetPrimaryNameValueGetter : public FTableCellValueGetter
		{
		public:
			virtual const TOptional<FTableCellValue> GetValue(const FTableColumn& Column, const FBaseTreeNode& Node) const
			{
				if (Node.IsGroup())
				{
					const FTableTreeNode& NodePtr = static_cast<const FTableTreeNode&>(Node);
					if (NodePtr.HasAggregatedValue(Column.GetId()))
					{
						return NodePtr.GetAggregatedValue(Column.GetId());
					}
				}
				else //if (Node->Is<FAssetTreeNode>())
				{
					const FAssetTreeNode& TreeNode = static_cast<const FAssetTreeNode&>(Node);
					const FAssetTableRow& Asset = TreeNode.GetAssetChecked();
					return FTableCellValue(Asset.GetPrimaryName());
				}

				return TOptional<FTableCellValue>();
			}
		};
		TSharedRef<ITableCellValueGetter> Getter = MakeShared<FAssetPrimaryNameValueGetter>();
		Column.SetValueGetter(Getter);

		TSharedRef<ITableCellValueFormatter> Formatter = MakeShared<FCStringValueFormatterAsText>();
		Column.SetValueFormatter(Formatter);

		TSharedRef<ITableCellValueSorter> Sorter = MakeShared<FSorterByCStringValue>(ColumnRef);
		Column.SetValueSorter(Sorter);

		Column.SetAggregation(ETableColumnAggregation::SameValue);

		AddColumn(ColumnRef);
	}
	//////////////////////////////////////////////////
	// Staged Compressed Size Column
	{
		TSharedRef<FTableColumn> ColumnRef = MakeShared<FTableColumn>(FAssetTableColumns::StagedCompressedSizeColumnId);
		FTableColumn& Column = *ColumnRef;

		Column.SetIndex(ColumnIndex++);

		Column.SetShortName(LOCTEXT("StagedCompressedSizeColumnName", "Staged Compressed Size"));
		Column.SetTitleName(LOCTEXT("StagedCompressedSizeColumnTitle", "Staged Compressed Size"));
		Column.SetDescription(LOCTEXT("StagedCompressedSizeColumnDesc", "Compressed size of iostore chunks for this asset's package. Only visible after staging."));

		Column.SetFlags(ETableColumnFlags::ShouldBeVisible | ETableColumnFlags::CanBeHidden | ETableColumnFlags::CanBeFiltered);

		Column.SetHorizontalAlignment(HAlign_Right);
		Column.SetInitialWidth(100.0f);

		Column.SetDataType(ETableCellDataType::Int64);

		class FStagedCompressedSizeValueGetter : public FTableCellValueGetter
		{
		public:
			virtual const TOptional<FTableCellValue> GetValue(const FTableColumn& Column, const FBaseTreeNode& Node) const override
			{
				if (Node.IsGroup())
				{
					const FTableTreeNode& NodePtr = static_cast<const FTableTreeNode&>(Node);
					if (NodePtr.HasAggregatedValue(Column.GetId()))
					{
						return NodePtr.GetAggregatedValue(Column.GetId());
					}
				}
				else //if (Node->Is<FAssetTreeNode>())
				{
					const FAssetTreeNode& TreeNode = static_cast<const FAssetTreeNode&>(Node);
					const FAssetTableRow& Asset = TreeNode.GetAssetChecked();
					return FTableCellValue(static_cast<int64>(Asset.GetStagedCompressedSize()));
				}

				return TOptional<FTableCellValue>();
			}
		};
		TSharedRef<ITableCellValueGetter> Getter = MakeShared<FStagedCompressedSizeValueGetter>();
		Column.SetValueGetter(Getter);

		TSharedRef<ITableCellValueFormatter> Formatter = MakeShared<FInt64ValueFormatterAsMemory>();
		Column.SetValueFormatter(Formatter);

		TSharedRef<ITableCellValueSorter> Sorter = MakeShared<FSorterByInt64Value>(ColumnRef);
		Column.SetValueSorter(Sorter);
		Column.SetInitialSortMode(EColumnSortMode::Descending);

		//TSharedRef<IFilterValueConverter> Converter = MakeShared<FMemoryFilterValueConverter>();
		//Column.SetValueConverter(Converter);

		Column.SetAggregation(ETableColumnAggregation::Sum);

		AddColumn(ColumnRef);
	}
	//////////////////////////////////////////////////
	// Total Size of Unique Dependencies
	{
		TSharedRef<FTableColumn> ColumnRef = MakeShared<FTableColumn>(FAssetTableColumns::TotalSizeUniqueDependenciesColumnId);
		FTableColumn& Column = *ColumnRef;

		Column.SetIndex(ColumnIndex++);

		Column.SetShortName(LOCTEXT("TotalSizeUniqueDependenciesColumnName", "Total Unique Dependency Size"));
		Column.SetTitleName(LOCTEXT("TotalSizeUniqueDependenciesColumnTitle", "Total Unique Dependency Size"));
		Column.SetDescription(LOCTEXT("TotalSizeUniqueDependenciesColumnIdDesc", "Sum of the staged compressed sizes of all dependencies of this asset, counted only once"));

		Column.SetFlags(ETableColumnFlags::ShouldBeVisible | ETableColumnFlags::CanBeHidden | ETableColumnFlags::CanBeFiltered);

		Column.SetHorizontalAlignment(HAlign_Right);
		Column.SetInitialWidth(120.0f);

		Column.SetDataType(ETableCellDataType::Int64);

		class FTotalSizeUniqueDependenciesValueGetter : public FTableCellValueGetter
		{
		public:
			virtual const TOptional<FTableCellValue> GetValue(const FTableColumn& Column, const FBaseTreeNode& Node) const override
			{
				if (Node.IsGroup())
				{
					const FTableTreeNode& NodePtr = static_cast<const FTableTreeNode&>(Node);
					if (NodePtr.HasAggregatedValue(Column.GetId()))
					{
						return NodePtr.GetAggregatedValue(Column.GetId());
					}
				}
				else //if (Node->Is<FAssetTreeNode>())
				{
					const FAssetTreeNode& TreeNode = static_cast<const FAssetTreeNode&>(Node);
					const FAssetTableRow& Asset = TreeNode.GetAssetChecked();
					return FTableCellValue(static_cast<int64>(Asset.GetOrComputeTotalSizeUniqueDependencies(&TreeNode.GetAssetTableChecked(), TreeNode.GetRowIndex())));
				}

				return TOptional<FTableCellValue>();
			}
		};
		TSharedRef<ITableCellValueGetter> Getter = MakeShared<FTotalSizeUniqueDependenciesValueGetter>();
		Column.SetValueGetter(Getter);

		TSharedRef<ITableCellValueFormatter> Formatter = MakeShared<FInt64ValueFormatterAsMemory>();
		Column.SetValueFormatter(Formatter);

		TSharedRef<ITableCellValueSorter> Sorter = MakeShared<FSorterByInt64Value>(ColumnRef);
		Column.SetValueSorter(Sorter);
		Column.SetInitialSortMode(EColumnSortMode::Descending);

		//TSharedRef<IFilterValueConverter> Converter = MakeShared<FMemoryFilterValueConverter>();
		//Column.SetValueConverter(Converter);

		Column.SetAggregation(ETableColumnAggregation::None);

		AddColumn(ColumnRef);
	}
	//////////////////////////////////////////////////
	// Total Size of Other Dependencies
	{
		TSharedRef<FTableColumn> ColumnRef = MakeShared<FTableColumn>(FAssetTableColumns::TotalSizeOtherDependenciesColumnId);
		FTableColumn& Column = *ColumnRef;

		Column.SetIndex(ColumnIndex++);

		Column.SetShortName(LOCTEXT("TotalSizeOtherDependenciesColumnName", "Total Other Dependency Size"));
		Column.SetTitleName(LOCTEXT("TotalSizeOtherDependenciesColumnTitle", "Total Other Dependency Size"));
		Column.SetDescription(LOCTEXT("TotalSizeOtherDependenciesColumnIdDesc", "Sum of the staged compressed sizes of all dependencies of this asset which are shared by other assets directly or indirectly, counted only once"));

		Column.SetFlags(ETableColumnFlags::ShouldBeVisible | ETableColumnFlags::CanBeHidden | ETableColumnFlags::CanBeFiltered);

		Column.SetHorizontalAlignment(HAlign_Right);
		Column.SetInitialWidth(120.0f);

		Column.SetDataType(ETableCellDataType::Int64);

		class FTotalSizeUniqueDependenciesValueGetter : public FTableCellValueGetter
		{
		public:
			virtual const TOptional<FTableCellValue> GetValue(const FTableColumn& Column, const FBaseTreeNode& Node) const override
			{
				if (Node.IsGroup())
				{
					const FTableTreeNode& NodePtr = static_cast<const FTableTreeNode&>(Node);
					if (NodePtr.HasAggregatedValue(Column.GetId()))
					{
						return NodePtr.GetAggregatedValue(Column.GetId());
					}
				}
				else //if (Node->Is<FAssetTreeNode>())
				{
					const FAssetTreeNode& TreeNode = static_cast<const FAssetTreeNode&>(Node);
					const FAssetTableRow& Asset = TreeNode.GetAssetChecked();
					return FTableCellValue(static_cast<int64>(Asset.GetOrComputeTotalSizeOtherDependencies(&TreeNode.GetAssetTableChecked(), TreeNode.GetRowIndex())));
				}

				return TOptional<FTableCellValue>();
			}
		};
		TSharedRef<ITableCellValueGetter> Getter = MakeShared<FTotalSizeUniqueDependenciesValueGetter>();
		Column.SetValueGetter(Getter);

		TSharedRef<ITableCellValueFormatter> Formatter = MakeShared<FInt64ValueFormatterAsMemory>();
		Column.SetValueFormatter(Formatter);

		TSharedRef<ITableCellValueSorter> Sorter = MakeShared<FSorterByInt64Value>(ColumnRef);
		Column.SetValueSorter(Sorter);
		Column.SetInitialSortMode(EColumnSortMode::Descending);

		//TSharedRef<IFilterValueConverter> Converter = MakeShared<FMemoryFilterValueConverter>();
		//Column.SetValueConverter(Converter);

		Column.SetAggregation(ETableColumnAggregation::None);

		AddColumn(ColumnRef);
	}
	//////////////////////////////////////////////////
	// Total Usage Count Column
	{
		TSharedRef<FTableColumn> ColumnRef = MakeShared<FTableColumn>(FAssetTableColumns::TotalUsageCountColumnId);
		FTableColumn& Column = *ColumnRef;

		Column.SetIndex(ColumnIndex++);

		Column.SetShortName(LOCTEXT("TotalUsageCountColumnName", "Total Usage"));
		Column.SetTitleName(LOCTEXT("TotalUsageCountColumnTitle", "Total Usage Count"));
		Column.SetDescription(LOCTEXT("TotalUsageCountColumnDesc", "Weighted count of Primary Assets that use this\nA higher usage means it's more likely to be in memory at runtime."));

		Column.SetFlags(ETableColumnFlags::ShouldBeVisible | ETableColumnFlags::CanBeHidden | ETableColumnFlags::CanBeFiltered);

		Column.SetHorizontalAlignment(HAlign_Right);
		Column.SetInitialWidth(100.0f);

		Column.SetDataType(ETableCellDataType::Int64);

		class FTotalUsageCountValueGetter : public FTableCellValueGetter
		{
		public:
			virtual const TOptional<FTableCellValue> GetValue(const FTableColumn& Column, const FBaseTreeNode& Node) const override
			{
				if (Node.IsGroup())
				{
					const FTableTreeNode& NodePtr = static_cast<const FTableTreeNode&>(Node);
					if (NodePtr.HasAggregatedValue(Column.GetId()))
					{
						return NodePtr.GetAggregatedValue(Column.GetId());
					}
				}
				else //if (Node->Is<FAssetTreeNode>())
				{
					const FAssetTreeNode& TreeNode = static_cast<const FAssetTreeNode&>(Node);
					const FAssetTableRow& Asset = TreeNode.GetAssetChecked();
					return FTableCellValue(static_cast<int64>(Asset.GetTotalUsageCount()));
				}

				return TOptional<FTableCellValue>();
			}
		};
		TSharedRef<ITableCellValueGetter> Getter = MakeShared<FTotalUsageCountValueGetter>();
		Column.SetValueGetter(Getter);

		TSharedRef<ITableCellValueFormatter> Formatter = MakeShared<FInt64ValueFormatterAsNumber>();
		Column.SetValueFormatter(Formatter);

		TSharedRef<ITableCellValueSorter> Sorter = MakeShared<FSorterByInt64Value>(ColumnRef);
		Column.SetValueSorter(Sorter);
		Column.SetInitialSortMode(EColumnSortMode::Descending);

		Column.SetAggregation(ETableColumnAggregation::Sum);

		AddColumn(ColumnRef);
	}
	//////////////////////////////////////////////////
	// Chunks Column
	{
		TSharedRef<FTableColumn> ColumnRef = MakeShared<FTableColumn>(FAssetTableColumns::ChunksColumnId);
		FTableColumn& Column = *ColumnRef;

		Column.SetIndex(ColumnIndex++);

		Column.SetShortName(LOCTEXT("ChunksColumnName", "Chunks"));
		Column.SetTitleName(LOCTEXT("ChunksColumnTitle", "Chunks"));
		Column.SetDescription(LOCTEXT("ChunksColumnDesc", "List of chunks this will be added to when cooked"));

		Column.SetFlags(ETableColumnFlags::ShouldBeVisible | ETableColumnFlags::CanBeHidden | ETableColumnFlags::CanBeFiltered);

		Column.SetHorizontalAlignment(HAlign_Left);
		Column.SetInitialWidth(120.0f);

		Column.SetDataType(ETableCellDataType::CString);

		class FChunksValueGetter : public FTableCellValueGetter
		{
		public:
			virtual const TOptional<FTableCellValue> GetValue(const FTableColumn& Column, const FBaseTreeNode& Node) const
			{
				if (Node.IsGroup())
				{
					const FTableTreeNode& NodePtr = static_cast<const FTableTreeNode&>(Node);
					if (NodePtr.HasAggregatedValue(Column.GetId()))
					{
						return NodePtr.GetAggregatedValue(Column.GetId());
					}
				}
				else //if (Node->Is<FAssetTreeNode>())
				{
					const FAssetTreeNode& TreeNode = static_cast<const FAssetTreeNode&>(Node);
					const FAssetTableRow& Asset = TreeNode.GetAssetChecked();
					return FTableCellValue(Asset.GetChunks());
				}

				return TOptional<FTableCellValue>();
			}
		};
		TSharedRef<ITableCellValueGetter> Getter = MakeShared<FChunksValueGetter>();
		Column.SetValueGetter(Getter);

		TSharedRef<ITableCellValueFormatter> Formatter = MakeShared<FCStringValueFormatterAsText>();
		Column.SetValueFormatter(Formatter);

		TSharedRef<ITableCellValueSorter> Sorter = MakeShared<FSorterByCStringValue>(ColumnRef);
		Column.SetValueSorter(Sorter);

		Column.SetAggregation(ETableColumnAggregation::SameValue);

		AddColumn(ColumnRef);
	}
	//////////////////////////////////////////////////
	// Native Class Column
	{
		TSharedRef<FTableColumn> ColumnRef = MakeShared<FTableColumn>(FAssetTableColumns::NativeClassColumnId);
		FTableColumn& Column = *ColumnRef;

		Column.SetIndex(ColumnIndex++);

		Column.SetShortName(LOCTEXT("NativeClassColumnName", "Native Class"));
		Column.SetTitleName(LOCTEXT("NativeClassColumnTitle", "Native Class"));
		Column.SetDescription(LOCTEXT("NativeClassColumnDesc", "Native class of the asset"));

		Column.SetFlags(ETableColumnFlags::ShouldBeVisible | ETableColumnFlags::CanBeHidden | ETableColumnFlags::CanBeFiltered);

		Column.SetHorizontalAlignment(HAlign_Left);
		Column.SetInitialWidth(120.0f);

		Column.SetDataType(ETableCellDataType::CString);

		class FNativeClassValueGetter : public FTableCellValueGetter
		{
		public:
			virtual const TOptional<FTableCellValue> GetValue(const FTableColumn& Column, const FBaseTreeNode& Node) const
			{
				if (Node.IsGroup())
				{
					const FTableTreeNode& NodePtr = static_cast<const FTableTreeNode&>(Node);
					if (NodePtr.HasAggregatedValue(Column.GetId()))
					{
						return NodePtr.GetAggregatedValue(Column.GetId());
					}
				}
				else //if (Node->Is<FAssetTreeNode>())
				{
					const FAssetTreeNode& TreeNode = static_cast<const FAssetTreeNode&>(Node);
					const FAssetTableRow& Asset = TreeNode.GetAssetChecked();
					return FTableCellValue(Asset.GetNativeClass());
				}

				return TOptional<FTableCellValue>();
			}
		};
		TSharedRef<ITableCellValueGetter> Getter = MakeShared<FNativeClassValueGetter>();
		Column.SetValueGetter(Getter);

		TSharedRef<ITableCellValueFormatter> Formatter = MakeShared<FCStringValueFormatterAsText>();
		Column.SetValueFormatter(Formatter);

		TSharedRef<ITableCellValueSorter> Sorter = MakeShared<FSorterByCStringValue>(ColumnRef);
		Column.SetValueSorter(Sorter);

		Column.SetAggregation(ETableColumnAggregation::SameValue);

		AddColumn(ColumnRef);
	}
	//////////////////////////////////////////////////
	// GameFeaturePlugin Column
	{
		TSharedRef<FTableColumn> ColumnRef = MakeShared<FTableColumn>(FAssetTableColumns::GameFeaturePluginColumnId);
		FTableColumn& Column = *ColumnRef;

		Column.SetIndex(ColumnIndex++);

		Column.SetShortName(LOCTEXT("GameFeaturePluginColumnName", "GameFeaturePlugin"));
		Column.SetTitleName(LOCTEXT("GameFeaturePluginColumnTitle", "GameFeaturePlugin"));
		Column.SetDescription(LOCTEXT("GameFeaturePluginColumnDesc", "GameFeaturePlugin of the asset"));

		Column.SetFlags(ETableColumnFlags::ShouldBeVisible | ETableColumnFlags::CanBeHidden | ETableColumnFlags::CanBeFiltered);

		Column.SetHorizontalAlignment(HAlign_Left);
		Column.SetInitialWidth(120.0f);

		Column.SetDataType(ETableCellDataType::CString);

		class FNativeClassValueGetter : public FTableCellValueGetter
		{
		public:
			virtual const TOptional<FTableCellValue> GetValue(const FTableColumn& Column, const FBaseTreeNode& Node) const
			{
				if (Node.IsGroup())
				{
					const FTableTreeNode& NodePtr = static_cast<const FTableTreeNode&>(Node);
					if (NodePtr.HasAggregatedValue(Column.GetId()))
					{
						return NodePtr.GetAggregatedValue(Column.GetId());
					}
				}
				else //if (Node->Is<FAssetTreeNode>())
				{
					const FAssetTreeNode& TreeNode = static_cast<const FAssetTreeNode&>(Node);
					const FAssetTableRow& Asset = TreeNode.GetAssetChecked();
					return FTableCellValue(Asset.GetGameFeaturePlugin());
				}

				return TOptional<FTableCellValue>();
			}
		};
		TSharedRef<ITableCellValueGetter> Getter = MakeShared<FNativeClassValueGetter>();
		Column.SetValueGetter(Getter);

		TSharedRef<ITableCellValueFormatter> Formatter = MakeShared<FCStringValueFormatterAsText>();
		Column.SetValueFormatter(Formatter);

		TSharedRef<ITableCellValueSorter> Sorter = MakeShared<FSorterByCStringValue>(ColumnRef);
		Column.SetValueSorter(Sorter);

		Column.SetAggregation(ETableColumnAggregation::SameValue);

		AddColumn(ColumnRef);
	}
	//////////////////////////////////////////////////
}

// Iteratively refines the dependency set into unique and shared sets. PreviouslyVisitedIndices is split in each pass, moving some elements into 
// OutExcludedIndices (preserving its original contents) and putting those that are still potentially uniquely owned by the ThisIndex asset into 
// OutIncrementallyRefinedUniqueIndices. Returns 'true' if another pass is required (i.e., if work was done).
/*static*/ bool FAssetTableRow::RefineDependencies(TSet<int32> PreviouslyVisitedIndices, FAssetTable* OwningTable, int32 ThisIndex, TSet<int32>* OutIncrementallyRefinedUniqueIndices, TSet<int32>* OutExcludedIndices)
{
	const FString& ThisGFP = OwningTable->GetAsset(ThisIndex)->GameFeaturePlugin;

	ensure(OutIncrementallyRefinedUniqueIndices != nullptr);
	ensure(OutExcludedIndices != nullptr);

	// "visit" ThisIndex to seed the exploration
	TArray<int32> IndicesToVisit = OwningTable->GetAsset(ThisIndex)->GetDependencies();

	while (IndicesToVisit.Num() > 0)
	{
		int32 CurrentIndex = IndicesToVisit.Pop(false);

		FAssetTableRow* Row = OwningTable->GetAsset(CurrentIndex);
		if (Row->GameFeaturePlugin != ThisGFP)
		{
			// Don't traverse outside this plugin
			continue;
		}

		bool ShouldIncludeInTotal = true;
		for (int32 ReferencerIndex : Row->Referencers)
		{
			if (PreviouslyVisitedIndices.Contains(ReferencerIndex) == false && ThisIndex != ReferencerIndex)
			{
				ShouldIncludeInTotal = false;
				break;
			}
		}
		if (ShouldIncludeInTotal == false)
		{
			OutExcludedIndices->Add(CurrentIndex);
			continue;
		}
		OutIncrementallyRefinedUniqueIndices->Add(CurrentIndex);

		for (int32 ChildIndex : OwningTable->GetAsset(CurrentIndex)->GetDependencies())
		{
			// Don't revisit nodes we've already visited and don't re-add ThisIndex to avoid loops (and to avoid counting ourself)
			if (!OutIncrementallyRefinedUniqueIndices->Contains(ChildIndex) && ThisIndex != ChildIndex)
			{
				IndicesToVisit.AddUnique(ChildIndex);
			}
		}
	}

	return OutIncrementallyRefinedUniqueIndices->Num() != PreviouslyVisitedIndices.Num();
}

void FAssetTableRow::ComputeDependencySizes(FAssetTable* OwningTable, int32 ThisIndex, TSet<int32>* OutUniqueDependencies/* = nullptr*/, TSet<int32>* OutSharedDependencies/* = nullptr*/) const
{
	if (OutUniqueDependencies != nullptr && OutSharedDependencies != nullptr)
	{
		TotalSizeUniqueDependencies = -1;
	}
	if (TotalSizeUniqueDependencies == -1)
	{
		TArray<int32> IndicesToVisit;
		TSet<int32> VisitedIndices;

		TotalSizeUniqueDependencies = 0;
		TotalSizeOtherDependencies = 0;
		
		// Break any loops in the dependency graph
		VisitedIndices.Add(ThisIndex);

		const FString& ThisGFP = OwningTable->GetAsset(ThisIndex)->GameFeaturePlugin;

		// Don't include this asset itself in the total, just its children
		for (int32 ChildIndex : Dependencies)
		{
			IndicesToVisit.Add(ChildIndex);
		}

		while (IndicesToVisit.Num() > 0)
		{
			int32 CurrentIndex = IndicesToVisit.Pop(false);

			FAssetTableRow* Row = OwningTable->GetAsset(CurrentIndex);
			if (Row->GameFeaturePlugin != ThisGFP)
			{
				// Don't traverse outside this plugin
				continue;
			}
			VisitedIndices.Add(CurrentIndex);

			for (int32 ChildIndex : OwningTable->GetAsset(CurrentIndex)->GetDependencies())
			{
				if (!VisitedIndices.Contains(ChildIndex))
				{
					IndicesToVisit.AddUnique(ChildIndex);
				}
			}
		}

		// Iteratively separate the graph of "all things referenced by ThisIndex, directly or indirectly"
		// into "UniqueDependencies -- things referenced ONLY by ThisIndex and by other things themselves referenced ONLY by ThisIndex"
		// and "OtherDependencies" -- things removed from the list of "all things referenced by ThisIndex" in order to identify UniqueDependencies
		TSet<int32> UniqueDependencies;
		TSet<int32> OtherDependencies;
		while (RefineDependencies(VisitedIndices, OwningTable, ThisIndex, &UniqueDependencies, &OtherDependencies))
		{
			VisitedIndices = UniqueDependencies;
			UniqueDependencies.Empty();
		}

		for (int32 Index : UniqueDependencies)
		{
			int64 DependencySize = OwningTable->GetAsset(Index)->GetStagedCompressedSize();
			TotalSizeUniqueDependencies += DependencySize;
		}
		if (OutUniqueDependencies != nullptr)
		{
			*OutUniqueDependencies = UniqueDependencies;
		}

		// Now explore all the dependencies of OtherDependencies and gather up their sizes
		// This is necessary because the process calling RefineDependencies doesn't produce a complete list of other dependencies,
		// just a partial set. By exploring the dependencies of that partial set we can find all the things that were 
		// referenced by ThisIndex but also by some other asset outside the subgraph defined by ThisIndex and its UniqueDependencies
		VisitedIndices.Empty();
		IndicesToVisit.Empty();
		for (int32 Index : OtherDependencies)
		{
			IndicesToVisit.Add(Index);
		}

		while (IndicesToVisit.Num() > 0)
		{
			int32 CurrentIndex = IndicesToVisit.Pop(false);
			FAssetTableRow* Row = OwningTable->GetAsset(CurrentIndex);
			VisitedIndices.Add(CurrentIndex);
			if (Row->GameFeaturePlugin != ThisGFP)
			{
				// Don't traverse outside this plugin
				continue;
			}
			
			int64 DependencySize = OwningTable->GetAsset(CurrentIndex)->GetStagedCompressedSize();
			TotalSizeOtherDependencies += DependencySize;
			if (OutSharedDependencies != nullptr)
			{
				OutSharedDependencies->Add(CurrentIndex);
			}

			for (int32 ChildIndex : OwningTable->GetAsset(CurrentIndex)->GetDependencies())
			{
				if (!VisitedIndices.Contains(ChildIndex) && ThisIndex != ChildIndex)
				{
					IndicesToVisit.AddUnique(ChildIndex);
				}
			}
		}
	}
}

int64 FAssetTableRow::GetOrComputeTotalSizeUniqueDependencies(FAssetTable* OwningTable, int32 ThisIndex) const
{
	if (TotalSizeUniqueDependencies == -1)
	{
		ComputeDependencySizes(OwningTable, ThisIndex);
	}
	return TotalSizeUniqueDependencies;
}

int64 FAssetTableRow::GetOrComputeTotalSizeOtherDependencies(FAssetTable* OwningTable, int32 ThisIndex) const
{
	if (TotalSizeOtherDependencies == -1)
	{
		ComputeDependencySizes(OwningTable, ThisIndex);
	}
	return TotalSizeOtherDependencies;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

#undef LOCTEXT_NAMESPACE
