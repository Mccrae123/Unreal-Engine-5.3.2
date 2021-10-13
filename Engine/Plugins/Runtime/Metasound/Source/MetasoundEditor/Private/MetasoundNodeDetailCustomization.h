// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Components/Widget.h"
#include "CoreMinimal.h"
#include "DetailLayoutBuilder.h"
#include "IDetailCustomization.h"
#include "IPropertyTypeCustomization.h"
#include "Layout/Visibility.h"
#include "MetasoundAssetBase.h"
#include "MetasoundEditorGraphNode.h"
#include "MetasoundEditorGraphInputNodes.h"
#include "MetasoundEditorModule.h"
#include "MetasoundUObjectRegistry.h"
#include "PropertyHandle.h"
#include "ScopedTransaction.h"
#include "Types/SlateEnums.h"
#include "UObject/NameTypes.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "WorkflowOrientedApp/SModeWidget.h"

// Forward Declarations
class FDetailWidgetRow;
class FPropertyRestriction;
class IDetailLayoutBuilder;
class IPropertyHandle;
class SCheckBox;
class STextComboBox;

#define LOCTEXT_NAMESPACE "MetaSoundEditor"

namespace Metasound
{
	namespace Editor
	{
		class FMetasoundFloatLiteralCustomization : public IMetaSoundInputLiteralCustomization
		{
			IDetailCategoryBuilder* InputCategoryBuilder = nullptr;
			TWeakObjectPtr<UMetasoundEditorGraphInputFloat> FloatLiteral;

			// Delegate for updating the clamp min/max of the input value when the slider range is changed 
			FDelegateHandle InputWidgetOnRangeChangedDelegateHandle;

			// Delegate for clamping the input value or not
			FDelegateHandle OnClampInputChangedDelegateHandle;

		public:
			FMetasoundFloatLiteralCustomization(IDetailCategoryBuilder& InInputCategoryBuilder)
				: InputCategoryBuilder(&InInputCategoryBuilder)
			{
			}

			virtual ~FMetasoundFloatLiteralCustomization();

			virtual void CustomizeLiteral(UMetasoundEditorGraphInputLiteral& InLiteral, TSharedPtr<IPropertyHandle> InDefaultValueHandle) override;
		};

		class FMetasoundFloatLiteralCustomizationFactory : public IMetaSoundInputLiteralCustomizationFactory
		{
		public:
			virtual TUniquePtr<IMetaSoundInputLiteralCustomization> CreateLiteralCustomization(IDetailCategoryBuilder& DefaultCategoryBuilder) const override
			{
				return TUniquePtr<IMetaSoundInputLiteralCustomization>(new FMetasoundFloatLiteralCustomization(DefaultCategoryBuilder));
			}
		};

		class FMetasoundInputArrayDetailCustomizationBase : public IPropertyTypeCustomization
		{
		public:
			//~ Begin IPropertyTypeCustomization
			virtual void CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override;
			virtual void CustomizeChildren(TSharedRef<IPropertyHandle> StructPropertyHandle, IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override;
			//~ End IPropertyTypeCustomization

		protected:
			virtual FText GetPropertyNameOverride() const { return FText::GetEmpty(); }
			virtual TSharedRef<SWidget> CreateStructureWidget(TSharedPtr<IPropertyHandle>& PropertyHandle) const = 0;
			virtual void CacheProxyData(TSharedPtr<IPropertyHandle> ProxyHandle) { }

		private:
			TSharedRef<SWidget> CreateNameWidget(TSharedPtr<IPropertyHandle> StructPropertyHandle) const;
			TSharedRef<SWidget> CreateValueWidget(TSharedPtr<IPropertyHandleArray> ParentArrayProperty, TSharedPtr<IPropertyHandle> StructPropertyHandle, bool bIsInArray) const;
		};

		class FMetasoundInputBoolDetailCustomization : public FMetasoundInputArrayDetailCustomizationBase
		{
		protected:
			virtual FText GetPropertyNameOverride() const override;
			virtual TSharedRef<SWidget> CreateStructureWidget(TSharedPtr<IPropertyHandle>& StructPropertyHandle) const override;
			virtual void CacheProxyData(TSharedPtr<IPropertyHandle> ProxyHandle) override;

		private:
			FName DataTypeName;
		};

		class FMetasoundInputIntDetailCustomization : public FMetasoundInputArrayDetailCustomizationBase
		{
		protected:
			virtual TSharedRef<SWidget> CreateStructureWidget(TSharedPtr<IPropertyHandle>& StructPropertyHandle) const override;
			virtual void CacheProxyData(TSharedPtr<IPropertyHandle> ProxyHandle) override;

		private:
			FName DataTypeName;
		};

		class FMetasoundInputObjectDetailCustomization : public FMetasoundInputArrayDetailCustomizationBase
		{
		protected:
			virtual TSharedRef<SWidget> CreateStructureWidget(TSharedPtr<IPropertyHandle>& StructPropertyHandle) const override;
			virtual void CacheProxyData(TSharedPtr<IPropertyHandle> ProxyHandle) override;

		private:
			TWeakObjectPtr<UClass> ProxyGenClass;
		};

		class FMetasoundDataTypeSelector
		{
		public:
			void AddDataTypeSelector(IDetailLayoutBuilder& InDetailLayoutBuilder, const FText& InRowName, TWeakObjectPtr<UMetasoundEditorGraphMember> InGraphMember, bool bIsRequired);

			void OnDataTypeArrayChanged(TWeakObjectPtr<UMetasoundEditorGraphMember> InGraphMember, ECheckBoxState InNewState);
			ECheckBoxState OnGetDataTypeArrayCheckState(TWeakObjectPtr<UMetasoundEditorGraphMember> InGraphMember) const;
			void OnBaseDataTypeChanged(TWeakObjectPtr<UMetasoundEditorGraphMember> InGraphMember, TSharedPtr<FString> ItemSelected, ESelectInfo::Type SelectInfo);

		protected:
			TFunction<void()> OnDataTypeChanged;
		
		private:
				TSharedPtr<SCheckBox> DataTypeArrayCheckbox;
				TSharedPtr<STextComboBox> DataTypeComboBox;
				TArray<TSharedPtr<FString>> DataTypeNames;

				IDetailLayoutBuilder* DetailLayoutBuilder = nullptr;
		};

		template <typename GraphMemberType>
		class TMetasoundGraphMemberDetailCustomization : public IDetailCustomization, public FMetasoundDataTypeSelector
		{
		public:
			TMetasoundGraphMemberDetailCustomization(const FText& InGraphMemberLabel)
				: IDetailCustomization()
				, GraphMemberLabel(InGraphMemberLabel)
			{
			}

		protected:
			FText GraphMemberLabel;

			TWeakObjectPtr<GraphMemberType> GraphMember;
			TSharedPtr<SEditableTextBox> NameEditableTextBox;
			TSharedPtr<SEditableTextBox> DisplayNameEditableTextBox;
			bool bIsNameInvalid = false;

			// IDetailCustomization interface
			virtual void CustomizeDetails(IDetailLayoutBuilder& DetailLayout) override
			{
				TArray<TWeakObjectPtr<UObject>> Objects;
				DetailLayout.GetObjectsBeingCustomized(Objects);
				if (Objects.IsEmpty())
				{
					return;
				}

				GraphMember = Cast<GraphMemberType>(Objects[0].Get());
			}
			// End of IDetailCustomization interface

			void OnNameChanged(const FText& InNewName)
			{
				using namespace Frontend;

				bIsNameInvalid = false;
				DisplayNameEditableTextBox->SetError(FText::GetEmpty());

				if (!ensure(GraphMember.IsValid()))
				{
					return;
				}

				FText Error;
				if (!GraphMember->CanRename(InNewName, Error))
				{
					bIsNameInvalid = true;
					DisplayNameEditableTextBox->SetError(Error);
				}
			}

			FText GetDisplayName() const
			{
				using namespace Frontend;

				if (GraphMember.IsValid())
				{
					return GraphMember->GetConstNodeHandle()->GetDisplayName();
				}

				return FText::GetEmpty();
			}

			FText GetName() const
			{
				using namespace Frontend;

				if (GraphMember.IsValid())
				{
					return FText::FromName(GraphMember->GetConstNodeHandle()->GetNodeName());
				}

				return FText::GetEmpty();
			}

			bool IsGraphEditable() const
			{
				if (GraphMember.IsValid())
				{
					Metasound::Frontend::FConstNodeHandle NodeHandle = GraphMember->GetConstNodeHandle();
					return NodeHandle->GetOwningGraph()->GetGraphStyle().bIsGraphEditable;
				}

				return false;
			}

			bool IsRequired() const
			{
				if (GraphMember.IsValid())
				{
					return GraphMember->IsRequired();
				}

				return true;
			}

			void OnTooltipCommitted(const FText& InNewText, ETextCommit::Type InTextCommit)
			{
				using namespace Frontend;

				if (GraphMember.IsValid())
				{
					GraphMember->SetDescription(InNewText);
				}
			}

			FText GetTooltip() const
			{
				using namespace Frontend;
				if (GraphMember.IsValid())
				{
					FNodeHandle NodeHandle = GraphMember->GetNodeHandle();
					return NodeHandle->GetDescription();
				}

				return FText::GetEmpty();
			}

			void OnDisplayNameCommitted(const FText& InNewName, ETextCommit::Type InTextCommit)
			{
				using namespace Frontend;

				if (!bIsNameInvalid && GraphMember.IsValid())
				{
					GraphMember->SetDisplayName(InNewName);
				}

				DisplayNameEditableTextBox->SetError(FText::GetEmpty());
				bIsNameInvalid = false;
			}

			void OnNameCommitted(const FText& InNewName, ETextCommit::Type InTextCommit)
			{
				using namespace Frontend;

				if (!bIsNameInvalid && GraphMember.IsValid())
				{
					GraphMember->SetName(*InNewName.ToString());
				}

				DisplayNameEditableTextBox->SetError(FText::GetEmpty());
				bIsNameInvalid = false;
			}

			ECheckBoxState OnGetPrivateCheckboxState() const
			{
				if (GraphMember.IsValid())
				{
					return GraphMember->GetNodeHandle()->GetNodeStyle().bIsPrivate ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				}

				return ECheckBoxState::Unchecked;
			}

			void OnPrivateChanged(ECheckBoxState InNewState)
			{
				if (GraphMember.IsValid())
				{
					const bool bIsChecked = InNewState == ECheckBoxState::Checked;
					Frontend::FNodeHandle NodeHandle = GraphMember->GetNodeHandle();
					FMetasoundFrontendNodeStyle NodeStyle = NodeHandle->GetNodeStyle();
					NodeStyle.bIsPrivate = bIsChecked;
					NodeHandle->SetNodeStyle(NodeStyle);
				}
			}
		};

		class FMetasoundInputDetailCustomization : public TMetasoundGraphMemberDetailCustomization<UMetasoundEditorGraphInput>
		{
		public:
			FMetasoundInputDetailCustomization()
				: TMetasoundGraphMemberDetailCustomization<UMetasoundEditorGraphInput>(LOCTEXT("InputGraphMemberLabel", "Input"))
			{
			}
			virtual ~FMetasoundInputDetailCustomization() = default;

			// IDetailCustomization interface
			virtual void CustomizeDetails(IDetailLayoutBuilder& DetailLayout) override;
			// End of IDetailCustomization interface

		private:
			void SetDefaultPropertyMetaData(TSharedRef<IPropertyHandle> InDefaultPropertyHandle) const;

			FName GetLiteralDataType() const;

			TUniquePtr<IMetaSoundInputLiteralCustomization> LiteralCustomization;
		};

		class FMetasoundOutputDetailCustomization : public TMetasoundGraphMemberDetailCustomization<UMetasoundEditorGraphOutput>
		{
		public:
			FMetasoundOutputDetailCustomization()
				: TMetasoundGraphMemberDetailCustomization<UMetasoundEditorGraphOutput>(LOCTEXT("OutputGraphMemberLabel", "Output"))
			{
			}

			// IDetailCustomization interface
			virtual void CustomizeDetails(IDetailLayoutBuilder& DetailLayout) override;
			// End of IDetailCustomization interface

		private:
			void SetDefaultPropertyMetaData(TSharedRef<IPropertyHandle> InDefaultPropertyHandle) const;

			FName GetLiteralDataType() const;
		};
	} // namespace Editor
} // namespace Metasound
#undef LOCTEXT_NAMESPACE
