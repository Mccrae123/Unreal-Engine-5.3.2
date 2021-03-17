// Copyright Epic Games, Inc. All Rights Reserved.

#include "Export.h"
#include "ResourcesIDs.h"
#include "Error.h"
#include "AutoChangeDatabase.h"
#include "Exporter.h"

#include "exp.h"
#include "Sight.hpp"
DISABLE_SDK_WARNINGS_START
#include "AttributeReader.hpp"
DISABLE_SDK_WARNINGS_END
#include "FileSystem.hpp"

BEGIN_NAMESPACE_UE_AC

enum : GSType
{
	kDatasmithFileRefCon = 'Tuds',
	kStrFileType = 'TEXT',
	kStrFileCreator = '    '
};

const utf8_t* StrFileExtension = "udatasmith";

static GSErrCode __ACENV_CALL SaveToDatasmithFile(const API_IOParams* IOParams, Modeler::SightPtr sight)
{
	GSErrCode GSErr =
		TryFunction("FExport::SaveDatasmithFile", FExport::SaveDatasmithFile, (void*)IOParams, (void*)&sight);
	ACAPI_KeepInMemory(true);
	return GSErr;
}

GSErrCode FExport::Register()
{
	return ACAPI_Register_FileType(kDatasmithFileRefCon, kStrFileType, kStrFileCreator, StrFileExtension, 0,
								   LocalizeResId(kStrListFileTypes), 1, SaveAs3DSupported);
}

GSErrCode FExport::Initialize()
{
	GSErrCode GSErr = ACAPI_Install_FileTypeHandler3D(kDatasmithFileRefCon, SaveToDatasmithFile);
	if (GSErr != NoError)
	{
		UE_AC_DebugF("FExport::Initialize - ACAPI_Install_FileTypeHandler3D error=%d\n", GSErr);
	}
	return GSErr;
}

GSErrCode FExport::SaveDatasmithFile(void* inIOParams, void* InSight)
{
	const API_IOParams&		 IOParams = *(const API_IOParams*)inIOParams;
	const Modeler::SightPtr& sight = *reinterpret_cast< const Modeler::SightPtr* >(InSight);

	try
	{
		FAutoChangeDatabase db(APIWind_FloorPlanID);

		ModelerAPI::Model		 model;
		Modeler::ConstModel3DPtr model3D(sight->GetMainModelPtr());
		AttributeReader			 AttrReader; // deprecated constructor, temporary!
		UE_AC_TestGSError(EXPGetModel(model3D, &model, &AttrReader));

		FExporter exporter;
		exporter.DoExport(model, IOParams);
	}
	catch (...)
	{
		IO::fileSystem.Delete(*IOParams.fileLoc); // Delete tmp file
		throw;
	}
	return GS::NoError;
}

END_NAMESPACE_UE_AC
