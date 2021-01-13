// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MetasoundBuilderInterface.h"
#include "MetasoundFrontendBaseClasses.h"
#include "MetasoundFrontendController.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendInvalidController.h"
#include "MetasoundLog.h"

namespace Metasound
{
	namespace Frontend
	{
		/*
		using FVertexAccessPtr = TAccessPtr<FMetasoundFrontendVertex>;
		using FConstVertexAccessPtr = TAccessPtr<const FMetasoundFrontendVertex>;
		using FClassOutputAccessPtr = TAccessPtr<FMetasoundFrontendClassOutput>;
		using FConstClassOutputAccessPtr = TAccessPtr<const FMetasoundFrontendClassOutput>;
		using FClassInputAccessPtr = TAccessPtr<FMetasoundFrontendClassInput>;
		using FConstClassInputAccessPtr = TAccessPtr<const FMetasoundFrontendClassInput>;
		using FNodeAccessPtr = TAccessPtr<FMetasoundFrontendNode>;
		using FConstNodeAccessPtr = TAccessPtr<const FMetasoundFrontendNode>;
		using FGraphAccessPtr = TAccessPtr<FMetasoundFrontendGraph>;
		using FConstGraphAccessPtr = TAccessPtr<const FMetasoundFrontendGraph>;
		using FClassAccessPtr = TAccessPtr<FMetasoundFrontendClass>;
		using FConstClassAccessPtr = TAccessPtr<const FMetasoundFrontendClass>;
		using FGraphClassAccessPtr = TAccessPtr<FMetasoundFrontendGraphClass>;
		using FConstGraphClassAccessPtr = TAccessPtr<const FMetasoundFrontendGraphClass>;
		using FDocumentAccessPtr = TAccessPtr<FMetasoundFrontendDocument>;
		using FConstDocumentAccessPtr = TAccessPtr<const FMetasoundFrontendDocument>;
		*/

		/** FBaseOutputController provides common functionality for multiple derived
		 * output controllers.
		 */
		class METASOUNDFRONTEND_API FBaseOutputController: public IOutputController
		{
		public:

			struct FInitParams
			{
				int32 ID;

				FConstVertexAccessPtr NodeVertexPtr;

				/* Node handle which owns this output. */
				FNodeHandle OwningNode;
			};

			/** Construct the output controller base.  */
			FBaseOutputController(const FInitParams& InParams);

			virtual ~FBaseOutputController() = default;

			bool IsValid() const override;

			int32 GetID() const override;
			const FName& GetDataType() const override;
			const FString& GetName() const override;

			// Return info on containing node. 
			int32 GetOwningNodeID() const override;
			FNodeHandle GetOwningNode() override;
			FConstNodeHandle GetOwningNode() const override;

			// Connection logic.
			FConnectability CanConnectTo(const IInputController& InController) const override;
			bool Connect(IInputController& InController) override;
			bool ConnectWithConverterNode(IInputController& InController, const FConverterNodeInfo& InNodeClassName) override;
			bool Disconnect(IInputController& InController) override;

		protected:

			int32 ID;
			FConstVertexAccessPtr NodeVertexPtr;	
			FNodeHandle OwningNode;
		};

		/** FNodeOutputController represents a single output of a single node. */
		class METASOUNDFRONTEND_API FNodeOutputController : public FBaseOutputController
		{
		public:
			struct FInitParams
			{
				int32 ID;

				FConstVertexAccessPtr NodeVertexPtr;
			
				FConstClassOutputAccessPtr ClassOutputPtr;

				/* Node handle which owns this output. */
				FNodeHandle OwningNode;
			};

			FNodeOutputController(const FInitParams& InInitParams);

			virtual ~FNodeOutputController() = default;

			bool IsValid() const override;

			// Output metadata
			const FText& GetDisplayName() const override;
			const FText& GetTooltip() const override;

		private:

			FConstClassOutputAccessPtr ClassOutputPtr;
		};

		/** FInputNodeOutputController represents the output vertex of an input 
		 * node. 
		 *
		 * FInputNodeOutputController is largely to represent inputs coming into 
		 * graph. 
		 */
		class METASOUNDFRONTEND_API FInputNodeOutputController : public FBaseOutputController
		{
		public:
			struct FInitParams
			{
				int32 ID;

				FConstVertexAccessPtr NodeVertexPtr;
			
				FConstClassInputAccessPtr OwningGraphClassInputPtr;

				/* Node handle which owns this output. */
				FNodeHandle OwningNode;
			};

			/** Constructs the output controller. */
			FInputNodeOutputController(const FInitParams& InParams);

			virtual ~FInputNodeOutputController() = default;

			bool IsValid() const override;

			// Output metadata
			const FText& GetDisplayName() const override;
			const FText& GetTooltip() const override;

		private:

			FConstClassInputAccessPtr OwningGraphClassInputPtr;
		};

		
		/** FBaseInputController provides common functionality for multiple derived
		 * input controllers.
		 */
		class METASOUNDFRONTEND_API FBaseInputController : public IInputController 
		{
		public:

			struct FInitParams
			{
				int32 ID; 
				FConstVertexAccessPtr NodeVertexPtr;
				FGraphAccessPtr GraphPtr; 
				FNodeHandle OwningNode;
			};

			/** Construct the input controller base. */
			FBaseInputController(const FInitParams& InParams);

			virtual ~FBaseInputController() = default;

			bool IsValid() const override;

			int32 GetID() const override;
			const FName& GetDataType() const override;
			const FString& GetName() const override;

			// Owning node info
			int32 GetOwningNodeID() const override;
			FNodeHandle GetOwningNode() override;
			FConstNodeHandle GetOwningNode() const override;

			// Connection info
			bool IsConnected() const override;
			FOutputHandle GetCurrentlyConnectedOutput() override;
			FConstOutputHandle GetCurrentlyConnectedOutput() const override;

			FConnectability CanConnectTo(const IOutputController& InController) const override;
			bool Connect(IOutputController& InController) override;

			// Connection controls.
			bool ConnectWithConverterNode(IOutputController& InController, const FConverterNodeInfo& InNodeClassName) override;

			bool Disconnect(IOutputController& InController) override;
			bool Disconnect() override;

		protected:

			const FMetasoundFrontendEdge* FindEdge() const;
			FMetasoundFrontendEdge* FindEdge();

		private:

			int32 ID;
			FConstVertexAccessPtr NodeVertexPtr;
			FGraphAccessPtr GraphPtr;
			FNodeHandle OwningNode;
		};

		/** FNodeInputController represents a single input of a single node. */
		class METASOUNDFRONTEND_API FNodeInputController : public FBaseInputController 
		{
		public:

			struct FInitParams
			{
				int32 ID; 
				FConstVertexAccessPtr NodeVertexPtr;
				FConstClassInputAccessPtr OwningGraphClassInputPtr;
				FGraphAccessPtr GraphPtr; 
				FNodeHandle OwningNode;
			};

			/** Constructs the input controller.  */
			FNodeInputController(const FInitParams& InParams);

			bool IsValid() const override;

			// Input metadata
			const FText& GetDisplayName() const override;
			const FText& GetTooltip() const override;

		private:

			FConstClassInputAccessPtr OwningGraphClassInputPtr;
		};

		/** FOutputNodeInputController represents the input vertex of an output 
		 * node. 
		 *
		 * FOutputNodeInputController is largely to represent outputs exposed from
		 * a graph. 
		 */
		class METASOUNDFRONTEND_API FOutputNodeInputController : public FBaseInputController 
		{
		public:
			struct FInitParams
			{
				int32 ID; 
				FConstVertexAccessPtr NodeVertexPtr;
				FConstClassOutputAccessPtr OwningGraphClassOutputPtr;
				FGraphAccessPtr GraphPtr; 
				FNodeHandle OwningNode;
			};

			/** Constructs the input controller. */
			FOutputNodeInputController(const FInitParams& InParams);

			bool IsValid() const override;

			// Input metadata
			const FText& GetDisplayName() const override;
			const FText& GetTooltip() const override;

		private:

			FConstClassOutputAccessPtr OwningGraphClassOutputPtr;
		};

		/** FBaseNodeController provides common functionality for multiple derived
		 * node controllers.
		 */
		class METASOUNDFRONTEND_API FBaseNodeController : public INodeController
		{
		public:

			struct FInitParams
			{
				FNodeAccessPtr NodePtr;
				FConstClassAccessPtr ClassPtr;
				FGraphHandle OwningGraph;
			};

			/** Construct a base node controller. */
			FBaseNodeController(const FInitParams& InParams);

			bool IsValid() const override;

			// Owning graph info
			int32 GetOwningGraphClassID() const override;
			FGraphHandle GetOwningGraph() override;
			FConstGraphHandle GetOwningGraph() const override;

			// Info about this node.
			int32 GetID() const override;
			int32 GetClassID() const override;
			FMetasoundFrontendVersionNumber GetClassVersionNumber() const override;
			const FText& GetClassDescription() const override;

			const FString& GetNodeName() const override;
			EMetasoundFrontendClassType GetClassType() const override;

			bool CanAddInput(const FString& InVertexName) const override;
			FInputHandle AddInput(const FString& InVertexName, const FMetasoundFrontendLiteral* InDefault) override;
			bool RemoveInput(int32 InPointID) override;

			bool CanAddOutput(const FString& InVertexName) const override;
			FInputHandle AddOutput(const FString& InVertexName, const FMetasoundFrontendLiteral* InDefault) override;
			bool RemoveOutput(int32 InPointID) override;

			const FString& GetClassName() const override;

			/** Returns all node inputs. */
			TArray<FInputHandle> GetInputs() override;

			/** Returns all node inputs. */
			TArray<FConstInputHandle> GetConstInputs() const override;

			TArray<FInputHandle> GetInputsWithVertexName(const FString& InName) override;
			TArray<FConstInputHandle> GetInputsWithVertexName(const FString& InName) const override;

			/** Returns all node outputs. */
			TArray<FOutputHandle> GetOutputs() override;

			/** Returns all node outputs. */
			TArray<FConstOutputHandle> GetConstOutputs() const override;

			TArray<FOutputHandle> GetOutputsWithVertexName(const FString& InName) override;
			TArray<FConstOutputHandle> GetOutputsWithVertexName(const FString& InName) const override;

			/** Returns an input with the given id. 
			 *
			 * If the input does not exist, an invalid handle is returned. 
			 */
			FInputHandle GetInputWithID(int32 InPointID) override;

			/** Returns an input with the given name. 
			 *
			 * If the input does not exist, an invalid handle is returned. 
			 */
			FConstInputHandle GetInputWithID(int32 InPointID) const override;

			/** Returns an output with the given name. 
			 *
			 * If the output does not exist, an invalid handle is returned. 
			 */
			FOutputHandle GetOutputWithID(int32 InPointID) override;

			/** Returns an output with the given name. 
			 *
			 * If the output does not exist, an invalid handle is returned. 
			 */
			FConstOutputHandle GetOutputWithID(int32 InPointID) const override;

			FGraphHandle AsGraph() override;
			FConstGraphHandle AsGraph() const override;

		protected:

			FNodeAccessPtr NodePtr;
			FConstClassAccessPtr ClassPtr;
			FGraphHandle OwningGraph;

		private:

			struct FInputControllerParams
			{
				int32 PointID;
				FConstVertexAccessPtr NodeVertexPtr;
				FConstClassInputAccessPtr ClassInputPtr;
			};

			struct FOutputControllerParams
			{
				int32 PointID;
				FConstVertexAccessPtr NodeVertexPtr;
				FConstClassOutputAccessPtr ClassOutputPtr;
			};

			TArray<FInputControllerParams> GetInputControllerParams() const;
			TArray<FOutputControllerParams> GetOutputControllerParams() const;

			TArray<FInputControllerParams> GetInputControllerParamsWithVertexName(const FString& InName) const;
			TArray<FOutputControllerParams> GetOutputControllerParamsWithVertexName(const FString& InName) const;

			bool FindInputControllerParamsWithID(int32 InPointID, FInputControllerParams& OutParams) const;
			bool FindOutputControllerParamsWithID(int32 InPointID, FOutputControllerParams& OutParams) const;

			const FMetasoundFrontendClassInput* FindClassInputWithName(const FString& InName) const;
			const FMetasoundFrontendClassOutput* FindClassOutputWithName(const FString& InName) const;

			const FMetasoundFrontendVertex* FindNodeInputWithName(const FString& InName) const;
			const FMetasoundFrontendVertex* FindNodeOutputWithName(const FString& InName) const;



			virtual FInputHandle CreateInputController(int32 InPointID, FConstVertexAccessPtr InNodeVertexPtr, FConstClassInputAccessPtr InClassInputPtr, FNodeHandle InOwningNode) const = 0;

			virtual FOutputHandle CreateOutputController(int32 InPointID, FConstVertexAccessPtr InNodeVertexPtr, FConstClassOutputAccessPtr InClassOutputPtr, FNodeHandle InOwningNode) const = 0;
		};

		/** FNodeController represents a external or subgraph node. */
		class METASOUNDFRONTEND_API FNodeController: public FBaseNodeController
		{
			// Private token only allows members or friends to call constructor.
			enum EPrivateToken { Token };

		public:
			struct FInitParams
			{
				FNodeAccessPtr NodePtr;
				FConstClassAccessPtr ClassPtr;
				FGraphAccessPtr GraphPtr;
				FGraphHandle OwningGraph;
			};

			// Constructor takes a private token so it can only be instantiated by
			// using the static creation functions. This protects against some
			// error conditions which would result in a zombie object. The creation
			// methods can detect the error conditions and return an invalid controller
			// on error
			FNodeController(EPrivateToken InToken, const FInitParams& InParams);

			/** Create a node handle for a external or subgraph node. 
			 *
			 * @return A Node handle. On error, an invalid node handle is returned. 
			 */
			static FNodeHandle CreateNodeHandle(const FInitParams& InParams);

			/** Create a node handle for a external or subgraph node. 
			 *
			 * @return A Node handle. On error, an invalid node handle is returned. 
			 */
			static FConstNodeHandle CreateConstNodeHandle(const FInitParams& InParams);

			virtual ~FNodeController() = default;

			bool IsValid() const override;

		private:

			FInputHandle CreateInputController(int32 InPointID, FConstVertexAccessPtr InNodeVertexPtr, FConstClassInputAccessPtr InClassInputPtr, FNodeHandle InOwningNode) const override;

			FOutputHandle CreateOutputController(int32 InPointID, FConstVertexAccessPtr InNodeVertexPtr, FConstClassOutputAccessPtr InClassOutputPtr, FNodeHandle InOwningNode) const override;

			FGraphAccessPtr GraphPtr;

		};

		/** FOutputNodeController represents an output node. */
		class METASOUNDFRONTEND_API FOutputNodeController: public FBaseNodeController
		{
			// Private token only allows members or friends to call constructor.
			enum EPrivateToken { Token };

		public:
			struct FInitParams
			{
				FNodeAccessPtr NodePtr;
				FConstClassAccessPtr ClassPtr;
				FConstClassOutputAccessPtr OwningGraphClassOutputPtr; 
				FGraphAccessPtr GraphPtr;
				FGraphHandle OwningGraph;
			};

			// Constructor takes a private token so it can only be instantiated by
			// using the static creation functions. This protects against some
			// error conditions which would result in a zombie object. The creation
			// methods can detect the error conditions and return an invalid controller
			// on error
			FOutputNodeController(EPrivateToken InToken, const FInitParams& InParams);

			/** Create a node handle for an output node. 
			 *
			 * @return A Node handle. On error, an invalid node handle is returned. 
			 */
			static FNodeHandle CreateOutputNodeHandle(const FInitParams& InParams);

			/** Create a node handle for an output node. 
			 *
			 * @return A Node handle. On error, an invalid node handle is returned. 
			 */
			static FConstNodeHandle CreateConstOutputNodeHandle(const FInitParams& InParams);

			virtual ~FOutputNodeController() = default;

			bool IsValid() const override;

		private:
			FInputHandle CreateInputController(int32 InPointID, FConstVertexAccessPtr InNodeVertexPtr, FConstClassInputAccessPtr InClassInputPtr, FNodeHandle InOwningNode) const override;

			FOutputHandle CreateOutputController(int32 InPointID, FConstVertexAccessPtr InNodeVertexPtr, FConstClassOutputAccessPtr InClassOutputPtr, FNodeHandle InOwningNode) const override;

			FGraphAccessPtr GraphPtr;
			FConstClassOutputAccessPtr OwningGraphClassOutputPtr; 
		};

		/** FInputNodeController represents an input node. */
		class METASOUNDFRONTEND_API FInputNodeController: public FBaseNodeController
		{
			// Private token only allows members or friends to call constructor.
			enum EPrivateToken { Token };

		public:
			struct FInitParams
			{
				FNodeAccessPtr NodePtr;
				FConstClassAccessPtr ClassPtr;
				FConstClassInputAccessPtr OwningGraphClassInputPtr; 
				FGraphAccessPtr GraphPtr;
				FGraphHandle OwningGraph;
			};

			// Constructor takes a private token so it can only be instantiated by
			// using the static creation functions. This protects against some
			// error conditions which would result in a zombie object. The creation
			// methods can detect the error conditions and return an invalid controller
			// on error
			FInputNodeController(EPrivateToken InToken, const FInitParams& InParams);

			/** Create a node handle for an input node. 
			 *
			 * @return A Node handle. On error, an invalid node handle is returned. 
			 */
			static FNodeHandle CreateInputNodeHandle(const FInitParams& InParams);

			/** Create a node handle for an input node. 
			 *
			 * @return A Node handle. On error, an invalid node handle is returned. 
			 */
			static FConstNodeHandle CreateConstInputNodeHandle(const FInitParams& InParams);

			virtual ~FInputNodeController() = default;

			bool IsValid() const override;


		private:
			FInputHandle CreateInputController(int32 InPointID, FConstVertexAccessPtr InNodeVertexPtr, FConstClassInputAccessPtr InClassInputPtr, FNodeHandle InOwningNode) const override;

			FOutputHandle CreateOutputController(int32 InPointID, FConstVertexAccessPtr InNodeVertexPtr, FConstClassOutputAccessPtr InClassOutputPtr, FNodeHandle InOwningNode) const override;

			FConstClassInputAccessPtr OwningGraphClassInputPtr;
			FGraphAccessPtr GraphPtr;
		};

		/** FGraphController represents a Metasound graph class. */
		class METASOUNDFRONTEND_API FGraphController : public IGraphController
		{
			// Util for setting template to literal based on template argument
			template<typename ArgType>
			bool SetDefaultInputToLiteralInternal(const FString& InInputName, int32 InPointID, ArgType InValue)
			{
				if (FMetasoundFrontendClassInput* Desc = FindInputDescriptionWithName(InInputName))
				{
					auto IsLiteralWithSamePointID = [&](const FMetasoundFrontendVertexLiteral& InVertexLiteral) 
					{ 
						return InVertexLiteral.PointID == InPointID; 
					};

					FMetasoundFrontendVertexLiteral* VertexLiteral = Desc->Defaults.FindByPredicate(IsLiteralWithSamePointID);
					if (nullptr == VertexLiteral)
					{
						VertexLiteral = &Desc->Defaults.AddDefaulted_GetRef();
						VertexLiteral->PointID = InPointID;
					}

					if (ensure(FMetasoundFrontendRegistryContainer::Get()->DoesDataTypeSupportLiteralType<ArgType>(Desc->TypeName)))
					{
						VertexLiteral->Value.Set(InValue);
						return true;
					}
				}

				return false;
			}

			// Private token only allows members or friends to call constructor.
			enum EPrivateToken { Token };

		public:

			struct FInitParams
			{
				FGraphClassAccessPtr GraphClassPtr;
				FDocumentHandle OwningDocument;
			};

			// Constructor takes a private token so it can only be instantiated by
			// using the static creation functions. This protects against some
			// error conditions which would result in a zombie object. The creation
			// methods can detect the error conditions and return an invalid controller
			// on error
			FGraphController(EPrivateToken InToken, const FInitParams& InParams);

			/** Create a graph handle. 
			 *
			 * @return A graph handle. On error, an invalid handle is returned. 
			 */
			static FGraphHandle CreateGraphHandle(const FInitParams& InParams);

			/** Create a graph handle. 
			 *
			 * @return A graph handle. On error, an invalid handle is returned. 
			 */
			static FConstGraphHandle CreateConstGraphHandle(const FInitParams& InParams);

			virtual ~FGraphController() = default;

			bool IsValid() const override;

			int32 GetClassID() const override;

			int32 GetNewPointID() const override;

			TArray<FString> GetInputVertexNames() const override;
			TArray<FString> GetOutputVertexNames() const override;
			FConstClassInputAccessPtr FindClassInputWithName(const FString& InName) const override;
			FConstClassOutputAccessPtr FindClassOutputWithName(const FString& InName) const override;
			TArray<int32> GetDefaultIDsForInputVertex(const FString& InInputName) const override;
			TArray<int32> GetDefaultIDsForOutputVertex(const FString& InOutputName) const override;

			TArray<FNodeHandle> GetNodes() override;
			TArray<FConstNodeHandle> GetConstNodes() const override;

			FConstNodeHandle GetNodeWithID(int32 InNodeID) const override;
			FNodeHandle GetNodeWithID(int32 InNodeID) override;

			TArray<FNodeHandle> GetInputNodes() override;
			TArray<FConstNodeHandle> GetConstInputNodes() const override;

			TArray<FNodeHandle> GetOutputNodes() override;
			TArray<FConstNodeHandle> GetConstOutputNodes() const override;

			bool ContainsInputVertexWithName(const FString& InName) const override;
			bool ContainsOutputVertexWithName(const FString& InName) const override;

			FConstNodeHandle GetInputNodeWithName(const FString& InName) const override;
			FConstNodeHandle GetOutputNodeWithName(const FString& InName) const override;

			FNodeHandle GetInputNodeWithName(const FString& InName) override;
			FNodeHandle GetOutputNodeWithName(const FString& InName) override;

			FNodeHandle AddInputVertex(const FMetasoundFrontendClassInput& InDescription) override;
			bool RemoveInputVertex(const FString& InName) override;

			FNodeHandle AddOutputVertex(const FMetasoundFrontendClassOutput& InDescription) override;
			bool RemoveOutputVertex(const FString& InName) override;

			// This can be used to determine what kind of property editor we should use for the data type of a given input.
			// Will return Invalid if the input couldn't be found, or if the input doesn't support any kind of literals.
			ELiteralType GetPreferredLiteralTypeForInputVertex(const FString& InInputName) const override;

			// For inputs whose preferred literal type is UObject or UObjectArray, this can be used to determine the UObject corresponding to that input's datatype.
			UClass* GetSupportedClassForInputVertex(const FString& InInputName) override;

			// These can be used to set the default value for a given input on this graph.
			// @returns false if the input name couldn't be found, or if the literal type was incompatible with the Data Type of this input.
			bool SetDefaultInputToLiteral(const FString& InInputName, int32 InPointID, bool bInValue) override;
			bool SetDefaultInputToLiteral(const FString& InInputName, int32 InPointID, int32 InValue) override;
			bool SetDefaultInputToLiteral(const FString& InInputName, int32 InPointID, float InValue) override;
			bool SetDefaultInputToLiteral(const FString& InInputName, int32 InPointID, const FString& InValue) override;
			bool SetDefaultInputToLiteral(const FString& InInputName, int32 InPointID, UObject* InValue) override;
			bool SetDefaultInputToLiteral(const FString& InInputName, int32 InPointID, const TArray<UObject*>& InValue) override;

			// Set the display name for the input with the given name
			void SetInputDisplayName(FString InName, const FText& InDisplayName) override;

			// Set the display name for the output with the given name
			void SetOutputDisplayName(FString InName, const FText& InDisplayName) override;

			// This can be used to clear the current literal for a given input.
			// @returns false if the input name couldn't be found.
			bool ClearLiteralForInput(const FString& InInputName, int32 InPointID) override;

			FNodeHandle AddNode(const FNodeClassInfo& InNodeClass) override;
			FNodeHandle AddNode(const FNodeRegistryKey& InNodeClass) override;

			// Remove the node corresponding to this node handle.
			// On success, invalidates the received node handle.
			bool RemoveNode(INodeController& InNode) override;

			// Returns the metadata for the current graph, including the name, description and author.
			const FMetasoundFrontendClassMetadata& GetGraphMetadata() const override;

			bool InflateNodeDirectlyIntoGraph(const INodeController& InNode) override;

			FNodeHandle CreateEmptySubgraph(const FMetasoundFrontendClassMetadata& InInfo) override;

			TUniquePtr<IOperator> BuildOperator(const FOperatorSettings& InSettings, const FMetasoundEnvironment& InEnvironment, TArray<IOperatorBuilder::FBuildErrorPtr>& OutBuildErrors) const override;

			FDocumentHandle GetOwningDocument() override;
			FConstDocumentHandle GetOwningDocument() const override;

		private:

			// Add/remove nodes
			//FNodeHandle AddNode(const FMetasoundFrontendClass& InExistingDependency);
			FNodeHandle AddNode(FConstClassAccessPtr InExistingDependency);
			bool RemoveNode(const FMetasoundFrontendNode& InDesc);

			// Remove inputs
			bool RemoveInput(const FMetasoundFrontendNode& InNode);
			bool RemoveOutput(const FMetasoundFrontendNode& InNode);

			int32 NewNodeID() const;

			FNodeHandle GetNodeByPredicate(TFunctionRef<bool (const FMetasoundFrontendClass&, const FMetasoundFrontendNode&)> InPredicate);
			FConstNodeHandle GetNodeByPredicate(TFunctionRef<bool (const FMetasoundFrontendClass&, const FMetasoundFrontendNode&)> InPredicate) const;

			TArray<FNodeHandle> GetNodesByPredicate(TFunctionRef<bool (const FMetasoundFrontendClass&, const FMetasoundFrontendNode&)> InFilterFunc);
			TArray<FConstNodeHandle> GetNodesByPredicate(TFunctionRef<bool (const FMetasoundFrontendClass&, const FMetasoundFrontendNode&)> InFilterFunc) const;


			struct FNodeAndClass
			{
				FNodeAccessPtr Node;
				FConstClassAccessPtr Class;

				bool IsValid() const { return Node.IsValid() && Class.IsValid(); }
			};

			struct FConstNodeAndClass
			{
				FConstNodeAccessPtr Node;
				FConstClassAccessPtr Class;

				bool IsValid() const { return Node.IsValid() && Class.IsValid(); }
			};

			bool ContainsNodesAndClassesByPredicate(TFunctionRef<bool (const FMetasoundFrontendClass&, const FMetasoundFrontendNode&)> InPredicate) const;
			TArray<FNodeAndClass> GetNodesAndClasses();
			TArray<FConstNodeAndClass> GetNodesAndClasses() const;

			TArray<FNodeAndClass> GetNodesAndClassesByPredicate(TFunctionRef<bool (const FMetasoundFrontendClass&, const FMetasoundFrontendNode&)> InPredicate);
			TArray<FConstNodeAndClass> GetNodesAndClassesByPredicate(TFunctionRef<bool (const FMetasoundFrontendClass&, const FMetasoundFrontendNode&)> InPredicate) const;


			TArray<FNodeHandle> GetNodeHandles(TArrayView<const FNodeAndClass> InNodesAndClasses);
			TArray<FConstNodeHandle> GetNodeHandles(TArrayView<const FConstNodeAndClass> InNodesAndClasses) const;

			FNodeHandle GetNodeHandle(const FNodeAndClass& InNodeAndClass);
			FConstNodeHandle GetNodeHandle(const FConstNodeAndClass& InNodeAndClass) const;

			FMetasoundFrontendClassInput* FindInputDescriptionWithName(const FString& InName);
			const FMetasoundFrontendClassInput* FindInputDescriptionWithName(const FString& InName) const;

			FMetasoundFrontendClassOutput* FindOutputDescriptionWithName(const FString& InName);
			const FMetasoundFrontendClassOutput* FindOutputDescriptionWithName(const FString& InName) const;

			FClassInputAccessPtr FindInputDescriptionWithNodeID(int32 InNodeID);
			FConstClassInputAccessPtr FindInputDescriptionWithNodeID(int32 InNodeID) const;

			FClassOutputAccessPtr FindOutputDescriptionWithNodeID(int32 InNodeID);
			FConstClassOutputAccessPtr FindOutputDescriptionWithNodeID(int32 InNodeID) const;

			/*
			FMetasoundFrontendNode* FindNodeByID(int32 InNodeID);
			const FMetasoundFrontendNode* FindNodeByID(int32 InNodeID) const;
			*/

			FMetasoundFrontendClassMetadata CreateInputClassMetadata(const FMetasoundFrontendClassInput& InClassInput);
			FMetasoundFrontendClassMetadata CreateOutputClassMetadata(const FMetasoundFrontendClassOutput& InClassOutput);

			FGraphClassAccessPtr GraphClassPtr;
			FDocumentHandle OwningDocument; 
		};

		/** FDocumentController represents an entire Metasound document. */
		class METASOUNDFRONTEND_API FDocumentController : public IDocumentController 
		{
			using FRegistry = FMetasoundFrontendRegistryContainer;

		public:
			/** Construct a FDocumentController.
			 *
			 * @param InDocument - Document to be manipulated.
			 */
			FDocumentController(FDocumentAccessPtr InDocumentPtr);

			/** Create a FDocumentController.
			 *
			 * @param InDocument - Document to be manipulated.
			 *
			 * @return A document handle. 
			 */
			static FDocumentHandle CreateDocumentHandle(FDocumentAccessPtr InDocument)
			{
				return MakeShared<FDocumentController>(InDocument);
			}

			virtual ~FDocumentController() = default;

			bool IsValid() const override;

			bool IsRequiredInput(const FString& InInputName) const override;
			bool IsRequiredOutput(const FString& InOutputName) const override;

			TArray<FMetasoundFrontendClassVertex> GetRequiredInputs() const override;
			TArray<FMetasoundFrontendClassVertex> GetRequiredOutputs() const override;

			TArray<FMetasoundFrontendClass> GetDependencies() const override;
			TArray<FMetasoundFrontendGraphClass> GetSubgraphs() const override;
			TArray<FMetasoundFrontendClass> GetClasses() const override;

			FConstClassAccessPtr FindDependencyWithID(int32 InClassID) const override;
			FConstGraphClassAccessPtr FindSubgraphWithID(int32 InClassID) const override;
			FConstClassAccessPtr FindClassWithID(int32 InClassID) const override;

			FConstClassAccessPtr FindClass(const FNodeClassInfo& InNodeClass) const override;
			FConstClassAccessPtr FindClass(const FMetasoundFrontendClassMetadata& InMetadata) const override;

			FConstClassAccessPtr FindOrAddClass(const FNodeClassInfo& InNodeClass) override;
			FConstClassAccessPtr FindOrAddClass(const FMetasoundFrontendClassMetadata& InMetadata) override;

			void RemoveUnreferencedDependencies() override;

			FGraphHandle GetRootGraph() override;
			FConstGraphHandle GetRootGraph() const override;

			/** Returns an array of all subgraphs for this document. */
			TArray<FGraphHandle> GetSubgraphHandles() override;

			/** Returns an array of all subgraphs for this document. */
			TArray<FConstGraphHandle> GetSubgraphHandles() const override;

			/** Returns a graphs in the document with the given class ID.*/
			FGraphHandle GetSubgraphWithClassID(int32 InClassID) override;

			/** Returns a graphs in the document with the given class ID.*/
			FConstGraphHandle GetSubgraphWithClassID(int32 InClassID) const override;

			bool ExportToJSONAsset(const FString& InAbsolutePath) const override;
			FString ExportToJSON() const override;

			static bool IsMatchingMetasoundClass(const FMetasoundFrontendClassMetadata& InMetadataA, const FMetasoundFrontendClassMetadata& InMetadataB);
			static bool IsMatchingMetasoundClass(const FNodeClassInfo& InNodeClass, const FMetasoundFrontendClassMetadata& InMetadata);
		private:

			int32 NewClassID() const;

			FDocumentAccessPtr DocumentPtr;
		};
	}
}


