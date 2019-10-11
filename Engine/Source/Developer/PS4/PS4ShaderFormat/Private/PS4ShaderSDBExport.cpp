// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "PS4ShaderFormat.h"
#include "Serialization/MemoryReader.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFilemanager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

//
// These CVars are configured in PS4Engine.ini, under the [ShaderCompiler] section
// e.g.
//		[ShaderCompiler]
//		r.PS4ShaderSDBMode=1
//		r.PS4DumpShaderSDB=1
//		r.PS4SDBZip=1
//
static TAutoConsoleVariable<int32> CVarPS4ShaderSDBMode(
	TEXT("r.PS4ShaderSDBMode"),
	0,
	TEXT("Whether to include SDB data in the shader compiler output.\n")
	TEXT(" 0: Disabled. No SDB data is generated.\n")
	TEXT(" 1: Enabled, but file hashes are forced to zero.\n")
	TEXT(" 2: Enabled, with full file hashes, generating all unique combinations.\n"),
	ECVF_ReadOnly);

static TAutoConsoleVariable<int32> CVarPS4DumpShaderSDB(
	TEXT("r.PS4DumpShaderSDB"),
	0,
	TEXT("When enabled, dumps any shader SDBs found in the cook to the shader debug info path, even if -PS4SDBExport is not present in the command line.\n")
	TEXT("If -PS4SDKExport is present, the path given on the command line takes precedence over the automatic shader debug info path.\n")
	TEXT("r.PS4ShaderSDBMode must be non-zero for SDB dump to work."),
	ECVF_ReadOnly);

static TAutoConsoleVariable<int32> CVarPS4SDBZip(
	TEXT("r.PS4SDBZip"),
	0,
	TEXT("When enabled, writes exported SDB files to a single, uncompressed zip file for easier management and archival.\n")
	TEXT("Equivalent to passing -PS4SDBZip on the command line to the cook process. Use with r.PS4DumpShaderSDB."),
	ECVF_ReadOnly);

#if WITH_ENGINE

DECLARE_LOG_CATEGORY_CLASS(LogPS4SDBExport, Display, Display);

FZipArchiveWriter::FZipArchiveWriter(IFileHandle* InFile)
	: File(InFile)
{
}

FZipArchiveWriter::~FZipArchiveWriter()
{
	// Zip File Format Specification:
	// https://www.loc.gov/preservation/digital/formats/digformatspecs/APPNOTE%2820120901%29_Version_6.3.3.txt

	UE_LOG(LogPS4SDBExport, Display, TEXT("Closing zip file with %d entries."), Files.Num());

	// Write the file directory
	uint64 DirStartOffset = Tell();
	for (FFileEntry& Entry : Files)
	{
		const static uint8 Footer[] =
		{
			0x50, 0x4b, 0x01, 0x02, 0x3f, 0x00, 0x2d, 0x00,
			0x00, 0x00, 0x00, 0x00
		};
		Write((void*)Footer, sizeof(Footer));
		Write(Entry.Time);
		Write(Entry.Crc32);

		Write((uint64)0xffffffffffffffff);
		Write((uint16)Entry.Filename.Len());
		const static uint8 Fields[] =
		{
			0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x20, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff
		};
		Write((void*)Fields, sizeof(Fields));
		Write((void*)TCHAR_TO_UTF8(*Entry.Filename), Entry.Filename.Len());
		
		Write((uint16)0x01);
		Write((uint16)0x1c);

		Write((uint64)Entry.Length);
		Write((uint64)Entry.Length);
		Write((uint64)Entry.Offset);
		Write((uint32)0);

		Flush();
	}
	uint64 DirEndOffset = Tell();

	uint64 DirectorySizeInBytes = DirEndOffset - DirStartOffset;

	// Write ZIP64 end of central directory record
	const static uint8 Record[] =
	{
		0x50, 0x4b, 0x06, 0x06, 0x2c, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x2d, 0x00, 0x2d, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	Write((void*)Record, sizeof(Record));
	Write((uint64)Files.Num());
	Write((uint64)Files.Num());
	Write(DirectorySizeInBytes);
	Write(DirStartOffset);

	// Write ZIP64 end of central directory locator
	const static uint8 Locator[] =
	{
		0x50, 0x4b, 0x06, 0x07, 0x00, 0x00, 0x00, 0x00,
	};
	Write((void*)Locator, sizeof(Locator));
	Write(DirEndOffset);
	Write((uint32)0x01);

	// Write normal end of central directory record
	const static uint8 EndRecord[] =
	{
		0x50, 0x4b, 0x05, 0x06, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0x00, 0x00
	};
	Write((void*)EndRecord, sizeof(EndRecord));

	Flush();

	if (File)
	{
		// Close the file
		delete File;
		File = nullptr;
	}
}

void FZipArchiveWriter::AddFile(const FString& Filename, const TArray<uint8>& Data, const FDateTime& Timestamp)
{
	uint32 Crc = FCrc::MemCrc32(Data.GetData(), Data.Num());

	// Convert the date-time to a zip file timestamp (2-second resolution).
	uint32 ZipTime =
		(Timestamp.GetSecond()         /  2) |
		(Timestamp.GetMinute()        <<  5) |
		(Timestamp.GetHour()          << 11) |
		(Timestamp.GetDay()           << 16) |
		(Timestamp.GetMonth()         << 21) |
		((Timestamp.GetYear() - 1980) << 25);

	uint64 FileOffset = Tell();

	FFileEntry* Entry = new (Files) FFileEntry(Filename, Crc, Data.Num(), FileOffset, ZipTime);

	static const uint8 Header[] =
	{
		0x50, 0x4b, 0x03, 0x04, 0x2d, 0x00, 0x00, 0x00,
		0x00, 0x00
	};
	Write((void*)Header, sizeof(Header));
	Write(ZipTime);
	Write(Crc);
	Write((uint64)0xffffffffffffffff);
	Write((uint16)Filename.Len());
	Write((uint16)0x20);

	Write((void*)TCHAR_TO_UTF8(*Entry->Filename), Filename.Len());

	Write((uint16)0x01);
	Write((uint16)0x1c);
	Write((uint64)Data.Num());
	Write((uint64)Data.Num());
	Write((uint64)FileOffset);
	Write((uint32)0);

	Write((void*)Data.GetData(), Data.Num());

	Flush();
}

void FZipArchiveWriter::Flush()
{
	if (Buffer.Num())
	{
		if (File && !File->Write(Buffer.GetData(), Buffer.Num()))
		{
			UE_LOG(LogPS4SDBExport, Error, TEXT("Failed to write to zip file. Zip file writing aborted."));
			delete File;
			File = nullptr;
		}

		Buffer.Reset(Buffer.Num());
	}
}

FPS4ShaderSDBExport::FPS4ShaderSDBExport()
	: bExportSDBs(false)
	, TotalSDBBytes(0)
	, TotalSDBs(0)
	, ZipWriter(nullptr)
{
	// Setup the CVar values from PS4 engine ini on module startup.
	FConfigFile PS4EngineSettings;
	FConfigCacheIni::LoadLocalIniFile(PS4EngineSettings, TEXT("Engine"), true, TEXT("PS4"));

	const FConfigSection* ConfigSection = PS4EngineSettings.Find(TEXT("ShaderCompiler"));
	if (ConfigSection)
	{
		const TCHAR* CVars[] =
		{
			TEXT("r.PS4ShaderSDBMode"),
			TEXT("r.PS4DumpShaderSDB"),
			TEXT("r.PS4SDBZip"),
		};

		for (int32 Index = 0; Index < ARRAY_COUNT(CVars); ++Index)
		{
			const auto CVar = IConsoleManager::Get().FindConsoleVariable(CVars[Index]);
			check(CVar);

			const FConfigValue* ConfigValue = ConfigSection->Find(FName(CVars[Index]));
			if (ConfigValue)
			{
				int32 Value;
				if (LexTryParseString(Value, *ConfigValue->GetValue()))
				{
					CVar->Set(Value, ECVF_SetBySystemSettingsIni);
				}
			}
		}
	}
}

FPS4ShaderSDBExport::~FPS4ShaderSDBExport()
{
	if (ZipWriter)
	{
		delete ZipWriter;
		ZipWriter = nullptr;
	}
}

void FPS4ShaderSDBExport::Initialize()
{
	// Use the path provided by the command line if present
	if (!FParse::Value(FCommandLine::Get(), TEXT("-PS4SDBExport="), SDBExportPath))
	{
		// Otherwise, use the debug shader info path if r.PS4DumpShaderSDB is enabled.
		if (CVarPS4DumpShaderSDB->GetInt() != 0)
		{
			SDBExportPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(
				*(FPaths::ProjectSavedDir() / TEXT("ShaderDebugInfo") / NAME_SF_PS4.ToString() / TEXT("sdb")));
		}
	}

	if (SDBExportPath.Len())
	{
		// Check if SDBs are enabled in the cook, otherwise nothing will happen.
		if (CVarPS4ShaderSDBMode->GetInt() == 0)
		{
			UE_LOG(LogPS4SDBExport, Error, TEXT("SDB export is enabled, but r.PS4ShaderSDBMode is zero. No SDBs will be exported."));
		}
		else
		{
			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
			bExportSDBs = PlatformFile.CreateDirectoryTree(*SDBExportPath);

			if (!bExportSDBs)
			{
				UE_LOG(LogPS4SDBExport, Error, TEXT("Failed to create SDB output directory. SDB export will be disabled."));
			}
			else if (FParse::Param(FCommandLine::Get(), TEXT("PS4SDBZip")) || CVarPS4SDBZip->GetInt() != 0)
			{
				FString SingleFilePath = SDBExportPath / TEXT("sdb.zip");

				IFileHandle* SDBZipFile = PlatformFile.OpenWrite(*SingleFilePath);
				if (!SDBZipFile)
				{
					UE_LOG(LogPS4SDBExport, Error, TEXT("Failed to create SDB output file \"%s\". SDB export will be disabled."), *SingleFilePath);
					bExportSDBs = false;
				}
				else
				{
					ZipWriter = new FZipArchiveWriter(SDBZipFile);
				}
			}
		}
	}

	if (bExportSDBs)
	{
		UE_LOG(LogPS4SDBExport, Display, TEXT("SDB export enabled. Output directory: \"%s\""), *SDBExportPath);
		if (ZipWriter)
		{
			UE_LOG(LogPS4SDBExport, Display, TEXT("SDB zip mode enabled. SDBs will be archived in a single (uncompressed) zip file."));
		}
	}
}

void FPS4ShaderSDBExport::NotifyShaderCooked(const TArray<uint8>& PlatformDebugData)
{
	static bool bFirst = true;
	if (bFirst)
	{
		// If we get called, we know we're cooking. Do one time initialization
		// which will create the output directory / open the open file stream.
		Initialize();
		bFirst = false;
	}

	if (bExportSDBs)
	{
		// Deserialize the platform debug data
		FPS4ShaderDebugData DebugData;
		FMemoryReader Ar(PlatformDebugData);
		Ar << DebugData;

		for (const FSDB& SDB : DebugData.SDBs)
		{
			if (SDB.Contents.Num() == 0)
				continue; // No data in this SDB.

			// Skip this SDB if we've already exported the SDB hash before.
			bool bAlreadyInSet = false;
			ExportedShaderHashes.Add(SDB.Hash, &bAlreadyInSet);
			if (bAlreadyInSet)
			{
				// We've already exported this shader hash
				continue;
			}

			// Emit periodic log messages detailing the size of the SDB output file/directory.
			static uint64 LastReport = 0;
			TotalSDBBytes += PlatformDebugData.Num();
			TotalSDBs++;

			if ((TotalSDBBytes - LastReport) >= (64 * 1024 * 1024))
			{
				UE_LOG(LogPS4SDBExport, Display, TEXT("SDB export size: %.2f MB, count: %llu"), double(TotalSDBBytes) / (1024.0 * 1024.0), TotalSDBBytes, TotalSDBs);
				LastReport = TotalSDBBytes;
			}

			if (ZipWriter)
			{
				// Append the platform data to the zip file
				ZipWriter->AddFile(FSDB::HashToExtension(SDB.Hash), SDB.Contents, FDateTime::Now());
			}
			else
			{
				// Write the SDB to the export directory
				FString OutputPath = SDBExportPath / FSDB::HashToExtension(SDB.Hash);
				IFileHandle* File = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*OutputPath);
				if (!File || !File->Write(SDB.Contents.GetData(), SDB.Contents.Num()))
				{
					UE_LOG(LogPS4SDBExport, Error, TEXT("Failed to export SDB file \"%s\"."), *OutputPath);
				}

				if (File)
				{
					delete File;
				}
			}
		}
	}
}

#endif // WITH_ENGINE
