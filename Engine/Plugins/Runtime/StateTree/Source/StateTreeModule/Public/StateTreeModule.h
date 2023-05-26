// Copyright Epic Games, Inc. All Rights Reserved.

#pragma  once

#include "Modules/ModuleManager.h"

namespace UE::Trace
{
	class FStoreClient;
}

/**
* The public interface to this module
*/
class IStateTreeModule : public IModuleInterface
{

public:

	/**
	* Singleton-like access to this module's interface.  This is just for convenience!
	* Beware of calling this during the shutdown phase, though.  Your module might have been unloaded already.
	*
	* @return Returns singleton instance, loading the module on demand if needed
	*/
	static IStateTreeModule& Get()
	{
		return FModuleManager::LoadModuleChecked<IStateTreeModule>("StateTreeModule");
	}

	/**
	* Checks to see if this module is loaded and ready.  It is only valid to call Get() if IsAvailable() returns true.
	*
	* @return True if the module is loaded and ready to use
	*/
	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("StateTreeModule");
	}

	/**
	 * Start tracing and enables StateTree debugging related channels (frame + statetree).
	 * If traces are already active we keep track of all channels previously activated to restore them on stop.
	 */
	virtual void StartTraces() = 0;

	/**
	 * Stops the trace service if it was not already connected when StartTraces was called.
	 * Restores previously enabled channels if necessary.
	 */
	virtual void StopTraces() = 0;

#if WITH_STATETREE_DEBUGGER
	/**
	 * Gets the store client.
	 */
	virtual UE::Trace::FStoreClient* GetStoreClient() = 0;
#endif // WITH_STATETREE_DEBUGGER
};


#if UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_2
#include "CoreMinimal.h"
#endif
