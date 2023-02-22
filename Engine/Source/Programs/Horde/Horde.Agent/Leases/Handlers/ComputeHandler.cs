// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using Amazon.EC2;
using EpicGames.Core;
using EpicGames.Horde.Compute;
using EpicGames.Horde.Compute.Cpp;
using EpicGames.Horde.Logs;
using EpicGames.Horde.Storage;
using EpicGames.Horde.Storage.Backends;
using EpicGames.Horde.Storage.Nodes;
using Horde.Agent.Services;
using HordeCommon.Rpc.Tasks;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.Extensions.Logging;

namespace Horde.Agent.Leases.Handlers
{
	/// <summary>
	/// Handler for compute tasks
	/// </summary>
	class ComputeHandler : LeaseHandler<ComputeTask>
	{
		readonly ILogger _logger;

		/// <summary>
		/// Constructor
		/// </summary>
		public ComputeHandler(ILogger<ComputeHandler> logger)
		{
			_logger = logger;
		}

		/// <inheritdoc/>
		public override async Task<LeaseResult> ExecuteAsync(ISession session, string leaseId, ComputeTask computeTask, CancellationToken cancellationToken)
		{
			_logger.LogInformation("Starting compute task (lease {LeaseId})", leaseId);

			try
			{
				await ConnectAsync(computeTask, cancellationToken);
			}
			catch (Exception ex)
			{
				_logger.LogError(ex, "Exception while executing compute task");
			}

			return LeaseResult.Success;
		}

		async Task ConnectAsync(ComputeTask computeTask, CancellationToken cancellationToken)
		{
			using TcpClient tcpClient = new TcpClient();
			await tcpClient.ConnectAsync(computeTask.RemoteIp, computeTask.RemotePort);

			Socket socket = tcpClient.Client;
			await socket.SendAsync(computeTask.Nonce.Memory, SocketFlags.None, cancellationToken);

			using SocketComputeChannel channel = new SocketComputeChannel(socket, computeTask.AesKey.Memory, computeTask.AesIv.Memory);
			await RunAsync(channel, cancellationToken);
		}

		public async Task RunAsync(IComputeChannel channel, CancellationToken cancellationToken)
		{
			for (; ; )
			{
				object? request = await channel.ReadCbMessageAsync(cancellationToken);
				switch (request)
				{
					case null:
					case CloseMessage _:
						return;
					case XorRequestMessage xorRequest:
						{
							XorResponseMessage response = new XorResponseMessage();
							response.Payload = new byte[xorRequest.Payload.Length];
							for (int idx = 0; idx < xorRequest.Payload.Length; idx++)
							{
								response.Payload[idx] = (byte)(xorRequest.Payload[idx] ^ xorRequest.Value);
							}
							await channel.WriteCbMessageAsync(response, cancellationToken);
						}
						break;
					case CppComputeMessage cppCompute:
						await RunCppAsync(channel, cppCompute.Locator, cancellationToken);
						break;
				}
			}
		}

		public async Task RunCppAsync(IComputeChannel channel, NodeLocator locator, CancellationToken cancellationToken)
		{
			DirectoryReference sandboxDir = DirectoryReference.Combine(Program.DataDir, "Sandbox");

			using ComputeStorageClient store = new ComputeStorageClient(channel);
			TreeReader reader = new TreeReader(store, null, _logger);

			CppComputeNode node = await reader.ReadNodeAsync<CppComputeNode>(locator, cancellationToken);
			DirectoryNode directoryNode = await node.Sandbox.ExpandAsync(reader, cancellationToken);

			await directoryNode.CopyToDirectoryAsync(reader, sandboxDir.ToDirectoryInfo(), _logger, cancellationToken);

			MemoryStorageClient storage = new MemoryStorageClient();
			using (TreeWriter writer = new TreeWriter(storage))
			{
				string executable = FileReference.Combine(sandboxDir, node.Executable).FullName;
				string commandLine = CommandLineArguments.Join(node.Arguments);
				string workingDir = DirectoryReference.Combine(sandboxDir, node.WorkingDirectory).FullName;

				Dictionary<string, string>? envVars = null;
				if (node.EnvVars.Count > 0)
				{
					envVars = ManagedProcess.GetCurrentEnvVars();
					foreach ((string key, string value) in node.EnvVars)
					{
						envVars[key] = value;
					}
				}

				int exitCode;

				LogWriter logWriter = new LogWriter(writer);
				using (ManagedProcessGroup group = new ManagedProcessGroup())
				{
					using (ManagedProcess process = new ManagedProcess(group, executable, commandLine, workingDir, envVars, null, ProcessPriorityClass.Normal))
					{
						byte[] buffer = new byte[1024];
						for (; ; )
						{
							int length = await process.ReadAsync(buffer, 0, buffer.Length, cancellationToken);
							if (length == 0)
							{
								process.WaitForExit();
								exitCode = process.ExitCode;
								break;
							}
							await logWriter.AppendAsync(buffer.AsMemory(0, length), cancellationToken);
						}
					}
				}

				TreeNodeRef<LogNode> logNodeRef = await logWriter.CompleteAsync(cancellationToken);

				FileFilter filter = new FileFilter(node.OutputPaths.Select(x => x.ToString()));
				List<FileReference> outputFiles = filter.ApplyToDirectory(sandboxDir, false);

				DirectoryNode outputTree = new DirectoryNode();
				await outputTree.CopyFilesAsync(sandboxDir, outputFiles, new ChunkingOptions(), writer, null, cancellationToken);

				CppComputeOutputNode outputNode = new CppComputeOutputNode(exitCode, logNodeRef, new TreeNodeRef<DirectoryNode>(outputTree));
				NodeHandle outputHandle = await writer.FlushAsync(outputNode, cancellationToken);

				CppComputeOutputMessage outputMessage = new CppComputeOutputMessage { Locator = outputHandle.Locator };
				await channel.WriteCbMessageAsync(outputMessage, cancellationToken);
			}

			for (; ; )
			{
				object? request = await channel.ReadCbMessageAsync(cancellationToken);
				switch (request)
				{
					case null:
					case CppComputeFinishMessage _:
						return;
					case BlobReadMessage blobRead:
						await channel.WriteBlobDataAsync(blobRead, storage, cancellationToken);
						break;
				}
			}
		}

		class LogWriter
		{
			const int MaxChunkSize = 128 * 1024;
			const int FlushChunkSize = 100 * 1024;

			readonly TreeWriter _writer;
			int _lineCount;
			long _offset;
			readonly LogChunkBuilder _chunkBuilder = new LogChunkBuilder(MaxChunkSize);
			readonly List<LogChunkRef> _chunks = new List<LogChunkRef>();

			public IReadOnlyList<LogChunkRef> Chunks => _chunks;

			public LogWriter(TreeWriter writer)
			{
				_writer = writer;
			}

			public async ValueTask AppendAsync(ReadOnlyMemory<byte> data, CancellationToken cancellationToken)
			{
				_chunkBuilder.Append(data.Span);

				if (_chunkBuilder.Length > FlushChunkSize)
				{
					await FlushAsync(cancellationToken);
				}
			}

			async Task FlushAsync(CancellationToken cancellationToken)
			{
				LogChunkNode chunkNode = _chunkBuilder.ToLogChunk();
				LogChunkRef chunkRef = new LogChunkRef(_lineCount, _offset, chunkNode);
				await _writer.WriteAsync(chunkRef, cancellationToken);

				_chunks.Add(chunkRef);
				_lineCount += chunkNode.LineCount;
				_offset += chunkNode.Length;
			}

			public async Task<TreeNodeRef<LogNode>> CompleteAsync(CancellationToken cancellationToken)
			{
				await FlushAsync(cancellationToken);
				LogIndexNode index = new LogIndexNode(new NgramSet(Array.Empty<ushort>()), 0, Array.Empty<LogChunkRef>());

				TreeNodeRef<LogNode> logNodeRef = new TreeNodeRef<LogNode>(new LogNode(LogFormat.Text, _lineCount, _offset, _chunks, new TreeNodeRef<LogIndexNode>(index), true));
				await _writer.WriteAsync(logNodeRef, cancellationToken);

				return logNodeRef;
			}
		}
	}
}

