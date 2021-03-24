// Copyright Epic Games, Inc. All Rights Reserved.

#include "MetasoundFrontend.h"

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "MetasoundAudioBuffer.h"
#include "MetasoundDataTypeRegistrationMacro.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendGraph.h"
#include "MetasoundFrontendRegistries.h"
#include "MetasoundJsonBackend.h"
#include "MetasoundOperatorBuilder.h"
#include "MetasoundPrimitives.h"
#include "MetasoundRouter.h"
#include "Modules/ModuleManager.h"
#include "StructDeserializer.h"
#include "StructSerializer.h"
#include "Serialization/MemoryReader.h"

static int32 MetasoundUndoRollLimitCvar = 128;
FAutoConsoleVariableRef CVarMetasoundUndoRollLimit(
	TEXT("au.Metasound.Frontend.UndoRollLimit"),
	MetasoundUndoRollLimitCvar,
	TEXT("Sets the maximum size of our undo buffer for graph editing in the Metasound Frontend.\n")
	TEXT("n: Number of undoable actions we buffer."),
	ECVF_Default);

namespace Metasound
{
	namespace Frontend
	{
		TArray<FNodeClassInfo> GetAllAvailableNodeClasses()
		{
			TArray<FNodeClassInfo> OutClasses;

			auto& Registry = GetExternalNodeRegistry();
			for (auto& NodeClassTuple : Registry)
			{
				FNodeClassInfo ClassInfo;
				ClassInfo.NodeType = EMetasoundFrontendClassType::External;
				ClassInfo.LookupKey = NodeClassTuple.Key;

				OutClasses.Add(ClassInfo);
			}

			return OutClasses;
		}


		// gets all metadata (name, description, author, what to say if it's missing) for a given node.
		FMetasoundFrontendClassMetadata GenerateClassMetadata(const FNodeClassInfo& InInfo)
		{
			return GenerateClassDescription(InInfo).Metadata;
		}

		FMetasoundFrontendClass GenerateClassDescription(const FNodeClassMetadata& InNodeMetadata, EMetasoundFrontendClassType ClassType)
		{
			FMetasoundFrontendClass ClassDescription;

			ClassDescription.Metadata = FMetasoundFrontendClassMetadata(InNodeMetadata);
			ClassDescription.Metadata.Type = ClassType;

			FMetasoundFrontendClassStyleDisplay DisplayStyle(InNodeMetadata.DisplayStyle);
			ClassDescription.Style = FMetasoundFrontendClassStyle
			{
				DisplayStyle
			};

			FMetasoundFrontendClassInterface& ClassInterface = ClassDescription.Interface;

			// External metasounds aren't dependent on any other nodes by definition, so all we need to do
			// is populate the Input and Output sets.
			const FInputVertexInterface& InputInterface = InNodeMetadata.DefaultInterface.GetInputInterface();
			TArray<int>& InputSortOrder = ClassInterface.InputStyle.DefaultSortOrder;
			for (auto& InputTuple : InputInterface)
			{
				// TODO: lots to be added here. 
				FMetasoundFrontendClassInput ClassInput;

				ClassInput.Name = InputTuple.Value.GetVertexName();
				ClassInput.TypeName = InputTuple.Value.GetDataTypeName();
				ClassInput.VertexID = FGuid::NewGuid();
				ClassInput.Metadata.DisplayName = FText::FromString(InputTuple.Value.GetVertexName());
				ClassInput.Metadata.Description = InputTuple.Value.GetDescription();

				FLiteral DefaultLiteral = InputTuple.Value.GetDefaultLiteral();
				if (DefaultLiteral.GetType() != ELiteralType::Invalid)
				{
					ClassInput.DefaultLiteral.SetFromLiteral(DefaultLiteral);
				}

				const int32 OrderIndex = InputInterface.GetOrderIndex(InputTuple.Key);
				InputSortOrder.Add(OrderIndex);

				ClassInterface.Inputs.Add(ClassInput);
			}

			const FOutputVertexInterface& OutputInterface = InNodeMetadata.DefaultInterface.GetOutputInterface();
			TArray<int32>& OutputSortOrder = ClassInterface.InputStyle.DefaultSortOrder;
			for (auto& OutputTuple : OutputInterface)
			{
				// TODO: lots to be added here. 
				FMetasoundFrontendClassOutput ClassOutput;

				ClassOutput.Name = OutputTuple.Value.GetVertexName();
				ClassOutput.TypeName = OutputTuple.Value.GetDataTypeName();
				ClassOutput.VertexID = FGuid::NewGuid();
				ClassOutput.Metadata.DisplayName = FText::FromString(OutputTuple.Value.GetVertexName());
				ClassOutput.Metadata.Description = OutputTuple.Value.GetDescription();

				const int32 OrderIndex = OutputInterface.GetOrderIndex(OutputTuple.Key);
				ClassInterface.OutputStyle.DefaultSortOrder.Add(OrderIndex);

				ClassDescription.Interface.Outputs.Add(ClassOutput);
			}

			for (auto& EnvTuple : InNodeMetadata.DefaultInterface.GetEnvironmentInterface())
			{
				// TODO: lots to be added here. 
				FMetasoundFrontendClassEnvironmentVariable EnvVar;

				EnvVar.Name = EnvTuple.Value.GetVertexName();
				EnvVar.Metadata.DisplayName = FText::FromString(EnvTuple.Value.GetVertexName());
				EnvVar.Metadata.Description = EnvTuple.Value.GetDescription();
				EnvVar.bIsRequired = true;

				ClassDescription.Interface.Environment.Add(EnvVar);
			}

			return ClassDescription;
		}

		FMetasoundFrontendClass GenerateClassDescription(const FNodeClassInfo& InInfo)
		{
			auto& Registry = GetExternalNodeRegistry();
			if (Registry.Contains(InInfo.LookupKey))
			{
				return Registry[InInfo.LookupKey].CreateFrontendClass();
			}
			else
			{
				ensureAlwaysMsgf(false, TEXT("Cannot generate description of unregistered node [NodeClassName:%s]"), *InInfo.LookupKey.NodeClassFullName.ToString());
				return FMetasoundFrontendClass();
			}
		}

		TArray<FName> GetAllAvailableDataTypes()
		{
			return FMetasoundFrontendRegistryContainer::Get()->GetAllValidDataTypes();
		}

		bool GetTraitsForDataType(FName InDataType, FDataTypeRegistryInfo& OutInfo)
		{
			return FMetasoundFrontendRegistryContainer::Get()->GetInfoForDataType(InDataType, OutInfo);
		}

		bool ImportJSONToMetasound(const FString& InJSON, FMetasoundFrontendDocument& OutMetasoundDocument)
		{
			TArray<uint8> ReadBuffer;
			ReadBuffer.SetNumUninitialized(InJSON.Len() * sizeof(ANSICHAR));
			FMemory::Memcpy(ReadBuffer.GetData(), StringCast<ANSICHAR>(*InJSON).Get(), InJSON.Len() * sizeof(ANSICHAR));
			FMemoryReader MemReader(ReadBuffer);

			TJsonStructDeserializerBackend<DefaultCharType> Backend(MemReader);
			bool DeserializeResult = FStructDeserializer::Deserialize(OutMetasoundDocument, Backend);

			MemReader.Close();
			return DeserializeResult && !MemReader.IsError();
		}

		bool ImportJSONAssetToMetasound(const FString& InPath, FMetasoundFrontendDocument& OutMetasoundDocument)
		{
			if (TUniquePtr<FArchive> FileReader = TUniquePtr<FArchive>(IFileManager::Get().CreateFileReader(*InPath)))
			{
				TJsonStructDeserializerBackend<DefaultCharType> Backend(*FileReader);
				bool DeserializeResult = FStructDeserializer::Deserialize(OutMetasoundDocument, Backend);

				FileReader->Close();
				return DeserializeResult && !FileReader->IsError();
			}

			return false;
		}

	}
}

class FMetasoundFrontendModule : public IModuleInterface
{
	virtual void StartupModule() override
	{
		Metasound::Frontend::RegisterPendingNodes();
	}
};


REGISTER_METASOUND_DATATYPE(bool, "Bool", ::Metasound::ELiteralType::Boolean)
REGISTER_METASOUND_DATATYPE(int32, "Int32", ::Metasound::ELiteralType::Integer)
REGISTER_METASOUND_DATATYPE(int64, "Int64", ::Metasound::ELiteralType::Integer)
REGISTER_METASOUND_DATATYPE(float, "Float", ::Metasound::ELiteralType::Float)
REGISTER_METASOUND_DATATYPE(double, "Double", ::Metasound::ELiteralType::Float)
REGISTER_METASOUND_DATATYPE(FString, "String", ::Metasound::ELiteralType::String)

REGISTER_METASOUND_DATATYPE(Metasound::FTrigger, "Trigger", ::Metasound::ELiteralType::Boolean)
REGISTER_METASOUND_DATATYPE(Metasound::FTime, "Time", ::Metasound::ELiteralType::Float)
REGISTER_METASOUND_DATATYPE(Metasound::FAudioBuffer, "Audio")
REGISTER_METASOUND_DATATYPE(Metasound::FSendAddress, "Transmission:Address", ::Metasound::ELiteralType::String)

IMPLEMENT_MODULE(FMetasoundFrontendModule, MetasoundFrontend);
