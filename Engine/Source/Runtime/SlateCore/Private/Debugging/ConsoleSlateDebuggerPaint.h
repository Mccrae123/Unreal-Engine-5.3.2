// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once


#include "Debugging/SlateDebugging.h"

#if WITH_SLATE_DEBUGGING

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Input/Reply.h"
#include "HAL/IConsoleManager.h"
#include "Rendering/DrawElements.h"

struct FGeometry;
class FPaintArgs;
class FSlateRect;
class FSlateWindowElementList;
class SWidget;
class SWindow;

/**
 * Allows debugging the behavior of SWidget::Paint from the console.
 * Basics:
 *   Start - SlateDebugger.Paint.Start
 *   Stop  - SlateDebugger.Paint.Stop
 */
class FConsoleSlateDebuggerPaint
{
public:
	FConsoleSlateDebuggerPaint();
	virtual ~FConsoleSlateDebuggerPaint();

	void StartDebugging();
	void StopDebugging();

	void SaveConfig();

private:
	void HandleLogOnce();
	void HandleToggleWidgetNameList();
	void HandleEndFrame();
	void HandleEndWidgetPaint(const SWidget* Widget, const FSlateWindowElementList& OutDrawElements, int32 LayerId);
	void HandlePaintDebugInfo(const FPaintArgs& InArgs, const FGeometry& InAllottedGeometry, FSlateWindowElementList& InOutDrawElements, int32& InOutLayerId);

private:
	bool bEnabled;

	//~ Settings
	bool bDisplayWidgetsNameList;
	bool bUseWidgetPathAsName;
	bool bDrawBox;
	bool bDrawQuad;
	bool bLogWidgetName;
	bool bLogWidgetNameOnce;
	bool bLogWarningIfWidgetIsPaintedMoreThanOnce;
	FLinearColor DrawBoxColor;
	FLinearColor DrawQuadColor;
	FLinearColor DrawWidgetNameColor;
	int32 MaxNumberOfWidgetInList;
	float CacheDuration;

	//~ Console objects
	FAutoConsoleCommand ShowPaintWidgetCommand;
	FAutoConsoleCommand HidePaintWidgetCommand;
	FAutoConsoleCommand LogPaintedWidgetOnceCommand;
	FAutoConsoleCommand DisplayWidgetsNameListCommand;
	FAutoConsoleVariableRef MaxNumberOfWidgetInListtRefCVar;
	FAutoConsoleVariableRef LogWarningIfWidgetIsPaintedMoreThanOnceRefCVar;

	using TSWidgetId = UPTRINT;
	using TSWindowId = UPTRINT;

	struct FPaintInfo 
	{
		TSWindowId Window;
		FVector2D PaintLocation;
		FVector2D PaintSize;
		FString WidgetName;
		double LastPaint;
		int32 PaintCount;
	};

	using TPaintedWidgetMap = TMap<TSWidgetId, FPaintInfo>;
	TPaintedWidgetMap PaintedWidgets;
};

#endif //WITH_SLATE_DEBUGGING