// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/GCObject.h"
#include "MovieSceneFwd.h"

#include "PoseSearchDatabasePreviewScene.h"

class UWorld;
class UPoseSearchDatabase;
struct FPoseSearchIndexAsset;
class UAnimPreviewInstance;
class UDebugSkelMeshComponent;
class UAnimSequence;
class UBlendSpace;
class UMirrorDataTable;

namespace UE::PoseSearch
{
	class FDatabaseAssetTreeNode;

	enum class EFeaturesDrawMode : uint8
	{
		None,
		All
	};

	enum class EAnimationPreviewMode : uint8
	{
		None,
		OriginalOnly,
		OriginalAndMirrored
	};


	struct FDatabasePreviewActor
	{
	public:
		TWeakObjectPtr<AActor> Actor = nullptr;
		TWeakObjectPtr<UDebugSkelMeshComponent> Mesh = nullptr;
		TWeakObjectPtr<UAnimPreviewInstance> AnimInstance = nullptr;
		const FPoseSearchIndexAsset* IndexAsset = nullptr;
		int32 CurrentPoseIndex = INDEX_NONE;

		bool IsValid()
		{
			const bool bIsValid = Actor.IsValid() && Mesh.IsValid() && AnimInstance.IsValid();
			return  bIsValid;
		}
	};

	class FDatabaseViewModel : public TSharedFromThis<FDatabaseViewModel>, public FGCObject
	{
	public:

		FDatabaseViewModel();
		virtual ~FDatabaseViewModel();

		// ~ FGCObject interface
		virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
		virtual FString GetReferencerName() const override { return TEXT("FPoseSearchDatabaseViewModel"); }

		void Initialize(
			UPoseSearchDatabase* InPoseSearchDatabase,
			const TSharedRef<FDatabasePreviewScene>& InPreviewScene);

		void RemovePreviewActors();
		void ResetPreviewActors();
		void RespawnPreviewActors();
		void BuildSearchIndex();

		UPoseSearchDatabase* GetPoseSearchDatabase() const { return PoseSearchDatabase; }
		void OnPreviewActorClassChanged();

		void Tick(float DeltaSeconds);

		TArray<FDatabasePreviewActor>& GetPreviewActors() { return PreviewActors; }
		const TArray<FDatabasePreviewActor>& GetPreviewActors() const { return PreviewActors; }

		void OnSetPoseFeaturesDrawMode(EFeaturesDrawMode DrawMode);
		bool IsPoseFeaturesDrawMode(EFeaturesDrawMode DrawMode) const;

		void OnSetAnimationPreviewMode(EAnimationPreviewMode PreviewMode);
		bool IsAnimationPreviewMode(EAnimationPreviewMode PreviewMode) const;

		void AddSequenceToDatabase(UAnimSequence* AnimSequence, int InitialGroupIdx = -1);
		void AddBlendSpaceToDatabase(UBlendSpace* BlendSpace, int InitialGroupIdx = -1);
		void AddGroupToDatabase();

		void DeleteSequenceFromDatabase(int32 SequenceIdx);
		void RemoveSequenceFromGroup(int32 SequenceIdx, int32 GroupIdx); 
		void DeleteBlendSpaceFromDatabase(int32 BlendSpaceIdx);
		void RemoveBlendSpaceFromGroup(int32 BlendSpaceIdx, int32 GroupIdx);
		void DeleteGroup(int32 GroupIdx);

		void SetSelectedNodes(const TArrayView<TSharedPtr<FDatabaseAssetTreeNode>>& InSelectedNodes);

	private:
		float PlayTime = 0.0f;

		/** Scene asset being viewed and edited by this view model. */
		TObjectPtr<UPoseSearchDatabase> PoseSearchDatabase;

		/** Weak pointer to the PreviewScene */
		TWeakPtr<FDatabasePreviewScene> PreviewScenePtr;

		/** Actors to be displayed in the preview viewport */
		TArray<FDatabasePreviewActor> PreviewActors;

		/** What features to show in the viewport */
		EFeaturesDrawMode PoseFeaturesDrawMode = EFeaturesDrawMode::None;

		/** What animations to show in the viewport */
		EAnimationPreviewMode AnimationPreviewMode = EAnimationPreviewMode::OriginalOnly;

		TArray<TSharedPtr<FDatabaseAssetTreeNode>> SelectedNodes;

		UWorld* GetWorld() const;

		UObject* GetPlaybackContext() const;

		FDatabasePreviewActor SpawnPreviewActor(const FPoseSearchIndexAsset& IndexAsset);

		void UpdatePreviewActors();

		FTransform MirrorRootMotion(FTransform RootMotion, const class UMirrorDataTable* MirrorDataTable);
	};
}
