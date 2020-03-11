// Copyright Epic Games, Inc. All Rights Reserved.

#include "SoundCueIndexer.h"
#include "Utility/IndexerUtilities.h"
#include "Sound/SoundCue.h"
#include "EdGraph/EdGraphNode.h"
#include "Sound/SoundNode.h"

enum class ESoundCueIndexerVersion
{
	Empty = 0,
	Initial = 1,

	// -----<new versions can be added above this line>-------------------------------------------------
	VersionPlusOne,
	LatestVersion = VersionPlusOne - 1
};

int32 FSoundCueIndexer::GetVersion() const
{
	return (int32)ESoundCueIndexerVersion::LatestVersion;
}

void FSoundCueIndexer::IndexAsset(const UObject* InAssetObject, FSearchSerializer& Serializer) const
{
	const USoundCue* SoundCue = Cast<USoundCue>(InAssetObject);
	check(SoundCue);

	for (USoundNode* SoundNode : SoundCue->AllNodes)
	{
		// Apparently SoundNodes can be null...
		if (SoundNode)
		{
			if (UEdGraphNode* GraphNode = SoundNode->GetGraphNode())
			{
				const FText NodeText = GraphNode->GetNodeTitle(ENodeTitleType::MenuTitle);
				Serializer.BeginIndexingObject(SoundNode, NodeText);
				Serializer.IndexProperty(TEXT("Name"), NodeText);
				FIndexerUtilities::IterateIndexableProperties(SoundNode, [&Serializer](const FProperty* Property, const FString& Value) {
					Serializer.IndexProperty(Property, Value);
				});
				Serializer.EndIndexingObject();
			}
		}
	}
}