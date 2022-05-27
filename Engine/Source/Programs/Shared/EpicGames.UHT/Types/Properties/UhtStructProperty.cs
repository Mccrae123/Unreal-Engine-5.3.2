// Copyright Epic Games, Inc. All Rights Reserved.

using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Globalization;
using System.Text;
using System.Text.Json.Serialization;
using EpicGames.Core;
using EpicGames.UHT.Tables;
using EpicGames.UHT.Tokenizer;
using EpicGames.UHT.Utils;

namespace EpicGames.UHT.Types
{
	/// <summary>
	/// FStructProperty
	/// </summary>
	[UnrealHeaderTool]
	[UhtEngineClass(Name = "StructProperty", IsProperty = true)]
	public class UhtStructProperty : UhtProperty
	{
		/// <inheritdoc/>
		public override string EngineClassName => "StructProperty";

		/// <inheritdoc/>
		protected override string CppTypeText => "invalid";

		/// <inheritdoc/>
		protected override string PGetMacroText => "STRUCT";

		/// <inheritdoc/>
		protected override UhtPGetArgumentType PGetTypeArgument => UhtPGetArgumentType.TypeText;

		/// <summary>
		/// USTRUCT referenced by the property
		/// </summary>
		[JsonConverter(typeof(UhtTypeSourceNameJsonConverter<UhtScriptStruct>))]
		public UhtScriptStruct ScriptStruct { get; set; }

		/// <summary>
		/// Construct property
		/// </summary>
		/// <param name="propertySettings">Property settings</param>
		/// <param name="scriptStruct">USTRUCT being referenced</param>
		public UhtStructProperty(UhtPropertySettings propertySettings, UhtScriptStruct scriptStruct) : base(propertySettings)
		{
			this.ScriptStruct = scriptStruct;
			this.HeaderFile.AddReferencedHeader(scriptStruct);
			this.PropertyCaps |= UhtPropertyCaps.SupportsRigVM;
			if (this.ScriptStruct.HasNoOpConstructor)
			{
				this.PropertyCaps |= UhtPropertyCaps.RequiresNullConstructorArg;
			}
			if (this.ScriptStruct.MetaData.GetBoolean(UhtNames.BlueprintType))
			{
				this.PropertyCaps |= UhtPropertyCaps.CanExposeOnSpawn | UhtPropertyCaps.IsParameterSupportedByBlueprint | UhtPropertyCaps.IsMemberSupportedByBlueprint;
			}
			else if (this.ScriptStruct.MetaData.GetBooleanHierarchical(UhtNames.BlueprintType))
			{
				this.PropertyCaps |= UhtPropertyCaps.IsParameterSupportedByBlueprint | UhtPropertyCaps.IsMemberSupportedByBlueprint;
			}
		}

		/// <inheritdoc/>
		protected override bool ResolveSelf(UhtResolvePhase phase)
		{
			bool results = base.ResolveSelf(phase);
			switch (phase)
			{
				case UhtResolvePhase.Final:
					if (ScanForInstancedReferenced(true))
					{
						this.PropertyFlags |= EPropertyFlags.ContainsInstancedReference;
					}
					break;
			}
			return results;
		}

		/// <inheritdoc/>
		public override bool ScanForInstancedReferenced(bool deepScan)
		{
			return this.ScriptStruct.ScanForInstancedReferenced(deepScan);
		}

		/// <inheritdoc/>
		public override void CollectReferencesInternal(IUhtReferenceCollector collector, bool templateProperty)
		{
			collector.AddCrossModuleReference(this.ScriptStruct, true);
		}

		/// <inheritdoc/>
		public override string? GetForwardDeclarations()
		{
			return !this.ScriptStruct.IsCoreType ? $"struct {this.ScriptStruct.SourceName};" : null;
		}

		/// <inheritdoc/>
		public override IEnumerable<UhtType> EnumerateReferencedTypes()
		{
			yield return this.ScriptStruct;
		}

		/// <inheritdoc/>
		public override StringBuilder AppendText(StringBuilder builder, UhtPropertyTextType textType, bool isTemplateArgument)
		{
			switch (textType)
			{
				default:
					builder.Append(this.ScriptStruct.SourceName);
					break;
			}
			return builder;
		}

		/// <inheritdoc/>
		public override StringBuilder AppendMemberDecl(StringBuilder builder, IUhtPropertyMemberContext context, string name, string nameSuffix, int tabs)
		{
			return AppendMemberDecl(builder, context, name, nameSuffix, tabs, "FStructPropertyParams");
		}

		/// <inheritdoc/>
		public override StringBuilder AppendMemberDef(StringBuilder builder, IUhtPropertyMemberContext context, string name, string nameSuffix, string? offset, int tabs)
		{
			AppendMemberDefStart(builder, context, name, nameSuffix, offset, tabs, "FStructPropertyParams", "UECodeGen_Private::EPropertyGenFlags::Struct");
			AppendMemberDefRef(builder, context, this.ScriptStruct, true);
			AppendMemberDefEnd(builder, context, name, nameSuffix);
			return builder;
		}

		/// <inheritdoc/>
		public override void AppendObjectHashes(StringBuilder builder, int startingLength, IUhtPropertyMemberContext context)
		{
			builder.AppendObjectHash(startingLength, this, context, this.ScriptStruct);
		}

		/// <inheritdoc/>
		public override StringBuilder AppendNullConstructorArg(StringBuilder builder, bool isInitializer)
		{
			bool hasNoOpConstructor = this.ScriptStruct.HasNoOpConstructor;
			if (isInitializer && hasNoOpConstructor)
			{
				builder.Append("ForceInit");
			}
			else
			{
				builder.AppendPropertyText(this, UhtPropertyTextType.Construction);
				if (hasNoOpConstructor)
				{
					builder.Append("(ForceInit)");
				}
				else
				{
					builder.Append("()");
				}
			}
			return builder;
		}

		/// <inheritdoc/>
		public override bool SanitizeDefaultValue(IUhtTokenReader defaultValueReader, StringBuilder innerDefaultValue)
		{
			if (!this.Session.TryGetStructDefaultValue(this.ScriptStruct.SourceName, out UhtStructDefaultValue structDefaultValue))
			{
				structDefaultValue = this.Session.DefaultStructDefaultValue;
			}
			return structDefaultValue.Delegate(this, defaultValueReader, innerDefaultValue);
		}

		/// <inheritdoc/>
		public override bool IsSameType(UhtProperty other)
		{
			if (other is UhtStructProperty otherObject)
			{
				return this.ScriptStruct == otherObject.ScriptStruct;
			}
			return false;
		}

		/// <inheritdoc/>
		protected override void ValidateFunctionArgument(UhtFunction function, UhtValidationOptions options)
		{
			base.ValidateFunctionArgument(function, options);

			if (function.FunctionFlags.HasAnyFlags(EFunctionFlags.Net))
			{
				if (!function.FunctionFlags.HasAnyFlags(EFunctionFlags.NetRequest | EFunctionFlags.NetResponse))
				{
					this.Session.ValidateScriptStructOkForNet(this, this.ScriptStruct);
				}
			}
		}

		/// <inheritdoc/>
		protected override void ValidateMember(UhtStruct structObj, UhtValidationOptions options)
		{
			if (this.PropertyFlags.HasAnyFlags(EPropertyFlags.Net))
			{
				this.Session.ValidateScriptStructOkForNet(this, this.ScriptStruct);
			}
		}

		///<inheritdoc/>
		public override bool ValidateStructPropertyOkForNet(UhtProperty referencingProperty)
		{
			return referencingProperty.Session.ValidateScriptStructOkForNet(referencingProperty, this.ScriptStruct);
		}

		///<inheritdoc/>
		public override bool ContainsEditorOnlyProperties()
		{
			foreach (UhtType Child in this.ScriptStruct.Children)
			{
				if (Child is UhtProperty Property)
				{
					if (Property.PropertyFlags.HasAnyFlags(EPropertyFlags.EditorOnly) || Property.ContainsEditorOnlyProperties())
					{
						return true;
					}
				}
			}
			return false;
		}

		#region Structure default value sanitizers
		[UhtStructDefaultValue(Name = "FVector")]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		[SuppressMessage("Style", "IDE0060:Remove unused parameter", Justification = "Attribute accessed method")]
		private static bool VectorStructDefaultValue(UhtStructProperty property, IUhtTokenReader defaultValueReader, StringBuilder innerDefaultValue)
		{
			const string Format = "{0:F6},{1:F6},{2:F6}";

			defaultValueReader.Require("FVector");
			if (defaultValueReader.TryOptional("::"))
			{
				switch (defaultValueReader.GetIdentifier().Value.ToString())
				{
					case "ZeroVector": return true;
					case "UpVector": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 0, 0, 1); return true;
					case "ForwardVector": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 1, 0, 0); return true;
					case "RightVector": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 0, 1, 0); return true;
					default: return false;
				}
			}
			else
			{
				defaultValueReader.Require("(");
				if (defaultValueReader.TryOptional("ForceInit"))
				{
				}
				else
				{
					double x, y, z;
					x = y = z = defaultValueReader.GetConstDoubleExpression();
					if (defaultValueReader.TryOptional(','))
					{
						y = defaultValueReader.GetConstDoubleExpression();
						defaultValueReader.Require(',');
						z = defaultValueReader.GetConstDoubleExpression();
					}
					innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, x, y, z);
				}
				defaultValueReader.Require(")");
				return true;
			}
		}

		[UhtStructDefaultValue(Name = "FRotator")]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		[SuppressMessage("Style", "IDE0060:Remove unused parameter", Justification = "Attribute accessed method")]
		private static bool RotatorStructDefaultValue(UhtStructProperty property, IUhtTokenReader defaultValueReader, StringBuilder innerDefaultValue)
		{
			defaultValueReader.Require("FRotator");
			if (defaultValueReader.TryOptional("::"))
			{
				switch (defaultValueReader.GetIdentifier().Value.ToString())
				{
					case "ZeroRotator": return true;
					default: return false;
				}
			}
			else
			{
				defaultValueReader.Require("(");
				if (defaultValueReader.TryOptional("ForceInit"))
				{
				}
				else
				{
					double x = defaultValueReader.GetConstDoubleExpression();
					defaultValueReader.Require(',');
					double y = defaultValueReader.GetConstDoubleExpression();
					defaultValueReader.Require(',');
					double z = defaultValueReader.GetConstDoubleExpression();
					innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, "{0:F6},{1:F6},{2:F6}", x, y, z);
				}
				defaultValueReader.Require(")");
				return true;
			}
		}

		[UhtStructDefaultValue(Name = "FVector2D")]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		[SuppressMessage("Style", "IDE0060:Remove unused parameter", Justification = "Attribute accessed method")]
		private static bool Vector2DStructDefaultValue(UhtStructProperty property, IUhtTokenReader defaultValueReader, StringBuilder innerDefaultValue)
		{
			const string Format = "(X={0:F3},Y={1:F3})";

			defaultValueReader.Require("FVector2D");
			if (defaultValueReader.TryOptional("::"))
			{
				switch (defaultValueReader.GetIdentifier().Value.ToString())
				{
					case "ZeroVector": return true;
					case "UnitVector": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 1.0, 1.0); return true;
					default: return false;
				}
			}
			else
			{
				defaultValueReader.Require("(");
				if (defaultValueReader.TryOptional("ForceInit"))
				{
				}
				else
				{
					double x = defaultValueReader.GetConstDoubleExpression();
					defaultValueReader.Require(',');
					double y = defaultValueReader.GetConstDoubleExpression();
					innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, x, y);
				}
				defaultValueReader.Require(")");
				return true;
			}
		}

		[UhtStructDefaultValue(Name = "FLinearColor")]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		[SuppressMessage("Style", "IDE0060:Remove unused parameter", Justification = "Attribute accessed method")]
		private static bool LinearColorStructDefaultValue(UhtStructProperty property, IUhtTokenReader defaultValueReader, StringBuilder innerDefaultValue)
		{
			const string Format = "(R={0:F6},G={1:F6},B={2:F6},A={3:F6})";

			defaultValueReader.Require("FLinearColor");
			if (defaultValueReader.TryOptional("::"))
			{
				switch (defaultValueReader.GetIdentifier().Value.ToString())
				{
					case "White": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 1.0, 1.0, 1.0, 1.0); return true;
					case "Gray": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 0.5, 0.5, 0.5, 1.0); return true;
					case "Black": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 0.0, 0.0, 0.0, 1.0); return true;
					case "Transparent": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 0.0, 0.0, 0.0, 0.0); return true;
					case "Red": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 1.0, 0.0, 0.0, 1.0); return true;
					case "Green": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 0.0, 1.0, 0.0, 1.0); return true;
					case "Blue": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 0.0, 0.0, 1.0, 1.0); return true;
					case "Yellow": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 1.0, 1.0, 0.0, 1.0); return true;
					default: return false;
				}
			}
			else
			{
				defaultValueReader.Require("(");
				if (defaultValueReader.TryOptional("ForceInit"))
				{
				}
				else
				{
					double r = defaultValueReader.GetConstDoubleExpression();
					defaultValueReader.Require(',');
					double g = defaultValueReader.GetConstDoubleExpression();
					defaultValueReader.Require(',');
					double b = defaultValueReader.GetConstDoubleExpression();
					double a = 1.0;
					if (defaultValueReader.TryOptional(','))
					{
						a = defaultValueReader.GetConstDoubleExpression();
					}
					innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, r, g, b, a);
				}
				defaultValueReader.Require(")");
				return true;
			}
		}

		[UhtStructDefaultValue(Name = "FColor")]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		[SuppressMessage("Style", "IDE0060:Remove unused parameter", Justification = "Attribute accessed method")]
		private static bool ColorStructDefaultValue(UhtStructProperty property, IUhtTokenReader defaultValueReader, StringBuilder innerDefaultValue)
		{
			const string Format = "(R={0},G={1},B={2},A={3})";

			defaultValueReader.Require("FColor");
			if (defaultValueReader.TryOptional("::"))
			{
				switch (defaultValueReader.GetIdentifier().Value.ToString())
				{
					case "White": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 255, 255, 255, 255); return true;
					case "Black": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 0, 0, 0, 255); return true;
					case "Red": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 255, 0, 0, 255); return true;
					case "Green": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 0, 255, 0, 255); return true;
					case "Blue": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 0, 0, 255, 255); return true;
					case "Yellow": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 255, 255, 0, 255); return true;
					case "Cyan": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 0, 255, 255, 255); return true;
					case "Magenta": innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, 255, 0, 255, 255); return true;
					default: return false;
				}
			}
			else
			{
				defaultValueReader.Require("(");
				if (defaultValueReader.TryOptional("ForceInit"))
				{
				}
				else
				{
					int r = defaultValueReader.GetConstIntExpression();
					defaultValueReader.Require(',');
					int g = defaultValueReader.GetConstIntExpression();
					defaultValueReader.Require(',');
					int b = defaultValueReader.GetConstIntExpression();
					int a = 255;
					if (defaultValueReader.TryOptional(','))
					{
						a = defaultValueReader.GetConstIntExpression();
					}
					innerDefaultValue.AppendFormat(CultureInfo.InvariantCulture, Format, r, g, b, a);
				}
				defaultValueReader.Require(")");
				return true;
			}
		}

		[UhtStructDefaultValue(Options = UhtStructDefaultValueOptions.Default)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static bool DefaultStructDefaultValue(UhtStructProperty property, IUhtTokenReader defaultValueReader, StringBuilder innerDefaultValue)
		{
			defaultValueReader
				.Require(property.ScriptStruct.SourceName)
				.Require('(')
				.Require(')');
			innerDefaultValue.Append("()");
			return true;
		}
		#endregion
	}
}
