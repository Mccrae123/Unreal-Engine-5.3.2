// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FTimingEventsTrack;
struct FSlateBrush;
struct FSlateFontInfo;

/** Helper allowing access to common drawing elements for tracks */
class ITimingViewDrawHelper
{
public:
	virtual const FSlateBrush* GetWhiteBrush() const = 0;
	virtual const FSlateFontInfo& GetEventFont() const = 0;
	virtual FLinearColor GetEdgeColor() const = 0;
	virtual FLinearColor GetTrackNameTextColor(const FTimingEventsTrack& Track) const = 0;
	virtual int32 GetHeaderBackgroundLayerId() const = 0;
	virtual int32 GetHeaderTextLayerId() const = 0;
};