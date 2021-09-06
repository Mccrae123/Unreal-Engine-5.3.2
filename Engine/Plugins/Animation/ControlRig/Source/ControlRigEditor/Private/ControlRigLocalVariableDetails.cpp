﻿// Copyright Epic Games, Inc. All Rights Reserved.

#include "ControlRigLocalVariableDetails.h"

#include "DetailWidgetRow.h"
#include "DetailLayoutBuilder.h"
#include "IDetailChildrenBuilder.h"
#include "SPinTypeSelector.h"
#include "Graph/ControlRigGraphSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Widgets/Input/STextComboBox.h"
#include "ControlRigBlueprintGeneratedClass.h"
#include "RigVMCore/RigVM.h"

#define LOCTEXT_NAMESPACE "LocalVariableDetails"

void FRigVMLocalVariableDetails::CustomizeHeader(TSharedRef<IPropertyHandle> InStructPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils)
{
	ObjectsBeingCustomized.Reset();
	
	TArray<UObject*> Objects;
	InStructPropertyHandle->GetOuterObjects(Objects);
	for (UObject* Object : Objects)
	{
		UDetailsViewWrapperObject* WrapperObject = CastChecked<UDetailsViewWrapperObject>(Object);
		ObjectsBeingCustomized.Add(WrapperObject);
	}

	if(ObjectsBeingCustomized[0].IsValid())
	{
		VariableDescription = *ObjectsBeingCustomized[0]->GetContent<FRigVMGraphVariableDescription>();
		GraphBeingCustomized = ObjectsBeingCustomized[0]->GetTypedOuter<URigVMGraph>();
		BlueprintBeingCustomized = GraphBeingCustomized->GetTypedOuter<UControlRigBlueprint>();
	}
}

void FRigVMLocalVariableDetails::CustomizeChildren(TSharedRef<IPropertyHandle> InStructPropertyHandle, IDetailChildrenBuilder& StructBuilder, IPropertyTypeCustomizationUtils& StructCustomizationUtils)
{
	StructBuilder.GetParentCategory().GetParentLayout().HideCategory("RigVMGraphVariableDescription");
	IDetailCategoryBuilder& Category = StructBuilder.GetParentCategory().GetParentLayout().EditCategory("Local Variable");
		
	NameHandle = InStructPropertyHandle->GetChildHandle(TEXT("Name"));
	TypeHandle = InStructPropertyHandle->GetChildHandle(TEXT("CPPType"));
	TypeObjectHandle = InStructPropertyHandle->GetChildHandle(TEXT("CPPTypeObject"));
	DefaultValueHandle = InStructPropertyHandle->GetChildHandle(TEXT("DefaultValue"));
	
	const UEdGraphSchema* Schema = GetDefault<UControlRigGraphSchema>();

	const FSlateFontInfo DetailFontInfo = IDetailLayoutBuilder::GetDetailFont();
	Category.AddCustomRow( LOCTEXT("LocalVariableName", "Variable Name") )
	.NameContent()
	[
		SNew(STextBlock)
		.Text(LOCTEXT("LocalVariableName", "Variable Name"))
		.Font(IDetailLayoutBuilder::GetDetailFont())
	]
	.ValueContent()
	.MaxDesiredWidth(250.0f)
	[
		NameHandle->CreatePropertyValueWidget()
	];

	TSharedPtr<IPinTypeSelectorFilter> CustomPinTypeFilter;
	Category.AddCustomRow(LOCTEXT("VariableTypeLabel", "Variable Type"))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("VariableTypeLabel", "Variable Type"))
			.Font(DetailFontInfo)
		]
		.ValueContent()
		.MaxDesiredWidth(980.f)
		[
			SNew(SPinTypeSelector, FGetPinTypeTree::CreateUObject(GetDefault<UEdGraphSchema_K2>(), &UEdGraphSchema_K2::GetVariableTypeTree))
			.TargetPinType(this, &FRigVMLocalVariableDetails::OnGetPinInfo)
			.OnPinTypeChanged(this, &FRigVMLocalVariableDetails::HandlePinInfoChanged)
			.Schema(Schema)
			.TypeTreeFilter(ETypeTreeFilter::None)
			.Font(DetailFontInfo)
			.CustomFilter(CustomPinTypeFilter)
		];


	if (BlueprintBeingCustomized)
	{
		UControlRigBlueprintGeneratedClass* RigClass = BlueprintBeingCustomized->GetControlRigBlueprintGeneratedClass();
		UControlRig* CDO = Cast<UControlRig>(RigClass->GetDefaultObject(true /* create if needed */));
		if (CDO->GetVM() != nullptr)
		{
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
			
#else
			FString SourcePath = FString::Printf(TEXT("LocalVariableDefault::%s|%s::Const"), *GraphBeingCustomized->GetGraphName(), *VariableDescription.Name.ToString());
			URigVMMemoryStorage* LiteralMemory = CDO->GetVM()->GetLiteralMemory();
			FProperty* Property = LiteralMemory->FindPropertyByName(*SourcePath);
			if (Property)
			{
				IDetailCategoryBuilder& DefaultValueCategory = StructBuilder.GetParentCategory().GetParentLayout().EditCategory(TEXT("DefaultValueCategory"), LOCTEXT("DefaultValueCategoryHeading", "Default Value"));
				Property->ClearPropertyFlags(CPF_EditConst);
			
				const FName SanitizedName = FRigVMPropertyDescription::SanitizeName(*SourcePath);
				TArray<UObject*> Objects = {LiteralMemory};
				IDetailPropertyRow* Row = DefaultValueCategory.AddExternalObjectProperty(Objects, SanitizedName);
				Row->DisplayName(FText::FromName(VariableDescription.Name));

				const FSimpleDelegate OnDefaultValueChanged = FSimpleDelegate::CreateLambda([this, Property, LiteralMemory]()
				{
					VariableDescription.DefaultValue = LiteralMemory->GetDataAsString(LiteralMemory->GetPropertyIndex(Property));
					DefaultValueHandle->SetValue(VariableDescription.DefaultValue);
				});

				TSharedPtr<IPropertyHandle> Handle = Row->GetPropertyHandle();
				Handle->SetOnPropertyValueChanged(OnDefaultValueChanged);
				Handle->SetOnChildPropertyValueChanged(OnDefaultValueChanged);
			}
#endif
		}
	}
}

FEdGraphPinType FRigVMLocalVariableDetails::OnGetPinInfo() const
{
	if (!VariableDescription.Name.IsNone())
	{
		return VariableDescription.ToPinType();
	}
	return FEdGraphPinType();
}

void FRigVMLocalVariableDetails::HandlePinInfoChanged(const FEdGraphPinType& PinType)
{
	VariableDescription.ChangeType(PinType);
	BlueprintBeingCustomized->IncrementVMRecompileBracket();
	TypeHandle->SetValue(VariableDescription.CPPType);
	TypeObjectHandle->SetValue(VariableDescription.CPPTypeObject);	
	BlueprintBeingCustomized->DecrementVMRecompileBracket();
}

ECheckBoxState FRigVMLocalVariableDetails::HandleBoolDefaultValueIsChecked() const
{
	return VariableDescription.DefaultValue == "1" ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void FRigVMLocalVariableDetails::OnBoolDefaultValueChanged(ECheckBoxState InCheckBoxState)
{
	VariableDescription.DefaultValue = InCheckBoxState == ECheckBoxState::Checked ? "1" : "0";
	DefaultValueHandle->SetValue(VariableDescription.DefaultValue);
}

#undef LOCTEXT_NAMESPACE
