// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class UMoviePipelineConfigBase;
class SMoviePipelineQueueEditor;
class SWindow;
class UMoviePipelineExecutorJob;
class UMoviePipelineExecutorShot;
class IDetailsView;
struct FAssetData;

/**
 * Outermost widget that is used for adding and removing jobs from the Movie Pipeline Queue Subsystem.
 */
class SMoviePipelineGraphPanel : public SCompoundWidget
{
public:
	DECLARE_DELEGATE_OneParam(FOnGraphSelectionChanged, TArray<UObject*>);
	
	SLATE_BEGIN_ARGS(SMoviePipelineGraphPanel)
		: _Graph(nullptr)

		{}

		SLATE_EVENT(FOnGraphSelectionChanged, OnGraphSelectionChanged)

		/** The graph that is initially displayed */
		SLATE_ARGUMENT(class UMovieGraphConfig*, Graph)

	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FReply OnRenderLocalRequested();
	bool IsRenderLocalEnabled() const;
	FReply OnRenderRemoteRequested();
	bool IsRenderRemoteEnabled() const;

	/** When they want to edit the current configuration for the job */
	void OnEditJobConfigRequested(TWeakObjectPtr<UMoviePipelineExecutorJob> InJob, TWeakObjectPtr<UMoviePipelineExecutorShot> InShot);
	/** When an existing preset is chosen for the specified job. */
	void OnJobPresetChosen(TWeakObjectPtr<UMoviePipelineExecutorJob> InJob, TWeakObjectPtr<UMoviePipelineExecutorShot> InShot);
	void OnConfigUpdatedForJob(TWeakObjectPtr<UMoviePipelineExecutorJob> InJob, TWeakObjectPtr<UMoviePipelineExecutorShot> InShot, UMoviePipelineConfigBase* InConfig);
	void OnConfigUpdatedForJobToPreset(TWeakObjectPtr<UMoviePipelineExecutorJob> InJob, TWeakObjectPtr<UMoviePipelineExecutorShot> InShot, UMoviePipelineConfigBase* InConfig);
	void OnConfigWindowClosed();

	void OnSelectionChanged(const TArray<UMoviePipelineExecutorJob*>& InSelectedJobs);

	TSharedRef<SWidget> OnGenerateSavedQueuesMenu();
	bool OpenSaveDialog(const FString& InDefaultPath, const FString& InNewNameSuggestion, FString& OutPackageName);
	bool GetSavePresetPackageName(const FString& InExistingName, FString& OutName);
	void OnSaveAsAsset();
	void OnImportSavedQueueAssest(const FAssetData& InPresetAsset);
	
	void OnSelectedNodesChanged(const TSet<class UObject*>& NewSelection);
	void OnNodeDoubleClicked(class UEdGraphNode* Node);
	void OnNodeTitleCommitted(const FText& NewText, ETextCommit::Type CommitInfo, UEdGraphNode* NodeBeingChanged);
	
	FReply OnDebugButtonClicked();
	TObjectPtr<class UMovieGraphConfig> CurrentGraph;
private:
	/** Allocates a transient preset so that the user can use the pipeline without saving it to an asset first. */
	//UMoviePipelineConfigBase* AllocateTransientPreset();


private:
	/** The main movie pipeline queue editor widget */
	TSharedPtr<SMoviePipelineQueueEditor> PipelineQueueEditorWidget;

	TWeakPtr<SWindow> WeakEditorWindow;
	
	int32 NumSelectedJobs;

	FOnGraphSelectionChanged OnGraphSelectionChangedEvent;
};