// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Misc/EnumClassFlags.h"
#include "WorldPartition/WorldPartitionBuilder.h"
#include "WorldPartitionHLODsBuilder.generated.h"

struct FHLODModifiedFiles
{
	enum EFileOperation
	{
		FileAdded,
		FileEdited,
		FileDeleted,
		NumFileOperations
	};

	void Add(EFileOperation FileOp, const FString& File)
	{
		Files[FileOp].Add(File);
	}

	const TSet<FString>& Get(EFileOperation FileOp) const
	{
		return Files[FileOp];
	}

	void Append(EFileOperation FileOp, const TArray<FString>& InFiles)
	{
		Files[FileOp].Append(InFiles);
	}

	void Append(const FHLODModifiedFiles& Other)
	{
		Files[EFileOperation::FileAdded].Append(Other.Files[EFileOperation::FileAdded]);
		Files[EFileOperation::FileEdited].Append(Other.Files[EFileOperation::FileEdited]);
		Files[EFileOperation::FileDeleted].Append(Other.Files[EFileOperation::FileDeleted]);
	}

	void Empty()
	{
		Files[EFileOperation::FileAdded].Empty();
		Files[EFileOperation::FileEdited].Empty();
		Files[EFileOperation::FileDeleted].Empty();
	}

	TArray<FString> GetAllFiles() const
	{
		TArray<FString> AllFiles;
		AllFiles.Append(Files[EFileOperation::FileAdded].Array());
		AllFiles.Append(Files[EFileOperation::FileEdited].Array());
		AllFiles.Append(Files[EFileOperation::FileDeleted].Array());
		return AllFiles;
	}

private:
	TSet<FString> Files[NumFileOperations];
};

enum class EHLODBuildStep : uint8
{
	None		= 0,
	HLOD_Setup	= 1 << 0,
	HLOD_Build	= 1 << 1,
	HLOD_Submit = 1 << 2,
	HLOD_Delete = 1 << 3,
	HLOD_Stats	= 1 << 4
};
ENUM_CLASS_FLAGS(EHLODBuildStep);

UCLASS()
class UWorldPartitionHLODsBuilder : public UWorldPartitionBuilder
{
	GENERATED_UCLASS_BODY()
public:
	// UWorldPartitionBuilder interface begin
	virtual bool RequiresCommandletRendering() const override;
	virtual ELoadingMode GetLoadingMode() const override { return ELoadingMode::Custom; }
	virtual bool PreWorldInitialization(FPackageSourceControlHelper& PackageHelper) override;
protected:
	virtual bool RunInternal(UWorld* World, const FBox& Bounds, FPackageSourceControlHelper& PackageHelper) override;
	// UWorldPartitionBuilder interface end

	bool IsDistributedBuild() const { return bDistributedBuild; }
	bool IsUsingBuildManifest() const { return !BuildManifest.IsEmpty(); }
	bool ValidateParams() const;

	bool SetupHLODActors();
	bool BuildHLODActors();
	bool DeleteHLODActors();
	bool SubmitHLODActors();
	bool DumpStats();

	bool GenerateBuildManifest(TMap<FString, int32>& FilesToBuilderMap) const;
	bool GetHLODActorsToBuild(TArray<FGuid>& HLODActorsToBuild) const;

	TArray<TArray<FGuid>> GetHLODWorldloads(int32 NumWorkloads) const;
	bool ValidateWorkload(const TArray<FGuid>& Workload) const;

	bool CopyFilesToWorkingDir(const FString& TargetDir, const FHLODModifiedFiles& ModifiedFiles, TArray<FString>& BuildProducts);
	bool CopyFilesFromWorkingDir(const FString& SourceDir);

	bool ShouldRunStep(const EHLODBuildStep BuildStep) const;

private:
	class UWorldPartition* WorldPartition;
	class FSourceControlHelper* SourceControlHelper;

	// Options --
	EHLODBuildStep BuildOptions;

	bool bDistributedBuild;
	FString BuildManifest;
	int32 BuilderIdx;
	int32 BuilderCount;
	bool bResumeBuild;
	int32 ResumeBuildIndex;
	int32 HLODLevelToBuild;


	const FString DistributedBuildWorkingDir;
	const FString DistributedBuildManifest;
	
	FHLODModifiedFiles ModifiedFiles;
};