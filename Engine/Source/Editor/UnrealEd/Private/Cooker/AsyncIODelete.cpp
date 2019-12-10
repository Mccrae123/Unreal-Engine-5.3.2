// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "AsyncIODelete.h"
#include "CoreMinimal.h"

#include "Async/Async.h"
#include "Containers/UnrealString.h"
#include "CookOnTheSide/CookOnTheFlyServer.h" // needed for DECLARE_LOG_CATEGORY_EXTERN(LogCook,...)
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "Math/NumericLimits.h"
#include "Misc/StringBuilder.h"
#include "Misc/Paths.h"
#include "Templates/UnrealTemplate.h"

#if WITH_ASYNCIODELETE_DEBUG
TArray<FString> FAsyncIODelete::AllTempRoots;
#endif

FAsyncIODelete::FAsyncIODelete(const FStringView& InOwnedTempRoot)
{
	SetTempRoot(InOwnedTempRoot);
}

FAsyncIODelete::~FAsyncIODelete()
{
	SetTempRoot(FStringView());
}

void FAsyncIODelete::SetTempRoot(const FStringView& InOwnedTempRoot)
{
	Teardown();

#if WITH_ASYNCIODELETE_DEBUG
	if (!TempRoot.IsEmpty())
	{
		RemoveTempRoot(*TempRoot);
	}
#endif

	TempRoot = InOwnedTempRoot;

#if WITH_ASYNCIODELETE_DEBUG
	if (!TempRoot.IsEmpty())
	{
		AddTempRoot(*TempRoot);
	}
#endif
}

void FAsyncIODelete::SetDeletesPaused(bool bInPaused)
{
	bPaused = bInPaused;
	if (!bPaused)
	{
		IFileManager& FileManager = IFileManager::Get();
		for (const FString& DeletePath : PausedDeletes)
		{
			const bool IsDirectory = FileManager.DirectoryExists(*DeletePath);
			const bool IsFile = !IsDirectory && FileManager.FileExists(*DeletePath);
			if (!IsDirectory && !IsFile)
			{
				continue;
			}
			CreateDeleteTask(DeletePath, IsDirectory ? EPathType::Directory : EPathType::File);
		}
		PausedDeletes.Empty();
	}
}

bool FAsyncIODelete::Setup()
{
	if (bInitialized)
	{
		return true;
	}

	if (TempRoot.IsEmpty())
	{
		checkf(false, TEXT("DeleteDirectory called without having first set a TempRoot"));
		return false;
	}

	// Delete the TempRoot directory to clear the results from any previous process using the same TempRoot that did not shut down cleanly
	if (!ensure(DeleteTempRootDirectory()))
	{
		return false;
	}

	// Create the empty directory to work in
	if (!ensure(IFileManager::Get().MakeDirectory(*TempRoot, true)))
	{
		return false;
	}

	// Allocate the task event
	check(TasksComplete == nullptr);
	TasksComplete = FPlatformProcess::GetSynchEventFromPool(true /* IsManualReset */);
	check(ActiveTaskCount == 0);
	TasksComplete->Trigger(); // We have 0 tasks so the event should be in the Triggered state

	// Assert that all other teardown-transient variables were cleared by the constructor or by the previous teardown
	// TempRoot and bPaused are preserved across setup/teardown and may have any value
	check(PausedDeletes.Num() == 0);
	check(DeleteCounter == 0);

	// We are now setup and ready to create DeleteTasks
	bInitialized = true;

	return true;
}

void FAsyncIODelete::Teardown()
{
	if (!bInitialized)
	{
		return;
	}

	// Clear task variables
	WaitForAllTasks();
	check(ActiveTaskCount == 0 && TasksComplete != nullptr && TasksComplete->Wait(0));
	FPlatformProcess::ReturnSynchEventToPool(TasksComplete);
	TasksComplete = nullptr;

	// Remove the temp directory from disk
	if (!DeleteTempRootDirectory())
	{
		// This will leave directories (and potentially files, if we were paused or if any of the asyncdeletes failed) on disk, so it is bad for users, but is not fatal for our operations.
		UE_LOG(LogCook, Warning, TEXT("Could not delete asyncdelete directory '%s'."), *TempRoot);
	}

	// Clear delete variables; we don't need to run the tasks for the remaining pauseddeletes because synchronously deleting the temp directory above did the work they were going to do
	PausedDeletes.Empty();
	DeleteCounter = 0;

	// We are now torn down and ready for a new setup
	bInitialized = false;
}

void FAsyncIODelete::WaitForAllTasks()
{
	if (!bInitialized)
	{
		return;
	}

	TasksComplete->Wait();
	check(ActiveTaskCount == 0);
}

bool FAsyncIODelete::Delete(const FStringView& PathToDelete, EPathType ExpectedType)
{
	IFileManager& FileManager = IFileManager::Get();
	TStringBuilder<128> PathToDeleteBuffer;
	PathToDeleteBuffer << PathToDelete;
	const TCHAR* PathToDeleteSZ = PathToDeleteBuffer.ToString();

	const bool IsDirectory = FileManager.DirectoryExists(PathToDeleteSZ);
	const bool IsFile = !IsDirectory && FileManager.FileExists(PathToDeleteSZ);
	if (!IsDirectory && !IsFile)
	{
		return true;
	}
	if (ExpectedType == EPathType::Directory && !IsDirectory)
	{
		checkf(false, TEXT("DeleteDirectory called on \"%.*s\" which is not a directory."), PathToDelete.Len(), PathToDelete.GetData());
		return false;
	}
	if (ExpectedType == EPathType::File && !IsFile)
	{
		checkf(false, TEXT("DeleteFile called on \"%.*s\" which is not a file."), PathToDelete.Len(), PathToDelete.GetData());
		return false;
	}
	// Prevent the user from trying to delete our temproot or anything inside it
	FString PathToDeleteStr(PathToDelete);
	if (FPaths::IsUnderDirectory(PathToDeleteStr, TempRoot) || FPaths::IsUnderDirectory(TempRoot, PathToDeleteStr))
	{
		return false;
	}

	if (DeleteCounter == UINT32_MAX)
	{
		Teardown();
	}
	if (!Setup())
	{
		// Setup failed; we are not able to provide asynchronous deletes; fall back to synchronous
		return SynchronousDelete(PathToDeleteSZ, ExpectedType);
	}

	const FString TempPath = FPaths::Combine(TempRoot, FString::Printf(TEXT("Delete%u"), DeleteCounter));
	DeleteCounter++;

	const bool bReplace = true;
	const bool bEvenIfReadOnly = true;
	const bool bMoveAttributes = false;
	const bool bDoNotRetryOnError = true;
	if (!IFileManager::Get().Move(*TempPath, PathToDeleteSZ, bReplace, bEvenIfReadOnly, bMoveAttributes, bDoNotRetryOnError)) // IFileManager::Move works on either files or directories
	{
		// The move failed; try a synchronous delete as backup
		UE_LOG(LogCook, Warning, TEXT("Failed to move path '%.*s' for async delete; falling back to synchronous delete."), PathToDelete.Len(), PathToDelete.GetData());
		return SynchronousDelete(PathToDeleteSZ, ExpectedType);
	}

	if (bPaused)
	{
		PausedDeletes.Add(TempPath);
	}
	else
	{
		CreateDeleteTask(TempPath, ExpectedType);
	}
	return true;
}

void FAsyncIODelete::CreateDeleteTask(const FStringView& InDeletePath, EPathType PathType)
{
	{
		FScopeLock Lock(&CriticalSection);
		TasksComplete->Reset();
		ActiveTaskCount++;
	}

	AsyncThread(
		[this, DeletePath = FString(InDeletePath), PathType]() { SynchronousDelete(*DeletePath, PathType); },
		0, TPri_Normal,
		[this]() { OnTaskComplete(); });
}

void FAsyncIODelete::OnTaskComplete()
{
	FScopeLock Lock(&CriticalSection);
	check(ActiveTaskCount > 0);
	ActiveTaskCount--;
	if (ActiveTaskCount == 0)
	{
		TasksComplete->Trigger();
	}
}

bool FAsyncIODelete::SynchronousDelete(const TCHAR* InDeletePath, EPathType PathType)
{
	bool Result;
	const bool bRequireExists = false;
	if (PathType == EPathType::Directory)
	{
		const bool bTree = true;
		Result = IFileManager::Get().DeleteDirectory(InDeletePath, bRequireExists, bTree);
	}
	else
	{
		const bool bEvenIfReadOnly = true;
		Result = IFileManager::Get().Delete(InDeletePath, bRequireExists, bEvenIfReadOnly);
	}

	if (!Result)
	{
		UE_LOG(LogCook, Warning, TEXT("Could not delete asyncdelete %s '%s'."), PathType == EPathType::Directory ? TEXT("directory") : TEXT("file"), InDeletePath);
	}
	return Result;
}

bool FAsyncIODelete::DeleteTempRootDirectory()
{
	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.DirectoryExists(*TempRoot))
	{
		return true;
	}

	// Since we sometimes will be creating the directory again immediately, we need to take precautions against the delayed delete of directories that
	// occurs on Windows platforms; creating a new file/directory in one that was just deleted can fail.  So we need to move-delete our TempRoot
	// in addition to move-delete our clients' directories.  Since we don't have a TempRoot to move-delete into, we create a unique sibling directory name.
	FString UniqueDirectory = FPaths::CreateTempFilename(*FPaths::GetPath(TempRoot), TEXT("DeleteTemp"), TEXT(""));

	const bool bReplace = false;
	const bool bEvenIfReadOnly = true;
	const TCHAR* DirectoryToDelete = *UniqueDirectory;
	if (!FileManager.Move(DirectoryToDelete, *TempRoot, bReplace, bEvenIfReadOnly))
	{
		// Move failed; fallback to inplace delete
		DirectoryToDelete = *TempRoot;
	}

	const bool bRequireExists = false;
	const bool bTree = true;
	return FileManager.DeleteDirectory(DirectoryToDelete, bRequireExists, bTree);
}

#if WITH_ASYNCIODELETE_DEBUG
void FAsyncIODelete::AddTempRoot(const FStringView& InTempRoot)
{
	FString TempRoot(InTempRoot);
	for (FString& Existing : AllTempRoots)
	{
		checkf(!FPaths::IsUnderDirectory(Existing, TempRoot), TEXT("New FAsyncIODelete has TempRoot \"%s\" that is a subdirectory of existing TempRoot \"%s\"."), *TempRoot, *Existing);
		checkf(!FPaths::IsUnderDirectory(TempRoot, Existing), TEXT("New FAsyncIODelete has TempRoot \"%s\" that is a parent directory of existing TempRoot \"%s\"."), *TempRoot, *Existing);
	}
	AllTempRoots.Add(MoveTemp(TempRoot));
}

void FAsyncIODelete::RemoveTempRoot(const FStringView& InTempRoot)
{
	AllTempRoots.Remove(FString(InTempRoot));
}
#endif
