// Copyright Epic Games, Inc. All Rights Reserved.

#include "Widgets/SNiagaraScriptSourceFilter.h"
#include "NiagaraEditorStyle.h"
#include "NiagaraEditorUtilities.h"
#include "ViewModels/NiagaraCurveSelectionViewModel.h"

#define LOCTEXT_NAMESPACE "NiagaraSourceFilter"

void SNiagaraSourceFilterButton::Construct(const FArguments& Args, EScriptSource InSource)
{
	Source = InSource;
	OnSourceStateChanged = Args._OnSourceStateChanged;
	OnShiftClicked = Args._OnShiftClicked;
	
	UEnum* ScriptSourceEnum = StaticEnum<EScriptSource>();
	FText DisplayName = ScriptSourceEnum->GetDisplayNameTextByValue((int64) Source);
	FText ToolTipText = LOCTEXT("SourceFilterToolTip", "Display actions from source: {0}.\n Use Shift+Click to exclusively select this filter.");
	ToolTipText = FText::Format(ToolTipText, DisplayName);
	
	SCheckBox::FArguments ParentArgs;
	ParentArgs.Style(FNiagaraEditorStyle::Get(), "GraphActionMenu.FilterCheckBox")
	.BorderBackgroundColor(this, &SNiagaraSourceFilterButton::GetBackgroundColor)
	.IsChecked(Args._IsChecked)
	.OnCheckStateChanged(FOnCheckStateChanged::CreateLambda([=](ECheckBoxState NewState)
    {
        OnSourceStateChanged.ExecuteIfBound(Source, NewState == ECheckBoxState::Checked ? true : false);
    }));
	SCheckBox::Construct(ParentArgs);

	SetToolTipText(ToolTipText);
	
	SetContent(
		SNew(SHorizontalBox)
	    + SHorizontalBox::Slot()
	    .HAlign(HAlign_Center)
	    .VAlign(VAlign_Center)
	    [
	        SNew(STextBlock)
	        .Text(DisplayName)
	        .ColorAndOpacity_Raw(this, &SNiagaraSourceFilterButton::GetColor)
	        .ShadowOffset(1.f)
	        .TextStyle(FNiagaraEditorStyle::Get(), "GraphActionMenu.ActionFilterTextBlock")
	    ]
    );
}

FReply SNiagaraSourceFilterButton::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	FReply Reply = SCheckBox::OnMouseButtonUp(MyGeometry, MouseEvent);
	
	if(FSlateApplication::Get().GetModifierKeys().IsShiftDown() && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		bool bExecuted = OnShiftClicked.ExecuteIfBound(Source, !IsChecked());
		return FReply::Handled().ReleaseMouseCapture();
	}

	return Reply;
}

FSlateColor SNiagaraSourceFilterButton::GetColor() const
{
	if(IsChecked())
	{
		return FLinearColor::White;
	}

	return FLinearColor::Gray;
}

FSlateColor SNiagaraSourceFilterButton::GetBackgroundColor() const
{
	if(IsChecked())
	{
		return FNiagaraEditorUtilities::GetScriptSourceColor(Source);
	}

	return FSlateColor::UseForeground();
}


void SNiagaraSourceFilterBox::Construct(const FArguments& Args)
{
	OnFiltersChanged = Args._OnFiltersChanged;
	
	TSharedRef<SHorizontalBox> SourceContainer = SNew(SHorizontalBox);
    UEnum* ScriptSourceEnum = StaticEnum<EScriptSource>();

    // initialize the source states with the saved data @todo
    for(int32 SourceIndex = 0; SourceIndex < (int32) EScriptSource::Unknown; SourceIndex++)
    {
        SourceState.Add((EScriptSource) ScriptSourceEnum->GetValueByIndex(SourceIndex), true);
    }

    // a hard coded "Show all" button
    SourceContainer->AddSlot()
    .Padding(5.f)
    [
        SNew(SBorder)
        .BorderImage(FEditorStyle::GetBrush(TEXT("NoBorder")))
        .ToolTipText(LOCTEXT("ShowAllToolTip", "Show all"))
        .Padding(3.f)
        [
            SNew(SCheckBox)
            .Style(FNiagaraEditorStyle::Get(), "GraphActionMenu.FilterCheckBox")
            .BorderBackgroundColor_Lambda([=]() -> FSlateColor
            {
                bool bChecked = true;
                for(int32 SourceIndex = 0; SourceIndex < (int32) EScriptSource::Unknown; SourceIndex++)
                {
                    bChecked &= SourceState[(EScriptSource) ScriptSourceEnum->GetValueByIndex(SourceIndex)];
                }
    
                return bChecked ? FLinearColor::White : FSlateColor::UseForeground();
            })  
            .IsChecked_Lambda([=]() -> ECheckBoxState
            {
                bool bChecked = true;
                for(int32 SourceIndex = 0; SourceIndex < (int32) EScriptSource::Unknown; SourceIndex++)
                {
                    bChecked &= SourceState[(EScriptSource) ScriptSourceEnum->GetValueByIndex(SourceIndex)];
                }
    
                return bChecked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged(FOnCheckStateChanged::CreateLambda([=](ECheckBoxState NewState)
            {
                for(int32 SourceIndex = 0; SourceIndex < (int32) EScriptSource::Unknown; SourceIndex++)
                {
                    SourceState.Add((EScriptSource) ScriptSourceEnum->GetValueByIndex(SourceIndex), NewState == ECheckBoxState::Checked ? true : false);
                }
                
                BroadcastFiltersChanged();
            }))
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .HAlign(HAlign_Center)
                .VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("ShowAll", "Show all"))
                    .ColorAndOpacity_Lambda([=]() -> FSlateColor
                    {
                        bool bChecked = true;
                        for(int32 SourceIndex = 0; SourceIndex < (int32) EScriptSource::Unknown; SourceIndex++)
                        {
                            bChecked &= SourceState[(EScriptSource) ScriptSourceEnum->GetValueByIndex(SourceIndex)];
                        }
                        return bChecked ? FLinearColor::Black : FLinearColor::White;
                    })
                    .TextStyle(FNiagaraEditorStyle::Get(), "GraphActionMenu.ActionFilterTextBlock")
                ]
            ]	
        ]
    ];

    // create a button for every source option
    for(int32 SourceIndex = 0; SourceIndex < (int32) EScriptSource::Unknown; SourceIndex++)
    {
        SourceContainer->AddSlot()
    	.Padding(5.f)
    	[
    		SNew(SBorder)
    		.BorderImage(FEditorStyle::GetBrush(TEXT("NoBorder")))
			//.ToolTipText(LOCTEXT("ShowAllToolTip", "Show all"))
			.Padding(3.f)
    		[
    			SNew(SNiagaraSourceFilterButton, (EScriptSource) ScriptSourceEnum->GetValueByIndex(SourceIndex))
    			.IsChecked_Lambda([=]()
    			{
    				return SourceState[(EScriptSource)ScriptSourceEnum->GetValueByIndex(SourceIndex)] ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
    			})
    			.OnSourceStateChanged_Lambda([=](EScriptSource Source, bool bState)
    			{
    				SourceState.Add(Source, bState);
    				BroadcastFiltersChanged();
    			})
    			.OnShiftClicked_Lambda([=](EScriptSource ChangedSource, bool bState)
    			{
    				TArray<EScriptSource> Keys;
    				SourceState.GenerateKeyArray(Keys);
				
    				for(EScriptSource& Source : Keys)
    				{
    					if(Source == ChangedSource)
    					{
    						SourceState.Add(Source, true);
    					}
    					else
    					{
    						SourceState.Add(Source, false);
    					}
    				}

    				BroadcastFiltersChanged();
    			})
    		]
    	];
    }

	ChildSlot
    [
        SourceContainer
    ];
}

bool SNiagaraSourceFilterBox::IsFilterActive(EScriptSource Source) const
{
	if(SourceState.Contains(Source))
	{
		return SourceState[Source];
	}

	return true;
}

ECheckBoxState SNiagaraSourceFilterBox::OnIsFilterActive(EScriptSource Source) const
{
	return IsFilterActive(Source) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

#undef LOCTEXT_NAMESPACE