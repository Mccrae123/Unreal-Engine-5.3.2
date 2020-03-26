// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/UnrealString.h"
#include "HAL/CriticalSection.h"
#include "Misc/DateTime.h"
#include "Modules/ModuleInterface.h"
#include "Templates/Atomic.h"

struct EDITORANALYTICSSESSION_API FEditorAnalyticsSession
{
	enum class EEventType
	{
		Crashed = 0,
		GpuCrashed,
		Terminated,
	};

	FString SessionId;

	FString AppId;
	FString AppVersion;
	FString UserId;

	FString ProjectName;
	FString ProjectID;
	FString ProjectDescription;
	FString ProjectVersion;
	FString EngineVersion;
	int32 PlatformProcessID;

	FDateTime StartupTimestamp;
	FDateTime Timestamp;
	volatile int32 IdleSeconds; // Can be updated from concurrent threads.
	volatile int32 Idle1Min;
	volatile int32 Idle5Min;
	volatile int32 Idle30Min;
	FString CurrentUserActivity;
	TArray<FString> Plugins;
	float AverageFPS;

	FString DesktopGPUAdapter;
	FString RenderingGPUAdapter;
	uint32 GPUVendorID;
	uint32 GPUDeviceID;
	uint32 GRHIDeviceRevision;
	FString GRHIAdapterInternalDriverVersion;
	FString GRHIAdapterUserDriverVersion;

	uint64 TotalPhysicalRAM;
	int32 CPUPhysicalCores;
	int32 CPULogicalCores;
	FString CPUVendor;
	FString CPUBrand;

	FString OSMajor;
	FString OSMinor;
	FString OSVersion;

	bool bIs64BitOS : 1;
	bool bCrashed : 1;
	bool bGPUCrashed : 1;
	bool bIsDebugger : 1;
	bool bWasEverDebugger : 1;
	bool bIsVanilla : 1;
	bool bIsTerminating : 1;
	bool bWasShutdown : 1;
	bool bIsInPIE : 1;
	bool bIsInEnterprise : 1;
	bool bIsInVRMode : 1;
	bool bIsLowDriveSpace : 1;

	FEditorAnalyticsSession();

	/** 
	 * Save this session to stored values.
	 * @returns true if the session was successfully saved.
	 */
	bool Save();

	/**
	 *  Load a session with the given session ID from stored values.
	 * @returns true if the session was found and successfully loaded.
	 */
	bool Load(const FString& InSessionID);

	/**
	 * Delete the stored values of this session.
	 * Does not modify the actual session object.
	 * @returns true if the session was successfully deleted.
	 */
	bool Delete() const;

	/**
	 * Retrieve a list of session IDs that are currently stored locally.
	 * @returns true if the session IDs were successfully retrieved.
	 */
	static bool GetStoredSessionIDs(TArray<FString>& OutSessions);

	/**
	 * Read all stored sessions into the given array.
	 * @returns true if the sessions were successfully loaded.
	 */
	static bool LoadAllStoredSessions(TArray<FEditorAnalyticsSession>& OutSessions);

	/**
	 * Save the given session IDs to storage.
	 * @returns true if the session IDs were successfully saved.
	 */
	static bool SaveStoredSessionIDs(const TArray<FString>& InSessions);

	/**
	 * Try to acquire the local storage lock without blocking.
	 * @return true if the lock was acquired successfully.
	 */
	static bool TryLock() { return Lock(FTimespan::Zero()); }

	/**
	 * Acquire a lock for local storage.
	 * @returns true if the lock was acquired successfully.
	 */
	static bool Lock(FTimespan Timeout = FTimespan::Zero());

	/**
	 * Unlock the local storage.
	 */
	static void Unlock();

	/** Is the local storage already locked? */
	static bool IsLocked();

	/**
	 * Append an event to the session log. The function is meant to record concurrent events, especially during a crash
	 * with minimum contention. The logger appends and persists the events of interest locklessly on spot as opposed to
	 * overriding existing values in the key-store. Appending is better because it prevent dealing with event ordering
	 * on the spot (no synchronization needed) and will preserve more information.
	 *
	 * @note They key-store is not easily usable in a 'lockless' fashion. On Windows, the OS provides thread safe API
	 *       to modify the registry (add/update). On Mac/Linux, the key-store is a simple file and without synchronization,
	 *       concurrent writes will likely corrupt the file.
	 */
	void LogEvent(EEventType EventTpe, const FDateTime& Timestamp);

private:
	static FSystemWideCriticalSection* StoredValuesLock;

	/** 
	 * Has this session already been saved? 
	 * If not, then the first save will write out session invariant details such as hardware specs.
	 */
	bool bAlreadySaved : 1;
};

class FEditorAnalyticsSessionModule : public IModuleInterface
{

};