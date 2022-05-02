// Copyright Epic Games, Inc. All Rights Reserved.

using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using EpicGames.Core;
using EpicGames.UHT.Tables;
using EpicGames.UHT.Types;
using EpicGames.UHT.Utils;

namespace EpicGames.UHT.Parsers
{
	/// <summary>
	/// Collection of UCLASS specifiers
	/// </summary>
	[UnrealHeaderTool]
	public static class UhtClassSpecifiers
	{
		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void NoExportSpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.ClassExportFlags |= UhtClassExportFlags.NoExport;
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void IntrinsicSpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.Intrinsic);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void ComponentWrapperClassSpecifier(UhtSpecifierContext specifierContext)
		{
			specifierContext.MetaData.Add(UhtNames.IgnoreCategoryKeywordsInSubclasses, true);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.SingleString)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void WithinSpecifier(UhtSpecifierContext specifierContext, StringView value)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.ClassWithinIdentifier = value.ToString();
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void EditInlineNewSpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.EditInlineNew);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void NotEditInlineNewSpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.RemoveClassFlags(EClassFlags.EditInlineNew);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void PlaceableSpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.RemoveClassFlags(EClassFlags.NotPlaceable);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void NotPlaceableSpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.NotPlaceable);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void DefaultToInstancedSpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.DefaultToInstanced);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void HideDropdownSpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.HideDropDown);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void HiddenSpecifier(UhtSpecifierContext specifierContext)
		{
			// Prevents class from appearing in the editor class browser and edit inline menus.
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.Hidden);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void DependsOnSpecifier(UhtSpecifierContext specifierContext)
		{
			specifierContext.MessageSite.LogError("The dependsOn specifier is deprecated. Please use #include \"ClassHeaderFilename.h\" instead.");
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void MinimalAPISpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.MinimalAPI);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void ConstSpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.Const);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void PerObjectConfigSpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.PerObjectConfig);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void ConfigDoNotCheckDefaultsSpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.ConfigDoNotCheckDefaults);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void AbstractSpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.Abstract);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void DeprecatedSpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.Deprecated | EClassFlags.NotPlaceable);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void TransientSpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.Transient);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void NonTransientSpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.RemoveClassFlags(EClassFlags.Transient);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void OptionalSpecifier(UhtSpecifierContext specifierContext)
		{
			// Optional class
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.Optional);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void CustomConstructorSpecifier(UhtSpecifierContext specifierContext)
		{
			// we will not export a constructor for this class, assuming it is in the CPP block
			UhtClass classObj = (UhtClass)specifierContext.Scope.ScopeType;
			classObj.ClassExportFlags |= UhtClassExportFlags.HasCustomConstructor;
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.SingleString)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void ConfigSpecifier(UhtSpecifierContext specifierContext, StringView value)
		{
			UhtClass classObj = (UhtClass)specifierContext.Scope.ScopeType;
			classObj.Config = value.ToString();
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void DefaultConfigSpecifier(UhtSpecifierContext specifierContext)
		{
			// Save object config only to Default INIs, never to local INIs.
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.DefaultConfig);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void GlobalUserConfigSpecifier(UhtSpecifierContext specifierContext)
		{
			// Save object config only to global user overrides, never to local INIs
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.GlobalUserConfig);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void ProjectUserConfigSpecifier(UhtSpecifierContext specifierContext)
		{
			// Save object config only to project user overrides, never to INIs that are checked in
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.ProjectUserConfig);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.SingleString)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void EditorConfigSpecifier(UhtSpecifierContext specifierContext, StringView value)
		{
			specifierContext.MetaData.Add(UhtNames.EditorConfig, value.ToString());
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.NonEmptyStringList)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void ShowCategoriesSpecifier(UhtSpecifierContext specifierContext, List<StringView> value)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.ShowCategories.AddUniqueRange(value);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.NonEmptyStringList)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void HideCategoriesSpecifier(UhtSpecifierContext specifierContext, List<StringView> value)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.HideCategories.AddUniqueRange(value);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.NonEmptyStringList)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void ShowFunctionsSpecifier(UhtSpecifierContext specifierContext, List<StringView> value)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.ShowFunctions.AddUniqueRange(value);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.NonEmptyStringList)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void HideFunctionsSpecifier(UhtSpecifierContext specifierContext, List<StringView> value)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.HideFunctions.AddUniqueRange(value);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.SingleString)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void SparseClassDataTypesSpecifier(UhtSpecifierContext specifierContext, StringView value)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.SparseClassDataTypes.AddUnique(value.ToString());
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.NonEmptyStringList)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void ClassGroupSpecifier(UhtSpecifierContext specifierContext, List<StringView> value)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			foreach (StringView element in value)
			{
				classObj.ClassGroupNames.Add(element.ToString());
			}
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.NonEmptyStringList)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void AutoExpandCategoriesSpecifier(UhtSpecifierContext specifierContext, List<StringView> value)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AutoExpandCategories.AddUniqueRange(value);
			classObj.AutoCollapseCategories.RemoveSwapRange(value);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.NonEmptyStringList)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void AutoCollapseCategoriesSpecifier(UhtSpecifierContext specifierContext, List<StringView> value)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AutoCollapseCategories.AddUniqueRange(value);
			classObj.AutoExpandCategories.RemoveSwapRange(value);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.NonEmptyStringList)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void PrioritizeCategoriesSpecifier(UhtSpecifierContext specifierContext, List<StringView> value)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.PrioritizeCategories.AddUniqueRange(value);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.NonEmptyStringList)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void DontAutoCollapseCategoriesSpecifier(UhtSpecifierContext specifierContext, List<StringView> value)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AutoCollapseCategories.RemoveSwapRange(value);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void CollapseCategoriesSpecifier(UhtSpecifierContext specifierContext)
		{
			// Class' properties should not be shown categorized in the editor.
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.CollapseCategories);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void DontCollapseCategoriesSpecifier(UhtSpecifierContext specifierContext)
		{
			// Class' properties should be shown categorized in the editor.
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.RemoveClassFlags(EClassFlags.CollapseCategories);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void AdvancedClassDisplaySpecifier(UhtSpecifierContext specifierContext)
		{
			// By default the class properties are shown in advanced sections in UI
			UhtClass classObj = (UhtClass)specifierContext.Scope.ScopeType;
			classObj.MetaData.Add(UhtNames.AdvancedClassDisplay, "true");
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void ConversionRootSpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClass classObj = (UhtClass)specifierContext.Scope.ScopeType;
			classObj.MetaData.Add(UhtNames.IsConversionRoot, "true");
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void NeedsDeferredDependencyLoadingSpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.NeedsDeferredDependencyLoading);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void MatchedSerializersSpecifier(UhtSpecifierContext specifierContext)
		{
			if (!specifierContext.Scope.HeaderParser.HeaderFile.IsNoExportTypes)
			{
				specifierContext.MessageSite.LogError("The 'MatchedSerializers' class specifier is only valid in the NoExportTypes.h file");
			}
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.MatchedSerializers);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.Legacy)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void InterfaceSpecifier(UhtSpecifierContext specifierContext)
		{
			if (!specifierContext.Scope.HeaderParser.HeaderFile.IsNoExportTypes)
			{
				specifierContext.MessageSite.LogError("The 'Interface' class specifier is only valid in the NoExportTypes.h file");
			}
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.AddClassFlags(EClassFlags.Interface);
		}

		[UhtSpecifier(Extends = UhtTableNames.Class, ValueType = UhtSpecifierValueType.None)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		private static void CustomFieldNotifySpecifier(UhtSpecifierContext specifierContext)
		{
			UhtClassParser classObj = (UhtClassParser)specifierContext.Scope.ScopeType;
			classObj.ClassExportFlags |= UhtClassExportFlags.HasCustomFieldNotify;
		}
	}
}
