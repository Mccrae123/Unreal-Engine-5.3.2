// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NiagaraGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "GraphEditorDragDropAction.h"
#include "DragAndDrop/DecoratedDragDropOp.h"
#include "NiagaraEditorCommon.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "NiagaraActions.generated.h"

UENUM()
enum class ENiagaraMenuSections : uint8
{
	// default should never be used. UENUMs require a 0 value
	Default = 0,
	Suggested = 1,
	General = 2
};

UENUM()
enum class EScriptSource : uint8
{
	Niagara,
	Game,
	Plugins,
	Developer,
	Unknown
};

USTRUCT()
struct NIAGARAEDITOR_API FNiagaraActionSourceData
{
	GENERATED_BODY()
	
	FNiagaraActionSourceData()
	{}
	FNiagaraActionSourceData(const EScriptSource& InSource, const FText& InSourceText, bool bInDisplaySource = false)
	{
		Source = InSource;
		SourceText = InSourceText;
		bDisplaySource = bInDisplaySource;
	}	
	
	EScriptSource Source = EScriptSource::Unknown;
	FText SourceText = FText::GetEmpty();
	bool bDisplaySource = false;	
};

USTRUCT()
struct NIAGARAEDITOR_API FNiagaraMenuAction : public FEdGraphSchemaAction
{
	GENERATED_USTRUCT_BODY();

	DECLARE_DELEGATE(FOnExecuteStackAction);
	DECLARE_DELEGATE_RetVal(bool, FCanExecuteStackAction);

	FNiagaraMenuAction() {}
	FNiagaraMenuAction(FText InNodeCategory, FText InMenuDesc, FText InToolTip, const int32 InGrouping, FText InKeywords, FOnExecuteStackAction InAction, int32 InSectionID = 0);
	FNiagaraMenuAction(FText InNodeCategory, FText InMenuDesc, FText InToolTip, const int32 InGrouping, FText InKeywords, FOnExecuteStackAction InAction, FCanExecuteStackAction InCanPerformAction, int32 InSectionID = 0);

	void ExecuteAction()
	{
		if (CanExecute())
		{
			Action.ExecuteIfBound();
		}
	}

	bool CanExecute() const
	{
		// Fire the 'can execute' delegate if we have one, otherwise always return true
		return CanPerformAction.IsBound() ? CanPerformAction.Execute() : true;
	}

	bool IsExperimental = false;

	TOptional<FNiagaraVariable> GetParameterVariable() const;
	void SetParamterVariable(const FNiagaraVariable& InParameterVariable);

private:
	TOptional<FNiagaraVariable> ParameterVariable;
	FOnExecuteStackAction Action;
	FCanExecuteStackAction CanPerformAction;
};

// this action does not have any use; inherit from it and provide your own functionality
USTRUCT()
struct NIAGARAEDITOR_API FNiagaraMenuAction_Base
{
	GENERATED_USTRUCT_BODY();

	DECLARE_DELEGATE(FOnExecuteAction);
	DECLARE_DELEGATE_RetVal(bool, FCanExecuteAction);

	FNiagaraMenuAction_Base() {}
	FNiagaraMenuAction_Base(FText DisplayName, ENiagaraMenuSections Section, TArray<FString> InNodeCategories, FText InToolTip, FText InKeywords);

	void UpdateFullSearchText();

	bool IsExperimental = false;

	bool bSuggested = false;

	bool bIsInLibrary = true;

	/** Top level section this action belongs to. */
	ENiagaraMenuSections Section;
	
	/** Nested categories below a top level section. Can be empty */
	TArray<FString> Categories;

	/** The DisplayName used in lists */
	FText DisplayName;

	/** The Tooltip text for this action */
	FText ToolTip;

	/** Additional keywords that should be considered for searching */
	FText Keywords;

	/** Additional data about where this action originates. Useful to display additional data such as the owning module. */
	FNiagaraActionSourceData SourceData;

	/** A string that combines all kinds of search terms */
	FString FullSearchString;
};

USTRUCT()
struct NIAGARAEDITOR_API FNiagaraAction_NewNode : public FNiagaraMenuAction_Base
{
	GENERATED_BODY()

	FNiagaraAction_NewNode() {}
	FNiagaraAction_NewNode(FText InDisplayName, ENiagaraMenuSections Section, TArray<FString> InNodeCategories, FText InToolTip, FText InKeywords)
	: FNiagaraMenuAction_Base(InDisplayName, Section, InNodeCategories, InToolTip, InKeywords)
	{		
	}

	class UEdGraphNode* CreateNode(UEdGraph* Graph, UEdGraphPin* FromPin, FVector2D NodePosition, bool bSelectNewNode = true) const;
	class UEdGraphNode* CreateNode(UEdGraph* Graph, TArray<UEdGraphPin*>& FromPins, FVector2D NodePosition, bool bSelectNewNode = true) const;

	UPROPERTY()
	class UEdGraphNode* NodeTemplate = nullptr;
};

USTRUCT()
struct NIAGARAEDITOR_API FNiagaraMenuAction_Generic : public FNiagaraMenuAction_Base
{
	GENERATED_BODY()

	FNiagaraMenuAction_Generic() {}

	FNiagaraMenuAction_Generic(FOnExecuteAction ExecuteAction, FCanExecuteAction InCanExecuteAction,
		FText InDisplayName, ENiagaraMenuSections Section, TArray<FString> InNodeCategories, FText InToolTip, FText InKeywords)
    : FNiagaraMenuAction_Base(InDisplayName, Section, InNodeCategories, InToolTip, InKeywords)
	{
		Action = ExecuteAction;
		CanExecuteAction = InCanExecuteAction;
	}

	FNiagaraMenuAction_Generic(FOnExecuteAction ExecuteAction,
        FText InDisplayName, ENiagaraMenuSections Section, TArray<FString> InNodeCategories, FText InToolTip, FText InKeywords)
    : FNiagaraMenuAction_Base(InDisplayName, Section, InNodeCategories, InToolTip, InKeywords)
	{
		Action = ExecuteAction;
	}

	void Execute()
	{
		if(CanExecuteAction.IsBound())
		{
			if(CanExecuteAction.Execute())
			{
				Action.ExecuteIfBound();
			}
		}
		else
		{
			Action.ExecuteIfBound();
		}
	}

protected:
	FOnExecuteAction Action;
	FCanExecuteAction CanExecuteAction;	
};

USTRUCT()
struct NIAGARAEDITOR_API FNiagaraMenuAction_Parameter : public FNiagaraMenuAction_Generic
{
	GENERATED_BODY()

	FNiagaraMenuAction_Parameter() {}

	FNiagaraMenuAction_Parameter(
		FOnExecuteAction ExecuteAction, FCanExecuteAction InCanExecuteAction,
        FText InDisplayName, ENiagaraMenuSections Section, TArray<FString> InNodeCategories, FText InToolTip, FText InKeywords)
    : FNiagaraMenuAction_Generic(ExecuteAction, InCanExecuteAction, InDisplayName, Section, InNodeCategories, InToolTip, InKeywords)
	{
	}

	FNiagaraMenuAction_Parameter(
		FOnExecuteAction ExecuteAction,
        FText InDisplayName, ENiagaraMenuSections Section, TArray<FString> InNodeCategories, FText InToolTip, FText InKeywords)
    : FNiagaraMenuAction_Generic(ExecuteAction, InDisplayName, Section, InNodeCategories, InToolTip, InKeywords)
	{
	}

	TOptional<FNiagaraVariable> GetParameterVariable() const;
	void SetParameterVariable(const FNiagaraVariable& InParameterVariable);
private:
	TOptional<FNiagaraVariable> ParameterVariable;
};
	
struct NIAGARAEDITOR_API FNiagaraScriptVarAndViewInfoAction : public FEdGraphSchemaAction
{	
	FNiagaraScriptVarAndViewInfoAction(const FNiagaraScriptVariableAndViewInfo& InScriptVariableAndViewInfo,
		FText InNodeCategory, FText InMenuDesc, FText InToolTip, const int32 InGrouping, FText InKeywords, int32 InSectionID = 0);

	const FNiagaraTypeDefinition GetScriptVarType() const { return ScriptVariableAndViewInfo.ScriptVariable.GetType(); };

	FNiagaraScriptVariableAndViewInfo ScriptVariableAndViewInfo;
};

struct NIAGARAEDITOR_API FNiagaraParameterAction : public FEdGraphSchemaAction
{
	FNiagaraParameterAction()
		: bIsExternallyReferenced(false)
		, bIsSourcedFromCustomStackContext(false)
	{
	}

	FNiagaraParameterAction(const FNiagaraVariable& InParameter,
		const TArray<FNiagaraGraphParameterReferenceCollection>& InReferenceCollection,
		FText InNodeCategory, FText InMenuDesc, FText InToolTip, const int32 InGrouping, FText InKeywords,
		TSharedPtr<TArray<FName>> ParameterWithNamespaceModifierRenamePending,
		int32 InSectionID = 0);

	FNiagaraParameterAction(const FNiagaraVariable& InParameter, 
		FText InNodeCategory, FText InMenuDesc, FText InToolTip, const int32 InGrouping, FText InKeywords,
		TSharedPtr<TArray<FName>> ParameterWithNamespaceModifierRenamePending,
		int32 InSectionID = 0);

	const FNiagaraVariable& GetParameter() const { return Parameter; }

	bool GetIsNamespaceModifierRenamePending() const;

	void SetIsNamespaceModifierRenamePending(bool bIsNamespaceModifierRenamePending);

	FNiagaraVariable Parameter;

	TArray<FNiagaraGraphParameterReferenceCollection> ReferenceCollection;

	bool bIsExternallyReferenced;

	bool bIsSourcedFromCustomStackContext;

private:
	TWeakPtr<TArray<FName>> ParameterWithNamespaceModifierRenamePendingWeak;
};

struct NIAGARAEDITOR_API FNiagaraScriptParameterAction : public FEdGraphSchemaAction
{
	FNiagaraScriptParameterAction() {}
	FNiagaraScriptParameterAction(const FNiagaraVariable& InVariable, const FNiagaraVariableMetaData& InVariableMetaData);
};

class NIAGARAEDITOR_API FNiagaraParameterGraphDragOperation : public FGraphSchemaActionDragDropAction
{
public:
	DRAG_DROP_OPERATOR_TYPE(FNiagaraParameterGraphDragOperation, FGraphSchemaActionDragDropAction)

	static TSharedRef<FNiagaraParameterGraphDragOperation> New(const TSharedPtr<FEdGraphSchemaAction>& InActionNode);

	// FGraphEditorDragDropAction interface
	virtual void HoverTargetChanged() override;
	virtual FReply DroppedOnNode(FVector2D ScreenPosition, FVector2D GraphPosition) override;
	virtual FReply DroppedOnPanel(const TSharedRef< class SWidget >& Panel, FVector2D ScreenPosition, FVector2D GraphPosition, UEdGraph& Graph) override;
	// End of FGraphEditorDragDropAction

	/** Set if operation is modified by alt */
	void SetAltDrag(bool InIsAltDrag) { bAltDrag = InIsAltDrag; }

	/** Set if operation is modified by the ctrl key */
	void SetCtrlDrag(bool InIsCtrlDrag) { bControlDrag = InIsCtrlDrag; }

	/** Returns true if the drag operation is currently hovering over the supplied node */
	bool IsCurrentlyHoveringNode(const UEdGraphNode* TestNode) const;

protected:
	/** Constructor */
	FNiagaraParameterGraphDragOperation();

	/** Structure for required node construction parameters */
	struct FNiagaraParameterNodeConstructionParams
	{
		FVector2D GraphPosition;
		UEdGraph* Graph;
		FNiagaraVariable Parameter;
	};

	static void MakeGetMap(FNiagaraParameterNodeConstructionParams InParams);
	static void MakeSetMap(FNiagaraParameterNodeConstructionParams InParams);
	static void MakeStaticSwitch(FNiagaraParameterNodeConstructionParams InParams);

	virtual EVisibility GetIconVisible() const override;
	virtual EVisibility GetErrorIconVisible() const override;

	/** Was ctrl held down at start of drag */
	bool bControlDrag;
	/** Was alt held down at the start of drag */
	bool bAltDrag;
};

class NIAGARAEDITOR_API FNiagaraParameterDragOperation : public FDecoratedDragDropOp
{
public:
	DRAG_DROP_OPERATOR_TYPE(FNiagaraParameterDragOperation, FDecoratedDragDropOp)

	FNiagaraParameterDragOperation(TSharedPtr<FEdGraphSchemaAction> InSourceAction)
		: SourceAction(InSourceAction)
	{
	}

	TSharedPtr<FEdGraphSchemaAction> GetSourceAction() const { return SourceAction; }

private:
	TSharedPtr<FEdGraphSchemaAction> SourceAction;
};