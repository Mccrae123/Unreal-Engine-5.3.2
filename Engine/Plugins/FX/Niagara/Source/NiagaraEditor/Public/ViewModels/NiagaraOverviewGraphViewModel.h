// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphNode.h"
#include "EditorUndoClient.h"
#include "Styling/SlateColor.h"
#include "Framework/Commands/UICommandList.h"

class UNiagaraGraph;
class FNiagaraSystemViewModel;
class FNiagaraObjectSelection;

/** A view model for editing a niagara system in a graph editor. */
class FNiagaraOverviewGraphViewModel : public TSharedFromThis<FNiagaraOverviewGraphViewModel>, public FEditorUndoClient
{
public:
	/** A multicast delegate which is called when nodes are pasted in the graph which supplies the pasted nodes. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnNodesPasted, const TSet<UEdGraphNode*>&);

	DECLARE_MULTICAST_DELEGATE(FOnGraphChanged);

public:
	/** Create a new view model with the supplied system editor data and graph widget. */
	FNiagaraOverviewGraphViewModel(TSharedRef<FNiagaraSystemViewModel> InSystemViewModel);

	~FNiagaraOverviewGraphViewModel();

	/** Gets the display text for this graph. */
	NIAGARAEDITOR_API FText GetDisplayName() const;

	/** Gets the graph which is used to edit and view the system */
	UEdGraph* GetGraph() const;

	/** Gets commands used for editing the graph. */
	NIAGARAEDITOR_API TSharedRef<FUICommandList> GetCommands();

	/** Gets the currently selected graph nodes. */
	TSharedRef<FNiagaraObjectSelection> GetNodeSelection();

	/** Sets the currently selected graph nodes. */
	void SetSelectedNodes(const TSet<UObject*>& InSelectedNodes);

	/** Clears the currently selected graph nodes. */
	void ClearSelectedNodes();

	/** Gets a multicast delegate which is called any time nodes are pasted in the graph. */
	FOnNodesPasted& OnNodesPasted();

	//~ FEditorUndoClient interface.
	virtual void PostUndo(bool bSuccess) override;
	virtual void PostRedo(bool bSuccess) override { PostUndo(bSuccess); }

private:
	void SetupCommands();

	void SelectAllNodes();
	void DeleteSelectedNodes();
	bool CanDeleteNodes() const;
	void CutSelectedNodes();
	bool CanCutNodes() const;
	void CopySelectedNodes();
	bool CanCopyNodes() const;
	void PasteNodes();
	bool CanPasteNodes() const;
	void DuplicateNodes();
	bool CanDuplicateNodes() const;

	void InitDisplayName();

private:

	/** The view model to interface with the system being viewed and edited by this view model. */
	TWeakPtr<FNiagaraSystemViewModel> SystemViewModel;

	/** The display name for the overview graph. */
	FText DisplayName;

	/** Commands for editing the graph. */
	TSharedRef<FUICommandList> Commands;

	/** The set of nodes objects currently selected in the graph. */
	TSharedRef<FNiagaraObjectSelection> NodeSelection;

	/** A multicast delegate which is called whenever nodes are pasted into the graph. */
	FOnNodesPasted OnNodesPastedDelegate;

	/** A multicast delegate which is called whenever the graph object is changed to a different graph. */
	FOnGraphChanged OnGraphChangedDelegate;
};
