// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Array.h"
#include "MetasoundAssetBase.h"
#include "MetasoundBuilderInterface.h"
#include "MetasoundFrontendController.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundGraph.h"
#include "MetasoundVertex.h"

class FText;
class FName;
class FString;

namespace Metasound
{
	namespace Frontend
	{
		namespace Invalid
		{
			const FText& GetInvalidText();
			const FName& GetInvalidName();
			const FString& GetInvalidString();
			const FMetasoundFrontendVertexMetadata& GetInvalidVertexMetadata();
			const FMetasoundFrontendLiteral& GetInvalidLiteral();
			const FMetasoundFrontendClassInterface& GetInvalidClassInterface();
			const FMetasoundFrontendClassMetadata& GetInvalidClassMetadata();
			const FMetasoundFrontendInterfaceStyle& GetInvalidInterfaceStyle();
			const FMetasoundFrontendClassStyle& GetInvalidClassStyle();
			const FMetasoundFrontendGraphStyle& GetInvalidGraphStyle();
			const FMetasoundFrontendGraphClass& GetInvalidGraphClass();
			const TArray<FMetasoundFrontendClass>& GetInvalidClassArray();
			const TArray<FMetasoundFrontendGraphClass>& GetInvalidGraphClassArray();
			const FMetasoundFrontendDocumentMetadata& GetInvalidDocumentMetadata();
		}

		/** FInvalidOutputController is a controller which is always invalid. 
		 *
		 *  All methods return defaults and return error flags or invalid values.
		 */
		class METASOUNDFRONTEND_API FInvalidOutputController : public IOutputController
		{
		public:
			FInvalidOutputController() = default;

			virtual ~FInvalidOutputController() = default;


			bool IsValid() const override { return false; }
			FGuid GetID() const override { return Metasound::FrontendInvalidID; }
			const FName& GetDataType() const override { return Invalid::GetInvalidName(); }
			const FVertexName& GetName() const override { return Invalid::GetInvalidName(); }
			FText GetDisplayName() const override { return FText::GetEmpty(); }
			const FText& GetTooltip() const override { return FText::GetEmpty(); }
			const FMetasoundFrontendVertexMetadata& GetMetadata() const override { return Invalid::GetInvalidVertexMetadata(); }
			FGuid GetOwningNodeID() const override { return Metasound::FrontendInvalidID; }
			FNodeHandle GetOwningNode() override;
			FConstNodeHandle GetOwningNode() const override;
			void SetName(const FVertexName& InName) override { }
			bool IsConnected() const override { return false; }
			TArray<FInputHandle> GetConnectedInputs() override { return TArray<FInputHandle>(); }
			TArray<FConstInputHandle> GetConstConnectedInputs() const override { return TArray<FConstInputHandle>(); }
			bool Disconnect() override { return false; }

			FConnectability CanConnectTo(const IInputController& InController) const override { return FConnectability(); }
			bool Connect(IInputController& InController) override { return false; }
			bool ConnectWithConverterNode(IInputController& InController, const FConverterNodeInfo& InNodeClassName) override { return false; }
			bool Disconnect(IInputController& InController) override { return false; }

		protected:
			FDocumentAccess ShareAccess() override { return FDocumentAccess(); }
			FConstDocumentAccess ShareAccess() const override { return FConstDocumentAccess(); }

		};

		/** FInvalidInputController is a controller which is always invalid. 
		 *
		 *  All methods return defaults and return error flags or invalid values.
		 */
		class METASOUNDFRONTEND_API FInvalidInputController : public IInputController 
		{
		public:
			FInvalidInputController() = default;
			virtual ~FInvalidInputController() = default;

			bool IsValid() const override { return false; }
			FGuid GetID() const override { return Metasound::FrontendInvalidID; }
			bool IsConnected() const override { return false; }
			const FName& GetDataType() const override { return Invalid::GetInvalidName(); }
			const FVertexName& GetName() const override { return Invalid::GetInvalidName(); }
			FText GetDisplayName() const override { return Invalid::GetInvalidText(); }
			const FText& GetTooltip() const override { return Invalid::GetInvalidText(); }
			const FMetasoundFrontendVertexMetadata& GetMetadata() const override { return Invalid::GetInvalidVertexMetadata(); }
			const FMetasoundFrontendLiteral* GetLiteral() const override { return nullptr; }
			void SetLiteral(const FMetasoundFrontendLiteral& InLiteral) { };
			const FMetasoundFrontendLiteral* GetClassDefaultLiteral() const override { return nullptr; }
			FGuid GetOwningNodeID() const override { return Metasound::FrontendInvalidID; }
			FNodeHandle GetOwningNode() override;
			FConstNodeHandle GetOwningNode() const override;

			virtual FOutputHandle GetConnectedOutput() override { return IOutputController::GetInvalidHandle(); }
			virtual FConstOutputHandle GetConnectedOutput() const override { return IOutputController::GetInvalidHandle(); }
			bool Disconnect() override { return false; }

			void SetName(const FVertexName& InName) override { }

			virtual FConnectability CanConnectTo(const IOutputController& InController) const override { return FConnectability(); }
			virtual bool Connect(IOutputController& InController) override { return false; }
			virtual bool ConnectWithConverterNode(IOutputController& InController, const FConverterNodeInfo& InNodeClassName) override { return false; }
			virtual bool Disconnect(IOutputController& InController) override { return false; }
		protected:
			FDocumentAccess ShareAccess() override { return FDocumentAccess(); }
			FConstDocumentAccess ShareAccess() const override { return FConstDocumentAccess(); }
		};

		class METASOUNDFRONTEND_API FInvalidVariableController : public IVariableController
		{
		public:
			FInvalidVariableController() = default;
			virtual ~FInvalidVariableController() = default;

			/** Returns true if the controller is in a valid state. */
			virtual bool IsValid() const override { return false; }

			virtual FGuid GetID() const override { return Metasound::FrontendInvalidID; }
			
			/** Returns the data type name associated with this output. */
			virtual const FName& GetDataType() const override { return Invalid::GetInvalidName(); }
			
			/** Returns the name associated with this variable. */
			virtual const FName& GetName() const override { return Invalid::GetInvalidName(); }
			virtual void SetName(const FName&) override { }

			/** Returns the human readable name associated with this output. */
			virtual FText GetDisplayName() const override { return Invalid::GetInvalidText(); }
			virtual void SetDisplayName(const FText&) override { }
			virtual FText GetDescription() const override { return Invalid::GetInvalidText(); }
			virtual void SetDescription(const FText&) override { }

			virtual FNodeHandle FindMutatorNode() override { return INodeController::GetInvalidHandle(); }
			virtual FConstNodeHandle FindMutatorNode() const override { return INodeController::GetInvalidHandle(); }
			virtual TArray<FNodeHandle> FindAccessorNodes() override { return TArray<FNodeHandle>(); }
			virtual TArray<FConstNodeHandle> FindAccessorNodes() const override { return TArray<FConstNodeHandle>(); }
			virtual TArray<FNodeHandle> FindDeferredAccessorNodes() override { return TArray<FNodeHandle>(); }
			virtual TArray<FConstNodeHandle> FindDeferredAccessorNodes() const override { return TArray<FConstNodeHandle>(); }
			
			/** Returns a FGraphHandle to the node which owns this output. */
			virtual FGraphHandle GetOwningGraph() override { return IGraphController::GetInvalidHandle(); }
			
			/** Returns a FConstGraphHandle to the node which owns this output. */
			virtual FConstGraphHandle GetOwningGraph() const override { return IGraphController::GetInvalidHandle(); }

			/** Returns the value for the given variable instance if set. */
			virtual const FMetasoundFrontendLiteral& GetLiteral() const override { return Invalid::GetInvalidLiteral(); }

			/** Sets the value for the given variable instance */
			virtual bool SetLiteral(const FMetasoundFrontendLiteral& InLiteral) override { return false; }
		protected:
			FDocumentAccess ShareAccess() override { return FDocumentAccess(); }
			FConstDocumentAccess ShareAccess() const override { return FConstDocumentAccess(); }
		};


		/** FInvalidNodeController is a controller which is always invalid. 
		 *
		 *  All methods return defaults and return error flags or invalid values.
		 */
		class METASOUNDFRONTEND_API FInvalidNodeController : public INodeController 
		{

		public:
			FInvalidNodeController() = default;
			virtual ~FInvalidNodeController() = default;

			bool IsValid() const override { return false; }

			TArray<FInputHandle> GetInputs() override { return TArray<FInputHandle>(); }
			TArray<FOutputHandle> GetOutputs() override { return TArray<FOutputHandle>(); }
			TArray<FConstInputHandle> GetConstInputs() const override { return TArray<FConstInputHandle>(); }
			TArray<FConstOutputHandle> GetConstOutputs() const override { return TArray<FConstOutputHandle>(); }

			virtual FInputHandle GetInputWithVertexName(const FVertexName& InName) override { return IInputController::GetInvalidHandle(); }
			virtual FConstInputHandle GetConstInputWithVertexName(const FVertexName& InName) const override { return IInputController::GetInvalidHandle(); }
			virtual FOutputHandle GetOutputWithVertexName(const FVertexName& InName) override { return IOutputController::GetInvalidHandle(); }
			virtual FConstOutputHandle GetConstOutputWithVertexName(const FVertexName& InName) const override { return IOutputController::GetInvalidHandle(); }
			FInputHandle GetInputWithID(FGuid InVertexID) override { return IInputController::GetInvalidHandle(); }
			FOutputHandle GetOutputWithID(FGuid InVertexID) override { return IOutputController::GetInvalidHandle(); }
			FConstInputHandle GetInputWithID(FGuid InVertexID) const override { return IInputController::GetInvalidHandle(); }
			FConstOutputHandle GetOutputWithID(FGuid InVertexID) const override { return IOutputController::GetInvalidHandle(); }

			const FMetasoundFrontendNodeStyle& GetNodeStyle() const override { static const FMetasoundFrontendNodeStyle Invalid; return Invalid; }
			void SetNodeStyle(const FMetasoundFrontendNodeStyle& InNodeStyle) { }
			void SetNodeName(const FVertexName& InName) override { }

			FNodeHandle ReplaceWithVersion(const FMetasoundFrontendVersionNumber& InNewVersion) override { return INodeController::GetInvalidHandle(); }

			bool CanAddInput(const FVertexName& InVertexName) const override { return false; }
			FInputHandle AddInput(const FVertexName& InVertexName, const FMetasoundFrontendLiteral* InDefault) override { return IInputController::GetInvalidHandle(); }
			bool RemoveInput(FGuid InVertexID) override { return false; }

			bool CanAddOutput(const FVertexName& InVertexName) const override { return false; }
			FInputHandle AddOutput(const FVertexName& InVertexName, const FMetasoundFrontendLiteral* InDefault) override { return IInputController::GetInvalidHandle(); }
			bool RemoveOutput(FGuid InVertexID) override { return false; }

			bool ClearInputLiteral(FGuid InVertexID) override { return false; };
			const FMetasoundFrontendLiteral* GetInputLiteral(const FGuid& InVertexID) const { return nullptr; }
			void SetInputLiteral(const FMetasoundFrontendVertexLiteral& InVertexLiteral) override { }

			const FMetasoundFrontendClassInterface& GetClassInterface() const override { return Invalid::GetInvalidClassInterface(); }
			const FMetasoundFrontendClassMetadata& GetClassMetadata() const override { return Invalid::GetInvalidClassMetadata(); }
			const FMetasoundFrontendInterfaceStyle& GetInputStyle() const override { return Invalid::GetInvalidInterfaceStyle(); }
			const FMetasoundFrontendInterfaceStyle& GetOutputStyle() const override { return Invalid::GetInvalidInterfaceStyle(); }
			const FMetasoundFrontendClassStyle& GetClassStyle() const override { return Invalid::GetInvalidClassStyle(); }

			const FText& GetDescription() const override { return Invalid::GetInvalidText(); }

			bool IsRequired() const override { return false; }

			bool DiffAgainstRegistryInterface(FClassInterfaceUpdates& OutInterfaceUpdates, bool bInUseHighestMinorVersion) const override { return false; }
			bool CanAutoUpdate(FClassInterfaceUpdates* OutInterfaceUpdates = nullptr) const override { return false; }
			FMetasoundFrontendVersionNumber FindHighestVersionInRegistry() const override { return FMetasoundFrontendVersionNumber::GetInvalid(); }
			FMetasoundFrontendVersionNumber FindHighestMinorVersionInRegistry() const override { return FMetasoundFrontendVersionNumber::GetInvalid(); }

			TSharedRef<IGraphController> AsGraph() override;
			TSharedRef<const IGraphController> AsGraph() const override;

			FGuid GetID() const override { return Metasound::FrontendInvalidID; }
			FGuid GetClassID() const override { return Metasound::FrontendInvalidID; }

			FGuid GetOwningGraphClassID() const override { return Metasound::FrontendInvalidID; }
			TSharedRef<IGraphController> GetOwningGraph() override;
			TSharedRef<const IGraphController> GetOwningGraph() const override;

			void IterateInputs(TUniqueFunction<void(FInputHandle)> InFunction) override { }
			void IterateConstInputs(TUniqueFunction<void(FConstInputHandle)> InFunction) const override { }

			void IterateOutputs(TUniqueFunction<void(FOutputHandle)> InFunction) override { }
			void IterateConstOutputs(TUniqueFunction<void(FConstOutputHandle)> InFunction) const override { }

			int32 GetNumInputs() const override { return 0; }
			int32 GetNumOutputs() const override { return 0; }

			const FVertexName& GetNodeName() const override { return Invalid::GetInvalidName(); }
			FText GetDisplayName() const override { return Invalid::GetInvalidText(); }
			const FText& GetDisplayTitle() const override { return Invalid::GetInvalidText(); }
			void SetDescription(const FText& InDescription) override { }
			void SetDisplayName(const FText& InText) override { }

		protected:
			FDocumentAccess ShareAccess() override { return FDocumentAccess(); }
			FConstDocumentAccess ShareAccess() const override { return FConstDocumentAccess(); }
		};

		/** FInvalidGraphController is a controller which is always invalid. 
		 *
		 *  All methods return defaults and return error flags or invalid values.
		 */
		class METASOUNDFRONTEND_API FInvalidGraphController : public IGraphController 
		{
			public:
			FInvalidGraphController() = default;
			virtual ~FInvalidGraphController() = default;

			bool IsValid() const override { return false; }
			FGuid GetClassID() const override { return Metasound::FrontendInvalidID; }
			FText GetDisplayName() const override { return Invalid::GetInvalidText(); }

			TArray<FVertexName> GetInputVertexNames() const override { return TArray<FVertexName>(); }
			TArray<FVertexName> GetOutputVertexNames() const override { return TArray<FVertexName>(); }

			TArray<FNodeHandle> GetNodes() override { return TArray<FNodeHandle>(); }
			TArray<FConstNodeHandle> GetConstNodes() const override { return TArray<FConstNodeHandle>(); }

			FConstNodeHandle GetNodeWithID(FGuid InNodeID) const override { return INodeController::GetInvalidHandle(); }
			FNodeHandle GetNodeWithID(FGuid InNodeID) override { return INodeController::GetInvalidHandle(); }

			TArray<FNodeHandle> GetOutputNodes() override { return TArray<FNodeHandle>(); }
			TArray<FNodeHandle> GetInputNodes() override { return TArray<FNodeHandle>(); }
			TArray<FConstNodeHandle> GetConstOutputNodes() const override { return TArray<FConstNodeHandle>(); }
			TArray<FConstNodeHandle> GetConstInputNodes() const override { return TArray<FConstNodeHandle>(); }

			virtual FVariableHandle AddVariable(const FName& InDataTypeName) override { return IVariableController::GetInvalidHandle(); }
			virtual FVariableHandle FindVariable(const FGuid& InVariableID) override { return IVariableController::GetInvalidHandle(); }
			virtual FConstVariableHandle FindVariable(const FGuid& InVariableID) const { return IVariableController::GetInvalidHandle(); }
			virtual bool RemoveVariable(const FGuid& InVariableID) override { return false; }
			virtual TArray<FVariableHandle> GetVariables() override { return TArray<FVariableHandle>(); }
			virtual TArray<FConstVariableHandle> GetVariables() const override { return TArray<FConstVariableHandle>(); }
			virtual FNodeHandle FindOrAddVariableMutatorNode(const FGuid& InVariableID) override { return INodeController::GetInvalidHandle(); }
			virtual FNodeHandle AddVariableAccessorNode(const FGuid& InVariableID) override { return INodeController::GetInvalidHandle(); }
			virtual FNodeHandle AddVariableDeferredAccessorNode(const FGuid& InVariableID) override{ return INodeController::GetInvalidHandle(); }


			const FMetasoundFrontendGraphStyle& GetGraphStyle() const override { return Invalid::GetInvalidGraphStyle(); }
			void SetGraphStyle(const FMetasoundFrontendGraphStyle& InStyle) override { }

			void ClearGraph() override { };

			void IterateConstNodes(TFunctionRef<void(FConstNodeHandle)> InFunction, EMetasoundFrontendClassType InClassType /* = EMetasoundFrontendClassType::Invalid */) const override { }
			void IterateNodes(TFunctionRef<void(FNodeHandle)> InFunction, EMetasoundFrontendClassType InClassType /* = EMetasoundFrontendClassType::Invalid */) override { }

			bool ContainsOutputVertex(const FVertexName& InName, const FName& InTypeName) const override { return false; }
			bool ContainsOutputVertexWithName(const FVertexName& InName) const override { return false; }
			bool ContainsInputVertex(const FVertexName& InName, const FName& InTypeName) const override { return false; }
			bool ContainsInputVertexWithName(const FVertexName& InName) const override { return false; }

			FConstNodeHandle GetOutputNodeWithName(const FVertexName& InName) const override { return INodeController::GetInvalidHandle(); }
			FConstNodeHandle GetInputNodeWithName(const FVertexName& InName) const override { return INodeController::GetInvalidHandle(); }
			FNodeHandle GetOutputNodeWithName(const FVertexName& InName) override { return INodeController::GetInvalidHandle(); }
			FNodeHandle GetInputNodeWithName(const FVertexName& InName) override { return INodeController::GetInvalidHandle(); }

			FConstClassInputAccessPtr FindClassInputWithName(const FVertexName& InName) const override { return FConstClassInputAccessPtr(); }
			FConstClassOutputAccessPtr FindClassOutputWithName(const FVertexName& InName) const override { return FConstClassOutputAccessPtr(); }

			FNodeHandle AddInputVertex(const FMetasoundFrontendClassInput& InDescription) override { return INodeController::GetInvalidHandle(); }
			TSharedRef<INodeController> AddInputVertex(const FVertexName& InName, const FName InTypeName, const FText& InToolTip, const FMetasoundFrontendLiteral* InDefaultValue) override { return INodeController::GetInvalidHandle(); }
			bool RemoveInputVertex(const FVertexName& InputName) override { return false; }

			FNodeHandle AddOutputVertex(const FMetasoundFrontendClassOutput& InDescription) override { return INodeController::GetInvalidHandle(); }
			TSharedRef<INodeController> AddOutputVertex(const FVertexName& InName, const FName InTypeName, const FText& InToolTip) override { return INodeController::GetInvalidHandle(); }
			bool RemoveOutputVertex(const FVertexName& OutputName) override { return false; }

			// This can be used to determine what kind of property editor we should use for the data type of a given input.
			// Will return Invalid if the input couldn't be found, or if the input doesn't support any kind of literals.
			ELiteralType GetPreferredLiteralTypeForInputVertex(const FVertexName& InInputName) const override { return ELiteralType::Invalid; }

			// For inputs whose preferred literal type is UObject or UObjectArray, this can be used to determine the UObject corresponding to that input's datatype.
			UClass* GetSupportedClassForInputVertex(const FVertexName& InInputName) override { return nullptr; }

			FGuid GetVertexIDForInputVertex(const FVertexName& InInputName) const { return Metasound::FrontendInvalidID; }
			FGuid GetVertexIDForOutputVertex(const FVertexName& InOutputName) const { return Metasound::FrontendInvalidID; }
			FMetasoundFrontendLiteral GetDefaultInput(const FGuid& InVertexID) const override { return FMetasoundFrontendLiteral{}; }

			// These can be used to set the default value for a given input on this graph.
			// @returns false if the input name couldn't be found, or if the literal type was incompatible with the Data Type of this input.
			bool SetDefaultInput(const FGuid& InVertexID, const FMetasoundFrontendLiteral& InLiteral) override { return false; }
			bool SetDefaultInputToDefaultLiteralOfType(const FGuid& InVertexID) override { return false; }

			const FText& GetInputDescription(const FVertexName& InName) const override { return FText::GetEmpty(); }
			const FText& GetOutputDescription(const FVertexName& InName) const override { return FText::GetEmpty(); }

			void SetInputDescription(const FVertexName& InName, const FText& InDescription) override { }
			void SetOutputDescription(const FVertexName& InName, const FText& InDescription) override { }
			void SetInputDisplayName(const FVertexName& InName, const FText& InDisplayName) override { }
			void SetOutputDisplayName(const FVertexName& InName, const FText& InDisplayName) override { }

			// This can be used to clear the current literal for a given input.
			// @returns false if the input name couldn't be found.
			bool ClearLiteralForInput(const FVertexName& InInputName, FGuid InVertexID) override { return false; }

			FNodeHandle AddNode(const FNodeRegistryKey& InNodeClass, FGuid InNodeGuid) override { return INodeController::GetInvalidHandle(); }
			FNodeHandle AddNode(const FMetasoundFrontendClassMetadata& InNodeClass, FGuid InNodeGuid) override { return INodeController::GetInvalidHandle(); }
			FNodeHandle AddDuplicateNode(const INodeController& InNode) override { return INodeController::GetInvalidHandle(); }

			// Remove the node corresponding to this node handle.
			// On success, invalidates the received node handle.
			bool RemoveNode(INodeController& InNode) override { return false; }

			// Returns the metadata for the current graph, including the name, description and author.
			const FMetasoundFrontendClassMetadata& GetGraphMetadata() const override { return Invalid::GetInvalidClassMetadata(); }

			void SetGraphMetadata(const FMetasoundFrontendClassMetadata& InMetadata) { }

			FNodeHandle CreateEmptySubgraph(const FMetasoundFrontendClassMetadata& InInfo) override { return INodeController::GetInvalidHandle(); }

			TUniquePtr<IOperator> BuildOperator(const FOperatorSettings& InSettings, const FMetasoundEnvironment& InEnvironment, TArray<IOperatorBuilder::FBuildErrorPtr>& OutBuildErrors) const override
			{
				return TUniquePtr<IOperator>(nullptr);
			}

			FDocumentHandle GetOwningDocument() override;
			FConstDocumentHandle GetOwningDocument() const override;

			void UpdateInterfaceChangeID() override { }

		protected:
			FDocumentAccess ShareAccess() override { return FDocumentAccess(); }
			FConstDocumentAccess ShareAccess() const override { return FConstDocumentAccess(); }
		};

		/** FInvalidDocumentController is a controller which is always invalid. 
		 *
		 *  All methods return defaults and return error flags or invalid values.
		 */
		class METASOUNDFRONTEND_API FInvalidDocumentController : public IDocumentController 
		{
			public:
				FInvalidDocumentController() = default;
				virtual ~FInvalidDocumentController() = default;

				bool IsValid() const override { return false; }

				const TArray<FMetasoundFrontendClass>& GetDependencies() const override { return Invalid::GetInvalidClassArray(); }
				const TArray<FMetasoundFrontendGraphClass>& GetSubgraphs() const override { return Invalid::GetInvalidGraphClassArray(); }
				const FMetasoundFrontendGraphClass& GetRootGraphClass() const override { return Invalid::GetInvalidGraphClass(); }

				FConstClassAccessPtr FindDependencyWithID(FGuid InClassID) const override { return FConstClassAccessPtr(); }
				FConstGraphClassAccessPtr FindSubgraphWithID(FGuid InClassID) const override { return FConstGraphClassAccessPtr(); }
				FConstClassAccessPtr FindClassWithID(FGuid InClassID) const override { return FConstClassAccessPtr(); }

				FConstClassAccessPtr FindClass(const FNodeRegistryKey& InKey) const override { return FConstClassAccessPtr(); }
				FConstClassAccessPtr FindOrAddClass(const FNodeRegistryKey& InKey) override { return FConstClassAccessPtr(); }
				FConstClassAccessPtr FindClass(const FMetasoundFrontendClassMetadata& InMetadata) const override{ return FConstClassAccessPtr(); }
				FConstClassAccessPtr FindOrAddClass(const FMetasoundFrontendClassMetadata& InMetadata) override{ return FConstClassAccessPtr(); }
				FGraphHandle AddDuplicateSubgraph(const IGraphController& InGraph) override { return IGraphController::GetInvalidHandle(); }

				const FMetasoundFrontendVersion& GetArchetypeVersion() const override { return FMetasoundFrontendVersion::GetInvalid(); }
				void SetArchetypeVersion(const FMetasoundFrontendVersion& InVersion) override { }

				void SetMetadata(const FMetasoundFrontendDocumentMetadata& InMetadata) override { }
				const FMetasoundFrontendDocumentMetadata& GetMetadata() const override { return Invalid::GetInvalidDocumentMetadata(); }

				const FMetasoundFrontendClass* SynchronizeDependency(const FNodeRegistryKey& InKey) override { return nullptr; }
				void SynchronizeDependencies() override { }

				TArray<FGraphHandle> GetSubgraphHandles() override { return TArray<FGraphHandle>(); }

				TArray<FConstGraphHandle> GetSubgraphHandles() const override { return TArray<FConstGraphHandle>(); }

				FGraphHandle GetSubgraphWithClassID(FGuid InClassID) { return IGraphController::GetInvalidHandle(); }

				FConstGraphHandle GetSubgraphWithClassID(FGuid InClassID) const { return IGraphController::GetInvalidHandle(); }

				TSharedRef<IGraphController> GetRootGraph() override;
				TSharedRef<const IGraphController> GetRootGraph() const override;
				bool ExportToJSONAsset(const FString& InAbsolutePath) const override { return false; }
				FString ExportToJSON() const override { return FString(); }

			protected:

				FDocumentAccess ShareAccess() override { return FDocumentAccess(); }
				FConstDocumentAccess ShareAccess() const override { return FConstDocumentAccess(); }
		};
	}
}
