// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MetasoundGraph.h"
#include "MetasoundFrontendDataLayout.h"
#include "MetasoundFrontendBaseClasses.h"
#include "MetasoundBuilderInterface.h"
#include "UObject/WeakObjectPtrTemplates.h"

// Forward Declarations
class FMetasoundAssetBase;

namespace Metasound
{	
	/**
	 * These classes are used for editing Metasounds.
	 * In general, the workflow for editing a Metasound graph will be:
	 * 1) Load or create a metasound asset.
	 * 2) Call UMetasound::GetGraphHandle() to get a handle to the graph for that Metasound asset.
	 * 
	 * All Metasound documents are saved as a FMetasoundClassDescription, which itself can own FMetasoundClassDescriptions in a tree-like hierarchy.
	 * Typically the workflow for creating a Metasound subgraph will be
	 * 1) Get a Metasound::Frontend::FGraphHandle (typically through the two steps described in the first paragraph)
	 * 2) Build a FMetasoundClassMetadata struct with whatever name/author/description you want to give the subgraph,
	 * 3) call Metasound::Frontend::FGraphHandle::CreateEmptySubgraphNode, which will return that subgraph as it's own FGraphHandle,
	 *    as well as the FNodeHandle for that subgraph in the current graph.
	 *
	 * If you want to bounce that subgraph to it's own saved Metasound Asset, call FGraphHandle::SaveToNewMetasoundAsset on that subgraph's FGraphHandle.
	 *
	 * General note- these apis are NOT thread safe. 
	 * Make sure that any FGraphHandle, FNodeHandle, FInputHandle and FOutputHandle that access similar data are called on the same thread.
	 */
	namespace Frontend
	{
		// Forward Declarations
		class FInputHandle;

		// Struct with the basics of a node class' information,
		// used to look up that node from our node browser functions,
		// and also used in FGraphHandle::AddNewNode.
		struct METASOUNDFRONTEND_API FNodeClassInfo
		{
			// The descriptive name of this node class.
			FString NodeName;

			// The type for this node.
			EMetasoundClassType NodeType;

			// The lookup key used for the internal node registry.
			FNodeRegistryKey LookupKey;
		};

		// Get all available nodes of any type.
		METASOUNDFRONTEND_API TArray<FNodeClassInfo> GetAllAvailableNodeClasses();

		// Get all possible nodes whose name begins with a specific namespace.
		METASOUNDFRONTEND_API TArray<FNodeClassInfo> GetAllNodeClassesInNamespace(const FString& InNamespace);

		// Similar to GetAllNodeClassesInNamespace, except searches all nodes for a match with a substring.
		METASOUNDFRONTEND_API TArray<FNodeClassInfo> GetAllNodesWhoseNameContains(const FString& InSubstring);

		// Searches for any node types that can output a specific type.
		METASOUNDFRONTEND_API TArray<FNodeClassInfo> GetAllNodesWithAnOutputOfType(const FName& InType);

		METASOUNDFRONTEND_API TArray<FNodeClassInfo> GetAllNodesWithAnInputOfType(const FName& InType);

		// gets all metadata (name, description, author, what to say if it's missing) for a given node.
		METASOUNDFRONTEND_API FMetasoundClassMetadata GenerateMetadataForNode(const FNodeClassInfo& InInfo);

		// Generates a new FMetasoundClassDescription for a given node class. Only used by classes that manipulate Metasound Description data directly.
		METASOUNDFRONTEND_API FMetasoundClassDescription GenerateClassDescriptionForNode(const FNodeClassInfo& InInfo);

		template<typename DataType>
		FName GetDataTypeName()
		{
			static FName DataTypeName = FName(TDataReferenceTypeInfo<DataType>::TypeName);
			return DataTypeName;
		}

		// Returns a list of all available data types.
		METASOUNDFRONTEND_API TArray<FName> GetAllAvailableDataTypes();

		// outputs the traits for a given data type.
		// returns false if InDataType couldn't be found.
		METASOUNDFRONTEND_API bool GetTraitsForDataType(FName InDataType, FDataTypeRegistryInfo& OutInfo);

		// This will open a json document at the given absolute path and convert it into a 
		// metasound document struct. 
		// @returns false if the file couldn't be found or parsed into a document.
		METASOUNDFRONTEND_API bool ImportJSONToMetasound(const FString& InPath, FMetasoundDocument& OutMetasoundDocument);

		struct METASOUNDFRONTEND_API FMetasoundArchetypeRegistryParams_Internal
		{
			FMetasoundArchetype ArchetypeDescription;

			// The UClass associated with this specific archetype.
			UClass* ArchetypeUClass;

			// template-generated lambdas used to safely sidecast to FMetasoundBase*.
			TUniqueFunction<FMetasoundAssetBase* (UObject*)> SafeCast;
			TUniqueFunction<const FMetasoundAssetBase* (const UObject*)> SafeConstCast;

			// This function should construct a new UObject of the given archetype's type
			// given a metasound document with a matching archetype.
			// The first argument is the document to use.
			// The second argument is the path relative to the content directory to save the soundwave to.
			TUniqueFunction<UObject* (const FMetasoundDocument&, const FString&)> ObjectGetter;
		};

		// ಠ╭╮ಠ         ಠ╭╮ಠ           ಠ╭╮ಠ
		// Please don't use this function.
		// See RegisterArchetype<Class> instead, in MetasoundArchetypeRegistration.h
		METASOUNDFRONTEND_API bool RegisterArchetype_Internal(FMetasoundArchetypeRegistryParams_Internal&& InParams);

		METASOUNDFRONTEND_API TArray<FName> GetAllRegisteredArchetypes();

		// Returns a new UObject, whose class corresponds to the archetype in the document.
		// @param InDocument a fully generated metasound document, typically retrieved from ImportJSONToMetasound().
		// @param InPath, path in content directory to save the generated UAsset to (ex. "/game/MyDir/MyMetasoundAsset".
		//                if InPath is invalid, we won't save to an asset.
		// @returns nullptr if we couldn't find the archetype.
		METASOUNDFRONTEND_API UObject* GetObjectForDocument(const FMetasoundDocument& InDocument, const FString& InPath);

		// This returns true if the object is listed as one of our metasound archetypes.
		// If it is, the object can be safely static cast to FMetasoundAssetBase*.
		METASOUNDFRONTEND_API bool IsObjectAMetasoundArchetype(const UObject* InObject);

		// These functions are used to safely sidecast between a UObject of a given metasound archetype
		// and FMetasoundAssetBase*.
		// @returns nullptr if the object wasn't a registered archetype.
		METASOUNDFRONTEND_API FMetasoundAssetBase* GetObjectAsAssetBase(UObject* InObject);
		METASOUNDFRONTEND_API const FMetasoundAssetBase* GetObjectAsAssetBase(const UObject* InObject);

		// Struct that indicates whether an input and an output can be connected,
		// and whether an intermediate node is necessary to connect the two.
		struct METASOUNDFRONTEND_API FConnectability
		{
			enum class EConnectable
			{
				Yes,
				No,
				YesWithConverterNode
			};

			EConnectable Connectable;

			// If Connectable is EConnectable::YesWithConverterNode,
			// this will be a populated list of nodes we can use 
			// to convert between the input and output.
			TArray<FNodeClassInfo> PossibleConverterNodeClasses;
		};

		struct METASOUNDFRONTEND_API FHandleInitParams
		{
			TWeakPtr<FDescriptionAccessPoint> InAccessPoint;

			// Path in the document to the element we're getting a handle to.
			// for the FGraphHandle, this is a path to the graph within the graph's Class description.
			// for the FNodeHandle, FInputHandle and FOutputHandle, this is a path to the individual node within a Graph description.
			const FDescPath& InPath;

			// Class name for the graph or node to get a handle for.
			const FString& InClassName;

			// The asset that owns the FMetasoundDocument this handle is associated with.
			TWeakObjectPtr<UObject> InOwningAsset;

		private:
			// This is used to grant specific classes the ability to create handles.
			enum EPrivateToken { Token };
			static const EPrivateToken PrivateToken;

			friend class FOutputHandle;
			friend class FInputHandle;
			friend class FNodeHandle;
			friend class FGraphHandle;
			friend class ::FMetasoundAssetBase;
		};

		class METASOUNDFRONTEND_API FOutputHandle : protected ITransactable
		{
		public:
			FOutputHandle() = delete;

			explicit FOutputHandle(FHandleInitParams::EPrivateToken InToken, const FHandleInitParams& InParams, const FString& InOutputName);

			// Constructor used for the outgoing connection from an input node.
			explicit FOutputHandle(FHandleInitParams::EPrivateToken InToken, const FHandleInitParams& InParams);

			~FOutputHandle() = default;

			static FOutputHandle InvalidHandle();

			bool IsValid() const;

			// @todo: with the current spec, we'd have to scan all nodes in the graph for this output to solve this.
			// Is this worth it, or should we just require folks to use FInputHandle::IsConnected()?
			//bool IsConnected() const;

			FName GetOutputType() const;
			FString GetOutputName() const;
			FText GetOutputTooltip() const;
			uint32 GetOwningNodeID() const;

			// @todo: with the current spec, we'd have to scan all nodes in the graph for this output to solve this.
			// Is this worth it, or should we just require folks to use FInputHandle::GetCurrentlyConnectedOutput()?
			//FInputHandle GetCurrentlyConnectedInput();

			FConnectability CanConnectTo(const FInputHandle& InHandle) const;
			bool Connect(FInputHandle& InHandle);
			bool ConnectWithConverterNode(FInputHandle& InHandle, FString& InNodeClassName);
			bool Disconnect(FInputHandle& InHandle);

		private:
			TDescriptionPtr<FMetasoundNodeDescription> NodePtr;
			TDescriptionPtr<FMetasoundClassDescription> NodeClass;

			// owning output description ptr for the node's class.
			// This can be none if the node that owns this output is an input node itself.
			TDescriptionPtr<FMetasoundOutputDescription> OutputPtr;

			// Optional description pointer used when this output connection is owned by an input node.
			TDescriptionPtr<FMetasoundInputDescription> InputNodePtr;
		};

		class METASOUNDFRONTEND_API FInputHandle : protected ITransactable
		{
		public:
			FInputHandle() = delete;
			~FInputHandle() = default;

			// Sole constructor for FInputHandle. Can only be used by friend classes for FHandleInitParams.
			explicit FInputHandle(FHandleInitParams::EPrivateToken PrivateToken, const FHandleInitParams& InParams, const FString& InputName);

			// constructor used exclusively by output nodes.
			explicit FInputHandle(FHandleInitParams::EPrivateToken PrivateToken, const FHandleInitParams& InParams);

			static FInputHandle InvalidHandle();

			bool IsValid() const;
			bool IsConnected() const;

			FName GetInputType() const;
			FString GetInputName() const;
			FText GetInputTooltip() const;

			FOutputHandle GetCurrentlyConnectedOutput() const;

			FConnectability CanConnectTo(const FOutputHandle& InHandle) const;
			bool Connect(FOutputHandle& InHandle);
			bool ConnectWithConverterNode(FOutputHandle& InHandle, FString& InNodeClassName);
			bool Disconnect(FOutputHandle& InHandle);
			bool Disconnect();

		private:
			TDescriptionPtr<FMetasoundNodeDescription> NodePtr;

			// owning node class for the node that owns this input connection.
			// This can be none if the node that owns this output is an output node itself.
			TDescriptionPtr<FMetasoundClassDescription> NodeClass;

			// owning input description ptr for the node's class.
			// This can be none if the node that owns this output is an output node itself.
			TDescriptionPtr<FMetasoundInputDescription> InputPtr;

			// Optional description pointer used when this input connection is owned by an output node.
			TDescriptionPtr<FMetasoundOutputDescription> OutputNodePtr;

			FString InputName;

			const FMetasoundNodeConnectionDescription* GetConnectionDescription() const;
			FMetasoundNodeConnectionDescription* GetConnectionDescription();
		};

		class FGraphHandle;

		// Opaque handle to a single node on a graph.
		// Can retrieve metadata about that node,
		// get FNodeHandles for other nodes it is connected to, and validate and modify connections.
		class METASOUNDFRONTEND_API FNodeHandle : protected ITransactable
		{
		public:
			FNodeHandle() = delete;
			~FNodeHandle() = default;

			// Sole constructor for FNodeHandle. Can only be used by friends of FHandleInitParams.
			explicit FNodeHandle(FHandleInitParams::EPrivateToken PrivateToken, const FHandleInitParams& InParams, EMetasoundClassType InNodeClassType);

			static FNodeHandle InvalidHandle();

			bool IsValid() const;

			TArray<FInputHandle> GetAllInputs();
			TArray<FOutputHandle> GetAllOutputs();

			FInputHandle GetInputWithName(const FString& InName);
			FOutputHandle GetOutputWithName(const FString& InName);

			FNodeClassInfo GetClassInfo() const;

			EMetasoundClassType GetNodeType() const;
			const FString& GetNodeClassName() const;

			// If this node is itself a Metasound,
			// use this to return the contained graph for the metasound.
			// Otherwise it will return an invalid FGraphHandle.
			void GetContainedGraph(FGraphHandle& OutGraph);

			uint32 GetNodeID() const;
			static uint32 GetNodeID(const FDescPath& InNodePath);

			const FString& GetNodeName() const;

		private:

			static TDescriptionPtr<FMetasoundClassDescription> GetNodeClassDescriptionForNodeHandle(const FHandleInitParams& InitParams, EMetasoundClassType InNodeClassType);

			TDescriptionPtr<FMetasoundNodeDescription> NodePtr;
			TDescriptionPtr<FMetasoundClassDescription> NodeClass;

			// whether this node is an input or output to it's owning graph,
			// an externally implemented node, or a metasound graph itself.
			EMetasoundClassType NodeClassType;

			uint32 NodeID;
		};

		class METASOUNDFRONTEND_API FGraphHandle : protected ITransactable
		{
		public:
			FGraphHandle() = delete;
			~FGraphHandle() = default;

			// Sole constructor for FGraphHandle. Can only be used by friends of FHandleInitParams.
			FGraphHandle(FHandleInitParams::EPrivateToken PrivateToken, const FHandleInitParams& InParams);

			static FGraphHandle GetHandle(UObject* InOwner, FMetasoundDocument& InRootMetasoundDocument, TSharedPtr<Metasound::Frontend::FDescriptionAccessPoint>& InAccessPoint)
			{
				using namespace Path;

				FDescPath PathToGraph = FDescPath()[EFromDocument::ToRootClass][EFromClass::ToGraph];
				FHandleInitParams InitParams = { InAccessPoint, PathToGraph, InRootMetasoundDocument.RootClass.Metadata.NodeName, MakeWeakObjectPtr(InOwner) };
				return FGraphHandle(FHandleInitParams::PrivateToken, InitParams);
			}

			static FGraphHandle InvalidHandle();

			bool IsValid() const;

			TArray<FNodeHandle> GetAllNodes();
			FNodeHandle GetNodeWithId(uint32 InNodeId) const;
			TArray<FNodeHandle> GetOutputNodes();
			TArray<FNodeHandle> GetInputNodes();

			bool ContainsOutputNodeWithName(const FString& InName) const;
			bool ContainsInputNodeWithName(const FString& InName) const;

			FNodeHandle GetOutputNodeWithName(const FString& InName);
			FNodeHandle GetInputNodeWithName(const FString& InName);

			FNodeHandle AddNewInput(const FMetasoundInputDescription& InDescription);
			bool RemoveInput(const FString& InputName);

			FNodeHandle AddNewOutput(const FMetasoundOutputDescription& InDescription);
			bool RemoveOutput(const FString& OutputName);

			// This can be used to determine what kind of property editor we should use for the data type of a given input.
			// Will return Invalid if the input couldn't be found, or if the input doesn't support any kind of literals.
			ELiteralArgType GetPreferredLiteralTypeForInput(const FString& InInputName);

			// These can be used to set the default value for a given input on this graph.
			// @returns false if the input name couldn't be found, or if the literal type was incompatible with the Data Type of this input.
			bool SetInputToLiteral(const FString& InInputName, bool bInValue);
			bool SetInputToLiteral(const FString& InInputName, int32 InValue);
			bool SetInputToLiteral(const FString& InInputName, float InValue);
			bool SetInputToLiteral(const FString& InInputName, const FString& InValue);
			bool SetInputToLiteral(const FString& InInputName, UObject* InValue);
			bool SetInputToLiteral(const FString& InInputName, TArray<UObject*> InValue);

			// This can be used to clear the current literal for a given input.
			// @returns false if the input name couldn't be found.
			bool ClearLiteralForInput(const FString& InInputName);

			FNodeHandle AddNewNode(const FNodeClassInfo& InNodeClass);

			// Remove the node corresponding to this node handle.
			// On success, invalidates the received node handle.
			bool RemoveNode(const FNodeHandle& InNode);

			// Returns the metadata for the current graph, including the name, description and author.
			FMetasoundClassMetadata GetGraphMetadata();

			// Exports this graph to a JSON at the given path.
			// @returns true on success.
			bool ExportToJSONAsset(const FString& InAbsolutePath) const;

			// If the FNodeHandle given is itself a Metasound graph,
			// and the FNodeHandle is a direct member of this FGraphHandle,
			// If successful, this will invalidate the FNodeHandle and paste the graph for this node directly
			// into the graph.
			// If not successful, InNode will not be affected.
			// @returns true on success, false on failure.
			bool InflateNodeDirectlyIntoGraph(FNodeHandle& InNode);

			// When called, creates a new node in this graph that can be edited directly with the FGraphHandle
			// that gets returned.
			// @param InInfo the name, author, etc that should be used for this graph.
			// @returns a tuple of the FGraphHandle for this subgraph (which can be independently manipulated),
			//          as well as the FNodeHandle for the subgraph's node in this graph.
			//          Both of these will be invalid on failure.
			TTuple<FGraphHandle, FNodeHandle> CreateEmptySubgraphNode(const FMetasoundClassMetadata& InInfo);

			// This invokes the Metasound Builder to synchronously compile a 
			// metasound operator, which can then be used for playback.
			// @returns nullptr on failure.
			// @todo: make an API for doing this async.
			TUniquePtr<IOperator> BuildOperator(const FOperatorSettings& InSettings, TArray<IOperatorBuilder::FBuildErrorPtr>& OutBuildErrors) const;

			// This function will ensure that the document's root class has the inputs and outputs required by the archetype.
			void FixDocumentToMatchArchetype();

		private:

			// The graph struct itself.
			TDescriptionPtr<FMetasoundGraphDescription> GraphPtr;

			// The class description for the graph corresponding to this.
			TDescriptionPtr<FMetasoundClassDescription> GraphsClassDeclaration;

			// the outermost document for all dependencies for the document this graph lives in.
			TDescriptionPtr<FMetasoundDocument> OwningDocument;

			// Remove the node corresponding to this node handle.
			// On success, invalidates the received node handle.
			// Can remove inputs and outputs, but does not remove
			// from the input/output arrays.
			bool RemoveNodeInternal(const FNodeHandle& InNode);

			// Scans all existing node ids to guarantee a new unique ID.
			uint32 FindNewUniqueNodeId();

			// Scans all existing dependency IDs in the root dependency list to guarantee a new unique ID when adding a dependency.
			uint32 FindNewUniqueDependencyId();

			FMetasoundLiteralDescription* GetLiteralDescriptionForInput(const FString& InInputName, FName& OutDataType) const;

			bool GetDataTypeForInput(const FString& InInputName, FName& OutDataType);
		};

		// If there's a Metasound node referenced by a class in it's dependencies list,
		// but the node' graph isn't implemented in that dependency list,
		// this can be used to attempt to find which asset that node is implemented in,
		// and return a graph handle for that node.
		FGraphHandle GetGraphHandleForClass(const FMetasoundClassDescription& InClass);
	} // namespace Frontend
} // namespace Metasound
