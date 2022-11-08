// Copyright Epic Games, Inc. All Rights Reserved.

#include "../../Source/stdafx.h"
#include <pthread.h>
#include <sys/mman.h>

namespace BlackmagicPlatform
{

	bool InitializeAPI()
	{
		return true;
	}

	void ReleaseAPI()
	{
	}

	IDeckLinkIterator* CreateDeckLinkIterator()
	{
		IDeckLinkIterator* DeckLinkIterator = CreateDeckLinkIteratorInstance();

		if (!DeckLinkIterator)
		{
			LOG_ERROR(TEXT("A DeckLink iterator could not be created. The DeckLink drivers may not be installed."));
		}

		return DeckLinkIterator;
	}

	void DestroyDeckLinkIterator(IDeckLinkIterator* DeckLink)
	{
		if (DeckLink)
		{
			DeckLink->Release();
		}
	}

	IDeckLinkVideoConversion* CreateDeckLinkVideoConversion()
	{
		IDeckLinkVideoConversion* DeckLinkVideoConversion = CreateVideoConversionInstance();

		if (!DeckLinkVideoConversion)
		{
			LOG_ERROR(TEXT("A DeckLink video conversion could not be created. The DeckLink drivers may not be installed."));
		}

		return DeckLinkVideoConversion;
	}

	void DestroyDeckLinkVideoConversion(IDeckLinkVideoConversion* DeckLink)
	{
		if (DeckLink)
		{
			DeckLink->Release();
		}
	}

	void SetThreadPriority_TimeCritical(std::thread& InThread)
	{
        pthread_attr_t Attributes;
        int Policy = 0;
        int Priority = 0;
        pthread_attr_init(&Attributes);
        pthread_attr_getschedpolicy(&Attributes, &Policy);
        Policy = sched_get_priority_max(Priority);

        pthread_setschedprio(InThread.native_handle(), Priority);

        pthread_attr_destroy(&Attributes);
	}

	bool GetDisplayName(IDeckLink* Device, TCHAR* OutDisplayName, int32_t Size)
	{
		const char* DisplayName = 0;
		HRESULT Result = Device->GetDisplayName(&DisplayName);
		if (Result == S_OK)
		{
			std::wstring_convert<std::codecvt_utf8_utf16<TCHAR>, TCHAR> Convert;
			auto OutSize = Convert.from_bytes(DisplayName).copy(OutDisplayName, Size - 1, 0);
			OutDisplayName[OutSize] = 0;
			free((void *)DisplayName);
			return true;
		}

		return false;
	}

	void* Allocate(uint32_t Size)
	{
		// TODO: Use mmap with, would MAP_SHARED be needed?
		return malloc(Size);
	}

	bool Free(void* Address, uint32_t Size)
	{
		free(Address);
		return true;
	}
}
