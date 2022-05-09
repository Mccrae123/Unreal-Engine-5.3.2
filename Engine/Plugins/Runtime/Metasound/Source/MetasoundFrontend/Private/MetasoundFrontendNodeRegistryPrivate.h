// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "MetasoundFrontendRegistries.h"

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "HAL/CriticalSection.h"
#include "MetasoundFrontendRegistryTransaction.h"

namespace Metasound
{
	namespace Frontend
	{
		using FNodeRegistryTransactionBuffer = TTransactionBuffer<FNodeRegistryTransaction>;
		using FNodeRegistryTransactionStream = TTransactionStream<FNodeRegistryTransaction>; 

		// Registry container private implementation.
		class FRegistryContainerImpl : public FMetasoundFrontendRegistryContainer
		{

		public:
			using FConverterNodeRegistryKey = ::Metasound::Frontend::FConverterNodeRegistryKey;
			using FConverterNodeRegistryValue = ::Metasound::Frontend::FConverterNodeRegistryValue;
			using FConverterNodeInfo = ::Metasound::Frontend::FConverterNodeInfo;

			using FNodeRegistryKey = Metasound::Frontend::FNodeRegistryKey;

			using FDataTypeRegistryInfo = Metasound::Frontend::FDataTypeRegistryInfo;
			using FNodeClassMetadata = Metasound::FNodeClassMetadata;
			using IEnumDataTypeInterface = Metasound::Frontend::IEnumDataTypeInterface;

			FRegistryContainerImpl();

			FRegistryContainerImpl(const FRegistryContainerImpl&) = delete;
			FRegistryContainerImpl& operator=(const FRegistryContainerImpl&) = delete;

			static FRegistryContainerImpl& Get();
			static void Shutdown();

			virtual ~FRegistryContainerImpl() = default;

			// Add a function to the init command array.
			virtual bool EnqueueInitCommand(TUniqueFunction<void()>&& InFunc) override;

			// This is called on module startup. This invokes any registration commands enqueued by our registration macros.
			virtual void RegisterPendingNodes() override;

			/** Register external node with the frontend.
			 *
			 * @param InCreateNode - Function for creating node from FNodeInitData.
			 * @param InCreateDescription - Function for creating a FMetasoundFrontendClass.
			 *
			 * @return True on success.
			 */
			virtual FNodeRegistryKey RegisterNode(TUniquePtr<Metasound::Frontend::INodeRegistryEntry>&&) override;
			virtual void ForEachNodeRegistryTransactionSince(Metasound::Frontend::FRegistryTransactionID InSince, Metasound::Frontend::FRegistryTransactionID* OutCurrentRegistryTransactionID, TFunctionRef<void(const Metasound::Frontend::FNodeRegistryTransaction&)> InFunc) const override;
			virtual bool UnregisterNode(const FNodeRegistryKey& InKey) override;
			virtual bool IsNodeRegistered(const FNodeRegistryKey& InKey) const override;
			virtual bool IsNodeNative(const FNodeRegistryKey& InKey) const override;

			virtual bool RegisterConversionNode(const FConverterNodeRegistryKey& InNodeKey, const FConverterNodeInfo& InNodeInfo) override;

			virtual void IterateRegistry(Metasound::FIterateMetasoundFrontendClassFunction InIterFunc, EMetasoundFrontendClassType InClassType = EMetasoundFrontendClassType::Invalid) const override;

			// Find Frontend Document data.
			virtual bool FindFrontendClassFromRegistered(const FNodeRegistryKey& InKey, FMetasoundFrontendClass& OutClass) override;
			virtual bool FindNodeClassInfoFromRegistered(const Metasound::Frontend::FNodeRegistryKey& InKey, FNodeClassInfo& OutInfo) override;
			virtual bool FindInputNodeRegistryKeyForDataType(const FName& InDataTypeName, FNodeRegistryKey& OutKey) override;
			virtual bool FindVariableNodeRegistryKeyForDataType(const FName& InDataTypeName, FNodeRegistryKey& OutKey) override;
			virtual bool FindOutputNodeRegistryKeyForDataType(const FName& InDataTypeName, FNodeRegistryKey& OutKey) override;

			// Create a new instance of a C++ implemented node from the registry.
			virtual TUniquePtr<Metasound::INode> CreateNode(const FNodeRegistryKey& InKey, const Metasound::FNodeInitData& InInitData) const override;
			virtual TUniquePtr<INode> CreateNode(const FNodeRegistryKey& InKey, FDefaultLiteralNodeConstructorParams&&) const override;
			virtual TUniquePtr<INode> CreateNode(const FNodeRegistryKey& InKey, FDefaultNamedVertexNodeConstructorParams&&) const override;
			virtual TUniquePtr<INode> CreateNode(const FNodeRegistryKey& InKey, FDefaultNamedVertexWithLiteralNodeConstructorParams&&) const override;

			// Returns a list of possible nodes to use to convert from FromDataType to ToDataType.
			// Returns an empty array if none are available.
			virtual TArray<FConverterNodeInfo> GetPossibleConverterNodes(const FName& FromDataType, const FName& ToDataType) override;

			// Create a transaction stream for any newly transactions
			TUniquePtr<FNodeRegistryTransactionStream> CreateTransactionStream();

		private:
			static FRegistryContainerImpl* LazySingleton;

			const INodeRegistryEntry* FindNodeEntry(const FNodeRegistryKey& InKey) const;


			// This buffer is used to enqueue nodes and datatypes to register when the module has been initialized,
			// in order to avoid bad behavior with ensures, logs, etc. on static initialization.
			// The bad news is that TInlineAllocator is the safest allocator to use on static init.
			// The good news is that none of these lambdas typically have captures, so this should have low memory overhead.
			static constexpr int32 MaxNumNodesAndDatatypesToInitialize = 2048;
			TArray<TUniqueFunction<void()>, TInlineAllocator<MaxNumNodesAndDatatypesToInitialize>> LazyInitCommands;
			
			FCriticalSection LazyInitCommandCritSection;

			// Registry in which we keep all information about nodes implemented in C++.
			TMap<FNodeRegistryKey, TSharedRef<INodeRegistryEntry, ESPMode::ThreadSafe>> RegisteredNodes;

			// Registry in which we keep lists of possible nodes to use to convert between two datatypes
			TMap<FConverterNodeRegistryKey, FConverterNodeRegistryValue> ConverterNodeRegistry;

			TSharedRef<FNodeRegistryTransactionBuffer> TransactionBuffer;
		};
	}
}

