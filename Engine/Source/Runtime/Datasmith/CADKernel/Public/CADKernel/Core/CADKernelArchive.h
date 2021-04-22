// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CADKernel/Core/Types.h"
#include "CADKernel/UI/Message.h"

#include "HAL/FileManager.h"
#include "Serialization/Archive.h"

namespace CADKernel
{
	class FEntity;
	class FSession;

	class FCADKernelArchive
	{
	public:
		FArchive& Archive;
		FSession& Session;

		FCADKernelArchive(FSession& InSession, FArchive& InArchive)
			: Archive(InArchive)
			, Session(InSession)
		{
		}

		static TSharedPtr<FCADKernelArchive> CreateArchiveWriter(FSession& InSession, const TCHAR* FileName)
		{
			FArchive* Ar = IFileManager::Get().CreateFileWriter(FileName);
			if (Ar == nullptr)
			{
				FMessage::Printf(Log, TEXT("The archive file %s is corrupted\n"), FileName);
				return TSharedPtr<FCADKernelArchive>();
			}
			return MakeShared<FCADKernelArchive>(InSession, *Ar);
		}

		static TSharedPtr<FCADKernelArchive> CreateArchiveReader(FSession& InSession, const TCHAR* FileName)
		{
			FArchive* Ar = IFileManager::Get().CreateFileReader(FileName);
			if (Ar == nullptr)
			{
				FMessage::Printf(Log, TEXT("The archive file %s is corrupted\n"), FileName);
				return TSharedPtr<FCADKernelArchive>();
			}
			return MakeShared<FCADKernelArchive>(InSession, *Ar);
		}

		template<typename EntityType>
		void operator<<(EntityType& Entity)
		{
			Archive << Entity;
		}

		bool IsLoading() const
		{
			return Archive.IsLoading();
		}

		bool IsSaving() const
		{
			return Archive.IsSaving();
		}

		void Serialize(void* Value, int64 Length) 
		{ 
			Archive.Serialize(Value, Length);
		}

		void SetReferencedEntityOrAddToWaitingList(FIdent ArchiveId, TWeakPtr<FEntity>& Entity);
		void SetReferencedEntityOrAddToWaitingList(FIdent ArchiveId, TSharedPtr<FEntity>& Entity);
		void AddEntityToSave(FIdent Id);
		void AddEntityFromArchive(TSharedRef<FEntity>& Entity);

		template<typename EntityType>
		void AddEntityFromArchive(TSharedRef<EntityType>& Entity)
		{
			AddEntityFromArchive((TSharedRef<FEntity>&) Entity);
		}

		int64 TotalSize()
		{
			return Archive.TotalSize();
		}

		int64 Tell()
		{
			return Archive.Tell();
		}

		void Close()
		{
			Archive.Close();
		}
	};
}

