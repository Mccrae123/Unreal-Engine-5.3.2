// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "SGraphPin.h"
#include "NiagaraNode.h"
#include "Widgets/Text/SInlineEditableTextBlock.h"

/** A graph pin widget for allowing a pin to have an editable name for a pin. */
template< class BaseClass >
class TNiagaraGraphPinEditableName : public BaseClass
{
public:
	SLATE_BEGIN_ARGS(TNiagaraGraphPinEditableName<BaseClass>) {}
	SLATE_END_ARGS()

	FORCENOINLINE void Construct(const FArguments& InArgs, UEdGraphPin* InGraphPinObj)
	{
		bPendingRename = false;
		BaseClass::Construct(typename BaseClass::FArguments(), InGraphPinObj);
	}

protected:
	FText GetParentPinLabel() const
	{
		return BaseClass::GetPinLabel();
	}

	EVisibility GetParentPinVisibility() const
	{
		return BaseClass::GetPinLabelVisibility();
	}

	FSlateColor GetParentPinTextColor() const
	{
		return BaseClass::GetPinTextColor();
	}

	bool OnVerifyTextChanged(const FText& InName, FText& OutErrorMessage) const
	{
		UNiagaraNode* ParentNode = Cast<UNiagaraNode>(this->GraphPinObj->GetOwningNode());
		if (ParentNode)
		{
			return ParentNode->VerifyEditablePinName(InName, OutErrorMessage, this->GraphPinObj);
		}
		return false;
	}

	void OnTextCommitted(const FText& InText, ETextCommit::Type InCommitType)
	{
		if (!this->GraphPinObj->PinName.ToString().Equals(InText.ToString(), ESearchCase::CaseSensitive))
		{
			UNiagaraNode* ParentNode = Cast<UNiagaraNode>(this->GraphPinObj->GetOwningNode());
			if (ParentNode != nullptr)
			{
				ParentNode->CommitEditablePinName(InText, this->GraphPinObj);
			}
		}
		else
		{
			UNiagaraNode* ParentNode = Cast<UNiagaraNode>(this->GraphPinObj->GetOwningNode());
			if (ParentNode != nullptr)
			{
				ParentNode->CancelEditablePinName(InText, this->GraphPinObj);
			}
		}
	}

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
	{
		if (bPendingRename && CreatedTextBlock.IsValid())
		{
			CreatedTextBlock->EnterEditingMode();
			bPendingRename = false;
		}
		BaseClass::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	}

	virtual TSharedRef<SWidget> GetLabelWidget(const FName& InLabelStyle) override
	{
		UNiagaraNode* ParentNode = Cast<UNiagaraNode>(this->GraphPinObj->GetOwningNode());

		auto CreateLabelTextBlock = [&]()->TSharedRef<SWidget> {
			return SNew(SInlineEditableTextBlock)
				.Style(&FEditorStyle::Get().GetWidgetStyle<FInlineEditableTextBlockStyle>("Graph.Node.InlineEditablePinName"))
				.Text(this, &TNiagaraGraphPinEditableName<BaseClass>::GetParentPinLabel)
				.Visibility(this, &TNiagaraGraphPinEditableName<BaseClass>::GetParentPinVisibility)
				.ColorAndOpacity(this, &TNiagaraGraphPinEditableName<BaseClass>::GetParentPinTextColor)
				.IsReadOnly(true);
		};

		auto CreateRenamableLabelTextBlock = [&]()->TSharedRef<SWidget> {
			if (ParentNode->IsPinNameEditableUponCreation(this->GraphPinObj))
			{
				bPendingRename = true;
			}
			
			return SNew(SInlineEditableTextBlock)
				.Style(&FEditorStyle::Get().GetWidgetStyle<FInlineEditableTextBlockStyle>("Graph.Node.InlineEditablePinName"))
				.Text(this, &TNiagaraGraphPinEditableName<BaseClass>::GetParentPinLabel)
				.Visibility(this, &TNiagaraGraphPinEditableName<BaseClass>::GetParentPinVisibility)
				.ColorAndOpacity(this, &TNiagaraGraphPinEditableName<BaseClass>::GetParentPinTextColor)
				.OnVerifyTextChanged(this, &TNiagaraGraphPinEditableName<BaseClass>::OnVerifyTextChanged)
				.OnTextCommitted(this, &TNiagaraGraphPinEditableName<BaseClass>::OnTextCommitted);
		};

		if (ParentNode && ParentNode->IsPinNameEditable(this->GraphPinObj))
		{
			UNiagaraGraph* NiagaraGraph = ParentNode->GetNiagaraGraph();
			if (FNiagaraEditorCommonCVar::GNiagaraEnableParameterPanel2 > 0)
			{
				if (ParentNode->IsA<UNiagaraNodeParameterMapBase>())
				{
					if (NiagaraGraph->IsPinVisualWidgetProviderRegistered())
					{
						return NiagaraGraph->GetPinVisualWidget(this->GraphPinObj);
					}
					return CreateLabelTextBlock();
				}
				else if (ParentNode->IsA<UNiagaraNodeCustomHlsl>())
				{
					return CreateRenamableLabelTextBlock();
				}
				else
				{
					ensureMsgf(false, TEXT("Tried to create a pin widget for unhandled node class!"));
					return CreateRenamableLabelTextBlock();
				}
			}
			else
			{
				return CreateRenamableLabelTextBlock();
			}
		}
		else
		{
			return BaseClass::GetLabelWidget(InLabelStyle);
		}
	}

	bool bPendingRename;
	TSharedPtr<SInlineEditableTextBlock> CreatedTextBlock;
};
