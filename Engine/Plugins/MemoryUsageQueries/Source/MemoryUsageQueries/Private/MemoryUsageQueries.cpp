// Copyright Epic Games, Inc. All Rights Reserved.

#include "MemoryUsageQueries.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "MemoryUsageQueriesConfig.h"
#include "ConsoleSettings.h"
#include "MemoryUsageQueriesPrivate.h"

#include "Engine/BlueprintGeneratedClass.h"
#include "Misc/PackageName.h"
#include "Misc/WildcardString.h"
#include "Modules/ModuleManager.h"
#include "Templates/Greater.h"
#include "UObject/UObjectAllocator.h"
#include "UObject/UObjectIterator.h"

namespace MemoryUsageQueries
{

FMemoryUsageInfoProviderLLM MemoryUsageInfoProviderLLM;
IMemoryUsageInfoProvider* CurrentMemoryUsageInfoProvider = &MemoryUsageInfoProviderLLM;

} // namespace MemoryUsageQueries

struct FMemoryUsageQueriesExec : public FSelfRegisteringExec
{
	FMemoryUsageQueriesExec() {}

	virtual bool Exec(UWorld* Inworld, const TCHAR* Cmd, FOutputDevice& Ar) override;
};

struct FAssetMemoryBreakdown
{
	FAssetMemoryBreakdown() :
		ExclusiveSize(0),
		UniqueSize(0),
		SharedSize(0),
		TotalSize(0)
	{

	}

	uint64 ExclusiveSize;
	uint64 UniqueSize;
	uint64 SharedSize;
	uint64 TotalSize;
};

struct FAssetMemoryDetails
{
	FAssetMemoryDetails() :
		UniqueRefCount(0),
		SharedRefCount(0)
	{
	}

	// Assets Package Name
	FName PackageName;

	FAssetMemoryBreakdown MemoryBreakdown;

	// List of dependencies for this asset
	TSet<FName> Dependencies;

	TMap<FName, FAssetMemoryBreakdown> DependenciesToMemoryMap;

	int32 UniqueRefCount;
	int32 SharedRefCount;
};

static FMemoryUsageQueriesExec DebugSettinsSubsystemExecInstance;
static int32 DefaultResultLimit = 15;

bool FMemoryUsageQueriesExec::Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar)
{
	if (FParse::Command(&Cmd, TEXT("MemQuery")))
	{
		Ar.Logf(TEXT("MemQuery: %s"), Cmd);

		const bool bTruncate = !FParse::Param(Cmd, TEXT("notrunc"));
		const bool bCSV = FParse::Param(Cmd, TEXT("csv"));

		// Parse some common options
		FString Name;
		FParse::Value(Cmd, TEXT("Name="), Name);

		FString Names;
		FParse::Value(Cmd, TEXT("Names="), Names);

		int32 Limit = -1;
		FParse::Value(Cmd, TEXT("Limit="), Limit);

		if (FParse::Command(&Cmd, TEXT("Usage")))
		{
			if (!Name.IsEmpty())
			{
				uint64 ExclusiveSize = 0U;
				uint64 InclusiveSize = 0U;

				if (MemoryUsageQueries::GetMemoryUsage(MemoryUsageQueries::CurrentMemoryUsageInfoProvider, Name, ExclusiveSize, InclusiveSize, &Ar))
				{
					Ar.Logf(TEXT("MemoryUsage: ExclusiveSize: %.2f MiB (%.2f KiB); InclusiveSize: %.2f MiB (%.2f KiB)"),
							ExclusiveSize / (1024.f * 1024.f),
							ExclusiveSize / 1024.f,
							InclusiveSize / (1024.f * 1024.f),
							InclusiveSize / 1024.f);
				}

				return true;
			}
		}
		else if (FParse::Command(&Cmd, TEXT("CombinedUsage")))
		{
			if (!Names.IsEmpty())
			{
				TArray<FString> Packages;
				Names.ParseIntoArrayWS(Packages);
				uint64 TotalSize;

				if (MemoryUsageQueries::GetMemoryUsageCombined(MemoryUsageQueries::CurrentMemoryUsageInfoProvider, Packages, TotalSize, &Ar))
				{
					Ar.Logf(TEXT("MemoryUsageCombined: TotalSize: %.2f MiB (%.2f KiB)"), TotalSize / (1024.f * 1024.f), TotalSize / 1024.f);
				}

				return true;
			}
		}
		else if (FParse::Command(&Cmd, TEXT("SharedUsage")))
		{
			if (!Names.IsEmpty())
			{
				TArray<FString> Packages;
				Names.ParseIntoArrayWS(Packages);
				uint64 SharedSize;

				if (MemoryUsageQueries::GetMemoryUsageShared(MemoryUsageQueries::CurrentMemoryUsageInfoProvider, Packages, SharedSize, &Ar))
				{
					Ar.Logf(TEXT("MemoryUsageShared: SharedSize: %.2f MiB (%.2f KiB)"), SharedSize / (1024.f * 1024.f), SharedSize / 1024.f);
				}

				return true;
			}
		}
		else if (FParse::Command(&Cmd, TEXT("UniqueUsage")))
		{
			if (!Names.IsEmpty())
			{
				TArray<FString> Packages;
				Names.ParseIntoArrayWS(Packages);
				uint64 UniqueSize = 0U;

				if (MemoryUsageQueries::GetMemoryUsageUnique(MemoryUsageQueries::CurrentMemoryUsageInfoProvider, Packages, UniqueSize, &Ar))
				{
					Ar.Logf(TEXT("MemoryUsageUnique: UniqueSize: %.2f MiB (%.2f KiB)"), UniqueSize / (1024.f * 1024.f), UniqueSize / 1024.f);
				}

				return true;
			}
		}
		else if (FParse::Command(&Cmd, TEXT("CommonUsage")))
		{
			if (!Names.IsEmpty())
			{
				TArray<FString> Packages;
				Names.ParseIntoArrayWS(Packages);
				uint64 CommonSize = 0U;

				if (MemoryUsageQueries::GetMemoryUsageCommon(MemoryUsageQueries::CurrentMemoryUsageInfoProvider, Packages, CommonSize, &Ar))
				{
					Ar.Logf(TEXT("MemoryUsageCommon: CommonSize: %.2f MiB (%.2f KiB)"), CommonSize / (1024.f * 1024.f), CommonSize / 1024.f);
				}

				return true;
			}
		}
		else if (FParse::Command(&Cmd, TEXT("Dependencies")))
		{
			if (!Name.IsEmpty())
			{
				TMap<FName, uint64> DependenciesWithSize;

				if (MemoryUsageQueries::GetDependenciesWithSize(MemoryUsageQueries::CurrentMemoryUsageInfoProvider, Name, DependenciesWithSize, &Ar))
				{
					MemoryUsageQueries::Internal::PrintTagsWithSize(Ar, DependenciesWithSize, TEXT("dependencies"), bTruncate, Limit, bCSV);
				}

				return true;
			}
		}
		else if (FParse::Command(&Cmd, TEXT("CombinedDependencies")))
		{
			if (!Names.IsEmpty())
			{
				TArray<FString> Packages;
				Names.ParseIntoArrayWS(Packages);
				TMap<FName, uint64> CombinedDependenciesWithSize;

				if (MemoryUsageQueries::GetDependenciesWithSizeCombined(MemoryUsageQueries::CurrentMemoryUsageInfoProvider, Packages, CombinedDependenciesWithSize, &Ar))
				{
					MemoryUsageQueries::Internal::PrintTagsWithSize(Ar, CombinedDependenciesWithSize, TEXT("combined dependencies"), bTruncate, Limit, bCSV);
				}

				return true;
			}
		}
		else if (FParse::Command(&Cmd, TEXT("SharedDependencies")))
		{
			if (!Names.IsEmpty())
			{
				TArray<FString> Packages;
				Names.ParseIntoArrayWS(Packages);
				TMap<FName, uint64> SharedDependenciesWithSize;

				if (MemoryUsageQueries::GetDependenciesWithSizeShared(MemoryUsageQueries::CurrentMemoryUsageInfoProvider, Packages, SharedDependenciesWithSize, &Ar))
				{
					MemoryUsageQueries::Internal::PrintTagsWithSize(Ar, SharedDependenciesWithSize, TEXT("shared dependencies"), bTruncate, Limit, bCSV);
				}

				return true;
			}
		}
		else if (FParse::Command(&Cmd, TEXT("UniqueDependencies")))
		{
			if (!Names.IsEmpty())
			{
				TArray<FString> Packages;
				Names.ParseIntoArrayWS(Packages);
				TMap<FName, uint64> UniqueDependenciesWithSize;

				if (MemoryUsageQueries::GetDependenciesWithSizeUnique(MemoryUsageQueries::CurrentMemoryUsageInfoProvider, Packages, UniqueDependenciesWithSize, &Ar))
				{
					MemoryUsageQueries::Internal::PrintTagsWithSize(Ar, UniqueDependenciesWithSize, TEXT("unique dependencies"), bTruncate, Limit, bCSV);
				}

				return true;
			}
		}
		else if (FParse::Command(&Cmd, TEXT("CommonDependencies")))
		{
			if (!Names.IsEmpty())
			{
				TArray<FString> Packages;
				Names.ParseIntoArrayWS(Packages);
				TMap<FName, uint64> CommonDependenciesWithSize;

				if (MemoryUsageQueries::GetDependenciesWithSizeCommon(MemoryUsageQueries::CurrentMemoryUsageInfoProvider, Packages, CommonDependenciesWithSize, &Ar))
				{
					MemoryUsageQueries::Internal::PrintTagsWithSize(Ar, CommonDependenciesWithSize, TEXT("common dependencies"), bTruncate, Limit, bCSV);
				}

				return true;
			}
		}
#if ENABLE_LOW_LEVEL_MEM_TRACKER
		else if (FParse::Command(&Cmd, TEXT("ListAssets")))
		{
			FString AssetName;
			FParse::Value(Cmd, TEXT("NAME="), AssetName);

			FName Group = NAME_None;
			FString GroupName;
			if (FParse::Value(Cmd, TEXT("GROUP="), GroupName))
			{
				Group = FName(*GroupName);
			}

			FName Class = NAME_None;
			FString ClassName;
			if (FParse::Value(Cmd, TEXT("CLASS="), ClassName))
			{
				Class = FName(*ClassName);
			}

			bool bSuccess;
			TMap<FName, uint64> AssetsWithSize;

			if (Group != NAME_None || Class != NAME_None)
			{
				bSuccess = MemoryUsageQueries::GetFilteredPackagesWithSize(AssetsWithSize, Group, AssetName, Class, &Ar);
			}
			else
			{
				// TODO - Implement using faster path if there are no group / class filters
				bSuccess = MemoryUsageQueries::GetFilteredPackagesWithSize(AssetsWithSize, Group, AssetName, Class, &Ar);
			}

			if (bSuccess)
			{
				MemoryUsageQueries::Internal::PrintTagsWithSize(Ar, AssetsWithSize, TEXT("largest assets"), bTruncate, Limit, bCSV);
			}

			return true;
		}
		else if (FParse::Command(&Cmd, TEXT("ListClasses")))
		{
			FName Group = NAME_None;
			FString GroupName;
			if (FParse::Value(Cmd, TEXT("GROUP="), GroupName))
			{
				Group = FName(*GroupName);
			}

			FString AssetName;
			FParse::Value(Cmd, TEXT("ASSET="), AssetName);
			
			TMap<FName, uint64> ClassesWithSize;

			if (MemoryUsageQueries::GetFilteredClassesWithSize(ClassesWithSize, Group, AssetName, &Ar))
			{
				MemoryUsageQueries::Internal::PrintTagsWithSize(Ar, ClassesWithSize, *FString::Printf(TEXT("largest classes")), bTruncate, Limit, bCSV);
			}

			return true;
		}
		else if (FParse::Command(&Cmd, TEXT("ListGroups")))
		{
			FString AssetName = "";
			FParse::Value(Cmd, TEXT("ASSET="), AssetName);

			FName Class = NAME_None;
			FString ClassName;
			if (FParse::Value(Cmd, TEXT("CLASS="), ClassName))
			{
				Class = FName(*ClassName);
			}

			TMap<FName, uint64> GroupsWithSize;

			if (MemoryUsageQueries::GetFilteredGroupsWithSize(GroupsWithSize, AssetName, Class, &Ar))
			{
				MemoryUsageQueries::Internal::PrintTagsWithSize(Ar, GroupsWithSize, *FString::Printf(TEXT("largest groups")), bTruncate, Limit, bCSV);
			}

			return true;
		}
#endif
		else if (FParse::Command(&Cmd, TEXT("Savings")))
		{
			const UMemoryUsageQueriesConfig* Config = GetDefault<UMemoryUsageQueriesConfig>();

			for (auto It = Config->SavingsPresets.CreateConstIterator(); It; ++It)
			{
				if (!FParse::Command(&Cmd, *It.Key()))
				{
					continue;
				}

				TMap<FName, uint64> PresetSavings;
				TSet<FName> Packages;

				UClass* SavingsClass = FindObject<UClass>(nullptr, *It.Value());
				if (SavingsClass != nullptr)
				{
					TArray<UClass*> DerivedClasses;
					TArray<UClass*> DerivedResults;

					GetDerivedClasses(SavingsClass, DerivedClasses, true);

					for (UClass* DerivedClass : DerivedClasses)
					{
						UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(DerivedClass);
						if (BPClass != nullptr)
						{
							DerivedResults.Reset();
							GetDerivedClasses(BPClass, DerivedResults, false);
							if (DerivedResults.Num() == 0)
							{
								Packages.Add(DerivedClass->GetPackage()->GetFName());
							}
						}
					}
				}

				for (const auto& Package : Packages)
				{
					uint64 Size = 0;
					MemoryUsageQueries::GetMemoryUsageUnique(MemoryUsageQueries::CurrentMemoryUsageInfoProvider, TArray<FString>({Package.ToString()}), Size, &Ar);
					PresetSavings.Add(Package, Size);
				}

				PresetSavings.ValueSort(TGreater<uint64>());
				MemoryUsageQueries::Internal::PrintTagsWithSize(Ar, PresetSavings, *FString::Printf(TEXT("possible savings")), bTruncate, bCSV);

				return true;
			}
		}
		else if (FParse::Command(&Cmd, TEXT("Collection")))
		{
			const bool bShowDependencies = FParse::Param(Cmd, TEXT("ShowDeps"));

			const UMemoryUsageQueriesConfig* Config = GetDefault<UMemoryUsageQueriesConfig>();
			for (auto It = Config->Collections.CreateConstIterator(); It; ++It)
			{
				const FCollectionInfo& CollectionInfo = *It;
				
				if (!FParse::Command(&Cmd, *CollectionInfo.Name))
				{
					continue;
				}
				
				// Retrieve a list of all assets that have allocations we are currently tracking.
				TMap<FName, uint64> AssetsWithSize;
				bool bSuccess = MemoryUsageQueries::GetFilteredPackagesWithSize(AssetsWithSize, NAME_None, "", NAME_None, &Ar);

				if (!bSuccess)
				{
					Ar.Logf(TEXT("Failed to gather assets for Collection %s"), *CollectionInfo.Name);
					return false;
				}

				// Will return true if the Package Name matches any of the conditions in the array of Paths
				auto PackageNameMatches = [](const FString& PackageName, const TArray<FString>& Conditions)->bool {
					for (const FString& Condition : Conditions)
					{
						if ((FWildcardString::ContainsWildcards(*Condition) && FWildcardString::IsMatch(*Condition, *PackageName)) || (PackageName.Contains(Condition)))
						{
							return true;
						}
					}

					return false;
				};
				
				// See if any of the asset paths match those of our matching paths and are valid
				TArray<FString> PackageNames;

				TMap<FName, FAssetMemoryDetails> AssetMemoryMap;
				for (const TPair<FName, uint64>& AssetSizePair : AssetsWithSize)
				{
					const FString& PackageName = AssetSizePair.Key.ToString();
					
					if (!FPackageName::IsValidLongPackageName(PackageName))
					{
						continue;
					}

					// If path is Included and NOT Excluded, it's a valid asset to consider
					if (PackageNameMatches(PackageName, CollectionInfo.Includes) && !PackageNameMatches(PackageName, CollectionInfo.Excludes))
					{
						PackageNames.Add(PackageName);
						FAssetMemoryDetails& AssetMemory = AssetMemoryMap.Add(AssetSizePair.Key);
						AssetMemory.MemoryBreakdown.ExclusiveSize = AssetSizePair.Value;

						FName LongPackageName;
						if (!MemoryUsageQueries::Internal::GetLongNameAndDependencies(PackageName, LongPackageName, AssetMemory.Dependencies, &Ar))
						{
							Ar.Logf(TEXT("Failed to get dependencies foro Asset %s"), *PackageName);
						}
					}
				}

				// Gather list of dependencies. Internal dependencies are confined only to the set of packages passed in.
				// External are dependencies that have additional references outside the set of packages passed in.
				TMap<FName, uint64> InternalDependencies;
				TMap<FName, uint64> ExternalDependencies;
				if (!MemoryUsageQueries::GatherDependenciesForPackages(MemoryUsageQueries::CurrentMemoryUsageInfoProvider, PackageNames, InternalDependencies, ExternalDependencies, &Ar))
				{
					Ar.Logf(TEXT("Failed to gather memory usage for dependencies in Collection %s"), *CollectionInfo.Name);
					return false;
				}

				uint64 TotalCollectionSize = 0;

				// Determine which category where each assets dependency should reside
				for (TPair<FName, FAssetMemoryDetails>& Asset : AssetMemoryMap)
				{
					FAssetMemoryDetails& AssetMemory = Asset.Value;
					FAssetMemoryBreakdown& AssetMemoryDetails = AssetMemory.MemoryBreakdown;

					for (FName& Dep : Asset.Value.Dependencies)
					{
						// Don't want to count asset itself, plus some dependencies might refer to other assets in the map
						if (AssetMemoryMap.Contains(Dep))
						{
							continue;
						}

						FAssetMemoryBreakdown DependencyMemory;
						uint64* UniqueMemory = InternalDependencies.Find(Dep);
						uint64* SharedMemory = ExternalDependencies.Find(Dep);
						bool bRecordDependency = false;

						if (UniqueMemory != nullptr && *UniqueMemory != 0)
						{
							DependencyMemory.UniqueSize = *UniqueMemory;
							AssetMemoryDetails.UniqueSize += DependencyMemory.UniqueSize;
							AssetMemory.UniqueRefCount++;
							bRecordDependency = true;
						}

						if (SharedMemory != nullptr && *SharedMemory != 0)
						{
							DependencyMemory.SharedSize = *SharedMemory;
							AssetMemoryDetails.SharedSize += DependencyMemory.SharedSize;
							AssetMemory.SharedRefCount++;
							bRecordDependency = true;
						}

						if (bRecordDependency)
						{
							AssetMemory.DependenciesToMemoryMap.Add(Dep, DependencyMemory);
						}
					}

					AssetMemoryDetails.TotalSize = AssetMemoryDetails.ExclusiveSize + AssetMemoryDetails.UniqueSize;
					TotalCollectionSize += AssetMemoryDetails.TotalSize;
				}

				// Sort by TotalSize
				AssetMemoryMap.ValueSort([](const FAssetMemoryDetails& A, const FAssetMemoryDetails& B) {
						return A.MemoryBreakdown.TotalSize > B.MemoryBreakdown.TotalSize;
					});

				if (bCSV)
				{
					Ar.Logf(TEXT(",Asset,Exclusive KiB,Unique Refs KiB,Unique Ref Count,Shared Refs KiB,Shared Ref Count,Total KiB"));
				}
				else
				{
					Ar.Logf(
						TEXT(" %100s %20s %20s %15s %20s %15s %25s"),
						TEXT("Asset"),
						TEXT("Exclusive KiB"),
						TEXT("Unique Refs KiB"),
						TEXT("Unique Ref Count"),
						TEXT("Shared Refs KiB"),
						TEXT("Shared Ref Count"),
						TEXT("Total KiB")
					);
				}

				// Asset listing
				for (TPair<FName, FAssetMemoryDetails>& Asset : AssetMemoryMap)
				{
					FAssetMemoryDetails& AssetMemory = Asset.Value;
					FAssetMemoryBreakdown& AssetMemoryDetails = AssetMemory.MemoryBreakdown;

					if (bCSV)
					{
						Ar.Logf(TEXT(",%s,%.2f,%.2f,%d,%.2f,%d,%.2f"),*Asset.Key.ToString(),
							AssetMemoryDetails.ExclusiveSize / 1024.f, 
							AssetMemoryDetails.UniqueSize / 1024.f, 
							AssetMemory.UniqueRefCount, 
							AssetMemoryDetails.SharedSize / 1024.f,
							AssetMemory.SharedRefCount,
							AssetMemoryDetails.TotalSize / 1024.f);
					}
					else
					{
						Ar.Logf(
							TEXT(" %100s %20.2f %20.2f %15d %20.2f %15d %25.2f"),
							*Asset.Key.ToString(),
							AssetMemoryDetails.ExclusiveSize / 1024.f, 
							AssetMemoryDetails.SharedSize / 1024.f,
							AssetMemory.SharedRefCount,
							AssetMemoryDetails.UniqueSize / 1024.f, 
							AssetMemory.UniqueRefCount, 
							AssetMemoryDetails.TotalSize / 1024.f
						);
					}
				}

				// Asset dependencies listing
				if (bShowDependencies)
				{
					if (bCSV)
					{
						Ar.Logf(TEXT(",Asset,Dependency,Unique KiB,Shared KiB"));
					}
					else
					{
						Ar.Logf(TEXT(" %100s %100s %20s %20s"),
							TEXT("Asset"),
							TEXT("Dependency"),
							TEXT("Unique KiB"),
							TEXT("Shared KiB")
						);
					}

					for (TPair<FName, FAssetMemoryDetails>& Asset : AssetMemoryMap)
					{
						for (TPair<FName, FAssetMemoryBreakdown>& Dep : Asset.Value.DependenciesToMemoryMap)
						{
							const FString DependencyAssetName = Dep.Key.ToString();
							const FAssetMemoryBreakdown& DepedencyMemoryDetails = Dep.Value;

							if (bCSV)
							{
								Ar.Logf(TEXT(",%s,%s,%.2f,%.2f"), *Asset.Key.ToString(), *DependencyAssetName,
									DepedencyMemoryDetails.UniqueSize / 1024.f, DepedencyMemoryDetails.SharedSize / 1024.f);
							}
							else
							{
								Ar.Logf(TEXT(" %100s %100s %20.2f %20.2f"), 
									*Asset.Key.ToString(), 
									*DependencyAssetName,
									DepedencyMemoryDetails.UniqueSize / 1024.f, 
									DepedencyMemoryDetails.SharedSize / 1024.f
								);
							}
						}
					}
				}

				if (bCSV)
				{
					Ar.Logf(TEXT(",TOTAL KiB,%.2f"), TotalCollectionSize / 1024.f);
				}
				else
				{
					Ar.Logf(TEXT("TOTAL KiB: %.2f"), TotalCollectionSize / 1024.f);
				}

				return true;
			}
		}
	}

	return false;
}

namespace MemoryUsageQueries
{

void RegisterConsoleAutoCompleteEntries(TArray<FAutoCompleteCommand>& AutoCompleteList)
{
	const UConsoleSettings* ConsoleSettings = GetDefault<UConsoleSettings>();

	{
		FAutoCompleteCommand AutoCompleteCommand;
		AutoCompleteCommand.Command = TEXT("MemQuery Usage");
		AutoCompleteCommand.Desc = TEXT("Name=<AssetName> Prints memory usage of the specified asset.");
		AutoCompleteCommand.Color = ConsoleSettings->AutoCompleteCommandColor;
		AutoCompleteList.Add(MoveTemp(AutoCompleteCommand));
	}

	{
		FAutoCompleteCommand AutoCompleteCommand;
		AutoCompleteCommand.Command = TEXT("MemQuery CombinedUsage");
		AutoCompleteCommand.Desc = TEXT("Names=\"<AssetName1> <AssetName2> ...\" Prints combined memory usage of the specified assets (including all dependencies).");
		AutoCompleteCommand.Color = ConsoleSettings->AutoCompleteCommandColor;
		AutoCompleteList.Add(MoveTemp(AutoCompleteCommand));
	}

	{
		FAutoCompleteCommand AutoCompleteCommand;
		AutoCompleteCommand.Command = TEXT("MemQuery SharedUsage");
		AutoCompleteCommand.Desc = TEXT("Names=\"<AssetName1> <AssetName2> ...\" Prints shared memory usage of the specified assets (including only dependencies shared by the specified assets).");
		AutoCompleteCommand.Color = ConsoleSettings->AutoCompleteCommandColor;
		AutoCompleteList.Add(MoveTemp(AutoCompleteCommand));
	}

	{
		FAutoCompleteCommand AutoCompleteCommand;
		AutoCompleteCommand.Command = TEXT("MemQuery UniqueUsage");
		AutoCompleteCommand.Desc = TEXT("Names=\"<AssetName1> <AssetName2> ...\" Prints unique memory usage of the specified assets (including only dependencies unique to the specified assets).");
		AutoCompleteCommand.Color = ConsoleSettings->AutoCompleteCommandColor;
		AutoCompleteList.Add(MoveTemp(AutoCompleteCommand));
	}

	{
		FAutoCompleteCommand AutoCompleteCommand;
		AutoCompleteCommand.Command = TEXT("MemQuery CommonUsage");
		AutoCompleteCommand.Desc = TEXT("Names=\"<AssetName1> <AssetName2> ...\" Prints common memory usage of the specified assets (including only dependencies that are not unique to the specified assets).");
		AutoCompleteCommand.Color = ConsoleSettings->AutoCompleteCommandColor;
		AutoCompleteList.Add(MoveTemp(AutoCompleteCommand));
	}

	{
		FAutoCompleteCommand AutoCompleteCommand;
		AutoCompleteCommand.Command = TEXT("MemQuery Dependencies");
		AutoCompleteCommand.Desc = TEXT("Name=<AssetName> Limit=<n> Lists dependencies of the specified asset, sorted by size.");
		AutoCompleteCommand.Color = ConsoleSettings->AutoCompleteCommandColor;
		AutoCompleteList.Add(MoveTemp(AutoCompleteCommand));
	}

	{
		FAutoCompleteCommand AutoCompleteCommand;
		AutoCompleteCommand.Command = TEXT("MemQuery CombinedDependencies");
		AutoCompleteCommand.Desc = TEXT("Names=\"<AssetName1> <AssetName2> ...\" Limit=<n> Lists n largest dependencies of the specified assets, sorted by size.");
		AutoCompleteCommand.Color = ConsoleSettings->AutoCompleteCommandColor;
		AutoCompleteList.Add(MoveTemp(AutoCompleteCommand));
	}

	{
		FAutoCompleteCommand AutoCompleteCommand;
		AutoCompleteCommand.Command = TEXT("MemQuery SharedDependencies");
		AutoCompleteCommand.Desc = TEXT("Names=\"<AssetName1> <AssetName2> ...\" Limit=<n> Lists n largest dependencies that are shared by the specified assets, sorted by size.");
		AutoCompleteCommand.Color = ConsoleSettings->AutoCompleteCommandColor;
		AutoCompleteList.Add(MoveTemp(AutoCompleteCommand));
	}
		
	{
		FAutoCompleteCommand AutoCompleteCommand;
		AutoCompleteCommand.Command = TEXT("MemQuery UniqueDependencies");
		AutoCompleteCommand.Desc = TEXT("Names=\"<AssetName1> <AssetName2> ...\" Limit=<n> Lists n largest dependencies that are unique to the specified assets, sorted by size.");
		AutoCompleteCommand.Color = ConsoleSettings->AutoCompleteCommandColor;
		AutoCompleteList.Add(MoveTemp(AutoCompleteCommand));
	}

	{
		FAutoCompleteCommand AutoCompleteCommand;
		AutoCompleteCommand.Command = TEXT("MemQuery CommonDependencies");
		AutoCompleteCommand.Desc = TEXT("Names=\"<AssetName1> <AssetName2> ...\" Limit=<n> Lists n largest dependencies that are NOT unique to the specified assets, sorted by size.");
		AutoCompleteCommand.Color = ConsoleSettings->AutoCompleteCommandColor;
		AutoCompleteList.Add(MoveTemp(AutoCompleteCommand));
	}

	{
		FAutoCompleteCommand AutoCompleteCommand;
		AutoCompleteCommand.Command = TEXT("MemQuery ListAssets");
		AutoCompleteCommand.Desc = TEXT("Name=<AssetNameSubstring> Group=<GroupName> Class=<ClassName> Limit=<n> Lists n largest assets.");
		AutoCompleteCommand.Color = ConsoleSettings->AutoCompleteCommandColor;
		AutoCompleteList.Add(MoveTemp(AutoCompleteCommand));
	}

	{
		FAutoCompleteCommand AutoCompleteCommand;
		AutoCompleteCommand.Command = TEXT("MemQuery ListClasses");
		AutoCompleteCommand.Desc = TEXT("Group=<GroupName> Asset=<AssetName> Limit=<n> Lists n largest classes.");
		AutoCompleteCommand.Color = ConsoleSettings->AutoCompleteCommandColor;
		AutoCompleteList.Add(MoveTemp(AutoCompleteCommand));
	}

	{
		FAutoCompleteCommand AutoCompleteCommand;
		AutoCompleteCommand.Command = TEXT("MemQuery ListGroups");
		AutoCompleteCommand.Desc = TEXT("Asset=<AssetName> Class=<ClassName> Limit=<n> Lists n largest groups.");
		AutoCompleteCommand.Color = ConsoleSettings->AutoCompleteCommandColor;
		AutoCompleteList.Add(MoveTemp(AutoCompleteCommand));
	}

	const UMemoryUsageQueriesConfig* MemQueryConfig = GetDefault<UMemoryUsageQueriesConfig>();

	for (auto It = MemQueryConfig->SavingsPresets.CreateConstIterator(); It; ++It)
	{
		FAutoCompleteCommand AutoCompleteCommand;
		AutoCompleteCommand.Command = FString::Printf(TEXT("MemQuery Savings %s"), *It.Key());
		AutoCompleteCommand.Desc = FString::Printf(TEXT("Limit=<n> Lists potential savings among %s. How much memory can be saved it we delete certain object."), *It.Key());
		AutoCompleteCommand.Color = ConsoleSettings->AutoCompleteCommandColor;
		AutoCompleteList.Add(MoveTemp(AutoCompleteCommand));
	}

	for (auto It = MemQueryConfig->Collections.CreateConstIterator(); It; ++It)
	{
		FAutoCompleteCommand AutoCompleteCommand;
		AutoCompleteCommand.Command = FString::Printf(TEXT("MemQuery Collection %s"), *It->Name);
		AutoCompleteCommand.Desc = TEXT("Lists memory used by a collection. Can show dependency breakdown. [-csv, -showdeps]");
		AutoCompleteCommand.Color = ConsoleSettings->AutoCompleteCommandColor;
		AutoCompleteList.Add(MoveTemp(AutoCompleteCommand));
	}
}

IMemoryUsageInfoProvider* GetCurrentMemoryUsageInfoProvider()
{
	return CurrentMemoryUsageInfoProvider;
}

bool GetMemoryUsage(const IMemoryUsageInfoProvider* MemoryUsageInfoProvider, const FString& PackageName, uint64& OutExclusiveSize, uint64& OutInclusiveSize, FOutputDevice* ErrorOutput /* = GLog */)
{
	FName LongPackageName;
	TSet<FName> Dependencies;
	if (!Internal::GetLongNameAndDependencies(PackageName, LongPackageName, Dependencies, ErrorOutput))
	{
		return false;
	}

	OutExclusiveSize = MemoryUsageInfoProvider->GetAssetMemoryUsage(LongPackageName, ErrorOutput);
	OutInclusiveSize = MemoryUsageInfoProvider->GetAssetsMemoryUsage(Dependencies, ErrorOutput);

	return true;
}

bool GetMemoryUsageCombined(const IMemoryUsageInfoProvider* MemoryUsageInfoProvider, const TArray<FString>& PackageNames, uint64& OutTotalSize, FOutputDevice* ErrorOutput /* = GLog */)
{
	TSet<FName> Dependencies;
	if (!Internal::GetDependenciesCombined(PackageNames, Dependencies, ErrorOutput))
	{
		return false;
	}

	OutTotalSize =  MemoryUsageInfoProvider->GetAssetsMemoryUsage(Dependencies, ErrorOutput);

	return true;
}

bool GetMemoryUsageShared(const IMemoryUsageInfoProvider* MemoryUsageInfoProvider, const TArray<FString>& PackageNames, uint64& OutTotalSize, FOutputDevice* ErrorOutput /* = GLog */)
{
	TSet<FName> Dependencies;
	if (!Internal::GetDependenciesShared(PackageNames, Dependencies, ErrorOutput))
	{
		return false;
	}

	OutTotalSize = MemoryUsageInfoProvider->GetAssetsMemoryUsage(Dependencies, ErrorOutput);

	return true;
}

bool GetMemoryUsageUnique(const IMemoryUsageInfoProvider* MemoryUsageInfoProvider, const TArray<FString>& PackageNames, uint64& OutUniqueSize, FOutputDevice* ErrorOutput /* = GLog */)
{
	TSet<FName> RemovablePackages;
	if (!Internal::GetRemovablePackages(PackageNames, RemovablePackages, ErrorOutput))
	{
		return false;
	}

	OutUniqueSize = MemoryUsageInfoProvider->GetAssetsMemoryUsage(RemovablePackages, ErrorOutput);

	return true;
}

bool GetMemoryUsageCommon(const IMemoryUsageInfoProvider* MemoryUsageInfoProvider, const TArray<FString>& PackageNames, uint64& OutCommonSize, FOutputDevice* ErrorOutput /* = GLog */)
{
	TSet<FName> UnremovablePackages;
	if (!Internal::GetUnremovablePackages(PackageNames, UnremovablePackages, ErrorOutput))
	{
		return false;
	}

	OutCommonSize = MemoryUsageInfoProvider->GetAssetsMemoryUsage(UnremovablePackages, ErrorOutput);

	return true;
}

bool GatherDependenciesForPackages(const IMemoryUsageInfoProvider* MemoryUsageInfoProvider, const TArray<FString>& PackageNames,
	TMap<FName, uint64>& OutInternalDeps, TMap<FName, uint64>& OutExternalDeps, FOutputDevice* ErrorOutput)
{
	TSet<FName> RemovablePackages;
	if (!Internal::GetRemovablePackages(PackageNames, RemovablePackages, ErrorOutput))
	{
		return false;
	}

	TSet<FName> UnremovablePackages;
	if (!Internal::GetUnremovablePackages(PackageNames, UnremovablePackages, ErrorOutput))
	{
		return false;
	}

	MemoryUsageInfoProvider->GetAssetsMemoryUsageWithSize(RemovablePackages, OutInternalDeps, ErrorOutput);
	MemoryUsageInfoProvider->GetAssetsMemoryUsageWithSize(UnremovablePackages, OutExternalDeps, ErrorOutput);

	return true;
}


bool GetDependenciesWithSize(const IMemoryUsageInfoProvider* MemoryUsageInfoProvider, const FString& PackageName, TMap<FName, uint64>& OutDependenciesWithSize, FOutputDevice* ErrorOutput /* = GLog */)
{
	FName LongPackageName;
	TSet<FName> Dependencies;

	if (!Internal::GetLongNameAndDependencies(PackageName, LongPackageName, Dependencies, ErrorOutput))
	{
		return false;
	}

	OutDependenciesWithSize.Empty();
	Internal::SortPackagesBySize(MemoryUsageInfoProvider, Dependencies, OutDependenciesWithSize, ErrorOutput);

	return true;
}

bool GetDependenciesWithSizeCombined(const IMemoryUsageInfoProvider* MemoryUsageInfoProvider, const TArray<FString>& PackageNames, TMap<FName, uint64>& OutDependenciesWithSize, FOutputDevice* ErrorOutput /* = GLog */)
{
	TSet<FName> Dependencies;

	if (!Internal::GetDependenciesCombined(PackageNames, Dependencies, ErrorOutput))
	{
		return false;
	}

	OutDependenciesWithSize.Empty();
	Internal::SortPackagesBySize(MemoryUsageInfoProvider, Dependencies, OutDependenciesWithSize, ErrorOutput);

	return true;
}

bool GetDependenciesWithSizeShared(const IMemoryUsageInfoProvider* MemoryUsageInfoProvider, const TArray<FString>& PackageNames, TMap<FName, uint64>& OutDependenciesWithSize, FOutputDevice* ErrorOutput /* = GLog */)
{
	TSet<FName> Dependencies;
	if (!Internal::GetDependenciesShared(PackageNames, Dependencies, ErrorOutput))
	{
		return false;
	}

	OutDependenciesWithSize.Empty();
	Internal::SortPackagesBySize(MemoryUsageInfoProvider, Dependencies, OutDependenciesWithSize, ErrorOutput);

	return true;
}

bool GetDependenciesWithSizeUnique(const IMemoryUsageInfoProvider* MemoryUsageInfoProvider, const TArray<FString>& PackageNames, TMap<FName, uint64>& OutDependenciesWithSize, FOutputDevice* ErrorOutput /* = GLog */)
{
	TSet<FName> RemovablePackages;
	if (!Internal::GetRemovablePackages(PackageNames, RemovablePackages, ErrorOutput))
	{
		return false;
	}

	OutDependenciesWithSize.Empty();
	Internal::SortPackagesBySize(MemoryUsageInfoProvider, RemovablePackages, OutDependenciesWithSize, ErrorOutput);

	return true;
}

bool GetDependenciesWithSizeCommon(const IMemoryUsageInfoProvider* MemoryUsageInfoProvider, const TArray<FString>& PackageNames, TMap<FName, uint64>& OutDependenciesWithSize, FOutputDevice* ErrorOutput /* = GLog */)
{
	TSet<FName> UnremovablePackages;
	if (!Internal::GetUnremovablePackages(PackageNames, UnremovablePackages, ErrorOutput))
	{
		return false;
	}

	OutDependenciesWithSize.Empty();
	Internal::SortPackagesBySize(MemoryUsageInfoProvider, UnremovablePackages, OutDependenciesWithSize, ErrorOutput);

	return true;
}

#if ENABLE_LOW_LEVEL_MEM_TRACKER
bool GetFilteredPackagesWithSize(TMap<FName, uint64>& OutPackagesWithSize, FName GroupName /* = NAME_None */, FString AssetSubstring /* = FString() */, FName ClassName /* = NAME_None */, FOutputDevice* ErrorOutput /* = GLog */)
{
	TArray<FLLMTagSetAllocationFilter> Filters;
	if (GroupName != NAME_None)
	{
		Filters.Add({GroupName, ELLMTagSet::None});
	}

	if (ClassName != NAME_None)
	{
		Filters.Add({ClassName, ELLMTagSet::AssetClasses});
	}

	MemoryUsageInfoProviderLLM.GetFilteredTagsWithSize(OutPackagesWithSize, ELLMTracker::Default, ELLMTagSet::Assets, Filters, ErrorOutput);

	if (AssetSubstring.Len() > 0)
	{
		Internal::RemoveFilteredPackages(OutPackagesWithSize, AssetSubstring);
	}

	OutPackagesWithSize.ValueSort(TGreater<uint64>());

	return true;
}

bool GetFilteredClassesWithSize(TMap<FName, uint64>& OutClassesWithSize, FName GroupName /* = NAME_None */, FString AssetName /* = FString() */, FOutputDevice* ErrorOutput /* = GLog */)
{
	TArray<FLLMTagSetAllocationFilter> Filters;

	FName LongName = NAME_None;
	if (!AssetName.IsEmpty() && !Internal::GetLongName(AssetName, LongName, ErrorOutput))
	{
		return false;
	}

	if (LongName != NAME_None)
	{
		Filters.Add({LongName, ELLMTagSet::Assets});
	}

	if (GroupName != NAME_None)
	{
		Filters.Add({GroupName, ELLMTagSet::None});
	}

	MemoryUsageInfoProviderLLM.GetFilteredTagsWithSize(OutClassesWithSize, ELLMTracker::Default, ELLMTagSet::AssetClasses, Filters, ErrorOutput);

	OutClassesWithSize.ValueSort(TGreater<uint64>());

	return true;
}

bool GetFilteredGroupsWithSize(TMap<FName, uint64>& OutGroupsWithSize, FString AssetName /* = FString() */, FName ClassName /* = NAME_None */, FOutputDevice* ErrorOutput /* = GLog */)
{
	TArray<FLLMTagSetAllocationFilter> Filters;

	FName LongName = NAME_None;
	if (!AssetName.IsEmpty() && !Internal::GetLongName(AssetName, LongName, ErrorOutput))
	{
		return false;
	}

	if (LongName != NAME_None)
	{
		Filters.Add({LongName, ELLMTagSet::Assets});
	}

	if (ClassName != NAME_None)
	{
		Filters.Add({ClassName, ELLMTagSet::AssetClasses});
	}

	MemoryUsageInfoProviderLLM.GetFilteredTagsWithSize(OutGroupsWithSize, ELLMTracker::Default, ELLMTagSet::None, Filters, ErrorOutput);

	OutGroupsWithSize.ValueSort(TGreater<uint64>());

	return true;
}
#endif

namespace Internal
{
FMemoryUsageReferenceProcessor::FMemoryUsageReferenceProcessor()
{
	int32 Num = GUObjectArray.GetObjectArrayNum();

	Excluded.Init(false, Num);
	ReachableFull.Init(false, Num);
	ReachableExcluded.Init(false, Num);
}

bool FMemoryUsageReferenceProcessor::Init(const TArray<FString>& PackageNames, FOutputDevice* ErrorOutput /* = GLog */)
{
	for (FRawObjectIterator It(true); It; ++It)
	{
		FUObjectItem* ObjectItem = *It;
		UObject* Object = (UObject*)ObjectItem->Object;

		if (ObjectItem->IsUnreachable())
		{
			continue;
		}

		if (ObjectItem->IsRootSet())
		{
			RootSetPackages.Add(Object);
		}

		if (UClass* Class = dynamic_cast<UClass*>(Object))
		{
			if (!Class->HasAnyClassFlags(CLASS_TokenStreamAssembled))
			{
				Class->AssembleReferenceTokenStream();
				check(Class->HasAnyClassFlags(CLASS_TokenStreamAssembled));
			}
		}
	}

	if (FPlatformProperties::RequiresCookedData() && FGCObject::GGCObjectReferencer && GUObjectArray.IsDisregardForGC(FGCObject::GGCObjectReferencer))
	{
		RootSetPackages.Add(FGCObject::GGCObjectReferencer);
	}

	TSet<FName> LongPackageNames;
	if (!GetLongNames(PackageNames, LongPackageNames, ErrorOutput))
	{
		return false;
	}

	for (const auto& PackageName : LongPackageNames)
	{
		UPackage* Package = Cast<UPackage>(StaticFindObjectFast(UPackage::StaticClass(), nullptr, PackageName, /*bExactClass*/ true));
		if (Package)
		{
			auto ExcludeObject = [&](UObject* ObjectToExclude) {
				if (ObjectToExclude && GUObjectArray.ObjectToIndex(ObjectToExclude) < Excluded.Num())
				{
					Excluded[GUObjectArray.ObjectToIndex(ObjectToExclude)] = true;
				}
			};

			auto ExcludeObjectOfClass = [&](UObject* ObjectOfClass) {
				if (ObjectOfClass)
				{
					ForEachObjectWithOuter(ObjectOfClass, ExcludeObject);
				}
				ExcludeObject(ObjectOfClass);
			};

			auto ExcludeObjectInPackage = [&](UObject* ObjectInPackage) {
				if (ObjectInPackage->IsA<UClass>())
				{
					UClass* Class = static_cast<UClass*>(ObjectInPackage);
					ForEachObjectOfClass(Class, ExcludeObjectOfClass);
				}
				ExcludeObject(ObjectInPackage);
			};

			ForEachObjectWithOuter(Package, ExcludeObjectInPackage);
			ExcludeObject(Package);
		}
	}

	return true;
}

TArray<UObject*>& FMemoryUsageReferenceProcessor::GetRootSet()
{
	return RootSetPackages;
}

void FMemoryUsageReferenceProcessor::HandleTokenStreamObjectReference(UE::GC::FWorkerContext& Context, const UObject* ReferencingObject, UObject*& Object, UE::GC::FTokenId TokenIndex, const EGCTokenType TokenType, bool bAllowReferenceElimination)
{
	FPermanentObjectPoolExtents PermanentPool;
	if (Object == nullptr || GUObjectArray.ObjectToIndex(Object) >= ReachableFull.Num() || PermanentPool.Contains(Object) || GUObjectArray.IsDisregardForGC(Object))
	{
		return;
	}

	int32 ObjectIndex = GUObjectArray.ObjectToIndex(Object);

	if (Mode == EMode::Full)
	{
		if (!ReachableFull[ObjectIndex])
		{
			ReachableFull[ObjectIndex] = true;
			Context.ObjectsToSerialize.Add<Options>(Object);
		}
	}
	else if (Mode == EMode::Excluding)
	{
		if (!ReachableExcluded[ObjectIndex] && !Excluded[ObjectIndex])
		{
			ReachableExcluded[ObjectIndex] = true;
			Context.ObjectsToSerialize.Add<Options>(Object);
		}
	}
}

bool FMemoryUsageReferenceProcessor::GetUnreachablePackages(TSet<FName>& OutUnreachablePackages)
{
	for (int i = 0; i < ReachableFull.Num(); i++)
	{
		if (ReachableFull[i] && !ReachableExcluded[i])
		{
			UObject* Obj = static_cast<UObject*>(GUObjectArray.IndexToObjectUnsafeForGC(i)->Object);
			if (Obj && Obj->IsA<UPackage>())
			{
				OutUnreachablePackages.Add(Obj->GetFName());
			}
		}
	}

	return true;
}

bool GetLongName(const FString& ShortPackageName, FName& OutLongPackageName, FOutputDevice* ErrorOutput /* = GLog */)
{
	FAssetRegistryModule& AssetRegistryModule = GetAssetRegistryModule();

	if (FPackageName::IsValidLongPackageName(ShortPackageName))
	{
		OutLongPackageName = FName(*ShortPackageName);
	}
	else
	{
		OutLongPackageName = AssetRegistryModule.Get().GetFirstPackageByName(ShortPackageName);
		if (OutLongPackageName == NAME_None)
		{
			ErrorOutput->Logf(TEXT("MemQuery Error: Package not found: %s"), *ShortPackageName);
			return false;
		}
	}

	return true;
}

bool GetLongNames(const TArray<FString>& PackageNames, TSet<FName>& OutLongPackageNames, FOutputDevice* ErrorOutput /* = GLog */)
{
	for (const auto& Package : PackageNames)
	{
		FName LongName;
		if (!GetLongName(Package, LongName, ErrorOutput))
		{
			return false;
		}
		OutLongPackageNames.Add(LongName);
	}

	return true;
}

bool GetLongNameAndDependencies(const FString& PackageName, FName& OutLongPackageName, TSet<FName>& OutDependencies, FOutputDevice* ErrorOutput /* = GLog */)
{
	if (!GetLongName(PackageName, OutLongPackageName, ErrorOutput))
	{
		return false;
	}

	FAssetRegistryModule& AssetRegistryModule = GetAssetRegistryModule();

	GetTransitiveDependencies(OutLongPackageName, AssetRegistryModule, OutDependencies);
	OutDependencies.Add(OutLongPackageName);

	return true;
}

bool GetDependenciesCombined(const TArray<FString>& PackageNames, TSet<FName>& OutDependencies, FOutputDevice* ErrorOutput /* = GLog */)
{
	FName LongPackageName;
	TSet<FName> Dependencies;

	for (const auto& Package : PackageNames)
	{
		Dependencies.Empty();
		if (!Internal::GetLongNameAndDependencies(Package, LongPackageName, Dependencies, ErrorOutput))
		{
			return false;
		}

		OutDependencies.Append(Dependencies);
	}

	return true;
}

bool GetDependenciesShared(const TArray<FString>& PackageNames, TSet<FName>& OutDependencies, FOutputDevice* ErrorOutput /* = GLog */)
{
	FName LongPackageName;
	TSet<FName> Dependencies;

	for (int i = 0; i < PackageNames.Num(); i++)
	{
		Dependencies.Empty();
		if (!Internal::GetLongNameAndDependencies(PackageNames[i], LongPackageName, Dependencies, ErrorOutput))
		{
			return false;
		}

		if (i == 0)
		{
			OutDependencies.Append(Dependencies);
			continue;
		}

		OutDependencies = OutDependencies.Intersect(Dependencies);
	}

	return true;
}

bool PerformReachabilityAnalysis(FMemoryUsageReferenceProcessor& ReferenceProcessor, FOutputDevice* ErrorOutput /* = GLog */)
{
	{
		FGCArrayStruct ArrayStruct;
		ArrayStruct.SetInitialObjectsUnpadded(ReferenceProcessor.GetRootSet());
		ReferenceProcessor.SetMode(FMemoryUsageReferenceProcessor::Full);
		CollectReferences<FMemoryUsageReferenceCollector, FMemoryUsageReferenceProcessor>(ReferenceProcessor, ArrayStruct);
	}

	{
		FGCArrayStruct ArrayStruct;
		ArrayStruct.SetInitialObjectsUnpadded(ReferenceProcessor.GetRootSet());
		ReferenceProcessor.SetMode(FMemoryUsageReferenceProcessor::Excluding);
		CollectReferences<FMemoryUsageReferenceCollector, FMemoryUsageReferenceProcessor>(ReferenceProcessor, ArrayStruct);
	}

	return true;
}

bool GetRemovablePackages(const TArray<FString>& PackagesToUnload, TSet<FName>& OutRemovablePackages, FOutputDevice* ErrorOutput /* = GLog */)
{
	FMemoryUsageReferenceProcessor ReferenceProcessor;

	if (!ReferenceProcessor.Init(PackagesToUnload, ErrorOutput))
	{
		return false;
	}

	if (!PerformReachabilityAnalysis(ReferenceProcessor, ErrorOutput))
	{
		return false;
	}

	return ReferenceProcessor.GetUnreachablePackages(OutRemovablePackages);
}

bool GetUnremovablePackages(const TArray<FString>& PackagesToUnload, TSet<FName>& OutUnremovablePackages, FOutputDevice* ErrorOutput /* = GLog */)
{
	FMemoryUsageReferenceProcessor ReferenceProcessor;

	if (!ReferenceProcessor.Init(PackagesToUnload, ErrorOutput))
	{
		return false;
	}

	if (!PerformReachabilityAnalysis(ReferenceProcessor, ErrorOutput))
	{
		return false;
	}

	TSet<FName> UnreachablePackages;
	if (!ReferenceProcessor.GetUnreachablePackages(UnreachablePackages))
	{
		return false;
	}

	TSet<FName> Dependencies;
	if (!Internal::GetDependenciesCombined(PackagesToUnload, Dependencies, ErrorOutput))
	{
		return false;
	}

	for (const auto& Package : Dependencies)
	{
		if (!UnreachablePackages.Contains(Package) && StaticFindObjectFast(UPackage::StaticClass(), nullptr, Package, true) != nullptr)
		{
			OutUnremovablePackages.Add(Package);
		}
	}

	return true;
}

FAssetRegistryModule& GetAssetRegistryModule()
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::Get().LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	if (IsInGameThread())
	{
		AssetRegistryModule.Get().WaitForCompletion();
	}

	return AssetRegistryModule;
}

void GetTransitiveDependencies(FName PackageName, FAssetRegistryModule& AssetRegistryModule, TSet<FName>& OutDependencies)
{
	TArray<FName> PackageQueue;
	TSet<FName> ExaminedPackages;
	TArray<FName> PackageDependencies;
	OutDependencies.Empty();
	
	PackageQueue.Push(PackageName);

	while (PackageQueue.Num() > 0)
	{
		const FName& CurrentPackage = PackageQueue.Pop();
		if (ExaminedPackages.Contains(CurrentPackage))
		{
			continue;
		}

		ExaminedPackages.Add(CurrentPackage);

		if (CurrentPackage != PackageName && !OutDependencies.Contains(CurrentPackage))
		{
			OutDependencies.Add(CurrentPackage);
		}

		PackageDependencies.Empty();
		AssetRegistryModule.Get().GetDependencies(CurrentPackage, PackageDependencies, UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::EDependencyQuery::Hard);

		for (const auto& Package : PackageDependencies)
		{
			if (!ExaminedPackages.Contains(Package))
			{
				PackageQueue.Push(Package);
			}
		}
	}
}

void SortPackagesBySize(const IMemoryUsageInfoProvider* MemoryUsageInfoProvider, const TSet<FName>& Packages, TMap<FName, uint64>& OutPackagesWithSize, FOutputDevice* ErrorOutput /* = GLog */)
{
	GetPackagesSize(MemoryUsageInfoProvider, Packages, OutPackagesWithSize, ErrorOutput);
	OutPackagesWithSize.ValueSort([&](const uint64& A, const uint64& B) { return A > B; });
}

void GetPackagesSize(const IMemoryUsageInfoProvider* MemoryUsageInfoProvider, const TSet<FName>& Packages, TMap<FName, uint64>& OutPackagesWithSize, FOutputDevice* ErrorOutput /* = GLog */)
{
	for (const auto& Package : Packages)
	{
		uint64 Size = MemoryUsageInfoProvider->GetAssetMemoryUsage(Package, ErrorOutput);
		OutPackagesWithSize.Add(Package, Size);
	}
}

void RemoveNonExistentPackages(TMap<FName, uint64>& OutPackagesWithSize)
{
	FAssetRegistryModule& AssetRegistryModule = GetAssetRegistryModule();

	for (auto It = OutPackagesWithSize.CreateIterator(); It; ++It)
	{
		if (!AssetRegistryModule.Get().DoesPackageExistOnDisk(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
}

void RemoveFilteredPackages(TMap<FName, uint64>& OutPackagesWithSize, const FString& AssetSubstring)
{
	for (auto It = OutPackagesWithSize.CreateIterator(); It; ++It)
	{
		FString KeyString = It.Key().ToString();
		if (!KeyString.Contains(AssetSubstring))
		{
			It.RemoveCurrent();
		}
	}
}

void PrintTagsWithSize(FOutputDevice& Ar, const TMap<FName, uint64>& TagsWithSize, const TCHAR* Name, bool bTruncate /* = false */, int32 Limit /* = -1 */, bool bCSV /* = false */)
{
	uint64 TotalSize = 0U;
	static const FString NoScopeString(TEXT("No scope"));

	if (Limit < 0)
	{
		Limit = DefaultResultLimit;
	}

	int32 Num = TagsWithSize.Num();
	int32 TagsToDisplay = bTruncate ? FMath::Min(Num, Limit) : Num;
	int It = 0;

	if (bCSV)
	{
		Ar.Logf(TEXT(",Name,SizeMB,SizeKB"));
	}

	for (auto& Elem : TagsWithSize)
	{
		if (It++ >= TagsToDisplay)
		{
			break;
		}

		TotalSize += Elem.Value;
		const FString KeyName = Elem.Key.IsValid() ? Elem.Key.ToString() : NoScopeString;

		const float SizeMB = Elem.Value / (1024.0f * 1024.0f);
		const float SizeKB = Elem.Value / 1024.0f;

		if (bCSV)
		{
			Ar.Logf(TEXT(",%s,%.2f,%.2f"), *KeyName, SizeMB, SizeKB);
		}
		else
		{
			Ar.Logf(TEXT("%s - %.2f MB (%.2f KB)"), *KeyName, SizeMB, SizeKB);
		}
	}

	if (TagsToDisplay < Num && !bCSV)
	{
		Ar.Logf(TEXT("----------------------------------------------------------"));
		Ar.Logf(TEXT("<<truncated>> - displayed %d out of %d %s."), TagsToDisplay, Num, Name);
	}

	const float TotalSizeMB = TotalSize / (1024.0f * 1024.0f);
	const float TotalSizeKB = TotalSize / 1024.0f;

	if (bCSV)
	{
		Ar.Logf(TEXT(",TOTAL,%.2f,%.2f"), TotalSizeMB, TotalSizeKB);
	}
	else
	{
		Ar.Logf(TEXT("TOTAL: %.2f MB (%.2f KB)"), TotalSizeMB, TotalSizeKB);
	}
}

} // namespace Internal

} // namespace MemoryUsageQueries
