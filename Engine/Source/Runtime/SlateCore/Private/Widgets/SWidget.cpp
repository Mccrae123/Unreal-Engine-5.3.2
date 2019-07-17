// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "Widgets/SWidget.h"
#include "Types/PaintArgs.h"
#include "Layout/ArrangedChildren.h"
#include "Layout/Children.h"
#include "SlateGlobals.h"
#include "Rendering/DrawElements.h"
#include "Widgets/IToolTip.h"
#include "Misc/Paths.h"
#include "HAL/IConsoleManager.h"
#include "Types/NavigationMetaData.h"
#include "Application/SlateApplicationBase.h"
#include "Styling/CoreStyle.h"
#include "Application/ActiveTimerHandle.h"
#include "Input/HittestGrid.h"
#include "Debugging/SlateDebugging.h"
#include "Widgets/SWindow.h"

#if WITH_ACCESSIBILITY
#include "Widgets/Accessibility/SlateCoreAccessibleWidgets.h"
#include "Widgets/Accessibility/SlateAccessibleMessageHandler.h"
#endif

DEFINE_STAT(STAT_SlateVeryVerboseStatGroupTester);
DEFINE_STAT(STAT_SlateTotalWidgetsPerFrame);
DEFINE_STAT(STAT_SlateNumPaintedWidgets);
DEFINE_STAT(STAT_SlateNumTickedWidgets);
DEFINE_STAT(STAT_SlateExecuteActiveTimers);
DEFINE_STAT(STAT_SlateTickWidgets);
DEFINE_STAT(STAT_SlatePrepass);
DEFINE_STAT(STAT_SlateTotalWidgets);
DEFINE_STAT(STAT_SlateSWidgetAllocSize);


#if SLATE_CULL_WIDGETS

float GCullingSlackFillPercent = 0.25f;
static FAutoConsoleVariableRef CVarCullingSlackFillPercent(TEXT("Slate.CullingSlackFillPercent"), GCullingSlackFillPercent, TEXT("Scales the culling rect by the amount to provide extra slack/wiggle room for widgets that have a true bounds larger than the root child widget in a container."), ECVF_Default);

#endif

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

int32 GShowClipping = 0;
static FAutoConsoleVariableRef CVarSlateShowClipRects(TEXT("Slate.ShowClipping"), GShowClipping, TEXT("Controls whether we should render a clipping zone outline.  Yellow = Axis Scissor Rect Clipping (cheap).  Red = Stencil Clipping (expensive)."), ECVF_Default);

int32 GDebugCulling = 0;
static FAutoConsoleVariableRef CVarSlateDebugCulling(TEXT("Slate.DebugCulling"), GDebugCulling, TEXT("Controls whether we should ignore clip rects, and just use culling."), ECVF_Default);

#endif

#if STATS

struct FScopeCycleCounterSWidget : public FCycleCounter
{
	/**
	 * Constructor, starts timing
	 */
	FORCEINLINE_STATS FScopeCycleCounterSWidget(const SWidget* Widget)
	{
		if (Widget)
		{
			TStatId WidgetStatId = Widget->GetStatID();
			if (FThreadStats::IsCollectingData(WidgetStatId))
			{
				Start(WidgetStatId);
			}
		}
	}

	/**
	 * Updates the stat with the time spent
	 */
	FORCEINLINE_STATS ~FScopeCycleCounterSWidget()
	{
		Stop();
	}
};

#else

struct FScopeCycleCounterSWidget
{
	FScopeCycleCounterSWidget(const SWidget* Widget)
	{
	}
	~FScopeCycleCounterSWidget()
	{
	}
};

#endif


void SWidget::CreateStatID() const
{
#if STATS
	StatID = FDynamicStats::CreateStatId<FStatGroup_STATGROUP_SlateVeryVerbose>( ToString() );
#endif
}

void SWidget::UpdateWidgetProxy(int32 NewLayerId, FSlateCachedElementListNode* CacheNode)
{
#if WITH_SLATE_DEBUGGING
	check(!CacheNode || CacheNode->GetValue().Widget == this);
#endif

	PersistentState.CachedElementListNode = CacheNode;

	if (FastPathProxyHandle.IsValid())
	{
		FWidgetProxy& MyProxy = FastPathProxyHandle.GetProxy();

		MyProxy.Visibility = GetVisibility();

		PersistentState.OutgoingLayerId = NewLayerId;

		Advanced_InvalidateVolatility();
		if ((IsVolatile() && !IsVolatileIndirectly()) || (Advanced_IsInvalidationRoot() && !Advanced_IsWindow()))
		{
			AddUpdateFlags(EWidgetUpdateFlags::NeedsVolatilePaint);
		}
		else
		{
			RemoveUpdateFlags(EWidgetUpdateFlags::NeedsVolatilePaint);
		}
		FastPathProxyHandle.MarkWidgetUpdatedThisFrame();
	}
}

FName NAME_MouseButtonDown(TEXT("MouseButtonDown"));
FName NAME_MouseButtonUp(TEXT("MouseButtonUp"));
FName NAME_MouseMove(TEXT("MouseMove"));
FName NAME_MouseDoubleClick(TEXT("MouseDoubleClick"));

SWidget::SWidget()
	: bIsHovered(false)
	, bCanSupportFocus(true)
	, bCanHaveChildren(true)
	, bClippingProxy(false)
	, bToolTipForceFieldEnabled(false)
	, bForceVolatile(false)
	, bCachedVolatile(false)
	, bInheritedVolatility(false)
	, bInvisibleDueToParentOrSelfVisibility(false)
	, bNeedsPrepass(true)
	, bNeedsDesiredSize(true)
	, bUpdatingDesiredSize(false)
	, bHasCustomPrepass(false)
	, bVolatilityAlwaysInvalidatesPrepass(false)
	, Clipping(EWidgetClipping::Inherit)
	, FlowDirectionPreference(EFlowDirectionPreference::Inherit)
	// Note we are defaulting to tick for backwards compatibility
	, UpdateFlags(EWidgetUpdateFlags::NeedsTick)
	, DesiredSize()
	, PrepassLayoutScaleMultiplier(1.0f)
	, CullingBoundsExtension()
	, EnabledState(true)
	, Visibility(EVisibility::Visible)
	, RenderOpacity(1.0f)
	, RenderTransform()
	, RenderTransformPivot(FVector2D::ZeroVector)
	, Cursor( TOptional<EMouseCursor::Type>() )
	, ToolTip()
{
	if (GIsRunning)
	{
		INC_DWORD_STAT(STAT_SlateTotalWidgets);
		INC_DWORD_STAT(STAT_SlateTotalWidgetsPerFrame);
	}
}

SWidget::~SWidget()
{
	// Unregister all ActiveTimers so they aren't left stranded in the Application's list.
	if ( FSlateApplicationBase::IsInitialized() )
	{
		for ( const auto& ActiveTimerHandle : ActiveTimers )
		{
			FSlateApplicationBase::Get().UnRegisterActiveTimer(ActiveTimerHandle);
		}

		if (FSlateInvalidationRoot* Root = FastPathProxyHandle.GetInvalidationRoot())
		{
			Root->OnWidgetDestroyed(this);
		}

		// Reset handle
		FastPathProxyHandle = FWidgetProxyHandle();

		check(!PersistentState.CachedElementListNode);

#if WITH_ACCESSIBILITY
		FSlateApplicationBase::Get().GetAccessibleMessageHandler()->OnWidgetRemoved(this);
#endif
	}

	DEC_DWORD_STAT(STAT_SlateTotalWidgets);
	DEC_MEMORY_STAT_BY(STAT_SlateSWidgetAllocSize, AllocSize);
}

void SWidget::Construct(
	const TAttribute<FText>& InToolTipText,
	const TSharedPtr<IToolTip>& InToolTip,
	const TAttribute< TOptional<EMouseCursor::Type> >& InCursor,
	const TAttribute<bool>& InEnabledState,
	const TAttribute<EVisibility>& InVisibility,
	const float InRenderOpacity,
	const TAttribute<TOptional<FSlateRenderTransform>>& InTransform,
	const TAttribute<FVector2D>& InTransformPivot,
	const FName& InTag,
	const bool InForceVolatile,
	const EWidgetClipping InClipping,
	const EFlowDirectionPreference InFlowPreference,
	const TOptional<FAccessibleWidgetData>& InAccessibleData,
	const TArray<TSharedRef<ISlateMetaData>>& InMetaData
)
{
	if ( InToolTip.IsValid() )
	{
		// If someone specified a fancy widget tooltip, use it.
		ToolTip = InToolTip;
	}
	else if ( InToolTipText.IsSet() )
	{
		// If someone specified a text binding, make a tooltip out of it
		ToolTip = FSlateApplicationBase::Get().MakeToolTip(InToolTipText);
	}
	else if( !ToolTip.IsValid() || (ToolTip.IsValid() && ToolTip->IsEmpty()) )
	{	
		// We don't have a tooltip.
		ToolTip.Reset();
	}

	Cursor = InCursor;
	EnabledState = InEnabledState;
	Visibility = InVisibility;
	RenderOpacity = InRenderOpacity;
	RenderTransform = InTransform;
	RenderTransformPivot = InTransformPivot;
	Tag = InTag;
	bForceVolatile = InForceVolatile;
	Clipping = InClipping;
	FlowDirectionPreference = InFlowPreference;
	MetaData = InMetaData;

#if WITH_ACCESSIBILITY
	if (InAccessibleData.IsSet())
	{
		SetCanChildrenBeAccessible(InAccessibleData->bCanChildrenBeAccessible);
		// If custom text is provided, force behavior to custom. Otherwise, use the passed-in behavior and set their default text.
		SetAccessibleBehavior(InAccessibleData->AccessibleText.IsSet() ? EAccessibleBehavior::Custom : InAccessibleData->AccessibleBehavior, InAccessibleData->AccessibleText, EAccessibleType::Main);
		SetAccessibleBehavior(InAccessibleData->AccessibleSummaryText.IsSet() ? EAccessibleBehavior::Custom : InAccessibleData->AccessibleSummaryBehavior, InAccessibleData->AccessibleSummaryText, EAccessibleType::Summary);
	}
	if (AccessibleData.AccessibleBehavior != EAccessibleBehavior::Custom)
	{
		SetDefaultAccessibleText(EAccessibleType::Main);
	}
	if (AccessibleData.AccessibleSummaryBehavior != EAccessibleBehavior::Custom)
	{
		SetDefaultAccessibleText(EAccessibleType::Summary);
	}
#endif
}

void SWidget::SWidgetConstruct(const TAttribute<FText>& InToolTipText, const TSharedPtr<IToolTip>& InToolTip, const TAttribute< TOptional<EMouseCursor::Type> >& InCursor, const TAttribute<bool>& InEnabledState,
							   const TAttribute<EVisibility>& InVisibility, const float InRenderOpacity, const TAttribute<TOptional<FSlateRenderTransform>>& InTransform, const TAttribute<FVector2D>& InTransformPivot,
							   const FName& InTag, const bool InForceVolatile, const EWidgetClipping InClipping, const EFlowDirectionPreference InFlowPreference, const TOptional<FAccessibleWidgetData>& InAccessibleData,
							   const TArray<TSharedRef<ISlateMetaData>>& InMetaData)
{
	Construct(InToolTipText, InToolTip, InCursor, InEnabledState, InVisibility, InRenderOpacity, InTransform, InTransformPivot, InTag, InForceVolatile, InClipping, InFlowPreference, InAccessibleData, InMetaData);
}

FReply SWidget::OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent)
{
	return FReply::Unhandled();
}

void SWidget::OnFocusLost(const FFocusEvent& InFocusEvent)
{
}

void SWidget::OnFocusChanging(const FWeakWidgetPath& PreviousFocusPath, const FWidgetPath& NewWidgetPath)
{
}

void SWidget::OnFocusChanging(const FWeakWidgetPath& PreviousFocusPath, const FWidgetPath& NewWidgetPath, const FFocusEvent& InFocusEvent)
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	OnFocusChanging(PreviousFocusPath, NewWidgetPath);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

FReply SWidget::OnKeyChar( const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnPreviewKeyDown( const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnKeyDown( const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent )
{
	if (bCanSupportFocus && SupportsKeyboardFocus())
	{
		EUINavigation Direction = FSlateApplicationBase::Get().GetNavigationDirectionFromKey(InKeyEvent);
		// It's the left stick return a navigation request of the correct direction
		if (Direction != EUINavigation::Invalid)
		{
			const ENavigationGenesis Genesis = InKeyEvent.GetKey().IsGamepadKey() ? ENavigationGenesis::Controller : ENavigationGenesis::Keyboard;
			return FReply::Handled().SetNavigation(Direction, Genesis);
		}
	}
	return FReply::Unhandled();
}

FReply SWidget::OnKeyUp( const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnAnalogValueChanged( const FGeometry& MyGeometry, const FAnalogInputEvent& InAnalogInputEvent )
{
	if (bCanSupportFocus && SupportsKeyboardFocus())
	{
		EUINavigation Direction = FSlateApplicationBase::Get().GetNavigationDirectionFromAnalog(InAnalogInputEvent);
		// It's the left stick return a navigation request of the correct direction
		if (Direction != EUINavigation::Invalid)
		{
			return FReply::Handled().SetNavigation(Direction, ENavigationGenesis::Controller);
		}
	}
	return FReply::Unhandled();
}

FReply SWidget::OnPreviewMouseButtonDown( const FGeometry& MyGeometry, const FPointerEvent& MouseEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (const FPointerEventHandler* Event = GetPointerEvent(NAME_MouseButtonDown))
	{
		if ( Event->IsBound() )
		{
			return Event->Execute(MyGeometry, MouseEvent);
		}
	}
	return FReply::Unhandled();
}

FReply SWidget::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (const FPointerEventHandler* Event = GetPointerEvent(NAME_MouseButtonUp) )
	{
		if ( Event->IsBound() )
		{
			return Event->Execute(MyGeometry, MouseEvent);
		}
	}
	return FReply::Unhandled();
}

FReply SWidget::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (const FPointerEventHandler* Event = GetPointerEvent(NAME_MouseMove) )
	{
		if ( Event->IsBound() )
		{
			return Event->Execute(MyGeometry, MouseEvent);
		}
	}
	return FReply::Unhandled();
}

FReply SWidget::OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if ( const FPointerEventHandler* Event = GetPointerEvent(NAME_MouseDoubleClick) )
	{
		if ( Event->IsBound() )
		{
			return Event->Execute(MyGeometry, MouseEvent);
		}
	}
	return FReply::Unhandled();
}

void SWidget::OnMouseEnter( const FGeometry& MyGeometry, const FPointerEvent& MouseEvent )
{
	bIsHovered = true;

	if (MouseEnterHandler.IsBound())
	{
		// A valid handler is assigned; let it handle the event.
		MouseEnterHandler.Execute(MyGeometry, MouseEvent);
	}
}

void SWidget::OnMouseLeave( const FPointerEvent& MouseEvent )
{
	bIsHovered = false;

	if (MouseLeaveHandler.IsBound())
	{
		// A valid handler is assigned; let it handle the event.
		MouseLeaveHandler.Execute(MouseEvent);
	}
}

FReply SWidget::OnMouseWheel( const FGeometry& MyGeometry, const FPointerEvent& MouseEvent )
{
	return FReply::Unhandled();
}

FCursorReply SWidget::OnCursorQuery( const FGeometry& MyGeometry, const FPointerEvent& CursorEvent ) const
{
	TOptional<EMouseCursor::Type> TheCursor = Cursor.Get();
	return ( TheCursor.IsSet() )
		? FCursorReply::Cursor( TheCursor.GetValue() )
		: FCursorReply::Unhandled();
}

TOptional<TSharedRef<SWidget>> SWidget::OnMapCursor(const FCursorReply& CursorReply) const
{
	return TOptional<TSharedRef<SWidget>>();
}

bool SWidget::OnVisualizeTooltip( const TSharedPtr<SWidget>& TooltipContent )
{
	return false;
}

TSharedPtr<FPopupLayer> SWidget::OnVisualizePopup(const TSharedRef<SWidget>& PopupContent)
{
	return TSharedPtr<FPopupLayer>();
}

FReply SWidget::OnDragDetected( const FGeometry& MyGeometry, const FPointerEvent& MouseEvent )
{
	return FReply::Unhandled();
}

void SWidget::OnDragEnter( const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent )
{
}

void SWidget::OnDragLeave( const FDragDropEvent& DragDropEvent )
{
}

FReply SWidget::OnDragOver( const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnDrop( const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnTouchGesture( const FGeometry& MyGeometry, const FPointerEvent& GestureEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnTouchStarted( const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnTouchMoved( const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnTouchEnded( const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent )
{
	return FReply::Unhandled();
}

FReply SWidget::OnTouchForceChanged(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent)
{
	return FReply::Unhandled();
}

FReply SWidget::OnTouchFirstMove(const FGeometry& MyGeometry, const FPointerEvent& InTouchEvent)
{
	return FReply::Unhandled();
}

FReply SWidget::OnMotionDetected( const FGeometry& MyGeometry, const FMotionEvent& InMotionEvent )
{
	return FReply::Unhandled();
}

TOptional<bool> SWidget::OnQueryShowFocus(const EFocusCause InFocusCause) const
{
	return TOptional<bool>();
}

FPopupMethodReply SWidget::OnQueryPopupMethod() const
{
	return FPopupMethodReply::Unhandled();
}

TSharedPtr<struct FVirtualPointerPosition> SWidget::TranslateMouseCoordinateFor3DChild(const TSharedRef<SWidget>& ChildWidget, const FGeometry& MyGeometry, const FVector2D& ScreenSpaceMouseCoordinate, const FVector2D& LastScreenSpaceMouseCoordinate) const
{
	return nullptr;
}

void SWidget::OnFinishedPointerInput()
{

}

void SWidget::OnFinishedKeyInput()
{

}

FNavigationReply SWidget::OnNavigation(const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent)
{
	EUINavigation Type = InNavigationEvent.GetNavigationType();
	TSharedPtr<FNavigationMetaData> NavigationMetaData = GetMetaData<FNavigationMetaData>();
	if (NavigationMetaData.IsValid())
	{
		TSharedPtr<SWidget> Widget = NavigationMetaData->GetFocusRecipient(Type).Pin();
		return FNavigationReply(NavigationMetaData->GetBoundaryRule(Type), Widget, NavigationMetaData->GetFocusDelegate(Type));
	}
	return FNavigationReply::Escape();
}

EWindowZone::Type SWidget::GetWindowZoneOverride() const
{
	// No special behavior.  Override this in derived widgets, if needed.
	return EWindowZone::Unspecified;
}

void SWidget::Tick( const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime )
{
}

void SWidget::SlatePrepass()
{
	SlatePrepass(FSlateApplicationBase::Get().GetApplicationScale());
}

void SWidget::SlatePrepass(float InLayoutScaleMultiplier)
{
	SCOPE_CYCLE_COUNTER(STAT_SlatePrepass);

	if (!GSlateIsOnFastUpdatePath || bNeedsPrepass)
	{
		// If the scale changed, that can affect the desired size of some elements that take it into
		// account, such as text, so when the prepass size changes, so must we invalidate desired size.
		bNeedsDesiredSize = true;

		Prepass_Internal(InLayoutScaleMultiplier);
	}

}

void SWidget::InvalidatePrepass()
{
	bNeedsPrepass = true;
}

void SWidget::InvalidateChildRemovedFromTree(SWidget& Child)
{
	if (Child.FastPathProxyHandle.IsValid())
	{
		SCOPED_NAMED_EVENT(SWidget_InvalidateChildRemovedFromTree, FColor::Red);
		Child.UpdateFastPathVisibility(false, true);
	}
}

FVector2D SWidget::GetDesiredSize() const
{
	return DesiredSize.Get(FVector2D::ZeroVector);
}


void SWidget::AssignParentWidget(TSharedPtr<SWidget> InParent)
{
#if !UE_BUILD_SHIPPING
	ensureMsgf(InParent != SNullWidget::NullWidget, TEXT("The Null Widget can't be anyone's parent."));
	ensureMsgf(this != &SNullWidget::NullWidget.Get(), TEXT("The Null Widget can't have a parent, because a single instance is shared everywhere."));
	ensureMsgf(InParent.IsValid(), TEXT("Are you trying to detatch the parent of a widget?  Use ConditionallyDetatchParentWidget()."));
#endif

	//@todo We should update inherited visibility and volatility here but currently we are relying on ChildOrder invalidation to do that for us

	ParentWidgetPtr = InParent;
#if WITH_ACCESSIBILITY
	if (FSlateApplicationBase::IsInitialized())
	{
		FSlateApplicationBase::Get().GetAccessibleMessageHandler()->MarkDirty();
	}
#endif
	if (InParent.IsValid())
	{
		InParent->Invalidate(EInvalidateWidget::ChildOrder);
	}
}

bool SWidget::ConditionallyDetatchParentWidget(SWidget* InExpectedParent)
{
#if !UE_BUILD_SHIPPING
	ensureMsgf(this != &SNullWidget::NullWidget.Get(), TEXT("The Null Widget can't have a parent, because a single instance is shared everywhere."));
#endif

	TSharedPtr<SWidget> Parent = ParentWidgetPtr.Pin();
	if (Parent.Get() == InExpectedParent)
	{
		ParentWidgetPtr.Reset();
#if WITH_ACCESSIBILITY
		if (FSlateApplicationBase::IsInitialized())
		{
			FSlateApplicationBase::Get().GetAccessibleMessageHandler()->MarkDirty();
		}
#endif

		if (Parent.IsValid())
		{
			Parent->Invalidate(EInvalidateWidget::ChildOrder);
		}

		InvalidateChildRemovedFromTree(*this);
		return true;
	}

	return false;
}


void SWidget::LayoutChanged(EInvalidateWidget InvalidateReason)
{
	if(EnumHasAnyFlags(InvalidateReason, EInvalidateWidget::Layout))
	{
		bNeedsDesiredSize = true;

		TSharedPtr<SWidget> ParentWidget = ParentWidgetPtr.Pin();
		if (ParentWidget.IsValid())
		{
			ParentWidget->ChildLayoutChanged(InvalidateReason);
		}
	}
}

void SWidget::ChildLayoutChanged(EInvalidateWidget InvalidateReason)
{
	if (!bNeedsDesiredSize || InvalidateReason == EInvalidateWidget::Visibility )
	{
		LayoutChanged(InvalidateReason);
	}
}

void SWidget::AssignIndicesToChildren(FSlateInvalidationRoot& Root, int32 ParentIndex, TArray<FWidgetProxy, TMemStackAllocator<>>& FastPathList, bool bParentVisible, bool bParentVolatile)
{
	FWidgetProxy MyProxy(this);
	MyProxy.Index = FastPathList.Num();
	MyProxy.ParentIndex = ParentIndex;
	MyProxy.Visibility = GetVisibility();

	check(ParentIndex != MyProxy.Index);

	// If this method is being called, child order changed.  Initial visibility and volatility needs to be propagated
	// Update visibility
	const bool bParentAndSelfVisible = bParentVisible && MyProxy.Visibility.IsVisible();
	const bool bWasInvisible = bInvisibleDueToParentOrSelfVisibility;
	bInvisibleDueToParentOrSelfVisibility = !bParentAndSelfVisible;
	MyProxy.bInvisibleDueToParentOrSelfVisibility = bInvisibleDueToParentOrSelfVisibility;

	// Update volatility
	bInheritedVolatility = bParentVolatile;

	FastPathProxyHandle = FWidgetProxyHandle(Root, MyProxy.Index);


	if (bInvisibleDueToParentOrSelfVisibility&& PersistentState.CachedElementListNode != nullptr)
	{
#if WITH_SLATE_DEBUGGING
		check(PersistentState.CachedElementListNode->GetValue().Widget == this);
#endif
		Root.GetCachedElements().ResetCache(PersistentState.CachedElementListNode);
	}

	FastPathList.Add(MyProxy);

	// Don't recur into children if we are at a different invalidation root(nested invalidation panels) than where we started and not at the root of the tree. Those children should belong to that roots tree.
	if (!Advanced_IsInvalidationRoot() || ParentIndex == INDEX_NONE)
	{
		FChildren* MyChildren = GetAllChildren();
		const int32 NumChildren = MyChildren->Num();

		int32 NumChildrenValidForFastPath = 0;
		for (int32 ChildIndex = 0; ChildIndex < NumChildren; ++ChildIndex)
		{
			// Because null widgets are a shared static widget they are not valid for the fast path and are treated as non-existent
			const TSharedRef<SWidget>& Child = MyChildren->GetChildAt(ChildIndex);
			if (Child != SNullWidget::NullWidget)
			{
				++NumChildrenValidForFastPath;
				Child->AssignIndicesToChildren(Root, MyProxy.Index, FastPathList, bParentAndSelfVisible, bParentVolatile || IsVolatile());
			}
		}

		{
			FWidgetProxy& MyProxyRef = FastPathList[MyProxy.Index];
			MyProxyRef.NumChildren = NumChildrenValidForFastPath;
			int32 LastIndex = FastPathList.Num() - 1;
			MyProxyRef.LeafMostChildIndex = LastIndex != MyProxy.Index ? LastIndex : INDEX_NONE;
		}

	}
}

void SWidget::UpdateFastPathVisibility(bool bParentVisible, bool bWidgetRemoved)
{
	const EVisibility CurrentVisibility = GetVisibility();
	const bool bParentAndSelfVisible = bParentVisible && CurrentVisibility.IsVisible();
	const bool bWasInvisible = bInvisibleDueToParentOrSelfVisibility;
	bInvisibleDueToParentOrSelfVisibility = !bParentAndSelfVisible;
	const bool bVisibilityChanged = bWasInvisible != bInvisibleDueToParentOrSelfVisibility;

	if (FastPathProxyHandle.IsValid())
	{
		FastPathProxyHandle.GetInvalidationRoot()->GetHittestGrid()->RemoveWidget(SharedThis(this));
		FWidgetProxy& Proxy = FastPathProxyHandle.GetProxy();
		Proxy.Visibility = CurrentVisibility;
		Proxy.bInvisibleDueToParentOrSelfVisibility = bInvisibleDueToParentOrSelfVisibility;

		if (bWidgetRemoved)
		{
			FastPathProxyHandle.GetInvalidationRoot()->RemoveWidgetFromFastPath(Proxy);
		}
		else if (PersistentState.CachedElementListNode != nullptr)
		{
			FastPathProxyHandle.GetInvalidationRoot()->GetCachedElements().ResetCache(PersistentState.CachedElementListNode);
		}
	}
	else
	{
		ensure(FastPathProxyHandle.GetIndex() == INDEX_NONE);
	}

	FChildren* MyChildren = GetAllChildren();
	const int32 NumChildren = MyChildren->Num();
	for (int32 ChildIndex = 0; ChildIndex < NumChildren; ++ChildIndex)
	{
		const TSharedRef<SWidget>& Child = MyChildren->GetChildAt(ChildIndex);
		Child->UpdateFastPathVisibility(bParentAndSelfVisible, bWidgetRemoved);
	}
}

void SWidget::UpdateFastPathVolatility(bool bParentVolatile)
{
	bInheritedVolatility = bParentVolatile;

	if (IsVolatile() && !IsVolatileIndirectly())
	{
		AddUpdateFlags(EWidgetUpdateFlags::NeedsVolatilePaint);
	}
	else
	{
		RemoveUpdateFlags(EWidgetUpdateFlags::NeedsVolatilePaint);
	}

	FChildren* MyChildren = GetAllChildren();
	const int32 NumChildren = MyChildren->Num();
	for (int32 ChildIndex = 0; ChildIndex < NumChildren; ++ChildIndex)
	{
		const TSharedRef<SWidget>& Child = MyChildren->GetChildAt(ChildIndex);
		Child->UpdateFastPathVolatility(bParentVolatile || IsVolatile());
	}


}

void SWidget::CacheDesiredSize(float InLayoutScaleMultiplier)
{
#if SLATE_VERBOSE_NAMED_EVENTS
	SCOPED_NAMED_EVENT(SWidget_CacheDesiredSize, FColor::Red);
#endif

	// Cache this widget's desired size.
	SetDesiredSize(ComputeDesiredSize(InLayoutScaleMultiplier));
}


bool SWidget::SupportsKeyboardFocus() const
{
	return false;
}

bool SWidget::HasKeyboardFocus() const
{
	return (FSlateApplicationBase::Get().GetKeyboardFocusedWidget().Get() == this);
}

TOptional<EFocusCause> SWidget::HasUserFocus(int32 UserIndex) const
{
	return FSlateApplicationBase::Get().HasUserFocus(SharedThis(this), UserIndex);
}

TOptional<EFocusCause> SWidget::HasAnyUserFocus() const
{
	return FSlateApplicationBase::Get().HasAnyUserFocus(SharedThis(this));
}

bool SWidget::HasUserFocusedDescendants(int32 UserIndex) const
{
	return FSlateApplicationBase::Get().HasUserFocusedDescendants(SharedThis(this), UserIndex);
}

bool SWidget::HasFocusedDescendants() const
{
	return FSlateApplicationBase::Get().HasFocusedDescendants(SharedThis(this));
}

bool SWidget::HasAnyUserFocusOrFocusedDescendants() const
{
	return HasAnyUserFocus().IsSet() || HasFocusedDescendants();
}

const FSlateBrush* SWidget::GetFocusBrush() const
{
	return FCoreStyle::Get().GetBrush("FocusRectangle");
}

bool SWidget::HasMouseCapture() const
{
	return FSlateApplicationBase::Get().DoesWidgetHaveMouseCapture(SharedThis(this));
}

bool SWidget::HasMouseCaptureByUser(int32 UserIndex, TOptional<int32> PointerIndex) const
{
	return FSlateApplicationBase::Get().DoesWidgetHaveMouseCaptureByUser(SharedThis(this), UserIndex, PointerIndex);
}

void SWidget::OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent)
{
}

bool SWidget::FindChildGeometries( const FGeometry& MyGeometry, const TSet< TSharedRef<SWidget> >& WidgetsToFind, TMap<TSharedRef<SWidget>, FArrangedWidget>& OutResult ) const
{
	FindChildGeometries_Helper(MyGeometry, WidgetsToFind, OutResult);
	return OutResult.Num() == WidgetsToFind.Num();
}


void SWidget::FindChildGeometries_Helper( const FGeometry& MyGeometry, const TSet< TSharedRef<SWidget> >& WidgetsToFind, TMap<TSharedRef<SWidget>, FArrangedWidget>& OutResult ) const
{
	// Perform a breadth first search!

	FArrangedChildren ArrangedChildren(EVisibility::Visible);
	this->ArrangeChildren(MyGeometry, ArrangedChildren);
	const int32 NumChildren = ArrangedChildren.Num();

	// See if we found any of the widgets on this level.
	for(int32 ChildIndex=0; ChildIndex < NumChildren; ++ChildIndex )
	{
		const FArrangedWidget& CurChild = ArrangedChildren[ ChildIndex ];
		
		if ( WidgetsToFind.Contains(CurChild.Widget) )
		{
			// We found one of the widgets for which we need geometry!
			OutResult.Add( CurChild.Widget, CurChild );
		}
	}

	// If we have not found all the widgets that we were looking for, descend.
	if ( OutResult.Num() != WidgetsToFind.Num() )
	{
		// Look for widgets among the children.
		for( int32 ChildIndex=0; ChildIndex < NumChildren; ++ChildIndex )
		{
			const FArrangedWidget& CurChild = ArrangedChildren[ ChildIndex ];
			CurChild.Widget->FindChildGeometries_Helper( CurChild.Geometry, WidgetsToFind, OutResult );
		}	
	}	
}

FGeometry SWidget::FindChildGeometry( const FGeometry& MyGeometry, TSharedRef<SWidget> WidgetToFind ) const
{
	// We just need to find the one WidgetToFind among our descendants.
	TSet< TSharedRef<SWidget> > WidgetsToFind;
	{
		WidgetsToFind.Add( WidgetToFind );
	}
	TMap<TSharedRef<SWidget>, FArrangedWidget> Result;

	FindChildGeometries( MyGeometry, WidgetsToFind, Result );

	return Result.FindChecked( WidgetToFind ).Geometry;
}

int32 SWidget::FindChildUnderMouse( const FArrangedChildren& Children, const FPointerEvent& MouseEvent )
{
	const FVector2D& AbsoluteCursorLocation = MouseEvent.GetScreenSpacePosition();
	return SWidget::FindChildUnderPosition( Children, AbsoluteCursorLocation );
}

int32 SWidget::FindChildUnderPosition( const FArrangedChildren& Children, const FVector2D& ArrangedSpacePosition )
{
	const int32 NumChildren = Children.Num();
	for( int32 ChildIndex=NumChildren-1; ChildIndex >= 0; --ChildIndex )
	{
		const FArrangedWidget& Candidate = Children[ChildIndex];
		const bool bCandidateUnderCursor = 
			// Candidate is physically under the cursor
			Candidate.Geometry.IsUnderLocation( ArrangedSpacePosition );

		if (bCandidateUnderCursor)
		{
			return ChildIndex;
		}
	}

	return INDEX_NONE;
}

FString SWidget::ToString() const
{
	return FString::Printf(TEXT("%s [%s]"), *this->TypeOfWidget.ToString(), *this->GetReadableLocation() );
}

FString SWidget::GetTypeAsString() const
{
	return this->TypeOfWidget.ToString();
}

FName SWidget::GetType() const
{
	return TypeOfWidget;
}

FString SWidget::GetReadableLocation() const
{
#if !UE_BUILD_SHIPPING
	return FString::Printf(TEXT("%s(%d)"), *FPaths::GetCleanFilename(this->CreatedInLocation.GetPlainNameString()), this->CreatedInLocation.GetNumber());
#else
	return FString();
#endif
}

FName SWidget::GetCreatedInLocation() const
{
#if !UE_BUILD_SHIPPING
	return CreatedInLocation;
#else
	return NAME_None;
#endif
}

FName SWidget::GetTag() const
{
	return Tag;
}

FSlateColor SWidget::GetForegroundColor() const
{
	static FSlateColor NoColor = FSlateColor::UseForeground();
	return NoColor;
}

const FGeometry& SWidget::GetCachedGeometry() const
{
	return GetTickSpaceGeometry();
}

const FGeometry& SWidget::GetTickSpaceGeometry() const
{
	return PersistentState.DesktopGeometry;
}

const FGeometry& SWidget::GetPaintSpaceGeometry() const
{
	return PersistentState.AllottedGeometry;
}

void SWidget::SetToolTipText(const TAttribute<FText>& ToolTipText)
{
	ToolTip = FSlateApplicationBase::Get().MakeToolTip(ToolTipText);
}

void SWidget::SetToolTipText( const FText& ToolTipText )
{
	ToolTip = FSlateApplicationBase::Get().MakeToolTip(ToolTipText);
}

void SWidget::SetToolTip( const TSharedPtr<IToolTip> & InToolTip )
{
	ToolTip = InToolTip;
}

TSharedPtr<IToolTip> SWidget::GetToolTip()
{
	return ToolTip;
}

void SWidget::OnToolTipClosing()
{
}

void SWidget::EnableToolTipForceField( const bool bEnableForceField )
{
	bToolTipForceFieldEnabled = bEnableForceField;
}

bool SWidget::IsDirectlyHovered() const
{
	return FSlateApplicationBase::Get().IsWidgetDirectlyHovered(SharedThis(this));
}

void SWidget::SetVisibility(TAttribute<EVisibility> InVisibility)
{
	if (!Visibility.IdenticalTo(InVisibility))
	{
		Visibility = InVisibility;

		Invalidate(EInvalidateWidget::Visibility);
	}
}

void SWidget::Invalidate(EInvalidateWidget InvalidateReason)
{
	SLATE_CROSS_THREAD_CHECK();

	SCOPED_NAMED_EVENT_TEXT("SWidget::Invalidate", FColor::Orange);
	const bool bWasVolatile = IsVolatileIndirectly() || IsVolatile();

	const bool bVolatilityChanged = EnumHasAnyFlags(InvalidateReason, EInvalidateWidget::Volatility) ? Advanced_InvalidateVolatility() : false;

	if (EnumHasAnyFlags(InvalidateReason, EInvalidateWidget::ChildOrder))
	{
		InvalidatePrepass();
	}

	if(FastPathProxyHandle.IsValid())
	{
		// Current thinking is that visibility and volatility should be updated right away, not during fast path invalidation processing next frame
		if (EnumHasAnyFlags(InvalidateReason, EInvalidateWidget::Visibility))
		{
			SCOPED_NAMED_EVENT(SWidget_UpdateFastPathVisibility, FColor::Red);
			TSharedPtr<SWidget> ParentWidget = GetParentWidget();

			UpdateFastPathVisibility(ParentWidget.IsValid() ? !ParentWidget->bInvisibleDueToParentOrSelfVisibility : false, false);
		}

		if (bVolatilityChanged)
		{
			SCOPED_NAMED_EVENT(SWidget_UpdateFastPathVolatility, FColor::Red);

			TSharedPtr<SWidget> ParentWidget = GetParentWidget();

			UpdateFastPathVolatility(ParentWidget.IsValid() ? ParentWidget->IsVolatile() || ParentWidget->IsVolatileIndirectly() : false);

			ensure(!IsVolatile() || EnumHasAnyFlags(UpdateFlags, EWidgetUpdateFlags::NeedsVolatilePaint));
		}

		FastPathProxyHandle.MarkWidgetDirty(InvalidateReason);
	}
}

void SWidget::SetCursor( const TAttribute< TOptional<EMouseCursor::Type> >& InCursor )
{
	Cursor = InCursor;
}

void SWidget::SetDebugInfo( const ANSICHAR* InType, const ANSICHAR* InFile, int32 OnLine, size_t InAllocSize )
{
	TypeOfWidget = InType;

	STAT(AllocSize = InAllocSize);
	INC_MEMORY_STAT_BY(STAT_SlateSWidgetAllocSize, AllocSize);

#if !UE_BUILD_SHIPPING
	CreatedInLocation = FName( InFile );
	CreatedInLocation.SetNumber(OnLine);
#endif
}

void SWidget::OnClippingChanged()
{

}

FSlateRect SWidget::CalculateCullingAndClippingRules(const FGeometry& AllottedGeometry, const FSlateRect& IncomingCullingRect, bool& bClipToBounds, bool& bAlwaysClip, bool& bIntersectClipBounds) const
{
	bClipToBounds = false;
	bIntersectClipBounds = true;
	bAlwaysClip = false;

	if (!bClippingProxy)
	{
		switch (Clipping)
		{
		case EWidgetClipping::ClipToBounds:
			bClipToBounds = true;
			break;
		case EWidgetClipping::ClipToBoundsAlways:
			bClipToBounds = true;
			bAlwaysClip = true;
			break;
		case EWidgetClipping::ClipToBoundsWithoutIntersecting:
			bClipToBounds = true;
			bIntersectClipBounds = false;
			break;
		case EWidgetClipping::OnDemand:
			const float OverflowEpsilon = 1.0f;
			const FVector2D& CurrentSize = GetDesiredSize();
			const FVector2D& LocalSize = AllottedGeometry.GetLocalSize();
			bClipToBounds =
				(CurrentSize.X - OverflowEpsilon) > LocalSize.X ||
				(CurrentSize.Y - OverflowEpsilon) > LocalSize.Y;
			break;
		}
	}

	if (bClipToBounds)
	{
		FSlateRect MyCullingRect(AllottedGeometry.GetRenderBoundingRect(CullingBoundsExtension));

		if (bIntersectClipBounds)
		{
			bool bClipBoundsOverlapping;
			return IncomingCullingRect.IntersectionWith(MyCullingRect, bClipBoundsOverlapping);
		}
		
		return MyCullingRect;
	}

	return IncomingCullingRect;
}

int32 SWidget::Paint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	// TODO, Maybe we should just make Paint non-const and keep OnPaint const.
	TSharedRef<SWidget> MutableThis = ConstCastSharedRef<SWidget>(AsShared());

	INC_DWORD_STAT(STAT_SlateNumPaintedWidgets);

	const SWidget* PaintParent = Args.GetPaintParent();
	//if (GSlateEnableGlobalInvalidation)
	//{
	//	bInheritedVolatility = PaintParent ? (PaintParent->IsVolatileIndirectly() || PaintParent->IsVolatile()) : false;
	//}


	// If this widget clips to its bounds, then generate a new clipping rect representing the intersection of the bounding
	// rectangle of the widget's geometry, and the current clipping rectangle.
	bool bClipToBounds, bAlwaysClip, bIntersectClipBounds;

	FSlateRect CullingBounds = CalculateCullingAndClippingRules(AllottedGeometry, MyCullingRect, bClipToBounds, bAlwaysClip, bIntersectClipBounds);

	FWidgetStyle ContentWidgetStyle = FWidgetStyle(InWidgetStyle)
		.BlendOpacity(RenderOpacity);

	// Cache the geometry for tick to allow external users to get the last geometry that was used,
	// or would have been used to tick the Widget.
	FGeometry DesktopSpaceGeometry = AllottedGeometry;
	DesktopSpaceGeometry.AppendTransform(FSlateLayoutTransform(Args.GetWindowToDesktopTransform()));

	if (HasAnyUpdateFlags(EWidgetUpdateFlags::NeedsActiveTimerUpdate))
	{
		SCOPE_CYCLE_COUNTER(STAT_SlateExecuteActiveTimers);
		MutableThis->ExecuteActiveTimers(Args.GetCurrentTime(), Args.GetDeltaTime());
	}

	if (HasAnyUpdateFlags(EWidgetUpdateFlags::NeedsTick))
	{
		INC_DWORD_STAT(STAT_SlateNumTickedWidgets);

		SCOPE_CYCLE_COUNTER(STAT_SlateTickWidgets);
		MutableThis->Tick(DesktopSpaceGeometry, Args.GetCurrentTime(), Args.GetDeltaTime());
	}

	// the rule our parent has set for us
	const bool bInheritedHittestability = Args.GetInheritedHittestability();
	const bool bOutgoingHittestability = bInheritedHittestability && GetVisibility().AreChildrenHitTestVisible();

#if WITH_SLATE_DEBUGGING
	if (GDebugCulling)
	{
		// When we're debugging culling, don't actually clip, we'll just pretend to, so we can see the effects of
		// any widget doing culling to know if it's doing the right thing.
		bClipToBounds = false;
	}
#endif

	SWidget* PaintParentPtr = const_cast<SWidget*>(Args.GetPaintParent());
	ensure(PaintParentPtr != this);
	if (PaintParentPtr)
	{
		PersistentState.PaintParent = PaintParentPtr->AsShared();
	}
	else
	{
		PaintParentPtr = nullptr;
	}
	
	// @todo This should not do this copy if the clipping state is unset
	PersistentState.InitialClipState = OutDrawElements.GetClippingState();
	PersistentState.LayerId = LayerId;
	PersistentState.bParentEnabled = bParentEnabled;
	PersistentState.bInheritedHittestability = bInheritedHittestability;
	PersistentState.AllottedGeometry = AllottedGeometry;
	PersistentState.DesktopGeometry = DesktopSpaceGeometry;
	PersistentState.WidgetStyle = InWidgetStyle;
	PersistentState.CullingBounds = MyCullingRect;
	PersistentState.IncomingFlowDirection = GSlateFlowDirection;

	FPaintArgs UpdatedArgs = Args.WithNewParent(this);
	UpdatedArgs.SetInheritedHittestability(bOutgoingHittestability);


	// test ensure that we are not the last thing holding this widget together
	ensure(!MutableThis.IsUnique());


	if (!FastPathProxyHandle.IsValid() && PersistentState.CachedElementListNode != nullptr)
	{
		ensure(!bInvisibleDueToParentOrSelfVisibility);
	}

	OutDrawElements.PushPaintingWidget(*this, LayerId, PersistentState.CachedElementListNode);

	if (bOutgoingHittestability)
	{
		Args.GetHittestGrid().AddWidget(MutableThis, 0, LayerId, FastPathProxyHandle.GetIndex());
	}

	if (bClipToBounds)
	{
		// This sets up the clip state for any children NOT myself
		FSlateClippingZone ClippingZone(AllottedGeometry);
		ClippingZone.SetShouldIntersectParent(bIntersectClipBounds);
		ClippingZone.SetAlwaysClip(bAlwaysClip);
		OutDrawElements.PushClip(ClippingZone);
	}


#if WITH_SLATE_DEBUGGING
	FSlateDebugging::BeginWidgetPaint.Broadcast(this, UpdatedArgs, AllottedGeometry, CullingBounds, OutDrawElements, LayerId);
#endif

	// Establish the flow direction if we're changing from inherit.
	// FOR RB mode, this should first set GSlateFlowDirection to the incoming state that was cached for the widget, then paint
	// will override it here to reflow is needed.
	TGuardValue<EFlowDirection> FlowGuard(GSlateFlowDirection, ComputeFlowDirection());
	
	// Paint the geometry of this widget.
	int32 NewLayerId = OnPaint(UpdatedArgs, AllottedGeometry, CullingBounds, OutDrawElements, LayerId, ContentWidgetStyle, bParentEnabled);

	// Just repainted
	MutableThis->RemoveUpdateFlags(EWidgetUpdateFlags::NeedsRepaint);

#if WITH_SLATE_DEBUGGING
	FSlateDebugging::EndWidgetPaint.Broadcast(this, OutDrawElements, NewLayerId);

	if (GShowClipping && bClipToBounds)
	{
		FSlateClippingZone ClippingZone(AllottedGeometry);

		TArray<FVector2D> Points;
		Points.Add(ClippingZone.TopLeft);
		Points.Add(ClippingZone.TopRight);
		Points.Add(ClippingZone.BottomRight);
		Points.Add(ClippingZone.BottomLeft);
		Points.Add(ClippingZone.TopLeft);

		const bool bAntiAlias = true;
		FSlateDrawElement::MakeLines(
			OutDrawElements,
			NewLayerId,
			FPaintGeometry(),
			Points,
			ESlateDrawEffect::None,
			ClippingZone.IsAxisAligned() ? FLinearColor::Yellow : FLinearColor::Red,
			bAntiAlias,
			2.0f);
	}
#endif // WITH_SLATE_DEBUGGING

	if (bClipToBounds)
	{
		OutDrawElements.PopClip();
	}


#if PLATFORM_UI_NEEDS_FOCUS_OUTLINES
	// Check if we need to show the keyboard focus ring, this is only necessary if the widget could be focused.
	if (bCanSupportFocus && SupportsKeyboardFocus())
	{
		bool bShowUserFocus = FSlateApplicationBase::Get().ShowUserFocus(SharedThis(this));
		if (bShowUserFocus)
		{
			const FSlateBrush* BrushResource = GetFocusBrush();

			if (BrushResource != nullptr)
			{
				FSlateDrawElement::MakeBox(
					OutDrawElements,
					NewLayerId,
					AllottedGeometry.ToPaintGeometry(),
					BrushResource,
					ESlateDrawEffect::None,
					BrushResource->GetTint(InWidgetStyle)
				);
			}
		}
	}
#endif

	FSlateCachedElementListNode* NewCacheNode = OutDrawElements.PopPaintingWidget();
	if (OutDrawElements.ShouldResolveDeferred())
	{
		NewLayerId = OutDrawElements.PaintDeferred(NewLayerId, MyCullingRect);
	}

	MutableThis->UpdateWidgetProxy(NewLayerId, NewCacheNode);

	return NewLayerId;

}

float SWidget::GetRelativeLayoutScale(const FSlotBase& Child, float LayoutScaleMultiplier) const
{
	return 1.0f;
}

void SWidget::ArrangeChildren(const FGeometry& AllottedGeometry, FArrangedChildren& ArrangedChildren) const
{
#if WITH_VERY_VERBOSE_SLATE_STATS
	SCOPED_NAMED_EVENT(SWidget_ArrangeChildren, FColor::Black);
#endif
	OnArrangeChildren(AllottedGeometry, ArrangedChildren);
}

void SWidget::Prepass_Internal(float InLayoutScaleMultiplier)
{
	PrepassLayoutScaleMultiplier = InLayoutScaleMultiplier;

	bool bShouldPrepassChildren = true;
	if (bHasCustomPrepass)
	{
		bShouldPrepassChildren = CustomPrepass(InLayoutScaleMultiplier);
	}

	if (bCanHaveChildren && bShouldPrepassChildren)
	{
		// Cache child desired sizes first. This widget's desired size is
		// a function of its children's sizes.
		FChildren* MyChildren = this->GetChildren();
		const int32 NumChildren = MyChildren->Num();
		for (int32 ChildIndex = 0; ChildIndex < MyChildren->Num(); ++ChildIndex)
		{
			const TSharedRef<SWidget>& Child = MyChildren->GetChildAt(ChildIndex);

			if (Child->Visibility.Get() != EVisibility::Collapsed)
			{
				const float ChildLayoutScaleMultiplier = GetRelativeLayoutScale(MyChildren->GetSlotAt(ChildIndex), InLayoutScaleMultiplier);
				// Recur: Descend down the widget tree.
				Child->Prepass_Internal(InLayoutScaleMultiplier * ChildLayoutScaleMultiplier);
			}
		}
		ensure(NumChildren == MyChildren->Num());
	}

	{
		// Cache this widget's desired size.
		CacheDesiredSize(PrepassLayoutScaleMultiplier);
		bNeedsPrepass = false;
	}
}

TSharedRef<FActiveTimerHandle> SWidget::RegisterActiveTimer(float TickPeriod, FWidgetActiveTimerDelegate TickFunction)
{
	TSharedRef<FActiveTimerHandle> ActiveTimerHandle = MakeShareable(new FActiveTimerHandle(TickPeriod, TickFunction, FSlateApplicationBase::Get().GetCurrentTime() + TickPeriod));
	FSlateApplicationBase::Get().RegisterActiveTimer(ActiveTimerHandle);
	ActiveTimers.Add(ActiveTimerHandle);

	AddUpdateFlags(EWidgetUpdateFlags::NeedsActiveTimerUpdate);

	return ActiveTimerHandle;
}

void SWidget::UnRegisterActiveTimer(const TSharedRef<FActiveTimerHandle>& ActiveTimerHandle)
{
	if (FSlateApplicationBase::IsInitialized())
	{
		FSlateApplicationBase::Get().UnRegisterActiveTimer(ActiveTimerHandle);
		ActiveTimers.Remove(ActiveTimerHandle);

		if (ActiveTimers.Num() == 0)
		{
			RemoveUpdateFlags(EWidgetUpdateFlags::NeedsActiveTimerUpdate);
		}
	}
}

void SWidget::ExecuteActiveTimers(double CurrentTime, float DeltaTime)
{
	// loop over the registered tick handles and execute them, removing them if necessary.
	for (int32 i = 0; i < ActiveTimers.Num();)
	{
		EActiveTimerReturnType Result = ActiveTimers[i]->ExecuteIfPending(CurrentTime, DeltaTime);
		if (Result == EActiveTimerReturnType::Continue)
		{
			++i;
		}
		else
		{
			// Possible that execution unregistered the timer 
			if (ActiveTimers.IsValidIndex(i))
			{
				if (FSlateApplicationBase::IsInitialized())
				{
					FSlateApplicationBase::Get().UnRegisterActiveTimer(ActiveTimers[i]);
				}
				ActiveTimers.RemoveAt(i);
			}
		}
	}

	if (ActiveTimers.Num() == 0)
	{
		RemoveUpdateFlags(EWidgetUpdateFlags::NeedsActiveTimerUpdate);
	}
}

const FPointerEventHandler* SWidget::GetPointerEvent(const FName EventName) const
{
	auto* FoundPair = PointerEvents.FindByPredicate([&EventName](const auto& TestPair) {return TestPair.Key == EventName; });
	if (FoundPair)
	{
		return &FoundPair->Value;
	}
	return nullptr;
}

void SWidget::SetPointerEvent(const FName EventName, FPointerEventHandler& InEvent)
{
	// Find the event name and if found, replace the delegate
	auto* FoundPair = PointerEvents.FindByPredicate([&EventName](const auto& TestPair) {return TestPair.Key == EventName; });
	if (FoundPair)
	{
		FoundPair->Value = InEvent;
	}
	else
	{
		PointerEvents.Emplace(EventName, InEvent);
	}

}

void SWidget::SetOnMouseButtonDown(FPointerEventHandler EventHandler)
{
	SetPointerEvent(NAME_MouseButtonDown, EventHandler);
}

void SWidget::SetOnMouseButtonUp(FPointerEventHandler EventHandler)
{
	SetPointerEvent(NAME_MouseButtonUp, EventHandler);
}

void SWidget::SetOnMouseMove(FPointerEventHandler EventHandler)
{
	SetPointerEvent(NAME_MouseMove, EventHandler);
}

void SWidget::SetOnMouseDoubleClick(FPointerEventHandler EventHandler)
{
	SetPointerEvent(NAME_MouseDoubleClick, EventHandler);
}

void SWidget::SetOnMouseEnter(FNoReplyPointerEventHandler EventHandler)
{
	MouseEnterHandler = EventHandler;
}

void SWidget::SetOnMouseLeave(FSimpleNoReplyPointerEventHandler EventHandler)
{
	MouseLeaveHandler = EventHandler;
}

#if WITH_ACCESSIBILITY
TSharedRef<FSlateAccessibleWidget> SWidget::CreateAccessibleWidget()
{
	return MakeShareable<FSlateAccessibleWidget>(new FSlateAccessibleWidget(AsShared()));
}

void SWidget::SetAccessibleBehavior(EAccessibleBehavior InBehavior, const TAttribute<FText>& InText, EAccessibleType AccessibleType)
{
	EAccessibleBehavior& Behavior = (AccessibleType == EAccessibleType::Main) ? AccessibleData.AccessibleBehavior : AccessibleData.AccessibleSummaryBehavior;
	if (Behavior != InBehavior)
	{
		// If switching off of custom, revert back to default text
		if (Behavior == EAccessibleBehavior::Custom)
		{
			SetDefaultAccessibleText(AccessibleType);
		}
		else if (InBehavior == EAccessibleBehavior::Custom)
		{
			TAttribute<FText>& Text = (AccessibleType == EAccessibleType::Main) ? AccessibleData.AccessibleText : AccessibleData.AccessibleSummaryText;
			Text = InText;
		}
		const bool bWasAccessible = Behavior != EAccessibleBehavior::NotAccessible;
		Behavior = InBehavior;
		if (AccessibleType == EAccessibleType::Main && bWasAccessible != (Behavior != EAccessibleBehavior::NotAccessible))
		{
			FSlateApplicationBase::Get().GetAccessibleMessageHandler()->MarkDirty();
		}
	}
}

void SWidget::SetCanChildrenBeAccessible(bool InCanChildrenBeAccessible)
{
	if (AccessibleData.bCanChildrenBeAccessible != InCanChildrenBeAccessible)
	{
		AccessibleData.bCanChildrenBeAccessible = InCanChildrenBeAccessible;
		FSlateApplicationBase::Get().GetAccessibleMessageHandler()->MarkDirty();
	}
}

void SWidget::SetDefaultAccessibleText(EAccessibleType AccessibleType)
{
	TAttribute<FText>& Text = (AccessibleType == EAccessibleType::Main) ? AccessibleData.AccessibleText : AccessibleData.AccessibleSummaryText;
	Text = TAttribute<FText>();
}

FText SWidget::GetAccessibleText(EAccessibleType AccessibleType) const
{
	const EAccessibleBehavior Behavior = (AccessibleType == EAccessibleType::Main) ? AccessibleData.AccessibleBehavior : AccessibleData.AccessibleSummaryBehavior;
	const EAccessibleBehavior OtherBehavior = (AccessibleType == EAccessibleType::Main) ? AccessibleData.AccessibleSummaryBehavior : AccessibleData.AccessibleBehavior;
	const TAttribute<FText>& Text = (AccessibleType == EAccessibleType::Main) ? AccessibleData.AccessibleText : AccessibleData.AccessibleSummaryText;
	const TAttribute<FText>& OtherText = (AccessibleType == EAccessibleType::Main) ? AccessibleData.AccessibleSummaryText : AccessibleData.AccessibleText;

	switch (Behavior)
	{
	case EAccessibleBehavior::Custom:
		return Text.Get(FText::GetEmpty());
	case EAccessibleBehavior::Summary:
		return GetAccessibleSummary();
	case EAccessibleBehavior::ToolTip:
		if (ToolTip.IsValid() && !ToolTip->IsEmpty())
		{
			return ToolTip->GetContentWidget()->GetAccessibleText(EAccessibleType::Main);
		}
		break;
	case EAccessibleBehavior::Auto:
		// Auto first checks if custom text was set. This should never happen with user-defined values as custom should be
		// used instead in that case - however, this will be used for widgets with special default text such as TextBlocks.
		// If no text is found, then it will attempt to use the other variable's text, so that a developer can do things like
		// leave Summary on Auto, set Main to Custom, and have Summary automatically use Main's value without having to re-type it.
		if (Text.IsSet())
		{
			return Text.Get(FText::GetEmpty());
		}
		switch (OtherBehavior)
		{
		case EAccessibleBehavior::Custom:
		case EAccessibleBehavior::ToolTip:
			return GetAccessibleText(AccessibleType == EAccessibleType::Main ? EAccessibleType::Summary : EAccessibleType::Main);
		case EAccessibleBehavior::NotAccessible:
		case EAccessibleBehavior::Summary:
			return GetAccessibleSummary();
		}
		break;
	}
	return FText::GetEmpty();
}

FText SWidget::GetAccessibleSummary() const
{
	FTextBuilder Builder;
	FChildren* Children = const_cast<SWidget*>(this)->GetChildren();
	if (Children)
	{
		for (int32 i = 0; i < Children->Num(); ++i)
		{
			FText Text = Children->GetChildAt(i)->GetAccessibleText(EAccessibleType::Summary);
			if (!Text.IsEmpty())
			{
				Builder.AppendLine(Text);
			}
		}
	}
	return Builder.ToText();
}

bool SWidget::IsAccessible() const
{
	if (AccessibleData.AccessibleBehavior == EAccessibleBehavior::NotAccessible)
	{
		return false;
	}

	TSharedPtr<SWidget> Parent = GetParentWidget();
	while (Parent.IsValid())
	{
		if (!Parent->CanChildrenBeAccessible())
		{
			return false;
		}
		Parent = Parent->GetParentWidget();
	}
	return true;
}

EAccessibleBehavior SWidget::GetAccessibleBehavior(EAccessibleType AccessibleType) const
{
	return AccessibleType == EAccessibleType::Main ? AccessibleData.AccessibleBehavior : AccessibleData.AccessibleSummaryBehavior;
}

bool SWidget::CanChildrenBeAccessible() const
{
	return AccessibleData.bCanChildrenBeAccessible;
}

#endif

#if SLATE_CULL_WIDGETS

bool SWidget::IsChildWidgetCulled(const FSlateRect& MyCullingRect, const FArrangedWidget& ArrangedChild) const
{
	QUICK_SCOPE_CYCLE_COUNTER(Slate_IsChildWidgetCulled);

	// We add some slack fill to the culling rect to deal with the common occurrence
	// of widgets being larger than their root level widget is.  Happens when nested child widgets
	// inflate their rendering bounds to render beyond their parent (the child of this panel doing the culling), 
	// or using render transforms.  In either case, it introduces offsets to a bounding volume we don't 
	// actually know about or track in slate, so we have have two choices.
	//    1) Don't cull, set SLATE_CULL_WIDGETS to 0.
	//    2) Cull with a slack fill amount users can adjust.
	const FSlateRect CullingRectWithSlack = MyCullingRect.ScaleBy(GCullingSlackFillPercent);

	// 1) We check if the rendered bounding box overlaps with the culling rect.  Which is so that
	//    a render transformed element is never culled if it would have been visible to the user.
	if (FSlateRect::DoRectanglesIntersect(CullingRectWithSlack, ArrangedChild.Geometry.GetRenderBoundingRect()))
	{
		return false;
	}

	// 2) We also check the layout bounding box to see if it overlaps with the culling rect.  The
	//    reason for this is a bit more nuanced.  Suppose you dock a widget on the screen on the side
	//    and you want have it animate in and out of the screen.  Even though the layout transform 
	//    keeps the widget on the screen, the render transform alone would have caused it to be culled
	//    and therefore not ticked or painted.  The best way around this for now seems to be to simply
	//    check both rects to see if either one is overlapping the culling volume.
	if (FSlateRect::DoRectanglesIntersect(CullingRectWithSlack, ArrangedChild.Geometry.GetLayoutBoundingRect()))
	{
		return false;
	}

	// There's a special condition if the widget's clipping state is set does not intersect with clipping bounds, they in effect
	// will be setting a new culling rect, so let them pass being culling from this step.
	if (ArrangedChild.Widget->GetClipping() == EWidgetClipping::ClipToBoundsWithoutIntersecting)
	{
		return false;
	}

	return true;
}

#endif
