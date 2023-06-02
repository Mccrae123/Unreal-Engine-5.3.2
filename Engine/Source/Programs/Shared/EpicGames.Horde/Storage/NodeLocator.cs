// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.ComponentModel;
using System.Diagnostics.CodeAnalysis;
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization;
using EpicGames.Core;
using EpicGames.Serialization;

namespace EpicGames.Horde.Storage
{
	/// <summary>
	/// Locates a node in storage
	/// </summary>
	[JsonConverter(typeof(NodeLocatorJsonConverter))]
	[TypeConverter(typeof(NodeLocatorTypeConverter))]
	[CbConverter(typeof(NodeLocatorCbConverter))]
	public struct NodeLocator : IEquatable<NodeLocator>
	{
		/// <summary>
		/// Location of the blob containing this node
		/// </summary>
		public BlobLocator Blob { get; }

		/// <summary>
		/// Index of the export within the blob
		/// </summary>
		public int ExportIdx { get; }

		/// <summary>
		/// Constructor
		/// </summary>
		public NodeLocator(BlobLocator blob, int exportIdx)
		{
			Blob = blob;
			ExportIdx = exportIdx;
		}

		/// <summary>
		/// Determines if this locator points to a valid entry
		/// </summary>
		public bool IsValid() => Blob.IsValid();

		/// <summary>
		/// Parse a string as a node locator
		/// </summary>
		/// <param name="text">Text to parse</param>
		/// <returns></returns>
		public static NodeLocator Parse(ReadOnlySpan<char> text)
		{
			int hashIdx = text.IndexOf('#');
			if (hashIdx == -1)
			{
				throw new ArgumentException("Invalid node locator", nameof(text));
			}

			int exportIdx = Int32.Parse(text.Slice(hashIdx + 1), NumberStyles.None, CultureInfo.InvariantCulture);
			BlobLocator blobLocator = new BlobLocator(new Utf8String(text.Slice(0, hashIdx)));
			return new NodeLocator(blobLocator, exportIdx);
		}

		/// <inheritdoc/>
		public override bool Equals([NotNullWhen(true)] object? obj) => obj is NodeLocator locator && Equals(locator);

		/// <inheritdoc/>
		public bool Equals(NodeLocator other) => Blob == other.Blob && ExportIdx == other.ExportIdx;

		/// <inheritdoc/>
		public override int GetHashCode() => HashCode.Combine(Blob, ExportIdx);

		/// <inheritdoc/>
		public override string ToString() => $"{Blob}#{ExportIdx}";

		/// <inheritdoc/>
		public static bool operator ==(NodeLocator left, NodeLocator right) => left.Equals(right);

		/// <inheritdoc/>
		public static bool operator !=(NodeLocator left, NodeLocator right) => !left.Equals(right);
	}

	/// <summary>
	/// Combination of a NodeLocator with Hash
	/// </summary>
	/// <param name="Hash">Hash of the target node</param>
	/// <param name="Locator">Locator for the node</param>
	public readonly record struct HashedNodeLocator(IoHash Hash, NodeLocator Locator)
	{
		/// <inheritdoc cref="NodeLocator.Blob"/>
		public BlobLocator Blob => Locator.Blob;

		/// <inheritdoc cref="NodeLocator.ExportIdx"/>
		public int ExportIdx => Locator.ExportIdx;

		/// <summary>
		/// Constructor
		/// </summary>
		public HashedNodeLocator(IoHash hash, BlobLocator blobLocator, int exportIdx) 
			: this(hash, new NodeLocator(blobLocator, exportIdx))
		{
		}

		/// <summary>
		/// Parse a node handle value from a string
		/// </summary>
		/// <param name="text">Text to parse</param>
		/// <returns>Parsed node handle</returns>
		public static HashedNodeLocator Parse(string text)
		{
			int hashLength = IoHash.NumBytes * 2;
			if (text.Length == hashLength)
			{
				return new HashedNodeLocator(IoHash.Parse(text), default);
			}
			else if (text[hashLength] == '@')
			{
				return new HashedNodeLocator(IoHash.Parse(text.Substring(0, hashLength)), NodeLocator.Parse(text.Substring(hashLength + 1)));
			}
			else
			{
				throw new FormatException("Invalid NodeHandle value");
			}
		}

		/// <inheritdoc/>
		public override string ToString()
		{
			if (Locator.IsValid())
			{
				return $"{Hash}@{Locator}";
			}
			else
			{
				return $"{Hash}";
			}
		}
	}

	/// <summary>
	/// Type converter for <see cref="NodeLocator"/> to and from JSON
	/// </summary>
	sealed class NodeLocatorJsonConverter : JsonConverter<NodeLocator>
	{
		/// <inheritdoc/>
		public override NodeLocator Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options) => NodeLocator.Parse(reader.GetString() ?? String.Empty);

		/// <inheritdoc/>
		public override void Write(Utf8JsonWriter writer, NodeLocator value, JsonSerializerOptions options) => writer.WriteStringValue(value.ToString());
	}

	/// <summary>
	/// Type converter from strings to <see cref="NodeLocator"/> objects
	/// </summary>
	sealed class NodeLocatorTypeConverter : TypeConverter
	{
		/// <inheritdoc/>
		public override bool CanConvertFrom(ITypeDescriptorContext? context, Type sourceType)
		{
			return sourceType == typeof(string);
		}

		/// <inheritdoc/>
		public override object ConvertFrom(ITypeDescriptorContext? context, CultureInfo? culture, object? value)
		{
			return NodeLocator.Parse((string)value!);
		}
	}

	/// <summary>
	/// Type converter to compact binary
	/// </summary>
	sealed class NodeLocatorCbConverter : CbConverterBase<NodeLocator>
	{
		/// <inheritdoc/>
		public override NodeLocator Read(CbField field) => NodeLocator.Parse(field.AsString());

		/// <inheritdoc/>
		public override void Write(CbWriter writer, NodeLocator value) => writer.WriteUtf8StringValue(value.ToString());

		/// <inheritdoc/>
		public override void WriteNamed(CbWriter writer, Utf8String name, NodeLocator value) => writer.WriteUtf8String(name, value.ToString());
	}

	/// <summary>
	/// Extension methods for node locators
	/// </summary>
	public static class NodeLocatorExtensions
	{
		/// <summary>
		/// Deserialize a node locator
		/// </summary>
		/// <param name="reader">Reader to deserialize from</param>
		/// <returns>The node id that was read</returns>
		public static NodeLocator ReadNodeLocator(this IMemoryReader reader)
		{
			return NodeLocator.Parse(reader.ReadString());
		}

		/// <summary>
		/// Serialize a node locator
		/// </summary>
		/// <param name="writer">Writer to serialize to</param>
		/// <param name="value">Value to serialize</param>
		public static void WriteNodeLocator(this IMemoryWriter writer, NodeLocator value)
		{
			writer.WriteString(value.ToString());
		}
	}
}
