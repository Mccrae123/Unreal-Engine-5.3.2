// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UPCGSettingsInterface;
struct FPCGContext;
struct FPCGCrc;
struct FPCGDataCollection;

class IPCGElement;
class UPCGComponent;
class UPCGSettings;
class UPCGNode;

typedef TSharedPtr<IPCGElement, ESPMode::ThreadSafe> FPCGElementPtr;

#define PCGE_LOG_C(Verbosity, CustomContext, Format, ...) \
	UE_LOG(LogPCG, \
		Verbosity, \
		TEXT("[%s - %s]: " Format), \
		*((CustomContext)->GetComponentName()), \
		*((CustomContext)->GetTaskName()), \
		##__VA_ARGS__)

#if WITH_EDITOR
#define PCGE_LOG(Verbosity, Format, ...) do{ if(ShouldLog()) { PCGE_LOG_C(Verbosity, Context, Format, ##__VA_ARGS__); } }while(0)
#else
#define PCGE_LOG(Verbosity, Format, ...) PCGE_LOG_C(Verbosity, Context, Format, ##__VA_ARGS__)
#endif

/**
* Base class for the processing bit of a PCG node/settings
*/
class PCG_API IPCGElement
{
public:
	virtual ~IPCGElement() = default;

	/** Creates a custom context object paired to this element */
	virtual FPCGContext* Initialize(const FPCGDataCollection& InputData, TWeakObjectPtr<UPCGComponent> SourceComponent, const UPCGNode* Node) = 0;

	/** Returns true if the element, in its current phase can be executed only from the main thread */
	virtual bool CanExecuteOnlyOnMainThread(FPCGContext* Context) const { return false; }

	/** Returns true if the node can be cached - also checks for instance flags, if any. */
	bool IsCacheableInstance(const UPCGSettingsInterface* InSettingsInterface) const;

	/** Returns true if the node can be cached (e.g. does not create artifacts & does not depend on untracked data */
	virtual bool IsCacheable(const UPCGSettings* InSettings) const { return true; }

	/**
	 * Calculate a Crc that provides a receipt for the input data that can be paired with output data from the cache. If any dependency (setting, node input or
	 * external data) changes then this value should change. For some elements it is inefficient or not possible to output a Crc here. These can return an invalid
	 * Crc and the Crc can either be computed during execution, or afterwards based on output data.
	 */
	virtual void GetDependenciesCrc(const FPCGDataCollection& InInput, const UPCGSettings* InSettings, UPCGComponent* InComponent, FPCGCrc& OutCrc) const;

	/** Public function that executes the element on the appropriately created context.
	* The caller should call the Execute function until it returns true.
	*/
	bool Execute(FPCGContext* Context) const;

	/** Note: the following methods must be called from the main thread */
#if WITH_EDITOR
	void DebugDisplay(FPCGContext* Context) const;
#endif

protected:
	/** This function will be called once and once only, at the beginning of an execution */
	void PreExecute(FPCGContext* Context) const;
	/** The prepare data phase is one where it is more likely to be able to multithread */
	virtual bool PrepareDataInternal(FPCGContext* Context) const;
	/** Core execution method for the given element. Will be called until it returns true. */
	virtual bool ExecuteInternal(FPCGContext* Context) const = 0;
	/** This function will be called once and once only, at the end of an execution */
	void PostExecute(FPCGContext* Context) const;

	/** Controls whether an element can skip its execution wholly when the input data has the cancelled tag */
	virtual bool IsCancellable() const { return true; }
	/** Used to specify that the element passes through the data without any manipulation - used to correct target pins, etc. */
	virtual bool IsPassthrough(const UPCGSettings* InSettings) const { return false; }

	/** Passes through data when the element is Disabled. Can be implemented to override what gets passed through. */
	virtual void DisabledPassThroughData(FPCGContext* Context) const;

#if WITH_EDITOR
	virtual bool ShouldLog() const { return true; }
#endif

private:
	void CleanupAndValidateOutput(FPCGContext* Context) const;
};

/**
* Basic PCG element class for elements that do not store any intermediate data in the context
*/
class PCG_API FSimplePCGElement : public IPCGElement
{
public:
	virtual FPCGContext* Initialize(const FPCGDataCollection& InputData, TWeakObjectPtr<UPCGComponent> SourceComponent, const UPCGNode* Node) override;
};

#if UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_2
#include "Misc/OutputDeviceRedirector.h"
#include "PCGContext.h"
#include "PCGData.h"
#endif
