// Copyright Epic Games, Inc. All Rights Reserved.

#include "OptimusEditorModule.h"

#include "AssetToolsModule.h"
#include "EdGraphUtilities.h"
#include "IAssetTools.h"
#include "OptimusBindingTypes.h"
#include "OptimusDataType.h"
#include "OptimusDeformerAssetActions.h"
#include "OptimusDetailsCustomization.h"
#include "OptimusEditor.h"
#include "OptimusEditorClipboard.h"
#include "OptimusEditorCommands.h"
#include "OptimusEditorGraphCommands.h"
#include "OptimusEditorGraphNodeFactory.h"
#include "OptimusEditorGraphPinFactory.h"
#include "OptimusEditorStyle.h"
#include "OptimusResourceDescription.h"
#include "OptimusShaderText.h"
#include "OptimusSource.h"
#include "OptimusSourceAssetActions.h"
#include "OptimusValueContainer.h"
#include "PropertyEditorModule.h"
#include "Widgets/SOptimusEditorGraphExplorer.h"
#include "Widgets/SOptimusShaderTextDocumentTextBox.h"

#define LOCTEXT_NAMESPACE "OptimusEditorModule"

DEFINE_LOG_CATEGORY(LogOptimusEditor);

FOptimusEditorModule::FOptimusEditorModule() :
	Clipboard(MakeShared<FOptimusEditorClipboard>())
{
}

void FOptimusEditorModule::StartupModule()
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	
	TSharedRef<IAssetTypeActions> OptimusDeformerAssetAction = MakeShared<FOptimusDeformerAssetActions>();
	AssetTools.RegisterAssetTypeActions(OptimusDeformerAssetAction);
	RegisteredAssetTypeActions.Add(OptimusDeformerAssetAction);

	TSharedRef<IAssetTypeActions> OptimusSourceAssetAction = MakeShared<FOptimusSourceAssetActions>();
	AssetTools.RegisterAssetTypeActions(OptimusSourceAssetAction);
	RegisteredAssetTypeActions.Add(OptimusSourceAssetAction);

	FOptimusEditorCommands::Register();
	FOptimusEditorGraphCommands::Register();
	FOptimusEditorGraphExplorerCommands::Register();
	FOptimusShaderTextEditorDocumentTextBoxCommands::Register();
	FOptimusEditorStyle::Register();

	GraphNodeFactory = MakeShared<FOptimusEditorGraphNodeFactory>();
	FEdGraphUtilities::RegisterVisualNodeFactory(GraphNodeFactory);

	GraphPinFactory = MakeShared<FOptimusEditorGraphPinFactory>();
	FEdGraphUtilities::RegisterVisualPinFactory(GraphPinFactory);

	RegisterPropertyCustomizations();
}

void FOptimusEditorModule::ShutdownModule()
{
	UnregisterPropertyCustomizations();

	FEdGraphUtilities::UnregisterVisualPinFactory(GraphPinFactory);
	FEdGraphUtilities::UnregisterVisualNodeFactory(GraphNodeFactory);

	FOptimusEditorStyle::Unregister();
	FOptimusEditorGraphExplorerCommands::Unregister();
	FOptimusEditorGraphCommands::Unregister();
	FOptimusEditorCommands::Unregister();
	
	if (FAssetToolsModule* AssetToolsModule = FModuleManager::GetModulePtr<FAssetToolsModule>("AssetTools"))
	{
		IAssetTools& AssetTools = AssetToolsModule->Get();

		for (const TSharedRef<IAssetTypeActions>& Action : RegisteredAssetTypeActions)
		{
			AssetTools.UnregisterAssetTypeActions(Action);
		}
	}
}

TSharedRef<IOptimusEditor> FOptimusEditorModule::CreateEditor(const EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost, UOptimusDeformer* DeformerObject)
{
	TSharedRef<FOptimusEditor> OptimusEditor = MakeShared<FOptimusEditor>();
	OptimusEditor->Construct(Mode, InitToolkitHost, DeformerObject);
	return OptimusEditor;
}

FOptimusEditorClipboard& FOptimusEditorModule::GetClipboard() const
{
	return Clipboard.Get();
}

void FOptimusEditorModule::RegisterPropertyCustomizations()
{
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	auto RegisterPropertyCustomization = [&](FName InStructName, auto InCustomizationFactory)
	{
		PropertyModule.RegisterCustomPropertyTypeLayout(
			InStructName, 
			FOnGetPropertyTypeCustomizationInstance::CreateStatic(InCustomizationFactory)
			);
		CustomizedProperties.Add(InStructName);
	};

	RegisterPropertyCustomization(FOptimusDataTypeRef::StaticStruct()->GetFName(), &FOptimusDataTypeRefCustomization::MakeInstance);
	RegisterPropertyCustomization(FOptimusDataDomain::StaticStruct()->GetFName(), &FOptimusDataDomainCustomization::MakeInstance);
	RegisterPropertyCustomization(FOptimusMultiLevelDataDomain::StaticStruct()->GetFName(), &FOptimusMultiLevelDataDomainCustomization::MakeInstance);
	RegisterPropertyCustomization(FOptimusShaderText::StaticStruct()->GetFName(), &FOptimusShaderTextCustomization::MakeInstance);
	RegisterPropertyCustomization(FOptimusParameterBinding::StaticStruct()->GetFName(), &FOptimusParameterBindingCustomization::MakeInstance);
	RegisterPropertyCustomization(FOptimusParameterBindingArray::StaticStruct()->GetFName(), &FOptimusParameterBindingArrayCustomization::MakeInstance);
	RegisterPropertyCustomization(UOptimusValueContainer::StaticClass()->GetFName(), &FOptimusValueContainerCustomization::MakeInstance);

	auto RegisterDetailCustomization = [&](FName InStructName, auto InCustomizationFactory)
	{
		PropertyModule.RegisterCustomClassLayout(
			InStructName,
			FOnGetDetailCustomizationInstance::CreateStatic(InCustomizationFactory)
		);
		CustomizedClasses.Add(InStructName);
	};

	RegisterDetailCustomization(UOptimusSource::StaticClass()->GetFName(), &FOptimusSourceDetailsCustomization::MakeInstance);
}

void FOptimusEditorModule::UnregisterPropertyCustomizations()
{
	if (FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor"))
	{
		for (const FName& PropertyName: CustomizedProperties)
		{
			PropertyModule->UnregisterCustomPropertyTypeLayout(PropertyName);
		}
		for (const FName& ClassName : CustomizedClasses)
		{
			PropertyModule->UnregisterCustomClassLayout(ClassName);
		}
	}
}

IMPLEMENT_MODULE(FOptimusEditorModule, OptimusEditor)

#undef LOCTEXT_NAMESPACE
