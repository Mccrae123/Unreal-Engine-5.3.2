// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using EpicGames.Core;
using EpicGames.UHT.Tables;
using EpicGames.UHT.Tokenizer;
using EpicGames.UHT.Utils;

namespace EpicGames.UHT.Parsers
{

	/// <summary>
	/// Class responsible for parsing specifiers and the field meta data.  To reduce allocations, one specifier parser is shared between all objects in
	/// a given header file.  This makes the Action pattern being used a bit more obtuse, but it does help performance by reducing the allocations fairly
	/// significantly.
	/// </summary>
	public class UhtSpecifierParser : IUhtMessageExtraContext
	{
		struct DeferredSpecifier
		{
			public UhtSpecifier _specifier;
			public object? _value;
		}

		private static readonly List<KeyValuePair<StringView, StringView>> s_emptyKVPValues = new List<KeyValuePair<StringView, StringView>>();

		private UhtSpecifierContext _specifierContext;
		private IUhtTokenReader _tokenReader;
		private UhtSpecifierTable _table;
		private StringView _context;
		private StringView _currentSpecifier = new StringView();
		private List<DeferredSpecifier> _deferredSpecifiers = new List<DeferredSpecifier>();
		private bool _isParsingFieldMetaData = false;
		private List<KeyValuePair<StringView, StringView>>? _currentKVPValues = null;
		private List<StringView>? _currentStringValues = null;
		private int _umetaElementsParsed = 0;

		private readonly Action _parseAction;
		private readonly Action _parseFieldMetaDataAction;
		private readonly Action _parseKVPValueAction;
		private readonly Action _parseStringViewListAction;

		/// <summary>
		/// Construct a new specifier parser
		/// </summary>
		/// <param name="specifierContext">Specifier context</param>
		/// <param name="context">User facing context added to messages</param>
		/// <param name="table">Specifier table</param>
		public UhtSpecifierParser(UhtSpecifierContext specifierContext, StringView context, UhtSpecifierTable table)
		{
			this._specifierContext = specifierContext;
			this._tokenReader = specifierContext.Scope.TokenReader;
			this._context = context;
			this._table = table;

			this._parseAction = ParseInternal;
			this._parseFieldMetaDataAction = ParseFieldMetaDataInternal;
			this._parseKVPValueAction = () =>
			{
				if (this._currentKVPValues == null)
				{
					this._currentKVPValues = new List<KeyValuePair<StringView, StringView>>();
				}
				this._currentKVPValues.Add(ReadKVP());
			};
			this._parseStringViewListAction = () =>
			{
				if (this._currentStringValues == null)
				{
					this._currentStringValues = new List<StringView>();
				}
				this._currentStringValues.Add(ReadValue());
			};
		}

		/// <summary>
		/// Reset an existing parser to parse a new specifier block
		/// </summary>
		/// <param name="specifierContext">Specifier context</param>
		/// <param name="context">User facing context added to messages</param>
		/// <param name="table">Specifier table</param>
		public void Reset(UhtSpecifierContext specifierContext, StringView context, UhtSpecifierTable table)
		{
			this._specifierContext = specifierContext;
			this._tokenReader = specifierContext.Scope.TokenReader;
			this._context = context;
			this._table = table;
		}

		/// <summary>
		/// Perform the specify parsing
		/// </summary>
		/// <returns>The parser</returns>
		public UhtSpecifierParser ParseSpecifiers()
		{
			this._isParsingFieldMetaData = false;
			this._specifierContext.MetaData.LineNumber = _tokenReader.InputLine;

			using (UhtMessageContext tokenContext = new UhtMessageContext(this))
			{
				this._tokenReader.RequireList('(', ')', ',', false, this._parseAction);
			}
			return this;
		}

		/// <summary>
		/// Parse field meta data
		/// </summary>
		/// <returns>Specifier parser</returns>
		public UhtSpecifierParser ParseFieldMetaData()
		{
			this._tokenReader = this._specifierContext.Scope.TokenReader;
			this._isParsingFieldMetaData = true;

			using (UhtMessageContext tokenContext = new UhtMessageContext(this))
			{
				if (_tokenReader.TryOptional("UMETA"))
				{
					this._umetaElementsParsed = 0;
					_tokenReader.RequireList('(', ')', ',', false, this._parseFieldMetaDataAction);
					if (this._umetaElementsParsed == 0)
					{
						this._tokenReader.LogError($"No metadata specified while parsing {UhtMessage.FormatContext(this)}");
					}
				}
			}
			return this;
		}

		/// <summary>
		/// Parse any deferred specifiers
		/// </summary>
		public void ParseDeferred()
		{
			foreach (DeferredSpecifier deferred in this._deferredSpecifiers)
			{
				Dispatch(deferred._specifier, deferred._value);
			}
			this._deferredSpecifiers.Clear();
		}

		#region IMessageExtraContext implementation

		/// <inheritdoc/>
		public IEnumerable<object?>? MessageExtraContext
		{
			get
			{
				Stack<object?> extraContext = new Stack<object?>(1);
				string what = this._isParsingFieldMetaData ? "metadata" : "specifiers";
				if (this._context.Span.Length > 0)
				{
					extraContext.Push($"{this._context} {what}");
				}
				else
				{
					extraContext.Push(what);
				}
				return extraContext;
			}
		}
		#endregion

		private void ParseInternal()
		{
			UhtToken identifier = this._tokenReader.GetIdentifier();

			this._currentSpecifier = identifier.Value;
			UhtSpecifier? specifier;
			if (this._table.TryGetValue(_currentSpecifier, out specifier))
			{
				if (TryParseValue(specifier.ValueType, out object? value))
				{
					if (specifier.When == UhtSpecifierWhen.Deferred)
					{
						if (this._deferredSpecifiers == null)
						{
							this._deferredSpecifiers = new List<DeferredSpecifier>();
						}
						this._deferredSpecifiers.Add(new DeferredSpecifier { _specifier = specifier, _value = value });
					}
					else
					{
						Dispatch(specifier, value);
					}
				}
			}
			else
			{
				this._tokenReader.LogError($"Unknown specifier '{_currentSpecifier}' found while parsing {UhtMessage.FormatContext(this)}");
			}
		}

		private void ParseFieldMetaDataInternal()
		{
			UhtToken key;
			if (!_tokenReader.TryOptionalIdentifier(out key))
			{
				throw new UhtException(this._tokenReader, $"UMETA expects a key and optional value", this);
			}

			StringViewBuilder builder = new StringViewBuilder();
			if (_tokenReader.TryOptional('='))
			{
				if (!ReadValue(_tokenReader, builder, true))
				{
					throw new UhtException(this._tokenReader, $"UMETA key '{key.Value}' expects a value", this);
				}
			}

			++this._umetaElementsParsed;
			this._specifierContext.MetaData.Add(key.Value.ToString(), this._specifierContext.MetaNameIndex, builder.ToString());
		}

		private void Dispatch(UhtSpecifier specifier, object? value)
		{
			UhtSpecifierDispatchResults results = specifier.Dispatch(this._specifierContext, value);
			if (results == UhtSpecifierDispatchResults.Unknown)
			{
				this._tokenReader.LogError($"Unknown specifier '{specifier.Name}' found while parsing {UhtMessage.FormatContext(this)}");
			}
		}

		private bool TryParseValue(UhtSpecifierValueType valueType, out object? value)
		{
			value = null;

			switch (valueType)
			{
				case UhtSpecifierValueType.NotSet:
					throw new UhtIceException("NotSet is an invalid value for value types");

				case UhtSpecifierValueType.None:
					if (this._tokenReader.TryOptional('='))
					{
						ReadValue(); // consume the value;
						this._tokenReader.LogError($"The specifier '{this._currentSpecifier}' found a value when none was expected", this);
						return false;
					}
					return true;

				case UhtSpecifierValueType.String:
					if (!this._tokenReader.TryOptional('='))
					{
						this._tokenReader.LogError($"The specifier '{this._currentSpecifier}' expects a value", this);
						return false;
					}
					value = ReadValue();
					return true;

				case UhtSpecifierValueType.OptionalString:
					{
						List<StringView>? stringList = ReadValueList();
						if (stringList != null && stringList.Count > 0)
						{
							value = stringList[0];
						}
						return true;
					}

				case UhtSpecifierValueType.SingleString:
					{
						List<StringView>? stringList = ReadValueList();
						if (stringList == null || stringList.Count != 1)
						{
							this._tokenReader.LogError($"The specifier '{this._currentSpecifier}' expects a single value", this);
							return false;
						}
						value = stringList[0];
						return true;
					}

				case UhtSpecifierValueType.StringList:
					value = ReadValueList();
					return true;

				case UhtSpecifierValueType.Legacy:
					value = ReadValueList();
					return true;

				case UhtSpecifierValueType.NonEmptyStringList:
					{
						List<StringView>? stringList = ReadValueList();
						if (stringList == null || stringList.Count == 0)
						{
							this._tokenReader.LogError($"The specifier '{this._currentSpecifier}' expects at list one value", this);
							return false;
						}
						value = stringList;
						return true;
					}

				case UhtSpecifierValueType.KeyValuePairList:
					{
						this._currentKVPValues = null;
						this._tokenReader
							.Require('=')
							.RequireList('(', ')', ',', false, this._parseKVPValueAction);
						List<KeyValuePair<StringView, StringView>> kvps = this._currentKVPValues ?? s_emptyKVPValues;
						this._currentKVPValues = null;
						value = kvps;
						return true;
					}

				case UhtSpecifierValueType.OptionalEqualsKeyValuePairList:
					{
						this._currentKVPValues = null;
						// This parser isn't as strict as the other parsers...
						if (this._tokenReader.TryOptional('='))
						{
							if (!this._tokenReader.TryOptionalList('(', ')', ',', false, this._parseKVPValueAction))
							{
								this._parseKVPValueAction();
							}
						}
						else
						{
							this._tokenReader.TryOptionalList('(', ')', ',', false, this._parseKVPValueAction);
						}
						List<KeyValuePair<StringView, StringView>> kvps = this._currentKVPValues ?? s_emptyKVPValues;
						this._currentKVPValues = null;
						value = kvps;
						return true;
					}

				default:
					throw new UhtIceException("Unknown value type");
			}
		}

		private KeyValuePair<StringView, StringView> ReadKVP()
		{
			UhtToken key;
			if (!this._tokenReader.TryOptionalIdentifier(out key))
			{
				throw new UhtException(this._tokenReader, $"The specifier '{this._currentSpecifier}' expects a key and optional value", this);
			}

			StringView value = "";
			if (this._tokenReader.TryOptional('='))
			{
				value = ReadValue();
			}
			return new KeyValuePair<StringView, StringView>(key.Value, value);
		}

		private List<StringView>? ReadValueList()
		{
			this._currentStringValues = null;

			// This parser isn't as strict as the other parsers...
			if (this._tokenReader.TryOptional('='))
			{
				if (!this._tokenReader.TryOptionalList('(', ')', ',', false, this._parseStringViewListAction))
				{
					this._parseStringViewListAction();
				}
			}
			else
			{
				this._tokenReader.TryOptionalList('(', ')', ',', false, this._parseStringViewListAction);
			}
			List<StringView>? stringValues = this._currentStringValues;
			this._currentStringValues = null;
			return stringValues;
		}

		private StringView ReadValue()
		{
			StringViewBuilder builder = new StringViewBuilder();
			if (!ReadValue(this._tokenReader, builder, false))
			{
				throw new UhtException(this._tokenReader, $"The specifier '{this._currentSpecifier}' expects a value", this);
			}
			return builder.ToStringView();
		}

		/// <summary>
		/// Parse the sequence of meta data
		/// </summary>
		/// <param name="tokenReader">Input token reader</param>
		/// <param name="builder">Output string builder</param>
		/// <param name="respectQuotes">If true, do not convert \" to " in string constants.  This is required for UMETA data</param>
		/// <returns>True if data was read</returns>
		private static bool ReadValue(IUhtTokenReader tokenReader, StringViewBuilder builder, bool respectQuotes)
		{
			UhtToken token = tokenReader.GetToken();
			switch (token.TokenType)
			{
				case UhtTokenType.EndOfFile:
				case UhtTokenType.EndOfDefault:
				case UhtTokenType.EndOfType:
				case UhtTokenType.EndOfDeclaration:
					return false;

				case UhtTokenType.Identifier:
					// We handle true/false differently for compatibility with old UHT
					if (token.IsValue("true", true))
					{
						builder.Append("TRUE");
					}
					else if (token.IsValue("false", true))
					{
						builder.Append("FALSE");
					}
					else
					{
						builder.Append(token.Value);
					}
					if (tokenReader.TryOptional('='))
					{
						builder.Append('=');
						if (!ReadValue(tokenReader, builder, respectQuotes))
						{
							return false;
						}
					}
					break;

				case UhtTokenType.Symbol:
					builder.Append(token.Value);
					if (tokenReader.TryOptional('='))
					{
						builder.Append('=');
						if (!ReadValue(tokenReader, builder, respectQuotes))
						{
							return false;
						}
					}
					break;

				default:
					builder.Append(token.GetConstantValue(respectQuotes));
					break;
			}

			return true;
		}
	}
}
