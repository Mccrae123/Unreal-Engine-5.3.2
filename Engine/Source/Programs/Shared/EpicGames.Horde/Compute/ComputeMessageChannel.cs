// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Buffers;
using System.Buffers.Binary;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Extensions.Logging;

namespace EpicGames.Horde.Compute
{
	/// <summary>
	/// Implementation of a compute channel
	/// </summary>
	public sealed class ComputeMessageChannel : IComputeMessageChannel
	{
		// Length of a message header. Consists of a 1 byte type field, followed by 4 byte length field.
		const int HeaderLength = 5;

		/// <summary>
		/// Standard implementation of a message
		/// </summary>
		sealed class Message : IComputeMessage
		{
			/// <inheritdoc/>
			public ComputeMessageType Type { get; }

			/// <inheritdoc/>
			public ReadOnlyMemory<byte> Data { get; }

			readonly IMemoryOwner<byte> _memoryOwner;
			int _position;

			public Message(ComputeMessageType type, ReadOnlyMemory<byte> data)
			{
				_memoryOwner = MemoryPool<byte>.Shared.Rent(data.Length);
				data.CopyTo(_memoryOwner.Memory);

				Type = type;
				Data = _memoryOwner.Memory.Slice(0, data.Length);
			}

			/// <inheritdoc/>
			public void Dispose()
			{
				_memoryOwner.Dispose();
			}

			/// <inheritdoc/>
			public ReadOnlyMemory<byte> GetMemory(int minSize = 1) => Data.Slice(_position);

			/// <inheritdoc/>
			public void Advance(int length) => _position += length;
		}

		/// <summary>
		/// Allows creating new messages in rented memory
		/// </summary>
		class MessageBuilder : IComputeMessageBuilder
		{
			readonly ComputeMessageChannel _channel;
			readonly IComputeBufferWriter _sendBufferWriter;
			readonly ComputeMessageType _type;
			int _length;

			/// <inheritdoc/>
			public int Length => _length;

			public MessageBuilder(ComputeMessageChannel channel, IComputeBufferWriter sendBufferWriter, ComputeMessageType type)
			{
				_channel = channel;
				_sendBufferWriter = sendBufferWriter;
				_type = type;
				_length = 0;
			}

			public void Dispose()
			{
				if (_channel._currentBuilder == this)
				{
					_channel._currentBuilder = null;
				}
			}

			public void Send()
			{
				Span<byte> header = _sendBufferWriter.GetWriteBuffer().Span;
				header[0] = (byte)_type;
				BinaryPrimitives.WriteInt32LittleEndian(header.Slice(1, 4), _length);

				if (_channel._logger.IsEnabled(LogLevel.Trace))
				{
					_channel.LogMessageInfo("SEND", _type, _sendBufferWriter.GetWriteBuffer().Slice(HeaderLength, _length).Span);
				}

				_sendBufferWriter.AdvanceWritePosition(HeaderLength + _length);
				_length = 0;
			}

			/// <inheritdoc/>
			public void Advance(int count) => _length += count;

			/// <inheritdoc/>
			public Memory<byte> GetMemory(int sizeHint = 0) => _sendBufferWriter.GetWriteBuffer().Slice(HeaderLength + _length);

			/// <inheritdoc/>
			public Span<byte> GetSpan(int sizeHint = 0) => GetMemory(sizeHint).Span;
		}

		readonly IComputeSocket _socket;
		readonly IComputeBufferReader _recvBufferReader;
		readonly IComputeBufferWriter _sendBufferWriter;

		// Can lock chunked memory writer to acuqire pointer
		readonly ILogger _logger;

#pragma warning disable CA2213
		MessageBuilder? _currentBuilder;
#pragma warning restore CA2213

		/// <summary>
		/// Constructor
		/// </summary>
		/// <param name="recvBufferReader"></param>
		/// <param name="sendBufferWriter"></param>
		/// <param name="logger">Logger for diagnostic output</param>
		public ComputeMessageChannel(IComputeBufferReader recvBufferReader, IComputeBufferWriter sendBufferWriter, ILogger logger)
		{
			_socket = null!;
			_recvBufferReader = recvBufferReader.AddRef();
			_sendBufferWriter = sendBufferWriter.AddRef();
			_logger = logger;
		}

		/// <summary>
		/// Constructor
		/// </summary>
		/// <param name="socket"></param>
		/// <param name="channelId"></param>
		/// <param name="recvBuffer"></param>
		/// <param name="sendBuffer"></param>
		/// <param name="logger">Logger for diagnostic output</param>
		public ComputeMessageChannel(IComputeSocket socket, int channelId, IComputeBuffer recvBuffer, IComputeBuffer sendBuffer, ILogger logger)
		{
			_socket = socket;
			_socket.AttachRecvBuffer(channelId, recvBuffer.Writer);
			_socket.AttachSendBuffer(channelId, sendBuffer.Reader);
			_recvBufferReader = recvBuffer.Reader.AddRef();
			_sendBufferWriter = sendBuffer.Writer.AddRef();
			_logger = logger;
		}

		/// <summary>
		/// Overridable dispose method
		/// </summary>
		public void Dispose()
		{
			_currentBuilder?.Dispose();

			_sendBufferWriter.MarkComplete();
			_sendBufferWriter.Dispose();

			_recvBufferReader.Dispose();
		}

		/// <summary>
		/// Mark the send buffer as complete
		/// </summary>
		public void MarkComplete()
		{
			_sendBufferWriter.MarkComplete();
		}

		/// <inheritdoc/>
		public async ValueTask<IComputeMessage> ReceiveAsync(CancellationToken cancellationToken)
		{
			while (!_recvBufferReader.IsComplete)
			{
				ReadOnlyMemory<byte> memory = _recvBufferReader.GetReadBuffer();
				if (memory.Length < HeaderLength)
				{
					await _recvBufferReader.WaitToReadAsync(HeaderLength, cancellationToken);
					continue;
				}

				int messageLength = BinaryPrimitives.ReadInt32LittleEndian(memory.Span.Slice(1, 4));
				if (memory.Length < HeaderLength + messageLength)
				{
					await _recvBufferReader.WaitToReadAsync(HeaderLength + messageLength, cancellationToken);
					continue;
				}

				ComputeMessageType type = (ComputeMessageType)memory.Span[0];
				Message message = new Message(type, memory.Slice(HeaderLength, messageLength));
				if (_logger.IsEnabled(LogLevel.Trace))
				{
					LogMessageInfo("RECV", message.Type, message.Data.Span);
				}

				_recvBufferReader.AdvanceReadPosition(HeaderLength + messageLength);
				return message;
			}
			return new Message(ComputeMessageType.None, ReadOnlyMemory<byte>.Empty);
		}

		void LogMessageInfo(string verb, ComputeMessageType type, ReadOnlySpan<byte> data)
		{
			StringBuilder bytes = new StringBuilder();
			for (int offset = 0; offset < 16 && offset < data.Length; offset++)
			{
				bytes.Append($" {data[offset]:X2}");
			}
			if (data.Length > 16)
			{
				bytes.Append("..");
			}
			_logger.LogTrace("{Verb} {Type,-22} [{Length,10:n0}] = {Bytes}", verb, type, data.Length, bytes.ToString());
		}

		/// <inheritdoc/>
		public async ValueTask<IComputeMessageBuilder> CreateMessageAsync(ComputeMessageType type, int maxSize, CancellationToken cancellationToken)
		{
			if (_currentBuilder != null)
			{
				throw new InvalidOperationException("Only one writer can be active at a time. Dispose of the previous writer first.");
			}

			await _sendBufferWriter.WaitToWriteAsync(maxSize, cancellationToken);

			_currentBuilder = new MessageBuilder(this, _sendBufferWriter, type);
			return _currentBuilder;
		}
	}
}
