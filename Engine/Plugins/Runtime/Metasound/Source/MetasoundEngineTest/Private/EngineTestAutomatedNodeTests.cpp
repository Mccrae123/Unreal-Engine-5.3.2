// Copyright Epic Games, Inc. All Rights Reserved.

#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Interfaces/MetasoundFrontendSourceInterface.h"
#include "Math/NumericLimits.h"
#include "Misc/AutomationTest.h"
#include "MetasoundAudioBuffer.h"
#include "MetasoundFrontendDataTypeRegistry.h"
#include "MetasoundFrontendRegistries.h"
#include "MetasoundFrontendSearchEngine.h"
#include "MetasoundLog.h"
#include "MetasoundPrimitives.h"
#include "MetasoundVertex.h"
#include "MetasoundVertexData.h"
#include "Templates/UniquePtr.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

#if WITH_EDITORONLY_DATA

namespace Metasound::MetasoundEngineTestPrivate {

	// Return audio mixer device if one is available
	Audio::FMixerDevice* GetAudioMixerDevice()
	{
		if (FAudioDeviceManager* DeviceManager = FAudioDeviceManager::Get())
		{
			if (FAudioDevice* AudioDevice = DeviceManager->GetMainAudioDeviceRaw())
			{
				if (AudioDevice->IsAudioMixerEnabled())
				{
					return static_cast<Audio::FMixerDevice*>(AudioDevice);
				}
			}
		}
		return nullptr;
	}

	// Create an example environment that generally exists for a UMetaSoundSoruce
	FMetasoundEnvironment GetSourceEnvironment()
	{
		using namespace Frontend;

		FMetasoundEnvironment Environment;

		Environment.SetValue<uint32>(SourceInterface::Environment::SoundUniqueID, 0);
		Environment.SetValue<bool>(SourceInterface::Environment::IsPreview, false);
		Environment.SetValue<uint64>(SourceInterface::Environment::TransmitterID, 0);
		Environment.SetValue<FString>(SourceInterface::Environment::GraphName, TEXT("ENGINE_TEST_REGISTERED_NODES"));

		if (Audio::FMixerDevice* MixerDevice = GetAudioMixerDevice())
		{
			Environment.SetValue<Audio::FDeviceId>(SourceInterface::Environment::DeviceID, MixerDevice->DeviceID);
			Environment.SetValue<int32>(SourceInterface::Environment::AudioMixerNumOutputFrames, MixerDevice->GetNumOutputFrames());
		}

		return Environment;
	}

	// TTestTypeInfo converts test types to strings
	template<typename DataType>
	struct TTestTypeInfo
	{
		static FString ToString(const DataType& InData)
		{
			return ::LexToString(InData);
		}
	};

	template<typename ElementType>
	struct TTestTypeInfo<TArray<ElementType>>
	{
		static FString ToString(const TArray<ElementType>& InData)
		{
			return FString::Printf(TEXT("[%s]"), *FString::JoinBy(InData, TEXT(","), &TTestTypeInfo<ElementType>::ToString));
		}
	};

	template<>
	struct TTestTypeInfo<FTime>
	{
		static FString ToString(const FTime& InData)
		{
			return ::LexToString(InData.GetSeconds());
		}
	};

	// TTestTypeValues should return basic bounds for tested input data types.
	// Similar to TNumericLimits<>
	template<typename DataType>
	struct TTestTypeValues
	{};

	// TArray specialization to defer to a single element array with array element's values
	template<typename ElementType>
	struct TTestTypeValues<TArray<ElementType>>
	{
		static TArray<ElementType> Min(const FOperatorSettings& InSettings) 
		{
			return TArray<ElementType>({TTestTypeValues<ElementType>::Min(InSettings)}); 
		}

		static TArray<ElementType> Max(const FOperatorSettings& InSettings) 
		{ 
			return TArray<ElementType>({TTestTypeValues<ElementType>::Max(InSettings)}); 
		}

		static TArray<ElementType> Default(const FOperatorSettings& InSettings) 
		{ 
			return TArray<ElementType>({TTestTypeValues<ElementType>::Default(InSettings)}); 
		}

		static TArray<ElementType> Random(const FOperatorSettings& InSettings)
		{ 
			return TArray<ElementType>({TTestTypeValues<ElementType>::Random(InSettings)}); 
		}
	};

	template<>
	struct TTestTypeValues<bool>
	{
		static bool Min(const FOperatorSettings& InSettings) { return false; }
		static bool Max(const FOperatorSettings& InSettings) { return true; }
		static bool Default(const FOperatorSettings& InSettings) { return true; }
		static bool Random(const FOperatorSettings& InSettings) { return FMath::RandRange(0.f, 1.f) > 0.5; }
	};

	template<>
	struct TTestTypeValues<int32>
	{
		static int32 Min(const FOperatorSettings& InSettings) { return TNumericLimits<int32>::Min(); }
		static int32 Max(const FOperatorSettings& InSettings) { return TNumericLimits<int32>::Max(); }
		static int32 Default(const FOperatorSettings& InSettings) { return 0; }
		static int32 Random(const FOperatorSettings& InSettings) { return FMath::RandRange(TNumericLimits<int32>::Min(), TNumericLimits<int32>::Max()); }
	};

	template<>
	struct TTestTypeValues<float>
	{
		static float Min(const FOperatorSettings& InSettings) { return TNumericLimits<float>::Min(); }
		static float Max(const FOperatorSettings& InSettings) { return TNumericLimits<float>::Max(); }
		static float Default(const FOperatorSettings& InSettings) { return 0.f; }
		static float Random(const FOperatorSettings& InSettings) { return FMath::RandRange(TNumericLimits<float>::Min(), TNumericLimits<float>::Max()); }
	};

	template<>
	struct TTestTypeValues<FTime>
	{
		static FTime Min(const FOperatorSettings& InSettings) { return FTime{TNumericLimits<float>::Min()}; }
		static FTime Max(const FOperatorSettings& InSettings) { return FTime{TNumericLimits<float>::Max()}; }
		static FTime Default(const FOperatorSettings& InSettings) { return FTime{0.f}; }
		static FTime Random(const FOperatorSettings& InSettings) { return FTime{FMath::RandRange(TNumericLimits<float>::Min(), TNumericLimits<float>::Max())}; }
	};

	template<>
	struct TTestTypeValues<FTrigger>
	{
		static FTrigger Min(const FOperatorSettings& InSettings) { return FTrigger{InSettings, false}; }
		static FTrigger Max(const FOperatorSettings& InSettings) 
		{ 
			FTrigger Trigger{InSettings, false};
			for (int32 i = 0; i < InSettings.GetNumFramesPerBlock(); i++)
			{
				Trigger.TriggerFrame(i);
			}
			return Trigger;
		}

		static FTrigger Default(const FOperatorSettings& InSettings) { return FTrigger{InSettings, true}; }
		static FTrigger Random(const FOperatorSettings& InSettings) 
		{ 
			FTrigger Trigger{InSettings, false};
			int32 NumTriggers = FMath::RandRange(0, InSettings.GetNumFramesPerBlock());
			while (NumTriggers > 0)
			{
				Trigger.TriggerFrame(FMath::RandRange(0, InSettings.GetNumFramesPerBlock()));
				NumTriggers--;
			}
			return Trigger;
		}
	};

	template<>
	struct TTestTypeValues<FString>
	{
		static FString Min(const FOperatorSettings& InSettings) { return TEXT(""); }
		static FString Max(const FOperatorSettings& InSettings) { return TEXT("THIS IS SUPPOSED TO REPRESENT A MAXIMUM STRING BUT THERE IS NO SUCH THING SO?"); }
		static FString Default(const FOperatorSettings& InSettings) { return TEXT("TestString"); }
		static FString Random(const FOperatorSettings& InSettings) { return TEXT("We should probably implement a random string."); }
	};

	// Interface for mutating data references
	struct IDataReferenceMutator
	{
		virtual ~IDataReferenceMutator() = default;
		virtual void SetDefault(const FOperatorSettings& InSettings, const FAnyDataReference& InDataRef) const = 0;
		virtual void SetMax(const FOperatorSettings& InSettings, const FAnyDataReference& InDataRef) const = 0;
		virtual void SetMin(const FOperatorSettings& InSettings, const FAnyDataReference& InDataRef) const = 0; 
		virtual void SetRandom(const FOperatorSettings& InSettings, const FAnyDataReference& InDataRef) const = 0;
		virtual FString ToString(const FAnyDataReference& InDataRef) const = 0;
	};

	template<typename DataType>
	struct TDataReferenceMutator : IDataReferenceMutator
	{
		virtual void SetDefault(const FOperatorSettings& InSettings, const FAnyDataReference& InDataRef) const override
		{
			*InDataRef.GetDataWriteReference<DataType>() = TTestTypeValues<DataType>::Default(InSettings);
		}

		virtual void SetMax(const FOperatorSettings& InSettings, const FAnyDataReference& InDataRef) const override
		{
			*InDataRef.GetDataWriteReference<DataType>() = TTestTypeValues<DataType>::Max(InSettings);
		}

		virtual void SetMin(const FOperatorSettings& InSettings, const FAnyDataReference& InDataRef) const override
		{
			*InDataRef.GetDataWriteReference<DataType>() = TTestTypeValues<DataType>::Min(InSettings);
		}

		virtual void SetRandom(const FOperatorSettings& InSettings, const FAnyDataReference& InDataRef) const override
		{
			*InDataRef.GetDataWriteReference<DataType>() = TTestTypeValues<DataType>::Random(InSettings);
		}

		virtual FString ToString(const FAnyDataReference& InDataRef) const override
		{
			if (const DataType* Data = InDataRef.GetValue<DataType>())
			{
				return FString::Printf(TEXT("%s:%s"), *GetMetasoundDataTypeString<DataType>(), *TTestTypeInfo<DataType>::ToString(*Data));
			}
			else
			{
				// Data references should never be null
				UE_LOG(LogMetaSound, Error, TEXT("Failed to get data type value of type %s"), *GetMetasoundDataTypeString<DataType>());
				return TEXT("");
			}
		}
	};


	template<typename DataType>
	void AddDataReferenceMutatorEntryToMap(TMap<FName, TSharedPtr<const IDataReferenceMutator>>& InMap)
	{
		InMap.Add(GetMetasoundDataTypeName<DataType>(), MakeShared<TDataReferenceMutator<DataType>>());
	}

	// Returns map of mutable input types
	const TMap<FName, TSharedPtr<const IDataReferenceMutator>>& GetDataTypeGeneratorMap()
	{
		static TMap<FName, TSharedPtr<const IDataReferenceMutator>> Map;

		AddDataReferenceMutatorEntryToMap<bool>(Map);
		AddDataReferenceMutatorEntryToMap<int32>(Map);
		AddDataReferenceMutatorEntryToMap<float>(Map);
		AddDataReferenceMutatorEntryToMap<FString>(Map);
		AddDataReferenceMutatorEntryToMap<FTime>(Map);
		AddDataReferenceMutatorEntryToMap<FTrigger>(Map);
		AddDataReferenceMutatorEntryToMap<TArray<bool>>(Map);
		AddDataReferenceMutatorEntryToMap<TArray<int32>>(Map);
		AddDataReferenceMutatorEntryToMap<TArray<float>>(Map);
		AddDataReferenceMutatorEntryToMap<TArray<FString>>(Map);
		AddDataReferenceMutatorEntryToMap<TArray<FTime>>(Map);
		AddDataReferenceMutatorEntryToMap<TArray<FTrigger>>(Map);

		return Map;
	}

	// Convenience class for setting node input data reference values to default, min, max or random values. 
	struct FInputVertexDataTestController
	{
		struct FMutableInput
		{
			FAnyDataReference DataReference;
			FVertexName VertexName;
			TSharedPtr<const IDataReferenceMutator> DataReferenceMutator;
		};

		FInputVertexDataTestController(const FOperatorSettings& InSettings, const FInputVertexInterface& InInputInterface, const FInputVertexInterfaceData& InInputData)
		: Settings(InSettings)
		{
			const TMap<FName, TSharedPtr<const IDataReferenceMutator>>& GeneratorMap = GetDataTypeGeneratorMap();

			for (const FInputDataVertex& Vertex : InInputInterface)
			{
				if (GeneratorMap.Contains(Vertex.DataTypeName))
				{
					if (const FAnyDataReference* Ref = InInputData.FindDataReference(Vertex.VertexName))
					{
						if (EDataReferenceAccessType::Write == Ref->GetAccessType())
						{
							MutableInputs.Add(FMutableInput{*Ref, Vertex.VertexName, GeneratorMap[Vertex.DataTypeName]});
						}
					}
				}
			}
		}

		int32 GetNumMutableInputs() const
		{
			return MutableInputs.Num();
		}

		void SetMutableInputsToMin()
		{
			for (const FMutableInput& MutableInput : MutableInputs)
			{
				MutableInput.DataReferenceMutator->SetMin(Settings, MutableInput.DataReference);
			}
			UE_LOG(LogMetaSound, Verbose, TEXT("Setting operator input values:%s%s"), LINE_TERMINATOR, *FString::Join(GetInputValueStrings(), LINE_TERMINATOR));
		}

		void SetMutableInputsToMax()
		{
			for (const FMutableInput& MutableInput : MutableInputs)
			{
				MutableInput.DataReferenceMutator->SetMax(Settings, MutableInput.DataReference);
			}
			UE_LOG(LogMetaSound, Verbose, TEXT("Setting operator input values:%s%s"), LINE_TERMINATOR, *FString::Join(GetInputValueStrings(), LINE_TERMINATOR));
		}

		void SetMutableInputsToDefault()
		{
			for (const FMutableInput& MutableInput : MutableInputs)
			{
				MutableInput.DataReferenceMutator->SetDefault(Settings, MutableInput.DataReference);
			}
			UE_LOG(LogMetaSound, Verbose, TEXT("Setting operator input values:%s%s"), LINE_TERMINATOR, *FString::Join(GetInputValueStrings(), LINE_TERMINATOR));
		}

		void SetMutableInputsToRandom()
		{
			for (const FMutableInput& MutableInput : MutableInputs)
			{
				MutableInput.DataReferenceMutator->SetRandom(Settings, MutableInput.DataReference);
			}
			UE_LOG(LogMetaSound, Verbose, TEXT("Setting operator input values:%s%s"), LINE_TERMINATOR, *FString::Join(GetInputValueStrings(), LINE_TERMINATOR));
		}

		TArray<FString> GetInputValueStrings() const
		{
			TArray<FString> ValueStrings;
			for (const FMutableInput& MutableInput : MutableInputs)
			{
				ValueStrings.Add(FString::Printf(TEXT("%s %s"), *MutableInput.VertexName.ToString(), *MutableInput.DataReferenceMutator->ToString(MutableInput.DataReference)));
			}
			return ValueStrings;
		}

	private:

		FOperatorSettings Settings;
		TArray<FMutableInput> MutableInputs;
	};

	static const FLazyName TestNodeName{"TEST_NODE"};
	static const FLazyName TestVertexName{"TEXT_VERTEX"};
	static const FGuid TestNodeID{0xA5A5A5A5, 0xA5A5A5A5, 0xA5A5A5A5, 0xA5A5A5A5};

	// Create a node from a node registry key
	TUniquePtr<INode> CreateNode(const Frontend::FNodeRegistryKey& InNodeRegistryKey)
	{
		using namespace Frontend;

		TUniquePtr<INode> Node;

		FMetasoundFrontendRegistryContainer* NodeRegistry = FMetasoundFrontendRegistryContainer::Get();
		check(nullptr != NodeRegistry);
		IDataTypeRegistry& DataTypeRegistry = IDataTypeRegistry::Get();

		// Lookup node class metadata to determine how to create this node.
		FMetasoundFrontendClass NodeClass;
		if (!NodeRegistry->FindFrontendClassFromRegistered(InNodeRegistryKey, NodeClass))
		{
			UE_LOG(LogMetaSound, Error, TEXT("Failed to find registered class with registry key %s"), *InNodeRegistryKey);
			return MoveTemp(Node);
		}

		// Build node differently dependent upon node type
		switch (NodeClass.Metadata.GetType())
		{
			case EMetasoundFrontendClassType::VariableDeferredAccessor:
			case EMetasoundFrontendClassType::VariableAccessor:
			case EMetasoundFrontendClassType::VariableMutator:
			case EMetasoundFrontendClassType::External:
			case EMetasoundFrontendClassType::Graph:
			{
				FNodeInitData NodeInitData{TestNodeName, TestNodeID};
				Node = NodeRegistry->CreateNode(InNodeRegistryKey, NodeInitData);
			}
			break;

			case EMetasoundFrontendClassType::Input:
			{
				FName DataTypeName = NodeClass.Metadata.GetClassName().Name;
				FInputNodeConstructorParams NodeInitData
				{
					TestNodeName, 
					TestNodeID, 
					TestVertexName,
					NodeClass.Interface.Inputs[0].DefaultLiteral.ToLiteral(DataTypeName)
				};

				Node = DataTypeRegistry.CreateInputNode(DataTypeName, MoveTemp(NodeInitData));
			}
			break;

			case EMetasoundFrontendClassType::Variable:
			{
				FName DataTypeName = NodeClass.Metadata.GetClassName().Name;
				FDefaultLiteralNodeConstructorParams NodeInitData{TestNodeName, TestNodeID, DataTypeRegistry.CreateDefaultLiteral(DataTypeName)};
				Node = DataTypeRegistry.CreateVariableNode(DataTypeName, MoveTemp(NodeInitData));
			}
			break;

			case EMetasoundFrontendClassType::Literal:
			{
				FName DataTypeName = NodeClass.Metadata.GetClassName().Name;
				FDefaultLiteralNodeConstructorParams NodeInitData{TestNodeName, TestNodeID, DataTypeRegistry.CreateDefaultLiteral(DataTypeName)};
				Node = DataTypeRegistry.CreateLiteralNode(DataTypeName, MoveTemp(NodeInitData));
			}
			break;

			case EMetasoundFrontendClassType::Output:
			{
				FName DataTypeName = NodeClass.Metadata.GetClassName().Name;
				FDefaultNamedVertexNodeConstructorParams NodeInitData{TestNodeName, TestNodeID, TestVertexName};
				Node = DataTypeRegistry.CreateOutputNode(DataTypeName, MoveTemp(NodeInitData));
			}
			break;

			case EMetasoundFrontendClassType::Template:
			default:
			static_assert(static_cast<int32>(EMetasoundFrontendClassType::Invalid) == 10, "Possible missed EMetasoundFrontendClassType case coverage");
		}

		return MoveTemp(Node);
	}

	// Create input vertex interface data for a node.
	FInputVertexInterfaceData CreateInputVertexInterfaceData(const INode& InNode, const FOperatorSettings& InOperatorSettings)
	{
		Frontend::IDataTypeRegistry& DataTypeRegistry = Frontend::IDataTypeRegistry::Get();

		// Populate inputs to node. 
		FVertexInterface NodeVertexInterface = InNode.GetVertexInterface();
		FInputVertexInterfaceData NodeInputVertexInterfaceData{NodeVertexInterface.GetInputInterface()};

		for (const FInputDataVertex& InputVertex : NodeVertexInterface.GetInputInterface())
		{
			if (InputVertex.AccessType != EVertexAccessType::Reference)
			{
				// Not testing constructor inputs.
				continue;
			}

			// input data type must be registered in order to create it.
			if (DataTypeRegistry.IsRegistered(InputVertex.DataTypeName))
			{
				Frontend::FDataTypeRegistryInfo DataTypeInfo;
				ensure(DataTypeRegistry.GetDataTypeInfo(InputVertex.DataTypeName, DataTypeInfo));

				// Can only create data types that are parsable from a literal
				if (DataTypeInfo.bIsParsable)
				{
					FLiteral DefaultLiteral = DataTypeRegistry.CreateDefaultLiteral(InputVertex.DataTypeName);
					TOptional<FAnyDataReference> DataReference = DataTypeRegistry.CreateDataReference(InputVertex.DataTypeName, EDataReferenceAccessType::Write, DefaultLiteral, InOperatorSettings);

					if (!DataReference.IsSet())
					{
						UE_LOG(LogMetaSound, Error, TEXT("Failed to create data reference for data type %s "), *InputVertex.DataTypeName.ToString());
						continue;
					}

					NodeInputVertexInterfaceData.BindVertex(InputVertex.VertexName, *DataReference);
				}
			}
		}
		return NodeInputVertexInterfaceData;
	}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FMetasoundAutomatedNodeTest, "Audio.Metasound.AutomatedNodeTest", EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)
void FMetasoundAutomatedNodeTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray <FString>& OutTestCommands) const
{
	using namespace Metasound;

	// Get all the classes that have been registered
	Frontend::ISearchEngine& NodeSearchEngine = Frontend::ISearchEngine::Get();
	TArray<FMetasoundFrontendClass> AllClasses = NodeSearchEngine.FindAllClasses(true /* IncludeAllVersions */);

	FMetasoundFrontendRegistryContainer* NodeRegistry = FMetasoundFrontendRegistryContainer::Get();
	check(nullptr != NodeRegistry);

	for (const FMetasoundFrontendClass& NodeClass : AllClasses)
	{
		// Exclude template classes because they cannot be created directly from the node registry
		if (NodeClass.Metadata.GetType() == EMetasoundFrontendClassType::Template)
		{
			continue;
		}

		Frontend::FNodeRegistryKey NodeRegistryKey = NodeRegistry->GetRegistryKey(NodeClass.Metadata);
		
		OutBeautifiedNames.Add(FString::Printf(TEXT("%s %s"), *NodeClass.Metadata.GetClassName().ToString(), *NodeClass.Metadata.GetVersion().ToString()));
		// Test commands are node registry keys
		OutTestCommands.Add(NodeRegistryKey);
	}

	UE_LOG(LogMetaSound, Verbose, TEXT("Found %d metasound nodes to test"), OutTestCommands.Num());
}

bool FMetasoundAutomatedNodeTest::RunTest(const FString& InRegistryKey)
{
	using namespace Metasound;
	using namespace MetasoundEngineTestPrivate;

	static const FOperatorSettings OperatorSettings{48000  /* samplerate */, 100.f /* block rate */};
	static const FMetasoundEnvironment SourceEnvironment = GetSourceEnvironment();

	TUniquePtr<INode> Node = CreateNode(InRegistryKey);
	if (!Node.IsValid())
	{
		AddError(FString::Printf(TEXT("Failed to create node %s from registry"), *InRegistryKey));
		return false;
	}

	// Populate inputs to node. 
	FInputVertexInterfaceData NodeInputVertexInterfaceData = CreateInputVertexInterfaceData(*Node, OperatorSettings);
	FInputVertexInterface InputInterface = Node->GetVertexInterface().GetInputInterface();
	FInputVertexDataTestController InputTester(OperatorSettings, InputInterface, NodeInputVertexInterfaceData);

	// Create operator 
	FBuildOperatorParams BuildParams
	{
		*Node,
		OperatorSettings,
		NodeInputVertexInterfaceData,
		SourceEnvironment
	};

	// Convenience function for testing entire lifecycle of an individual operator 
	// with a variety of inputs.
	auto RunTestIteration = [&]() -> bool
	{
		FBuildResults BuildResults;
		TUniquePtr<IOperator> Operator = Node->GetDefaultOperatorFactory()->CreateOperator(BuildParams, BuildResults);

		if (!Operator.IsValid())
		{
			AddError(FString::Printf(TEXT("Failed to create operator from node %s."), *InRegistryKey));
			return false;
		}
		
		IOperator::FExecuteFunction OpExecFunc = Operator->GetExecuteFunction();

		if (OpExecFunc)
		{
			OpExecFunc(Operator.Get());

			if (InputTester.GetNumMutableInputs() > 0)
			{
				InputTester.SetMutableInputsToDefault();
				OpExecFunc(Operator.Get());

				InputTester.SetMutableInputsToMin();
				OpExecFunc(Operator.Get());

				InputTester.SetMutableInputsToMax();
				OpExecFunc(Operator.Get());

				InputTester.SetMutableInputsToRandom();
				OpExecFunc(Operator.Get());
			}
		}

		return true;
	};

	bool bSuccess = true;
	// Test entire operator lifecycle with different starting conditions if
	// any of the inputs are mutable
	InputTester.SetMutableInputsToDefault();
	bSuccess &= RunTestIteration();
	if (InputTester.GetNumMutableInputs() > 0)
	{
		InputTester.SetMutableInputsToMin();
		bSuccess &= RunTestIteration();
		InputTester.SetMutableInputsToMax();
		bSuccess &= RunTestIteration();
		InputTester.SetMutableInputsToRandom();
		bSuccess &= RunTestIteration();
	}

	return bSuccess;
}

#endif // WITH_EDITORONLY_DATA

#endif //WITH_DEV_AUTOMATION_TESTS
