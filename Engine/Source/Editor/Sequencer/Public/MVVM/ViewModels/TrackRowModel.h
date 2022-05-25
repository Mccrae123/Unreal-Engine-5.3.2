// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MVVM/ViewModels/ViewModel.h"
#include "MVVM/ViewModels/OutlinerItemModel.h"
#include "MVVM/Extensions/ITrackExtension.h"
#include "MVVM/Extensions/ITrackAreaExtension.h"
#include "MVVM/Extensions/IResizableExtension.h"
#include "MVVM/Extensions/IDeletableExtension.h"

#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

class ISequencer;
class ISequencerTrackEditor;
class UMovieSceneTrack;

namespace UE
{
namespace Sequencer
{

class FSectionModel;

class FTrackRowModel
	: public FOutlinerItemModel
	, public ITrackAreaExtension
	, public ITrackExtension
	, public IResizableExtension
	, public IDeletableExtension
{
public:

	UE_SEQUENCER_DECLARE_CASTABLE(FTrackRowModel, FOutlinerItemModel, ITrackAreaExtension, ITrackExtension, IDeletableExtension);

	explicit FTrackRowModel(UMovieSceneTrack* InTrack, int32 InRowIndex);
	~FTrackRowModel();

	void Initialize();

	FViewModelChildren GetSections();

	TSharedPtr<ISequencerTrackEditor> GetTrackEditor() const { return TrackEditor; }

	/*~ FOutlinerItemModel */
	void BuildContextMenu(FMenuBuilder& MenuBuilder) override;
	void CreateCurveModels(TArray<TUniquePtr<FCurveModel>>& OutCurveModels) override;

	/*~ IOutlinerExtension */
	FOutlinerSizing GetOutlinerSizing() const override;
	TSharedRef<SWidget> CreateOutlinerView(const FCreateOutlinerViewParams& InParams) override;
	FText GetLabel() const override;
	FSlateColor GetLabelColor() const override;
	FSlateFontInfo GetLabelFont() const override;
	const FSlateBrush* GetIconBrush() const override;

	/*~ IResizableExtension */
	bool IsResizable() const override;
	void Resize(float NewSize) override;

	/*~ ITrackExtension */
	UMovieSceneTrack* GetTrack() const override;
	int32 GetRowIndex() const override;

	/*~ ITrackAreaExtension */
	FTrackAreaParameters GetTrackAreaParameters() const override;
	FViewModelVariantIterator GetTrackAreaModelList() const override;

	/*~ IDimmableExtension */
	bool IsDimmed() const override;

	/*~ IDeletableExtension */
	bool CanDelete(FText* OutErrorMessage) const override;
	void Delete() override;

private:

	FViewModelListHead SectionList;
	FViewModelListHead TopLevelChannelList;
	TWeakObjectPtr<UMovieSceneTrack> WeakTrack;
	int32 RowIndex;

	// @todo_sequencer_mvvm: move all the track editor behavior into the view model
	TSharedPtr<ISequencerTrackEditor> TrackEditor;

	friend class FTrackModel;
};

} // namespace Sequencer
} // namespace UE

