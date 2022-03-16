// Copyright Epic Games, Inc. All Rights Reserved.

using EpicGames.Core;
using EpicGames.Horde.Bundles;
using EpicGames.Horde.Bundles.Nodes;
using EpicGames.Horde.Storage;
using EpicGames.Horde.Storage.Impl;
using EpicGames.Serialization;
using Microsoft.Extensions.Caching.Memory;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;

namespace HordeServerTests
{
	[TestClass]
	public class BundleTests : IDisposable
	{
		MemoryCache Cache;
		MemoryStorageClient StorageClient = new MemoryStorageClient();
		NamespaceId NamespaceId = new NamespaceId("namespace");
		BucketId BucketId = new BucketId("bucket");

		public BundleTests()
		{
			Cache = new MemoryCache(new MemoryCacheOptions { SizeLimit = 50 * 1024 * 1024 });
		}

		public void Dispose()
		{
			Cache.Dispose();
		}

		[TestMethod]
		public void BuzHashTests()
		{
			byte[] Data = new byte[4096];
			new Random(0).NextBytes(Data);

			const int WindowSize = 128;

			uint RollingHash = 0;
			for (int MaxIdx = 0; MaxIdx < Data.Length + WindowSize; MaxIdx++)
			{
				int MinIdx = MaxIdx - WindowSize;

				if (MaxIdx < Data.Length)
				{
					RollingHash = BuzHash.Add(RollingHash, Data[MaxIdx]);
				}

				int Length = Math.Min(MaxIdx + 1, Data.Length) - Math.Max(MinIdx, 0);
				uint CleanHash = BuzHash.Add(0, Data.AsSpan(Math.Max(MinIdx, 0), Length));
				Assert.AreEqual(RollingHash, CleanHash);

				if (MinIdx >= 0)
				{
					RollingHash = BuzHash.Sub(RollingHash, Data[MinIdx], Length);
				}
			}
		}

		[TestMethod]
		public void BasicChunkingTests()
		{
			ChunkingOptions Options = new ChunkingOptions();
			Options.LeafOptions = new ChunkingOptionsForNodeType(8, 8, 8);

			FileNode Node = new FileNode();
			Node.Append(new byte[7], Options);
			Assert.AreEqual(0, Node.Depth);
			Assert.AreEqual(7, Node.Payload.Length);

			Node = new FileNode();
			Node.Append(new byte[8], Options);
			Assert.AreEqual(0, Node.Depth);
			Assert.AreEqual(8, Node.Payload.Length);

			Node = new FileNode();
			Node.Append(new byte[9], Options);
			Assert.AreEqual(1, Node.Depth);
			Assert.AreEqual(2, Node.Children.Count);

			FileNode? ChildNode1 = Node.Children[0].Node;
			Assert.IsNotNull(ChildNode1);
			Assert.AreEqual(0, ChildNode1!.Depth);
			Assert.AreEqual(8, ChildNode1!.Payload.Length);

			FileNode? ChildNode2 = Node.Children[1].Node;
			Assert.IsNotNull(ChildNode2);
			Assert.AreEqual(0, ChildNode2!.Depth);
			Assert.AreEqual(1, ChildNode2!.Payload.Length);
		}

		[TestMethod]
		public void SerializationTests()
		{
			List<BundleImport> Imports = new List<BundleImport>();
			Imports.Add(new BundleImport(IoHash.Compute(Encoding.UTF8.GetBytes("import1")), 0, 123));
			Imports.Add(new BundleImport(IoHash.Compute(Encoding.UTF8.GetBytes("import2")), 1, 456));

			BundleObject OldObject = new BundleObject();
			OldObject.ImportObjects.Add(new BundleImportObject(new CbObjectAttachment(IoHash.Compute(new byte[] { 1, 2, 3 })), 12345, Imports));
			OldObject.Exports.Add(new BundleExport(IoHash.Compute(Encoding.UTF8.GetBytes("export1")), 2, new BundleCompressionPacket(0) { EncodedLength = 20, DecodedLength = 40 }, 0, 40, new int[] { 1, 2 }));
			OldObject.Exports.Add(new BundleExport(IoHash.Compute(Encoding.UTF8.GetBytes("export2")), 3, new BundleCompressionPacket(20) { EncodedLength = 10, DecodedLength = 20 }, 0, 20, new int[] { -1 }));

			CbObject SerializedObject = CbSerializer.Serialize(OldObject);
			ReadOnlyMemory<byte> SerializedData = SerializedObject.GetView();

			BundleObject NewObject = CbSerializer.Deserialize<BundleObject>(SerializedData);

			Assert.AreEqual(OldObject.ImportObjects.Count, NewObject.ImportObjects.Count);
			for (int Idx = 0; Idx < OldObject.ImportObjects.Count; Idx++)
			{
				BundleImportObject OldObjectImport = OldObject.ImportObjects[Idx];
				BundleImportObject NewObjectImport = NewObject.ImportObjects[Idx];

				Assert.AreEqual(OldObjectImport.Object, NewObjectImport.Object);
				Assert.AreEqual(OldObjectImport.TotalCost, NewObjectImport.TotalCost);
				Assert.AreEqual(OldObjectImport.Imports.Count, NewObjectImport.Imports.Count);

				for (int ImportIdx = 0; ImportIdx < OldObjectImport.Imports.Count; ImportIdx++)
				{
					BundleImport OldImport = OldObjectImport.Imports[ImportIdx];
					BundleImport NewImport = NewObjectImport.Imports[ImportIdx];

					Assert.AreEqual(OldImport.Hash, NewImport.Hash);
					Assert.AreEqual(OldImport.Rank, NewImport.Rank);
					Assert.AreEqual(OldImport.Length, NewImport.Length);
				}
			}

			Assert.AreEqual(OldObject.Exports.Count, NewObject.Exports.Count);
			for (int Idx = 0; Idx < OldObject.Exports.Count; Idx++)
			{
				BundleExport OldExport = OldObject.Exports[Idx];
				BundleExport NewExport = NewObject.Exports[Idx];

				Assert.AreEqual(OldExport.Hash, NewExport.Hash);
				Assert.AreEqual(OldExport.Rank, NewExport.Rank);
				Assert.AreEqual(OldExport.Length, NewExport.Length);
				Assert.AreEqual(OldExport.Packet.Offset, NewExport.Packet.Offset);
				Assert.AreEqual(OldExport.Packet.EncodedLength, NewExport.Packet.EncodedLength);
				Assert.AreEqual(OldExport.Packet.DecodedLength, NewExport.Packet.DecodedLength);
				Assert.AreEqual(OldExport.Offset, NewExport.Offset);
				Assert.AreEqual(OldExport.Length, NewExport.Length);
				Assert.IsTrue(OldExport.References.AsSpan().SequenceEqual(NewExport.References.AsSpan()));
			}
		}

		[TestMethod]
		public async Task BasicTestDirectory()
		{
			using Bundle<DirectoryNode> NewBundle = Bundle.Create<DirectoryNode>(StorageClient, NamespaceId, new DirectoryNode(), new BundleOptions(), Cache);
			DirectoryNode Node = NewBundle.Root.AddDirectory("hello");
			DirectoryNode Node2 = Node.AddDirectory("world");

			RefId RefId = new RefId("testref");
			await NewBundle.WriteAsync(BucketId, RefId, CbObject.Empty, false);

			// Should be stored inline
			Assert.AreEqual(1, StorageClient.Refs.Count);
			Assert.AreEqual(0, StorageClient.Blobs.Count);

			// Check the ref
			BundleRoot Root = await StorageClient.GetRefAsync<BundleRoot>(NamespaceId, BucketId, RefId);
			BundleObject RootObject = Root.Object;
			Assert.AreEqual(3, RootObject.Exports.Count);
			Assert.AreEqual(0, RootObject.Exports[0].Rank);
			Assert.AreEqual(1, RootObject.Exports[1].Rank);
			Assert.AreEqual(2, RootObject.Exports[2].Rank);

			// Create a new bundle and read it back in again
			using Bundle<DirectoryNode> NewBundle2 = await Bundle.ReadAsync<DirectoryNode>(StorageClient, NamespaceId, BucketId, RefId, new BundleOptions(), Cache);

			Assert.AreEqual(0, NewBundle2.Root.Files.Count);
			Assert.AreEqual(1, NewBundle2.Root.Directories.Count);
			DirectoryNode? OutputNode = await NewBundle.FindDirectoryAsync(NewBundle2.Root, "hello");
			Assert.IsNotNull(OutputNode);

			Assert.AreEqual(0, OutputNode!.Files.Count);
			Assert.AreEqual(1, OutputNode!.Directories.Count);
			DirectoryNode? OutputNode2 = await NewBundle.FindDirectoryAsync(OutputNode, "world");
			Assert.IsNotNull(OutputNode2);

			Assert.AreEqual(0, OutputNode2!.Files.Count);
			Assert.AreEqual(0, OutputNode2!.Directories.Count);
		}

		[TestMethod]
		public async Task DedupTests()
		{
			BundleOptions Options = new BundleOptions();
			Options.MaxBlobSize = 1;
			Options.MaxInlineBlobSize = 1;

			using Bundle<DirectoryNode> NewBundle = Bundle.Create<DirectoryNode>(StorageClient, NamespaceId, new DirectoryNode(), Options, Cache);

			NewBundle.Root.AddDirectory("node1");
			NewBundle.Root.AddDirectory("node2");
			NewBundle.Root.AddDirectory("node3");

			Assert.AreEqual(0, StorageClient.Refs.Count);
			Assert.AreEqual(0, StorageClient.Blobs.Count);

			RefId RefId = new RefId("ref");
			await NewBundle.WriteAsync(BucketId, RefId, CbObject.Empty, false);

			Assert.AreEqual(1, StorageClient.Refs.Count);
			Assert.AreEqual(1, StorageClient.Blobs.Count);
		}

		[TestMethod]
		public async Task ReloadTests()
		{
			BundleOptions Options = new BundleOptions();
			Options.MaxBlobSize = 1;
			Options.MaxInlineBlobSize = 1;

			using Bundle<DirectoryNode> InitialBundle = Bundle.Create<DirectoryNode>(StorageClient, NamespaceId, new DirectoryNode(), Options, Cache);

			DirectoryNode Node1 = InitialBundle.Root.AddDirectory("node1");
			DirectoryNode Node2 = Node1.AddDirectory("node2");
			DirectoryNode Node3 = Node2.AddDirectory("node3");
			DirectoryNode Node4 = Node3.AddDirectory("node4");

			RefId RefId = new RefId("ref");
			await InitialBundle.WriteAsync(BucketId, RefId, CbObject.Empty, false);

			Assert.AreEqual(1, StorageClient.Refs.Count);
			Assert.AreEqual(4, StorageClient.Blobs.Count);

			using Bundle<DirectoryNode> NewBundle = await Bundle.ReadAsync<DirectoryNode>(StorageClient, NamespaceId, BucketId, RefId, Options, Cache);

			DirectoryNode? NewNode1 = await NewBundle.FindDirectoryAsync(NewBundle.Root, "node1");
			Assert.IsNotNull(NewNode1);

			DirectoryNode? NewNode2 = await NewBundle.FindDirectoryAsync(NewNode1!, "node2");
			Assert.IsNotNull(NewNode2);

			DirectoryNode? NewNode3 = await NewBundle.FindDirectoryAsync(NewNode2!, "node3");
			Assert.IsNotNull(NewNode3);

			DirectoryNode? NewNode4 = await NewBundle.FindDirectoryAsync(NewNode3!, "node4");
			Assert.IsNotNull(NewNode4);
		}

		[TestMethod]
		public async Task CompactTest()
		{
			BundleOptions Options = new BundleOptions();
			Options.MaxBlobSize = 1024 * 1024;
			Options.MaxInlineBlobSize = 1;

			using Bundle<DirectoryNode> NewBundle = Bundle.Create<DirectoryNode>(StorageClient, NamespaceId, new DirectoryNode(), Options, Cache);

			DirectoryNode Node1 = NewBundle.Root.AddDirectory("node1");
			DirectoryNode Node2 = Node1.AddDirectory("node2");
			DirectoryNode Node3 = Node2.AddDirectory("node3");
			DirectoryNode Node4 = NewBundle.Root.AddDirectory("node4"); // same contents as node 3

			RefId RefId1 = new RefId("ref1");
			await NewBundle.WriteAsync(BucketId, RefId1, CbObject.Empty, false);

			Assert.AreEqual(1, StorageClient.Refs.Count);
			Assert.AreEqual(1, StorageClient.Blobs.Count);

			IRef Ref1 = StorageClient.Refs[(NamespaceId, BucketId, RefId1)];

			BundleRoot Root1 = CbSerializer.Deserialize<BundleRoot>(Ref1.Value);
			BundleObject RootObject1 = Root1.Object;
			Assert.AreEqual(1, RootObject1.Exports.Count);
			Assert.AreEqual(1, RootObject1.ImportObjects.Count);

			IoHash LeafHash1 = RootObject1.ImportObjects[0].Object.Hash;
			BundleObject LeafObject1 = CbSerializer.Deserialize<BundleObject>(StorageClient.Blobs[(NamespaceId, LeafHash1)]);
			Assert.AreEqual(3, LeafObject1.Exports.Count); // node1 + node2 + node3 (== node4)
			Assert.AreEqual(0, LeafObject1.ImportObjects.Count);

			// Remove one of the nodes from the root without compacting. the existing blob should be reused.
			NewBundle.Root.DeleteDirectory("node1");

			RefId RefId2 = new RefId("ref2");
			await NewBundle.WriteAsync(BucketId, RefId2, CbObject.Empty, false);

			IRef Ref2 = StorageClient.Refs[(NamespaceId, BucketId, RefId2)];

			BundleRoot Root2 = CbSerializer.Deserialize<BundleRoot>(Ref2.Value);
			BundleObject RootObject2 = Root2.Object;
			Assert.AreEqual(1, RootObject2.Exports.Count);
			Assert.AreEqual(1, RootObject2.ImportObjects.Count);

			IoHash LeafHash2 = RootObject2.ImportObjects[0].Object.Hash;
			Assert.AreEqual(LeafHash1, LeafHash2);
			Assert.AreEqual(3, LeafObject1.Exports.Count); // unused: node1 + node2 + node3, used: node4 (== node3)
			Assert.AreEqual(0, LeafObject1.ImportObjects.Count);

			// Repack it and check that we make a new object
			RefId RefId3 = new RefId("ref3");
			await NewBundle.WriteAsync(BucketId, RefId3, CbObject.Empty, true);

			IRef Ref3 = StorageClient.Refs[(NamespaceId, BucketId, RefId3)];

			BundleRoot Root3 = CbSerializer.Deserialize<BundleRoot>(Ref3.Value);
			BundleObject RootObject3 = Root3.Object;
			Assert.AreEqual(1, RootObject3.Exports.Count);
			Assert.AreEqual(1, RootObject3.ImportObjects.Count);

			IoHash LeafHash3 = RootObject3.ImportObjects[0].Object.Hash;
			Assert.AreNotEqual(LeafHash1, LeafHash3);

			BundleObject LeafObject3 = CbSerializer.Deserialize<BundleObject>(StorageClient.Blobs[(NamespaceId, LeafHash3)]);
			Assert.AreEqual(1, LeafObject3.Exports.Count);
			Assert.AreEqual(0, LeafObject3.ImportObjects.Count);
		}

		[TestMethod]
		public async Task CoreAppendTest()
		{
			byte[] Data = new byte[4096];
			new Random(0).NextBytes(Data);

			ChunkingOptions Options = new ChunkingOptions();
			Options.LeafOptions = new ChunkingOptionsForNodeType(16, 64, 256);

			FileNode Node = new FileNode();
			for (int Idx = 0; Idx < Data.Length; Idx++)
			{
				Node.Append(Data.AsMemory(Idx, 1), Options);

				byte[] OutputData = await Node.ToByteArrayAsync(null!);
				Assert.IsTrue(Data.AsMemory(0, Idx + 1).Span.SequenceEqual(OutputData.AsSpan(0, Idx + 1)));
			}
		}

		[TestMethod]
		public async Task FixedSizeChunkingTests()
		{
			ChunkingOptions Options = new ChunkingOptions();
			Options.LeafOptions = new ChunkingOptionsForNodeType(64, 64, 64);
			Options.InteriorOptions = new ChunkingOptionsForNodeType(IoHash.NumBytes * 4, IoHash.NumBytes * 4, IoHash.NumBytes * 4);

			await ChunkingTests(Options);
		}

		[TestMethod]
		public async Task VariableSizeChunkingTests()
		{
			ChunkingOptions Options = new ChunkingOptions();
			Options.LeafOptions = new ChunkingOptionsForNodeType(32, 64, 96);
			Options.InteriorOptions = new ChunkingOptionsForNodeType(IoHash.NumBytes * 1, IoHash.NumBytes * 4, IoHash.NumBytes * 12);

			await ChunkingTests(Options);
		}

		async Task ChunkingTests(ChunkingOptions Options)
		{
			byte[] Data = new byte[4096];
			new Random(0).NextBytes(Data);

			for (int Idx = 0; Idx < Data.Length; Idx++)
			{
				Data[Idx] = (byte)Idx;
			}

			Bundle<FileNode> NewBundle = Bundle.Create<FileNode>(StorageClient, NamespaceId, new FileNode(), new BundleOptions(), Cache);
			FileNode Root = NewBundle.Root;

			const int NumIterations = 100;
			for (int Idx = 0; Idx < NumIterations; Idx++)
			{
				Root.Append(Data, Options);
			}

			byte[] Result = await Root.ToByteArrayAsync(null!);
			Assert.AreEqual(NumIterations * Data.Length, Result.Length);

			for (int Idx = 0; Idx < NumIterations; Idx++)
			{
				ReadOnlyMemory<byte> SpanData = Result.AsMemory(Idx * Data.Length, Data.Length);
				Assert.IsTrue(SpanData.Span.SequenceEqual(Data));
			}

			await CheckSizes(NewBundle, Root, Options, true);
		}

		async Task CheckSizes(Bundle Bundle, FileNode Node, ChunkingOptions Options, bool Rightmost)
		{
			if (Node.Depth == 0)
			{
				Assert.IsTrue(Rightmost || Node.Payload.Length >= Options.LeafOptions.MinSize);
				Assert.IsTrue(Node.Payload.Length <= Options.LeafOptions.MaxSize);
			}
			else
			{
				Assert.IsTrue(Rightmost || Node.Payload.Length >= Options.InteriorOptions.MinSize);
				Assert.IsTrue(Node.Payload.Length <= Options.InteriorOptions.MaxSize);

				int ChildCount = Node.Children.Count;
				for (int Idx = 0; Idx < ChildCount; Idx++)
				{
					FileNode ChildNode = await Bundle.GetAsync(Node.Children[Idx]);
					await CheckSizes(Bundle, ChildNode, Options, Idx == ChildCount - 1);
				}
			}
		}

		[TestMethod]
		public async Task SpillTestAsync()
		{
			BundleOptions Options = new BundleOptions();
			Options.MaxBlobSize = 1;

			using Bundle<DirectoryNode> NewBundle = Bundle.Create<DirectoryNode>(StorageClient, NamespaceId, new DirectoryNode(), Options, Cache);
			DirectoryNode Root = NewBundle.Root;

			long TotalLength = 0;
			for (int IdxA = 0; IdxA < 10; IdxA++)
			{
				DirectoryNode NodeA = Root.AddDirectory($"{IdxA}");
				for (int IdxB = 0; IdxB < 10; IdxB++)
				{
					DirectoryNode NodeB = NodeA.AddDirectory($"{IdxB}");
					for (int IdxC = 0; IdxC < 10; IdxC++)
					{
						DirectoryNode NodeC = NodeB.AddDirectory($"{IdxC}");
						for (int IdxD = 0; IdxD < 10; IdxD++)
						{
							FileNode File = NodeC.AddFile($"{IdxD}", 0);
							byte[] Data = Encoding.UTF8.GetBytes($"This is file {IdxA}/{IdxB}/{IdxC}/{IdxD}");
							TotalLength += Data.Length;
							File.Append(Data, new ChunkingOptions());
						}
					}

					int OldWorkingSetSize = GetWorkingSetSize(NewBundle.Root);
					await NewBundle.TrimAsync(20);
					int NewWorkingSetSize = GetWorkingSetSize(NewBundle.Root);
					Assert.IsTrue(NewWorkingSetSize <= OldWorkingSetSize);
					Assert.IsTrue(NewWorkingSetSize <= 20);
				}
			}

			Assert.IsTrue(StorageClient.Blobs.Count > 0);
			Assert.IsTrue(StorageClient.Refs.Count == 0);

			RefId RefId = new RefId("ref");
			await NewBundle.WriteAsync(BucketId, RefId, CbObject.Empty, true);

			Assert.AreEqual(TotalLength, Root.Length);

			Assert.IsTrue(StorageClient.Blobs.Count > 0);
			Assert.IsTrue(StorageClient.Refs.Count == 1);
		}

		int GetWorkingSetSize(BundleNode Node)
		{
			int Size = 0;
			foreach (BundleNodeRef NodeRef in Node.GetReferences())
			{
				if (NodeRef.Node != null)
				{
					Size += 1 + GetWorkingSetSize(NodeRef.Node);
				}
			}
			return Size;
		}
	}
}
