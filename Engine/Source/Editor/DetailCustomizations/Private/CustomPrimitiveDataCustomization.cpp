// Copyright Epic Games, Inc. All Rights Reserved.

#include "CustomPrimitiveDataCustomization.h"

#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Colors/SColorPicker.h"
#include "Widgets/Colors/SColorBlock.h"
#include "SlateOptMacros.h"

#include "EditorStyleSet.h"

#include "IDetailGroup.h"
#include "IDetailPropertyRow.h"
#include "IPropertyTypeCustomization.h"
#include "IPropertyUtilities.h"
#include "PropertyCustomizationHelpers.h"

#include "Materials/MaterialInstance.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "IMaterialEditor.h"

#define LOCTEXT_NAMESPACE "CustomPrimitiveDataCustomization"

TSharedRef<IPropertyTypeCustomization> FCustomPrimitiveDataCustomization::MakeInstance()
{
	return MakeShareable(new FCustomPrimitiveDataCustomization);
}

FCustomPrimitiveDataCustomization::~FCustomPrimitiveDataCustomization()
{
	Cleanup();
}

void FCustomPrimitiveDataCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	TSharedPtr<IPropertyHandle> DataProperty = PropertyHandle->GetChildHandle("Data");

	// Move the data array to be the outer, so we don't have to expand the struct
	HeaderRow.NameContent()
	[
		PropertyHandle->CreatePropertyNameWidget()
	]
	.ValueContent()
	[
		DataProperty->CreatePropertyValueWidget()
	];
}

void FCustomPrimitiveDataCustomization::CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle, IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	Cleanup();

	PropertyUtils = CustomizationUtils.GetPropertyUtilities();

	TSharedPtr<IPropertyHandle> DataProperty = PropertyHandle->GetChildHandle("Data");
	DataArrayHandle = DataProperty->AsArray();

	int32 NumSelectedComponents = 0;
	int32 MaxPrimitiveDataIndex = INDEX_NONE;

	ForEachSelectedComponent([&](UPrimitiveComponent* Component)
	{
		PopulateParameterData(Component, MaxPrimitiveDataIndex);
		NumSelectedComponents++;
		ComponentsToWatch.Add(Component);
	});

	uint32 NumElements;
	DataArrayHandle->GetNumElements(NumElements);

	FSimpleDelegate OnElemsChanged = FSimpleDelegate::CreateRaw(this, &FCustomPrimitiveDataCustomization::OnUpdated);
	PropertyHandle->SetOnPropertyValueChanged(OnElemsChanged);
	DataProperty->SetOnPropertyValueChanged(OnElemsChanged);
	DataArrayHandle->SetOnNumElementsChanged(OnElemsChanged);

	FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(this, &FCustomPrimitiveDataCustomization::OnObjectPropertyChanged);
	UMaterial::OnMaterialCompilationFinished().AddRaw(this, &FCustomPrimitiveDataCustomization::OnMaterialCompiled);

	const int32 NumPrimitiveIndices = FMath::Max(MaxPrimitiveDataIndex + 1, (int32)NumElements);

	if (NumPrimitiveIndices == 0)
	{
		return;
	}

	FString ArrayString;
	const bool bDataEditable = DataProperty.IsValid() && DataProperty->IsEditable() && DataProperty->GetValueAsDisplayString(ArrayString) == FPropertyAccess::Success;

	uint8 VectorGroupPrimIdx = 0;
	IDetailGroup* VectorGroup = NULL;

	for (uint8 PrimIdx = 0; PrimIdx < NumPrimitiveIndices; ++PrimIdx)
	{
		TSharedPtr<IPropertyHandle> ElementHandle = PrimIdx < NumElements ? TSharedPtr<IPropertyHandle>(DataArrayHandle->GetElement(PrimIdx)) : NULL;

		if (VectorGroup && (PrimIdx - VectorGroupPrimIdx) > 3)
		{
			// We're no longer in a vector group
			VectorGroup = NULL;
		}

		// Always prioritize the first vector found, and only if it's the first element of the vector
		if (VectorGroup == NULL && VectorParameterData.Contains(PrimIdx))
		{
			bool bContainsFirstElementOfVector = false;
			for (const FParameterData& ParameterData : VectorParameterData[PrimIdx])
			{
				if (ParameterData.IndexOffset == 0)
				{
					bContainsFirstElementOfVector = true;
					break;
				}
			}

			if (bContainsFirstElementOfVector)
			{
				// Create a collapsing group that contains our color picker, so we can quickly assign colors to our vector
				VectorGroupPrimIdx = PrimIdx;
				VectorGroup = CreateVectorGroup(ChildBuilder, PrimIdx, bDataEditable, NumElements);
			}
		}

		if (ScalarParameterData.Contains(PrimIdx) || VectorParameterData.Contains(PrimIdx))
		{
			CreateParameterRow(ChildBuilder, PrimIdx, ElementHandle, NumSelectedComponents, bDataEditable, VectorGroup, CustomizationUtils);
		}
		else
		{
			// We've encountered a gap in defined custom primitive data, mark it undefined
			TSharedRef<SWidget> NameWidget = GetUndefinedParameterWidget(PrimIdx, CustomizationUtils);

			if (ElementHandle.IsValid())
			{
				ChildBuilder.AddProperty(ElementHandle.ToSharedRef())
				.CustomWidget()
				.NameContent()
				[
					NameWidget
				]
				.ValueContent()
				[
					ElementHandle->CreatePropertyValueWidget(false)
				]
				.IsEnabled(bDataEditable);;
			}
			else
			{
				ChildBuilder.AddCustomRow(FText::AsNumber(PrimIdx))
				.NameContent()
				[
					NameWidget
				]
				.IsEnabled(bDataEditable);
			}
		}
	}
}

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION
IDetailGroup* FCustomPrimitiveDataCustomization::CreateVectorGroup(IDetailChildrenBuilder& ChildBuilder, uint8 PrimIdx, bool bDataEditable, int32 NumElements)
{
	IDetailGroup* VectorGroup = &ChildBuilder.AddGroup(VectorParameterData[PrimIdx][0].Info.Name, FText::FromName(VectorParameterData[PrimIdx][0].Info.Name));

	TSharedPtr<SColorBlock> ColorBlock;
	TSharedPtr<SVerticalBox> VectorGroupNameBox = SNew(SVerticalBox);

	// Use this to make sure we don't make duplicate parameters for the group header
	TSet<FGuid> AddedParametersForThisGroup;

	for (const FParameterData& ParameterData : VectorParameterData[PrimIdx])
	{
		if (AddedParametersForThisGroup.Contains(ParameterData.ExpressionID))
		{
			continue;
		}

		AddedParametersForThisGroup.Add(ParameterData.ExpressionID);

		VectorGroupNameBox->AddSlot()
		.Padding(2.f)
		[
			CreateHyperlink(FText::FromName(ParameterData.Info.Name), ParameterData.Material, ParameterData.ExpressionID)
		];
	}

	TSharedPtr<SWidget> VectorGroupNameWidget;
	if (VectorGroupNameBox->NumSlots() > 1)
	{
		// We have multiple overlapping parameter names, make sure to put a border around it to contain it
		// TODO: Maybe a UI designer needs to take a look at this?
		VectorGroupNameWidget = SNew(SBorder)
		.BorderImage(FEditorStyle::Get().GetBrush("FilledBorder"))
		.Padding(2.f)
		[
			VectorGroupNameBox.ToSharedRef()
		];
	}
	else
	{
		VectorGroupNameWidget = VectorGroupNameBox;
	}

	VectorGroup->HeaderRow()
	.NameContent()
	[
		VectorGroupNameWidget.ToSharedRef()
	]
	.ValueContent()
	[
		SNew(SHorizontalBox)
		.IsEnabled(bDataEditable)
		+ SHorizontalBox::Slot()
		.VAlign(VAlign_Center)
		.Padding(0.f, 2.f)
		[
			SAssignNew(ColorBlock, SColorBlock)
			.Color(this, &FCustomPrimitiveDataCustomization::GetVectorColor, PrimIdx)
			.ShowBackgroundForAlpha(true)
			.AlphaDisplayMode(EColorBlockAlphaDisplayMode::Separate)
			.OnMouseButtonDown(this, &FCustomPrimitiveDataCustomization::OnMouseButtonDownColorBlock, PrimIdx)
			.Size(FVector2D(35.0f, 12.0f))
			.Visibility(PrimIdx < NumElements ? EVisibility::Visible : EVisibility::Collapsed)
		]
		+ SHorizontalBox::Slot()
		.Padding(2.0f)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.AutoWidth()
		[
			PropertyCustomizationHelpers::MakeAddButton(FSimpleDelegate::CreateSP(this, &FCustomPrimitiveDataCustomization::OnAddedDesiredPrimitiveData, (uint8)(PrimIdx + 3)), FText(), (int32)NumElements < PrimIdx + 4)
		]
		+ SHorizontalBox::Slot()
		.Padding(2.0f)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.AutoWidth()
		[
			PropertyCustomizationHelpers::MakeEmptyButton(FSimpleDelegate::CreateSP(this, &FCustomPrimitiveDataCustomization::OnRemovedPrimitiveData, PrimIdx), LOCTEXT("RemoveVector", "Removes this vector (and anything after)"), (int32)PrimIdx < NumElements)
		]
		+ SHorizontalBox::Slot()
		.Padding(2.0f)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.AutoWidth()
		[
			PropertyCustomizationHelpers::MakeResetButton(FSimpleDelegate::CreateSP(this, &FCustomPrimitiveDataCustomization::SetDefaultVectorValue, PrimIdx), FText(), (int32)PrimIdx < NumElements)
		]
	];

	ColorBlocks.Add(PrimIdx, ColorBlock);
	
	return VectorGroup;
}
END_SLATE_FUNCTION_BUILD_OPTIMIZATION

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION
void FCustomPrimitiveDataCustomization::CreateParameterRow(IDetailChildrenBuilder& ChildBuilder, uint8 PrimIdx, TSharedPtr<IPropertyHandle> ElementHandle, int32 NumSelectedComponents, bool bDataEditable, IDetailGroup* VectorGroup, IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// Use this to make sure we don't make duplicate parameters in each row
	TSet<FGuid> AddedParametersForThisRow;

	TArray<FText> SearchText;
	TSharedPtr<SVerticalBox> VerticalBox = SNew(SVerticalBox);

	TSet<TWeakObjectPtr<UPrimitiveComponent>> Components;

	if (VectorParameterData.Contains(PrimIdx))
	{
		for (const FParameterData& ParameterData : VectorParameterData[PrimIdx])
		{
			Components.Add(ParameterData.Component);

			if (AddedParametersForThisRow.Contains(ParameterData.ExpressionID))
			{
				continue;
			}

			AddedParametersForThisRow.Add(ParameterData.ExpressionID);

			FMaterialParameterMetadata ParameterMetadata;
			ParameterData.Material->GetParameterDefaultValue(EMaterialParameterType::Vector, ParameterData.Info, ParameterMetadata);

			FText ChannelName;
			switch (ParameterData.IndexOffset)
			{
			case 0:
				ChannelName = ParameterMetadata.ChannelNames.R.IsEmpty() ? LOCTEXT("DefaultVectorChannelRed", "R") : ParameterMetadata.ChannelNames.R;
				break;
			case 1:
				ChannelName = ParameterMetadata.ChannelNames.G.IsEmpty() ? LOCTEXT("DefaultVectorChannelGreen", "G") : ParameterMetadata.ChannelNames.G;
				break;
			case 2:
				ChannelName = ParameterMetadata.ChannelNames.B.IsEmpty() ? LOCTEXT("DefaultVectorChannelBlue", "B") : ParameterMetadata.ChannelNames.B;
				break;
			case 3:
				ChannelName = ParameterMetadata.ChannelNames.A.IsEmpty() ? LOCTEXT("DefaultVectorChannelAlpha", "A") : ParameterMetadata.ChannelNames.A;
				break;
			default:
				checkNoEntry();
				break;
			}

			const FText ParameterName = FText::Format(
				LOCTEXT("VectorParameterName", "{0}.{1}"),
				FText::FromName(ParameterData.Info.Name),
				ChannelName
			);

			VerticalBox->AddSlot()
			.Padding(2.f)
			[
				CreateHyperlink(ParameterName, ParameterData.Material, ParameterData.ExpressionID)
			];

			SearchText.Add(ParameterName);
		}
	}

	if (ScalarParameterData.Contains(PrimIdx))
	{
		for (const FParameterData& ParameterData : ScalarParameterData[PrimIdx])
		{
			Components.Add(ParameterData.Component);

			if (AddedParametersForThisRow.Contains(ParameterData.ExpressionID))
			{
				continue;
			}

			AddedParametersForThisRow.Add(ParameterData.ExpressionID);

			const FText ParameterName = FText::FromName(ParameterData.Info.Name);

			VerticalBox->AddSlot()
			.Padding(2.f)
			[
				CreateHyperlink(ParameterName, ParameterData.Material, ParameterData.ExpressionID)
			];

			SearchText.Add(ParameterName);
		}
	}

	if (Components.Num() != NumSelectedComponents)
	{
		// Some components aren't defining parameters at this index, add the undefined parameter widget in case this was user error
		VerticalBox->AddSlot()
		.Padding(2.f)
		[
			GetUndefinedParameterWidget(PrimIdx, CustomizationUtils)
		];
	}

	TSharedPtr<SWidget> NameWidget;
	if (VerticalBox->NumSlots() > 1)
	{
		// We have multiple overlapping parameter names, make sure to put a border around it to contain it
		// TODO: Maybe a UI designer needs to take a look at this?
		NameWidget = SNew(SBorder)
		.BorderImage(FEditorStyle::Get().GetBrush("FilledBorder"))
		.Padding(2.f)
		[
			VerticalBox.ToSharedRef()
		];
	}
	else
	{
		NameWidget = VerticalBox;
	}

	if (ElementHandle.IsValid())
	{
		// We already have data for this row, be sure to use it
		TSharedRef<IPropertyHandle> ElementHandleRef = ElementHandle.ToSharedRef();
		IDetailPropertyRow& Row = VectorGroup ? VectorGroup->AddPropertyRow(ElementHandleRef) : ChildBuilder.AddProperty(ElementHandleRef);

		TSharedRef<SWidget> ValueWidget = ElementHandle->CreatePropertyValueWidget(false);
		ValueWidget->SetEnabled(bDataEditable);
		ElementHandleRef->SetOnPropertyResetToDefault(FSimpleDelegate::CreateSP(this, &FCustomPrimitiveDataCustomization::SetDefaultValue, ElementHandle, PrimIdx));

		Row.CustomWidget()
		.NameContent()
		[
			NameWidget.ToSharedRef()
		]
		.ValueContent()
		[
			ValueWidget
		];
	}
	else
	{
		// We don't have data for this row, add an empty row that contains the parameter names and the ability to add data up until this point
		FDetailWidgetRow& Row = VectorGroup ? VectorGroup->AddWidgetRow() : ChildBuilder.AddCustomRow(FText::Join(LOCTEXT("SearchTextDelimiter", " "), SearchText));

		Row.NameContent()
		[
			NameWidget.ToSharedRef()
		]
		.ValueContent()
		[
			SNew(SHorizontalBox)
			.IsEnabled(bDataEditable)
			+ SHorizontalBox::Slot()
			.Padding(2.0f)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.AutoWidth()
			[
				PropertyCustomizationHelpers::MakeAddButton(FSimpleDelegate::CreateSP(this, &FCustomPrimitiveDataCustomization::OnAddedDesiredPrimitiveData, PrimIdx))
			]
		];
	}
}
END_SLATE_FUNCTION_BUILD_OPTIMIZATION

template<typename Predicate>
void FCustomPrimitiveDataCustomization::ForEachSelectedComponent(Predicate Pred)
{
	if (PropertyUtils.IsValid())
	{
		for (TWeakObjectPtr<UObject> Object : PropertyUtils->GetSelectedObjects())
		{
			if (UPrimitiveComponent* Component = Cast<UPrimitiveComponent>(Object.Get()))
			{
				Pred(Component);
			}
			else if (AActor* Actor = Cast<AActor>(Object.Get()))
			{
				for (UActorComponent* ActorComponent : Actor->GetComponents())
				{
					if (UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(ActorComponent))
					{
						Pred(PrimitiveComponent);
					}
				}
			}
		}
	}
}

void FCustomPrimitiveDataCustomization::Cleanup()
{
	FCoreUObjectDelegates::OnObjectPropertyChanged.RemoveAll(this);
	UMaterial::OnMaterialCompilationFinished().RemoveAll(this);

	PropertyUtils = NULL;
	DataArrayHandle = NULL;

	ComponentsToWatch.Empty();
	MaterialsToWatch.Empty();
	VectorParameterData.Empty();
	ScalarParameterData.Empty();
	ColorBlocks.Empty();
}

void FCustomPrimitiveDataCustomization::PopulateParameterData(UPrimitiveComponent* PrimitiveComponent, int32& MaxPrimitiveDataIndex)
{
	const int32 NumMaterials = PrimitiveComponent->GetNumMaterials();

	for (int32 i = 0; i < NumMaterials; ++i)
	{
		UMaterialInterface* MaterialInterface = PrimitiveComponent->GetMaterial(i);
		UMaterial* Material = MaterialInterface ? MaterialInterface->GetBaseMaterial() : NULL;

		if (Material == NULL)
		{
			continue;
		}

		MaterialsToWatch.Add(Material);

		TMap<FMaterialParameterInfo, FMaterialParameterMetadata> Parameters;

		MaterialInterface->GetAllParametersOfType(EMaterialParameterType::Vector, Parameters);

		for (const TPair<FMaterialParameterInfo, FMaterialParameterMetadata>& Parameter : Parameters)
		{
			const FMaterialParameterInfo& Info = Parameter.Key;
			const FMaterialParameterMetadata& ParameterMetadata = Parameter.Value;
			
			if (ParameterMetadata.PrimitiveDataIndex > INDEX_NONE)
			{
				// Add each element individually, so that we can overlap vector parameter names
				VectorParameterData.FindOrAdd(ParameterMetadata.PrimitiveDataIndex + 0).Add(
				{
					PrimitiveComponent,
					MaterialInterface,
					Info,
					ParameterMetadata.ExpressionGuid,
					0
				});
				VectorParameterData.FindOrAdd(ParameterMetadata.PrimitiveDataIndex + 1).Add(
				{
					PrimitiveComponent,
					MaterialInterface,
					Info,
					ParameterMetadata.ExpressionGuid,
					1
				});
				VectorParameterData.FindOrAdd(ParameterMetadata.PrimitiveDataIndex + 2).Add(
				{
					PrimitiveComponent,
					MaterialInterface,
					Info,
					ParameterMetadata.ExpressionGuid,
					2
				});
				VectorParameterData.FindOrAdd(ParameterMetadata.PrimitiveDataIndex + 3).Add(
				{
					PrimitiveComponent,
					MaterialInterface,
					Info,
					ParameterMetadata.ExpressionGuid,
					3
				});
				MaxPrimitiveDataIndex = FMath::Max(MaxPrimitiveDataIndex, (int32)(ParameterMetadata.PrimitiveDataIndex + 3));
			}
		}

		Parameters.Reset();

		MaterialInterface->GetAllParametersOfType(EMaterialParameterType::Scalar, Parameters);

		for (const TPair<FMaterialParameterInfo, FMaterialParameterMetadata>& Parameter : Parameters)
		{
			const FMaterialParameterInfo& Info = Parameter.Key;
			const FMaterialParameterMetadata& ParameterMetadata = Parameter.Value;

			if (ParameterMetadata.PrimitiveDataIndex > INDEX_NONE)
			{
				ScalarParameterData.FindOrAdd(ParameterMetadata.PrimitiveDataIndex).Add(
				{
					PrimitiveComponent,
					MaterialInterface,
					Info,
					ParameterMetadata.ExpressionGuid,
					0
				});
				MaxPrimitiveDataIndex = FMath::Max(MaxPrimitiveDataIndex, (int32)ParameterMetadata.PrimitiveDataIndex);
			}
		}
	}
}

void FCustomPrimitiveDataCustomization::OnUpdated()
{
	if (PropertyUtils.IsValid())
	{
		PropertyUtils->ForceRefresh();
	}
}
	
void FCustomPrimitiveDataCustomization::OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent)
{
	if(PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive )
	{
		const bool bIsCustomPrimitiveDataProperty = PropertyChangedEvent.GetPropertyName() == "CustomPrimitiveData"
			|| (PropertyChangedEvent.MemberProperty != NULL && PropertyChangedEvent.MemberProperty->GetFName() == "CustomPrimitiveData");
		if (ComponentsToWatch.Contains(Object) && !bIsCustomPrimitiveDataProperty)
		{
			OnUpdated();
		}
	}
}

void FCustomPrimitiveDataCustomization::OnMaterialCompiled(UMaterialInterface* Material)
{
	// NOTE: We use a soft object ptr here as the old material object will be stale on compile
	if (MaterialsToWatch.Contains(Material))
	{
		OnUpdated();
	}
}

void FCustomPrimitiveDataCustomization::OnNavigate(TWeakObjectPtr<UMaterialInterface> MaterialInterface, FGuid ExpressionID)
{
	UMaterial* Material = MaterialInterface.IsValid() ? MaterialInterface->GetMaterial() : NULL;
	
	if (UMaterialExpression* Expression = Material ? Material->FindExpressionByGUID<UMaterialExpression>(ExpressionID) : NULL)
	{
		// FindExpression is recursive, so we need to ensure we open the correct asset
		UObject* Asset = Expression->GetOutermostObject();
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();

		IAssetEditorInstance* AssetEditorInstance = AssetEditorSubsystem->OpenEditorForAsset(Asset) ? AssetEditorSubsystem->FindEditorForAsset(Asset, true) : NULL;
		if (AssetEditorInstance != NULL)
		{
			if (AssetEditorInstance->GetEditorName() == "MaterialEditor")
			{
				((IMaterialEditor*)AssetEditorInstance)->JumpToExpression(Expression);
			}
			else
			{
				ensureMsgf(false, TEXT("Missing navigate to expression for editor '%s'"), *AssetEditorInstance->GetEditorName().ToString());
			}
		}
	}
}

void FCustomPrimitiveDataCustomization::OnAddedDesiredPrimitiveData(uint8 PrimIdx)
{
	uint32 NumElements;
	if (DataArrayHandle.IsValid() && DataArrayHandle->GetNumElements(NumElements) == FPropertyAccess::Success)
	{
		GEditor->BeginTransaction(LOCTEXT("OnAddedDesiredPrimitiveData", "Added Items"));

		for (int32 i = NumElements; i <= PrimIdx; ++i)
		{
			DataArrayHandle->AddItem();

			SetDefaultValue(DataArrayHandle->GetElement(i), i);
		}

		GEditor->EndTransaction();
	}
}

void FCustomPrimitiveDataCustomization::OnRemovedPrimitiveData(uint8 PrimIdx)
{
	uint32 NumElements;
	if (DataArrayHandle.IsValid() && DataArrayHandle->GetNumElements(NumElements) == FPropertyAccess::Success)
	{
		GEditor->BeginTransaction(LOCTEXT("OnRemovedPrimitiveData", "Removed Items"));

		for (int32 i = NumElements - 1; i >= PrimIdx; --i)
		{
			DataArrayHandle->DeleteItem(i);
		}

		GEditor->EndTransaction();
	}
}

FLinearColor FCustomPrimitiveDataCustomization::GetVectorColor(uint8 PrimIdx) const
{
	FVector4f Color(ForceInitToZero);

	uint32 NumElems;
	if (DataArrayHandle.IsValid() && DataArrayHandle->GetNumElements(NumElems) == FPropertyAccess::Success)
	{
		const int32 MaxElems = FMath::Min((int32)NumElems, PrimIdx + 4);

		for (int32 i = PrimIdx; i < MaxElems; ++i)
		{
			DataArrayHandle->GetElement(i)->GetValue(Color[i - PrimIdx]);
		}
	}

	return FLinearColor(Color);
}

void FCustomPrimitiveDataCustomization::SetVectorColor(FLinearColor NewColor, uint8 PrimIdx)
{
	FVector4f Color(NewColor);

	uint32 NumElems;
	if (DataArrayHandle.IsValid() && DataArrayHandle->GetNumElements(NumElems) == FPropertyAccess::Success)
	{
		const int32 MaxElems = FMath::Min((int32)NumElems, PrimIdx + 4);

		for (int32 i = PrimIdx; i < MaxElems; ++i)
		{
			DataArrayHandle->GetElement(i)->SetValue(Color[i - PrimIdx]);
		}
	}
}

void FCustomPrimitiveDataCustomization::SetDefaultValue(TSharedPtr<IPropertyHandle> Handle, uint8 PrimIdx)
{
	if (Handle.IsValid())
	{
		TSet<TWeakObjectPtr<UPrimitiveComponent>> ChangedComponents;

		// Prioritize vector data since we have a color picker
		if (VectorParameterData.Contains(PrimIdx))
		{
			for (const FParameterData& ParameterData : VectorParameterData[PrimIdx])
			{
				if (ParameterData.Component.IsValid() && !ChangedComponents.Contains(ParameterData.Component))
				{
					FLinearColor Color(ForceInitToZero);
					if (!ParameterData.Material.IsValid() || ParameterData.Material->GetVectorParameterDefaultValue(ParameterData.Info, Color))
					{
						float* ColorPtr = reinterpret_cast<float*>(&Color);
						ParameterData.Component->SetDefaultCustomPrimitiveDataFloat(PrimIdx, ColorPtr[ParameterData.IndexOffset]);

						FPropertyChangedEvent PropertyChangedEvent(Handle->GetParentHandle()->GetParentHandle()->GetProperty());
						PropertyChangedEvent.SetActiveMemberProperty(Handle->GetParentHandle()->GetProperty());
						ParameterData.Component->PostEditChangeProperty(PropertyChangedEvent);

						ChangedComponents.Add(ParameterData.Component);
					}
				}
			}
		}

		if (ScalarParameterData.Contains(PrimIdx))
		{
			for (const FParameterData& ParameterData : VectorParameterData[PrimIdx])
			{
				if (ParameterData.Component.IsValid() && !ChangedComponents.Contains(ParameterData.Component))
				{
					float Value = 0.f;
					if (!ParameterData.Material.IsValid() || ParameterData.Material->GetScalarParameterDefaultValue(ParameterData.Info, Value))
					{
						ParameterData.Component->SetDefaultCustomPrimitiveDataFloat(PrimIdx, Value);

						FPropertyChangedEvent PropertyChangedEvent(Handle->GetParentHandle()->GetParentHandle()->GetProperty());
						PropertyChangedEvent.SetActiveMemberProperty(Handle->GetParentHandle()->GetProperty());
						ParameterData.Component->PostEditChangeProperty(PropertyChangedEvent);

						ChangedComponents.Add(ParameterData.Component);
					}
				}
			}
		}
	}
}

void FCustomPrimitiveDataCustomization::SetDefaultVectorValue(uint8 PrimIdx)
{
	uint32 NumElems;
	if (DataArrayHandle.IsValid() && DataArrayHandle->GetNumElements(NumElems) == FPropertyAccess::Success)
	{
		GEditor->BeginTransaction(LOCTEXT("SetDefaultVectorValue", "Reset Vector To Default"));

		const int32 MaxElems = FMath::Min((int32)NumElems, PrimIdx + 4);
		for (int32 i = PrimIdx; i < MaxElems; ++i)
		{
			DataArrayHandle->GetElement(i)->ResetToDefault();
		}

		GEditor->EndTransaction();
	}
}

FReply FCustomPrimitiveDataCustomization::OnMouseButtonDownColorBlock(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent, uint8 PrimIdx)
{
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}

	GEditor->BeginTransaction(FText::Format(LOCTEXT("SetVectorColor", "Edit Primitive Data Vector: {0}"), FText::AsNumber(PrimIdx)));

	FColorPickerArgs PickerArgs;
	PickerArgs.bUseAlpha = true;
	PickerArgs.InitialColorOverride = GetVectorColor(PrimIdx);
	PickerArgs.ParentWidget = ColorBlocks[PrimIdx];
	PickerArgs.DisplayGamma = TAttribute<float>::Create(TAttribute<float>::FGetter::CreateUObject(GEngine, &UEngine::GetDisplayGamma));
	PickerArgs.OnColorCommitted = FOnLinearColorValueChanged::CreateSP(this, &FCustomPrimitiveDataCustomization::SetVectorColor, PrimIdx);
	PickerArgs.OnColorPickerCancelled = FOnColorPickerCancelled::CreateSP(this, &FCustomPrimitiveDataCustomization::OnColorPickerCancelled, PrimIdx);
	PickerArgs.OnColorPickerWindowClosed = FOnWindowClosed::CreateSP(this, &FCustomPrimitiveDataCustomization::OnColorPickerWindowClosed);

	OpenColorPicker(PickerArgs);

	return FReply::Handled();
}

void FCustomPrimitiveDataCustomization::OnColorPickerCancelled(FLinearColor OriginalColor, uint8 PrimIdx)
{
	SetVectorColor(OriginalColor, PrimIdx);
	GEditor->CancelTransaction(0);
}

void FCustomPrimitiveDataCustomization::OnColorPickerWindowClosed(const TSharedRef<SWindow>& Window)
{
	GEditor->EndTransaction();
}

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION
TSharedRef<SHyperlink> FCustomPrimitiveDataCustomization::CreateHyperlink(FText Text, TWeakObjectPtr<UMaterialInterface> Material, const FGuid& ExpressionID)
{
	return SNew(SHyperlink)
		.Text(Text)
		.OnNavigate(this, &FCustomPrimitiveDataCustomization::OnNavigate, Material, ExpressionID)
		.Style(FEditorStyle::Get(), "HoverOnlyHyperlink")
		.TextStyle(FEditorStyle::Get(), "DetailsView.HyperlinkStyle");
}
END_SLATE_FUNCTION_BUILD_OPTIMIZATION

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION
TSharedRef<SWidget> FCustomPrimitiveDataCustomization::GetUndefinedParameterWidget(int32 PrimIdx, IPropertyTypeCustomizationUtils& CustomizationUtils) const
{
	FText PrimIdxText = FText::AsNumber(PrimIdx);
	TSharedRef<SWidget> UndefinedParamWidget = SNew(STextBlock)
		.Text(FText::Format(LOCTEXT("UndefinedParameter", "{0} (Undefined)"), PrimIdxText))
		.Font(CustomizationUtils.GetRegularFont());
	UndefinedParamWidget->SetToolTipText(FText::Format(LOCTEXT("UndefinedParameterTooltip", "A component is selected that doesn't define a parameter for primitive index {0}"), PrimIdxText));
		
	return UndefinedParamWidget;
}
END_SLATE_FUNCTION_BUILD_OPTIMIZATION

#undef LOCTEXT_NAMESPACE
