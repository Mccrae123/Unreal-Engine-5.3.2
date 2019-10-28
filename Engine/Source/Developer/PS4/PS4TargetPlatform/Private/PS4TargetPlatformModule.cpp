// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "PS4TargetPlatform.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/ITargetPlatformModule.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "FPS4TargetPlatformModule"

// Holds the target platform singleton.
static ITargetPlatform* Singleton = NULL;


/**
 * Module for the PS4 target platform.
 */
class FPS4TargetPlatformModule
	: public ITargetPlatformModule
{
public:

	/** Destructor. */
	~FPS4TargetPlatformModule()
	{
		delete Singleton;
		Singleton = NULL;
	}
	
public:

	// ITargetPlatformModule interface

	virtual ITargetPlatform* GetTargetPlatform() override
	{
		if (Singleton == NULL && FPS4TargetPlatform::IsUsable())
		{
			// Make sure sure PS4 sdk is installed
			FString SCEDir = FPlatformMisc::GetEnvironmentVariable( TEXT( "SCE_ROOT_DIR" ) );
			if (SCEDir.Len() > 0)
			{
				FString DLLPath = FString::Printf( TEXT( "%s\\ORBIS\\Tools\\Target Manager Server\\bin\\Interop.ORTMAPILib.dll" ), *SCEDir );
				if( FPaths::FileExists( DLLPath ) )
				{
					Singleton = new FPS4TargetPlatform();
				}
			}
		}

		return Singleton;
	}

public:

	// IModuleInterface interface

	virtual void StartupModule() override
	{
	}

	virtual void ShutdownModule() override
	{
	}
};


#undef LOCTEXT_NAMESPACE


IMPLEMENT_MODULE( FPS4TargetPlatformModule, PS4TargetPlatform);
