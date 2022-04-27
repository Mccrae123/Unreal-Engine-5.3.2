// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "EditorStyleSet.h"


namespace UE::PoseSearch
{
	class FDatabaseEditorCommands : public TCommands<FDatabaseEditorCommands>
	{
	public:
		FDatabaseEditorCommands()
			: TCommands<FDatabaseEditorCommands>(
				TEXT("PoseSearchDatabaseEditor"),
				NSLOCTEXT("Contexts", "PoseSearchDatabase", "Pose Search Database"),
				NAME_None,
				FEditorStyle::GetStyleSetName())
		{
		}

		virtual void RegisterCommands() override;

	public:
		TSharedPtr<FUICommandInfo> StopPreviewScene;

		TSharedPtr<FUICommandInfo> ResetPreviewScene;

		TSharedPtr<FUICommandInfo> BuildSearchIndex;

		TSharedPtr<FUICommandInfo> ShowPoseFeaturesNone;

		TSharedPtr<FUICommandInfo> ShowPoseFeaturesAll;

		TSharedPtr<FUICommandInfo> ShowAnimationOriginalOnly;

		TSharedPtr<FUICommandInfo> ShowAnimationOriginalAndMirrored;

	};
}

