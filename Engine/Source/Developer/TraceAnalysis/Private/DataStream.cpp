// Copyright Epic Games, Inc. All Rights Reserved.

#include "DataStream.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformProcess.h"

namespace Trace
{

FFileStream::FFileStream(const TCHAR* InFilePath)
	: FilePath(InFilePath)
{
	OpenFileInternal();
}

FFileStream::~FFileStream()
{
	delete Inner;
}

int32 FFileStream::Read(void* Data, uint32 Size)
{
	if (!Inner)
	{
		return 0;
	}

	uint64 Remaining;
	while (true)
	{
		Remaining = End - Cursor;
		if (Remaining)
		{
			break;
		}

		FPlatformProcess::Sleep(0.25f);
		UpdateFileSize();
	}

	uint64 Size64 = Size;
	Size64 = (Remaining < Size64) ? Remaining : Size64;

	if (!Inner->Read((uint8*)Data, Size64))
	{
		return 0;
	}

	Cursor += Size64;
	return int32(Size64);
}

void FFileStream::UpdateFileSize()
{
	delete Inner;
	OpenFileInternal();
	if (Inner)
	{
		Inner->Seek(Cursor);
	}
}

void FFileStream::OpenFileInternal()
{
	IPlatformFile& FileSystem = IPlatformFile::GetPlatformPhysical();
	Inner = FileSystem.OpenRead(*FilePath, true);
	if (Inner)
	{
		End = Inner->Size();
	}
}

IInDataStream* DataStream_ReadFile(const TCHAR* FilePath)
{
	IPlatformFile& FileSystem = IPlatformFile::GetPlatformPhysical();
	if (!FileSystem.FileExists(FilePath))
	{
		return nullptr;
	}
	return new FFileStream(FilePath);
}

}
