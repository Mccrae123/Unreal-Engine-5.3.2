// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rigs/RigHierarchy.h"
#include "ControlRig.h"

#include "Rigs/RigHierarchyElements.h"
#include "Rigs/RigHierarchyController.h"
#include "Units/RigUnitContext.h"
#include "Math/ControlRigMathLibrary.h"
#include "UObject/AnimObjectVersion.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

#if WITH_EDITOR
#include "RigVMPythonUtils.h"
#include "ScopedTransaction.h"
#include "HAL/PlatformStackWalk.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

static FCriticalSection GRigHierarchyStackTraceMutex;
static char GRigHierarchyStackTrace[65536];
static void RigHierarchyCaptureCallStack(FString& OutCallstack, uint32 NumCallsToIgnore)
{
	FScopeLock ScopeLock(&GRigHierarchyStackTraceMutex);
	GRigHierarchyStackTrace[0] = 0;
	FPlatformStackWalk::StackWalkAndDump(GRigHierarchyStackTrace, 65535, 1 + NumCallsToIgnore);
	OutCallstack = ANSI_TO_TCHAR(GRigHierarchyStackTrace);
}

// CVar to record all transform changes 
static TAutoConsoleVariable<int32> CVarControlRigHierarchyTraceAlways(TEXT("ControlRig.Hierarchy.TraceAlways"), 0, TEXT("if nonzero we will record all transform changes."));
static TAutoConsoleVariable<int32> CVarControlRigHierarchyTraceCallstack(TEXT("ControlRig.Hierarchy.TraceCallstack"), 0, TEXT("if nonzero we will record the callstack for any trace entry.\nOnly works if(ControlRig.Hierarchy.TraceEnabled != 0)"));
static TAutoConsoleVariable<int32> CVarControlRigHierarchyTracePrecision(TEXT("ControlRig.Hierarchy.TracePrecision"), 3, TEXT("sets the number digits in a float when tracing hierarchies."));
static TAutoConsoleVariable<int32> CVarControlRigHierarchyTraceOnSpawn(TEXT("ControlRig.Hierarchy.TraceOnSpawn"), 0, TEXT("sets the number of frames to trace when a new hierarchy is spawned"));
static int32 sRigHierarchyLastTrace = INDEX_NONE;
static TCHAR sRigHierarchyTraceFormat[16];

// A console command to trace a single frame / single execution for a control rig anim node / control rig component
FAutoConsoleCommandWithWorldAndArgs FCmdControlRigHierarchyTraceFrames
(
	TEXT("ControlRig.Hierarchy.Trace"),
	TEXT("Traces changes in a hierarchy for a provided number of executions (defaults to 1).\nYou can use ControlRig.Hierarchy.TraceCallstack to enable callstack tracing as part of this."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& InParams, UWorld* InWorld)
	{
		int32 NumFrames = 1;
		if(InParams.Num() > 0)
		{
			NumFrames = FCString::Atoi(*InParams[0]);
		}
		
		TArray<UObject*> Instances;
		URigHierarchy::StaticClass()->GetDefaultObject()->GetArchetypeInstances(Instances);

		for(UObject* Instance : Instances)
		{
			if (Instance->HasAnyFlags(RF_ClassDefaultObject))
			{
				continue;
			}
			
			// we'll just trace all of them for now
			//if(Instance->GetWorld() == InWorld)
			if(Instance->GetTypedOuter<UControlRig>() != nullptr)
			{
				CastChecked<URigHierarchy>(Instance)->TraceFrames(NumFrames);
			}
		}
	})
);

#endif

////////////////////////////////////////////////////////////////////////////////
// URigHierarchy
////////////////////////////////////////////////////////////////////////////////

const FRigBaseElementChildrenArray URigHierarchy::EmptyElementArray;

URigHierarchy::URigHierarchy()
: TopologyVersion(0)
, bEnableDirtyPropagation(true)
, Elements()
, IndexLookup()
, TransformStackIndex(0)
, bTransactingForTransformChange(false)
, bIsInteracting(false)
, LastInteractedKey()
, bSuspendNotifications(false)
, ResetPoseHash(INDEX_NONE)
#if WITH_EDITOR
, bPropagatingChange(false)
, bForcePropagation(false)
, TraceFramesLeft(0)
, TraceFramesCaptured(0)
#endif
{
	Reset();
#if WITH_EDITOR
	TraceFrames(CVarControlRigHierarchyTraceOnSpawn->GetInt());
#endif
}

URigHierarchy::~URigHierarchy()
{
	Reset();
}

void URigHierarchy::Serialize(FArchive& Ar)
{
	Ar.UsingCustomVersion(FAnimObjectVersion::GUID);

	if (Ar.IsSaving() || Ar.IsObjectReferenceCollector() || Ar.IsCountingMemory())
	{
		Save(Ar);
	}
	else if (Ar.IsLoading())
	{
		Load(Ar);
	}
	else
	{
		// remove due to FPIEFixupSerializer hitting this checkNoEntry();
	}
}

void URigHierarchy::Save(FArchive& Ar)
{
	if(Ar.IsTransacting())
	{
		Ar << TransformStackIndex;
		Ar << bTransactingForTransformChange;
		
		if(bTransactingForTransformChange)
		{
			return;
		}
	}

	// make sure all parts of pose are valid.
	// this ensures cache validity.
	ComputeAllTransforms();

	int32 ElementCount = Elements.Num();
	Ar << ElementCount;

	for(int32 ElementIndex = 0; ElementIndex < ElementCount; ElementIndex++)
	{
		FRigBaseElement* Element = Elements[ElementIndex];

		// store the key
		FRigElementKey Key = Element->GetKey();
		Ar << Key;

		// allow the element to store more information
		Element->Serialize(Ar, this, FRigBaseElement::StaticData);
	}

	for(int32 ElementIndex = 0; ElementIndex < ElementCount; ElementIndex++)
	{
		FRigBaseElement* Element = Elements[ElementIndex];
		Element->Serialize(Ar, this, FRigBaseElement::InterElementData);
	}
}

void URigHierarchy::Load(FArchive& Ar)
{
	if(Ar.IsTransacting())
	{
		bool bOnlySerializedTransformStackIndex = false;
		Ar << TransformStackIndex;
		Ar << bOnlySerializedTransformStackIndex;
		
		if(bOnlySerializedTransformStackIndex)
		{
			return;
		}
	}

	Reset();

	int32 ElementCount = 0;
	Ar << ElementCount;

	for(int32 ElementIndex = 0; ElementIndex < ElementCount; ElementIndex++)
	{
		FRigElementKey Key;
		Ar << Key;

		FRigBaseElement* Element = MakeElement(Key.Type);
		check(Element);

		Element->SubIndex = Num(Key.Type);
		Element->Index = Elements.Add(Element);
		ElementsPerType[RigElementTypeToFlatIndex(Key.Type)].Add(Element);
		IndexLookup.Add(Key, Element->Index);
		
		Element->Load(Ar, this, FRigBaseElement::StaticData);
	}

	TopologyVersion++;

	for(int32 ElementIndex = 0; ElementIndex < ElementCount; ElementIndex++)
	{
		FRigBaseElement* Element = Elements[ElementIndex];
		Element->Load(Ar, this, FRigBaseElement::InterElementData);
	}

	TopologyVersion++;

	for(int32 ElementIndex = 0; ElementIndex < ElementCount; ElementIndex++)
	{
		if(FRigTransformElement* TransformElement = Cast<FRigTransformElement>(Elements[ElementIndex]))
		{
#if URIGHIERARCHY_RECURSIVE_DIRTY_PROPAGATION
			FRigBaseElementParentArray CurrentParents = GetParents(TransformElement, false);
#else
			FRigBaseElementParentArray CurrentParents = GetParents(TransformElement, true);
#endif
			for (FRigBaseElement* CurrentParent : CurrentParents)
			{
				if(FRigTransformElement* TransformParent = Cast<FRigTransformElement>(CurrentParent))
				{
					TransformParent->ElementsToDirty.AddUnique(TransformElement);
				}
			}
		}
	}

	UpdateAllCachedChildren();
	Notify(ERigHierarchyNotification::HierarchyReset, nullptr);
}

void URigHierarchy::PostLoad()
{
	UObject::PostLoad();

	struct Local
	{
		static bool NeedsCheck(const FRigLocalAndGlobalTransform& InTransform)
		{
			return !InTransform.Local.bDirty && !InTransform.Global.bDirty;
		}
	};

	// we need to check the elements for integrity (global vs local) to be correct.
	for(int32 ElementIndex = 0; ElementIndex < Elements.Num(); ElementIndex++)
	{
		FRigBaseElement* BaseElement = Elements[ElementIndex];

		if(FRigControlElement* ControlElement = Cast<FRigControlElement>(BaseElement))
		{
			if(Local::NeedsCheck(ControlElement->Offset.Initial))
			{
				const FTransform ComputedGlobalTransform = SolveParentConstraints(
					ControlElement->ParentConstraints, ERigTransformType::InitialGlobal,
					ControlElement->Offset.Get(ERigTransformType::InitialLocal), true,
					FTransform::Identity, false);

				const FTransform CachedGlobalTransform = ControlElement->Offset.Get(ERigTransformType::InitialGlobal);
				if(!FRigComputedTransform::Equals(ComputedGlobalTransform, CachedGlobalTransform, 0.01))
				{
					ControlElement->Offset.MarkDirty(ERigTransformType::InitialGlobal);
				}
			}

			if(Local::NeedsCheck(ControlElement->Pose.Initial))
			{
				const FTransform ComputedGlobalTransform = SolveParentConstraints(
					ControlElement->ParentConstraints, ERigTransformType::InitialGlobal,
					GetControlOffsetTransform(ControlElement, ERigTransformType::InitialGlobal), true,
					ControlElement->Pose.Get(ERigTransformType::InitialLocal), true);
				
				const FTransform CachedGlobalTransform = ControlElement->Pose.Get(ERigTransformType::InitialGlobal);
				
				if(!FRigComputedTransform::Equals(ComputedGlobalTransform, CachedGlobalTransform, 0.01))
				{
					// for nulls we perceive the local transform as less relevant
					ControlElement->Pose.MarkDirty(ERigTransformType::InitialLocal);
				}
			}

			// we also need to check the pose here - for controls it is a bit different than for other
			// types.
			continue;
		}

		if(FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(BaseElement))
		{
			if(Local::NeedsCheck(MultiParentElement->Pose.Initial))
			{
				const FTransform ComputedGlobalTransform = SolveParentConstraints(
					MultiParentElement->ParentConstraints, ERigTransformType::InitialGlobal,
					FTransform::Identity, false,
					MultiParentElement->Pose.Get(ERigTransformType::InitialLocal), true);
				
				const FTransform CachedGlobalTransform = MultiParentElement->Pose.Get(ERigTransformType::InitialGlobal);
				
				if(!FRigComputedTransform::Equals(ComputedGlobalTransform, CachedGlobalTransform, 0.01))
				{
					// for nulls we perceive the local transform as less relevant
					MultiParentElement->Pose.MarkDirty(ERigTransformType::InitialLocal);
				}
			}
		}

		if(FRigTransformElement* TransformElement = Cast<FRigTransformElement>(BaseElement))
		{
			if(Local::NeedsCheck(TransformElement->Pose.Initial))
			{
				const FTransform ParentTransform = GetParentTransform(TransformElement, ERigTransformType::InitialGlobal);
				const FTransform ComputedGlobalTransform = TransformElement->Pose.Get(ERigTransformType::InitialLocal) * ParentTransform;
				const FTransform CachedGlobalTransform = TransformElement->Pose.Get(ERigTransformType::InitialGlobal);
				if(!FRigComputedTransform::Equals(ComputedGlobalTransform, CachedGlobalTransform, 0.01))
				{
					TransformElement->Pose.MarkDirty(ERigTransformType::InitialGlobal);
				}
			}
		}
	}
}

void URigHierarchy::Reset()
{
	TopologyVersion = 0;
	bEnableDirtyPropagation = true;

	// walk in reverse since certain elements might not have been allocated themselves
	for(int32 ElementIndex = Elements.Num() - 1; ElementIndex >= 0; ElementIndex--)
	{
		DestroyElement(Elements[ElementIndex]);
	}
	Elements.Reset();
	ElementsPerType.Reset();
	for(int32 TypeIndex=0;TypeIndex<RigElementTypeToFlatIndex(ERigElementType::Last);TypeIndex++)
	{
		ElementsPerType.Add(TArray<FRigBaseElement*>());
	}
	IndexLookup.Reset();

	ResetPoseHash = INDEX_NONE;
	ResetPoseHasFilteredChildren.Reset();

	if(!IsGarbageCollecting())
	{
		Notify(ERigHierarchyNotification::HierarchyReset, nullptr);
	}
}

void URigHierarchy::CopyHierarchy(URigHierarchy* InHierarchy)
{
	check(InHierarchy);
	
	Reset();

	// Allocate the elements in batches to improve performance
	TArray<uint8*> NewElementsPerType;
	TArray<int32> StructureSizePerType;
	for(int32 ElementTypeIndex = 0; ElementTypeIndex < InHierarchy->ElementsPerType.Num(); ElementTypeIndex++)
	{
		const ERigElementType ElementType = FlatIndexToRigElementType(ElementTypeIndex);
		int32 StructureSize = 0;

		const int32 Count = InHierarchy->ElementsPerType[ElementTypeIndex].Num();
		if(Count)
		{
			FRigBaseElement* ElementMemory = MakeElement(ElementType, Count, &StructureSize);
			NewElementsPerType.Add((uint8*)ElementMemory);
		}
		else
		{
			NewElementsPerType.Add(nullptr);
		}

		StructureSizePerType.Add(StructureSize);
		ElementsPerType[ElementTypeIndex].Reserve(Count);
	}

	Elements.Reserve(InHierarchy->Elements.Num());
	IndexLookup.Reserve(InHierarchy->IndexLookup.Num());
	
	for(int32 Index = 0; Index < InHierarchy->Num(); Index++)
	{
		FRigBaseElement* Source = InHierarchy->Get(Index);
		const FRigElementKey& Key = Source->Key;

		const int32 ElementTypeIndex = RigElementTypeToFlatIndex(Key.Type);
		
		const int32 SubIndex = Num(Key.Type);

		const int32 StructureSize = StructureSizePerType[ElementTypeIndex];
		check(NewElementsPerType[ElementTypeIndex] != nullptr);
		FRigBaseElement* Target = (FRigBaseElement*)&NewElementsPerType[ElementTypeIndex][StructureSize * SubIndex];
		//FRigBaseElement* Target = MakeElement(Key.Type);
		
		Target->Key = Key;
		Target->SubIndex = SubIndex;
		Target->Index = Elements.Add(Target);

		ElementsPerType[ElementTypeIndex].Add(Target);
		IndexLookup.Add(Key, Target->Index);

		check(Source->Index == Index);
		check(Target->Index == Index);
	}

	for(int32 Index = 0; Index < InHierarchy->Num(); Index++)
	{
		FRigBaseElement* Source = InHierarchy->Get(Index);
		FRigBaseElement* Target = Elements[Index];
		Target->CopyFrom(this, Source, InHierarchy);
	}

	TopologyVersion = InHierarchy->GetTopologyVersion();
	UpdateAllCachedChildren();
}

#if WITH_EDITOR
void URigHierarchy::RegisterListeningHierarchy(URigHierarchy* InHierarchy)
{
	if (InHierarchy)
	{
		bool bFoundListener = false;
		for(int32 ListenerIndex = ListeningHierarchies.Num() - 1; ListenerIndex >= 0; ListenerIndex--)
		{
			const URigHierarchy::FRigHierarchyListener& Listener = ListeningHierarchies[ListenerIndex];
			if(Listener.Hierarchy.IsValid())
			{
				if(Listener.Hierarchy.Get() == InHierarchy)
				{
					bFoundListener = true;
					break;
				}
			}
		}

		if(!bFoundListener)
		{
			URigHierarchy::FRigHierarchyListener Listener;
			Listener.Hierarchy = InHierarchy; 
			ListeningHierarchies.Add(Listener);
		}
	}
}

void URigHierarchy::UnregisterListeningHierarchy(URigHierarchy* InHierarchy)
{
	if (InHierarchy)
	{
		for(int32 ListenerIndex = ListeningHierarchies.Num() - 1; ListenerIndex >= 0; ListenerIndex--)
		{
			const URigHierarchy::FRigHierarchyListener& Listener = ListeningHierarchies[ListenerIndex];
			if(Listener.Hierarchy.IsValid())
			{
				if(Listener.Hierarchy.Get() == InHierarchy)
				{
					ListeningHierarchies.RemoveAt(ListenerIndex);
				}
			}
		}
	}
}

void URigHierarchy::ClearListeningHierarchy()
{
	ListeningHierarchies.Reset();
}
#endif

void URigHierarchy::CopyPose(URigHierarchy* InHierarchy, bool bCurrent, bool bInitial)
{
	check(InHierarchy);

	for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ElementIndex++)
	{
		FRigBaseElement* Element = Elements[ElementIndex];
		if(FRigBaseElement* OtherElement = InHierarchy->Find(Element->GetKey()))
		{
			Element->CopyPose(OtherElement, bCurrent, bInitial);
		}
	}
}

void URigHierarchy::UpdateSockets(const FRigUnitContext* InContext)
{
	check(InContext);
	
	for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ElementIndex++)
	{
		if(FRigSocketElement* Socket = Cast<FRigSocketElement>(Elements[ElementIndex]))
		{
			const FTransform InitialWorldTransform = Socket->GetSocketWorldTransform(InContext, true);
			const FTransform CurrentWorldTransform = Socket->GetSocketWorldTransform(InContext, false);

			const FTransform InitialGlobalTransform = InitialWorldTransform.GetRelativeTransform(InContext->ToWorldSpaceTransform);
			const FTransform CurrentGlobalTransform = CurrentWorldTransform.GetRelativeTransform(InContext->ToWorldSpaceTransform);

			const FTransform InitialParentTransform = GetParentTransform(Socket, ERigTransformType::InitialGlobal); 
			const FTransform CurrentParentTransform = GetParentTransform(Socket, ERigTransformType::CurrentGlobal);

			const FTransform InitialLocalTransform = InitialGlobalTransform.GetRelativeTransform(InitialParentTransform);
			const FTransform CurrentLocalTransform = CurrentGlobalTransform.GetRelativeTransform(CurrentParentTransform);

			SetTransform(Socket, InitialLocalTransform, ERigTransformType::InitialLocal, true, false);
			SetTransform(Socket, CurrentLocalTransform, ERigTransformType::CurrentLocal, true, false);
		}
	}
}

void URigHierarchy::ResetPoseToInitial(ERigElementType InTypeFilter)
{
	bool bPerformFiltering = InTypeFilter != ERigElementType::All;

	// if we are resetting the pose on some elements, we need to check if
	// any of affected elements has any children that would not be affected
	// by resetting the pose. if all children are affected we can use the
	// fast path.
	if(bPerformFiltering)
	{
		const int32 Hash = HashCombine(GetTopologyVersion(), (int32)InTypeFilter);
		if(Hash != ResetPoseHash)
		{
			ResetPoseHasFilteredChildren.Reset();
			ResetPoseHash = Hash;

			// let's look at all elements and mark all parent of unaffected children
			bool bHitAnyParentWithFilteredChildren = false;
			ResetPoseHasFilteredChildren.AddZeroed(Elements.Num());

			Traverse([this, &bHitAnyParentWithFilteredChildren, InTypeFilter](FRigBaseElement* InElement, bool& bContinue)
			{
				bContinue = true;

				const bool bFilteredOut = (!InElement->IsTypeOf(InTypeFilter)) || ResetPoseHasFilteredChildren[InElement->Index];
				if(bFilteredOut)
				{
					const FRigBaseElementParentArray Parents = GetParents(InElement);
					for(const FRigBaseElement* Parent : Parents)
					{
						// only mark this up if the parent is not filtered out / 
						// if we want the parent to reset its pose to initial.
						if(Parent->IsTypeOf(InTypeFilter))
						{
							bHitAnyParentWithFilteredChildren = true;
						}
						ResetPoseHasFilteredChildren[Parent->GetIndex()] = true;
					}
				}

			}, false);

			if(!bHitAnyParentWithFilteredChildren)
			{
				ResetPoseHasFilteredChildren.Reset();
			}
		}

		// if the per element state is empty
		// it means that the filter doesn't affect 
		if(ResetPoseHasFilteredChildren.IsEmpty())
		{
			bPerformFiltering = false;
		}
	}
	
	for(int32 ElementIndex=0; ElementIndex<Elements.Num(); ElementIndex++)
	{
		bool bHasFilteredChildren = bPerformFiltering;
		if(bHasFilteredChildren)
		{
			bHasFilteredChildren = ResetPoseHasFilteredChildren[ElementIndex];
		}
		
		if(!Elements[ElementIndex]->IsTypeOf(InTypeFilter))
		{
			continue;
		}

		if(FRigControlElement* ControlElement = Cast<FRigControlElement>(Elements[ElementIndex]))
		{
			if(bHasFilteredChildren)
			{
				const FTransform OffsetTransform = GetControlOffsetTransform(ControlElement, ERigTransformType::InitialLocal);
				SetControlOffsetTransform(ControlElement, OffsetTransform, ERigTransformType::CurrentLocal, true, false, true);
				const FTransform GizmoTransform = GetControlGizmoTransform(ControlElement, ERigTransformType::InitialLocal);
				SetControlGizmoTransform(ControlElement, GizmoTransform, ERigTransformType::CurrentLocal, false, true);
			}
			else
			{
				ControlElement->Offset.Current = ControlElement->Offset.Initial;
				ControlElement->Gizmo.Current = ControlElement->Gizmo.Initial;
			}
		}

		if(FRigTransformElement* TransformElement = Cast<FRigTransformElement>(Elements[ElementIndex]))
		{
			if(bHasFilteredChildren)
			{
				const FTransform Transform = GetTransform(TransformElement, ERigTransformType::InitialLocal);
				SetTransform(TransformElement, Transform, ERigTransformType::CurrentLocal, true);
			}
			else
			{
				TransformElement->Pose.Current = TransformElement->Pose.Initial;
			}
		}

		if(FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(Elements[ElementIndex]))
		{
			if(bHasFilteredChildren)
			{
				MultiParentElement->Parent.MarkDirty(ERigTransformType::CurrentGlobal);
			}
			else
			{
				MultiParentElement->Parent.Current = MultiParentElement->Parent.Initial;
			}
		}
	}
}

void URigHierarchy::ResetCurveValues()
{
	for(int32 ElementIndex=0; ElementIndex<Elements.Num(); ElementIndex++)
	{
		if(FRigCurveElement* CurveElement = Cast<FRigCurveElement>(Elements[ElementIndex]))
		{
			SetCurveValue(CurveElement, 0.f);
		}
	}
}

int32 URigHierarchy::Num(ERigElementType InElementType) const
{
	return ElementsPerType[RigElementTypeToFlatIndex(InElementType)].Num();
}

TArray<FRigBaseElement*> URigHierarchy::GetSelectedElements(ERigElementType InTypeFilter) const
{
	TArray<FRigBaseElement*> Selection;

	if(URigHierarchy* HierarchyForSelection = HierarchyForSelectionPtr.Get())
	{
		TArray<FRigElementKey> SelectedKeys = HierarchyForSelection->GetSelectedKeys(InTypeFilter);
		for(const FRigElementKey& SelectedKey : SelectedKeys)
		{
			if(const FRigBaseElement* Element = Find(SelectedKey))
			{
				Selection.Add((FRigBaseElement*)Element);
			}
		}
		return Selection;
	}

	for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ElementIndex++)
	{
		FRigBaseElement* Element = Elements[ElementIndex];
		if(Element->IsTypeOf(InTypeFilter))
		{
			if(IsSelected(Element))
			{
				Selection.Add(Element);
			}
		}
	}
	return Selection;
}

TArray<FRigElementKey> URigHierarchy::GetSelectedKeys(ERigElementType InTypeFilter) const
{
	if(URigHierarchy* HierarchyForSelection = HierarchyForSelectionPtr.Get())
	{
		return HierarchyForSelection->GetSelectedKeys(InTypeFilter);
	}

	TArray<FRigElementKey> Selection;
	for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ElementIndex++)
	{
		FRigBaseElement* Element = Elements[ElementIndex];
		if(Element->IsTypeOf(InTypeFilter))
		{
			if(IsSelected(Element))
			{
				Selection.Add(Element->GetKey());
			}
		}
	}
	return Selection;
}

void URigHierarchy::SanitizeName(FString& InOutName)
{
	// Sanitize the name
	for (int32 i = 0; i < InOutName.Len(); ++i)
	{
		TCHAR& C = InOutName[i];

		const bool bGoodChar = FChar::IsAlpha(C) ||							// Any letter
			(C == '_') || (C == '-') || (C == '.') ||						// _  - . anytime
			((i > 0) && (FChar::IsDigit(C)));								// 0-9 after the first character

		if (!bGoodChar)
		{
			C = '_';
		}
	}

	if (InOutName.Len() > GetMaxNameLength())
	{
		InOutName.LeftChopInline(InOutName.Len() - GetMaxNameLength());
	}
}

FName URigHierarchy::GetSanitizedName(const FString& InName)
{
	FString Name = InName;
	SanitizeName(Name);

	if (Name.IsEmpty())
	{
		return NAME_None;
	}

	return *Name;
}

bool URigHierarchy::IsNameAvailable(const FString& InPotentialNewName, ERigElementType InType, FString* OutErrorMessage) const
{
	FString UnsanitizedName = InPotentialNewName;
	if (UnsanitizedName.Len() > GetMaxNameLength())
	{
		if (OutErrorMessage)
		{
			*OutErrorMessage = TEXT("Name too long.");
		}
		return false;
	}

	FString SanitizedName = UnsanitizedName;
	SanitizeName(SanitizedName);

	if (SanitizedName != UnsanitizedName)
	{
		if (OutErrorMessage)
		{
			*OutErrorMessage = TEXT("Name contains invalid characters.");
		}
		return false;
	}

	if (GetIndex(FRigElementKey(*InPotentialNewName, InType)) != INDEX_NONE)
	{
		if (OutErrorMessage)
		{
			*OutErrorMessage = TEXT("Name already used.");
		}
		return false;
	}

	return true;
}

FName URigHierarchy::GetSafeNewName(const FString& InPotentialNewName, ERigElementType InType) const
{
	FString SanitizedName = InPotentialNewName;
	SanitizeName(SanitizedName);
	FString Name = SanitizedName;

	int32 Suffix = 1;
	while (!IsNameAvailable(Name, InType))
	{
		FString BaseString = SanitizedName;
		if (BaseString.Len() > GetMaxNameLength() - 4)
		{
			BaseString.LeftChopInline(BaseString.Len() - (GetMaxNameLength() - 4));
		}
		Name = *FString::Printf(TEXT("%s_%d"), *BaseString, ++Suffix);
	}
	return *Name;
}

FEdGraphPinType URigHierarchy::GetControlPinType(FRigControlElement* InControlElement) const
{
	check(InControlElement);

	// local copy of UEdGraphSchema_K2::PC_ ... static members
	static const FName PC_Boolean(TEXT("bool"));
	static const FName PC_Float(TEXT("float"));
	static const FName PC_Int(TEXT("int"));
	static const FName PC_Struct(TEXT("struct"));

	FEdGraphPinType PinType;

	switch(InControlElement->Settings.ControlType)
	{
		case ERigControlType::Bool:
		{
			PinType.PinCategory = PC_Boolean;
			break;
		}
		case ERigControlType::Float:
		{
			PinType.PinCategory = PC_Float;
			break;
		}
		case ERigControlType::Integer:
		{
			PinType.PinCategory = PC_Int;
			break;
		}
		case ERigControlType::Vector2D:
		{
			PinType.PinCategory = PC_Struct;
			PinType.PinSubCategoryObject = TBaseStructure<FVector2D>::Get();
			break;
		}
		case ERigControlType::Position:
		case ERigControlType::Scale:
		{
			PinType.PinCategory = PC_Struct;
			PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
			break;
		}
		case ERigControlType::Rotator:
		{
			PinType.PinCategory = PC_Struct;
			PinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
			break;
		}
		case ERigControlType::Transform:
		case ERigControlType::TransformNoScale:
		case ERigControlType::EulerTransform:
		{
			PinType.PinCategory = PC_Struct;
			PinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
			break;
		}
	}

	return PinType;
}

FString URigHierarchy::GetControlPinDefaultValue(FRigControlElement* InControlElement, bool bForEdGraph, ERigControlValueType InValueType) const
{
	check(InControlElement);

	FRigControlValue Value = GetControlValue(InControlElement, InValueType);
	switch(InControlElement->Settings.ControlType)
	{
		case ERigControlType::Bool:
		{
			return Value.ToString<bool>();
		}
		case ERigControlType::Float:
		{
			return Value.ToString<float>();
		}
		case ERigControlType::Integer:
		{
			return Value.ToString<int32>();
		}
		case ERigControlType::Vector2D:
		{
			if(bForEdGraph)
			{
				const FVector3f Vector = Value.Get<FVector3f>();
				return FVector2D(Vector.X, Vector.Y).ToString();
			}
			return Value.ToString<FVector2D>();
		}
		case ERigControlType::Position:
		case ERigControlType::Scale:
		{
			if(bForEdGraph)
			{
				return FVector(Value.Get<FVector3f>()).ToString();
			}
			return Value.ToString<FVector>();
		}
		case ERigControlType::Rotator:
		{
				if(bForEdGraph)
				{
					const FRotator Rotator = FRotator::MakeFromEuler(Value.GetRef<FVector3f>());
					return Rotator.ToString();
				}
				return Value.ToString<FRotator>();
		}
		case ERigControlType::Transform:
		case ERigControlType::TransformNoScale:
		case ERigControlType::EulerTransform:
		{
			const FTransform Transform = Value.GetAsTransform(
				InControlElement->Settings.ControlType,
				InControlElement->Settings.PrimaryAxis);
				
			if(bForEdGraph)
			{
				return Transform.ToString();
			}

			FString Result;
			TBaseStructure<FTransform>::Get()->ExportText(Result, &Transform, nullptr, nullptr, PPF_None, nullptr);
			return Result;
		}
	}
	return FString();
}

TArray<FRigElementKey> URigHierarchy::GetChildren(FRigElementKey InKey, bool bRecursive) const
{
	FRigBaseElementChildrenArray LocalChildren;
	const FRigBaseElementChildrenArray* ChildrenPtr = nullptr;
	if(bRecursive)
	{
		LocalChildren = GetChildren(Find(InKey), true);
		ChildrenPtr = & LocalChildren;
	}
	else
	{
		ChildrenPtr = &GetChildren(Find(InKey));
	}

	const FRigBaseElementChildrenArray& Children = *ChildrenPtr;

	TArray<FRigElementKey> Keys;
	for(const FRigBaseElement* Child : Children)
	{
		Keys.Add(Child->Key);
	}
	return Keys;
}

TArray<int32> URigHierarchy::GetChildren(int32 InIndex, bool bRecursive) const
{
	FRigBaseElementChildrenArray LocalChildren;
	const FRigBaseElementChildrenArray* ChildrenPtr = nullptr;
	if(bRecursive)
	{
		LocalChildren = GetChildren(Get(InIndex), true);
		ChildrenPtr = & LocalChildren;
	}
	else
	{
		ChildrenPtr = &GetChildren(Get(InIndex));
	}

	const FRigBaseElementChildrenArray& Children = *ChildrenPtr;

	TArray<int32> Indices;
	for(const FRigBaseElement* Child : Children)
	{
		Indices.Add(Child->Index);
	}
	return Indices;
}

const FRigBaseElementChildrenArray& URigHierarchy::GetChildren(const FRigBaseElement* InElement) const
{
	if(InElement)
	{
		UpdateCachedChildren(InElement);
		return InElement->CachedChildren;
	}
	return EmptyElementArray;
}

FRigBaseElementChildrenArray URigHierarchy::GetChildren(const FRigBaseElement* InElement, bool bRecursive) const
{
	// call the non-recursive variation
	FRigBaseElementChildrenArray Children = GetChildren(InElement);
	
	if(bRecursive)
	{
		for(int32 ChildIndex = 0; ChildIndex < Children.Num(); ChildIndex++)
		{
			Children.Append(GetChildren(Children[ChildIndex], true));
		}
	}

	return Children;
}

TArray<FRigElementKey> URigHierarchy::GetParents(FRigElementKey InKey, bool bRecursive) const
{
	const FRigBaseElementParentArray& Parents = GetParents(Find(InKey), bRecursive);
	TArray<FRigElementKey> Keys;
	for(const FRigBaseElement* Parent : Parents)
	{
		Keys.Add(Parent->Key);
	}
	return Keys;
}

TArray<int32> URigHierarchy::GetParents(int32 InIndex, bool bRecursive) const
{
	const FRigBaseElementParentArray& Parents = GetParents(Get(InIndex), bRecursive);
	TArray<int32> Indices;
	for(const FRigBaseElement* Parent : Parents)
	{
		Indices.Add(Parent->Index);
	}
	return Indices;
}

FRigBaseElementParentArray URigHierarchy::GetParents(const FRigBaseElement* InElement, bool bRecursive) const
{
	FRigBaseElementParentArray Parents;

	if(const FRigSingleParentElement* SingleParentElement = Cast<FRigSingleParentElement>(InElement))
	{
		if(SingleParentElement->ParentElement)
		{
			Parents.Add(SingleParentElement->ParentElement);
		}
	}
	else if(const FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(InElement))
	{
		Parents.Reserve(MultiParentElement->ParentConstraints.Num());
		for(const FRigElementParentConstraint& ParentConstraint : MultiParentElement->ParentConstraints)
		{
			Parents.Add(ParentConstraint.ParentElement);
		}
	}

	if(bRecursive)
	{
		const int32 CurrentNumberParents = Parents.Num();
		for(int32 ParentIndex = 0;ParentIndex < CurrentNumberParents; ParentIndex++)
		{
			const FRigBaseElementParentArray GrandParents = GetParents(Parents[ParentIndex], bRecursive);
			for (FRigBaseElement* GrandParent : GrandParents)
			{
				Parents.AddUnique(GrandParent);
			}
		}
	}

	return Parents;
}

FRigElementKey URigHierarchy::GetFirstParent(FRigElementKey InKey) const
{
	if(FRigBaseElement* FirstParent = GetFirstParent(Find(InKey)))
	{
		return FirstParent->Key;
	}
	return FRigElementKey();
}

int32 URigHierarchy::GetFirstParent(int32 InIndex) const
{
	if(FRigBaseElement* FirstParent = GetFirstParent(Get(InIndex)))
	{
		return FirstParent->Index;
	}
	return INDEX_NONE;
}

FRigBaseElement* URigHierarchy::GetFirstParent(const FRigBaseElement* InElement) const
{
	if(const FRigSingleParentElement* SingleParentElement = Cast<FRigSingleParentElement>(InElement))
	{
		return SingleParentElement->ParentElement;
	}
	else if(const FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(InElement))
	{
		if(MultiParentElement->ParentConstraints.Num() > 0)
		{
			return MultiParentElement->ParentConstraints[0].ParentElement;
		}
	}
	
	return nullptr;
}

int32 URigHierarchy::GetNumberOfParents(FRigElementKey InKey) const
{
	return GetNumberOfParents(Find(InKey));
}

int32 URigHierarchy::GetNumberOfParents(int32 InIndex) const
{
	return GetNumberOfParents(Get(InIndex));
}

int32 URigHierarchy::GetNumberOfParents(const FRigBaseElement* InElement) const
{
	if(InElement == nullptr)
	{
		return 0;
	}

	if(const FRigSingleParentElement* SingleParentElement = Cast<FRigSingleParentElement>(InElement))
	{
		return SingleParentElement->ParentElement == nullptr ? 0 : 1;
	}
	else if(const FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(InElement))
	{
		return MultiParentElement->ParentConstraints.Num();
	}

	return 0;
}

FRigElementWeight URigHierarchy::GetParentWeight(FRigElementKey InChild, FRigElementKey InParent, bool bInitial) const
{
	return GetParentWeight(Find(InChild), Find(InParent), bInitial);
}

FRigElementWeight URigHierarchy::GetParentWeight(const FRigBaseElement* InChild, const FRigBaseElement* InParent, bool bInitial) const
{
	if(const FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(InChild))
	{
		if(const int32* ParentIndexPtr = MultiParentElement->IndexLookup.Find(InParent->GetKey()))
		{
			return GetParentWeight(InChild, *ParentIndexPtr, bInitial);
		}
	}
	return FRigElementWeight(FLT_MAX);
}

FRigElementWeight URigHierarchy::GetParentWeight(const FRigBaseElement* InChild, int32 InParentIndex, bool bInitial) const
{
	if(const FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(InChild))
	{
		if(MultiParentElement->ParentConstraints.IsValidIndex(InParentIndex))
		{
			if(bInitial)
			{
				return MultiParentElement->ParentConstraints[InParentIndex].InitialWeight;
			}
			else
			{
				return MultiParentElement->ParentConstraints[InParentIndex].Weight;
			}
		}
	}
	return FRigElementWeight(FLT_MAX);
}

TArray<FRigElementWeight> URigHierarchy::GetParentWeightArray(FRigElementKey InChild, bool bInitial) const
{
	return GetParentWeightArray(Find(InChild), bInitial);
}

TArray<FRigElementWeight> URigHierarchy::GetParentWeightArray(const FRigBaseElement* InChild, bool bInitial) const
{
	TArray<FRigElementWeight> Weights;
	if(const FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(InChild))
	{
		for(int32 ParentIndex = 0; ParentIndex < MultiParentElement->ParentConstraints.Num(); ParentIndex++)
		{
			if(bInitial)
			{
				Weights.Add(MultiParentElement->ParentConstraints[ParentIndex].InitialWeight);
			}
			else
			{
				Weights.Add(MultiParentElement->ParentConstraints[ParentIndex].Weight);
			}
		}
	}
	return Weights;
}

bool URigHierarchy::SetParentWeight(FRigElementKey InChild, FRigElementKey InParent, FRigElementWeight InWeight, bool bInitial, bool bAffectChildren)
{
	return SetParentWeight(Find(InChild), Find(InParent), InWeight, bInitial, bAffectChildren);
}

bool URigHierarchy::SetParentWeight(FRigBaseElement* InChild, const FRigBaseElement* InParent, FRigElementWeight InWeight, bool bInitial, bool bAffectChildren)
{
	if(const FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(InChild))
	{
		if(const int32* ParentIndexPtr = MultiParentElement->IndexLookup.Find(InParent->GetKey()))
		{
			return SetParentWeight(InChild, *ParentIndexPtr, InWeight, bInitial, bAffectChildren);
		}
	}
	return false;
}

bool URigHierarchy::SetParentWeight(FRigBaseElement* InChild, int32 InParentIndex, FRigElementWeight InWeight, bool bInitial, bool bAffectChildren)
{
	using namespace ERigTransformType;

	if(FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(InChild))
	{
		if(MultiParentElement->ParentConstraints.IsValidIndex(InParentIndex))
		{
			InWeight.Location = FMath::Max(InWeight.Location, 0.f);
			InWeight.Rotation = FMath::Max(InWeight.Rotation, 0.f);
			InWeight.Scale = FMath::Max(InWeight.Scale, 0.f);

			FRigElementWeight& TargetWeight = bInitial?
				MultiParentElement->ParentConstraints[InParentIndex].InitialWeight :
				MultiParentElement->ParentConstraints[InParentIndex].Weight;

			if(FMath::IsNearlyZero(InWeight.Location - TargetWeight.Location) &&
				FMath::IsNearlyZero(InWeight.Rotation - TargetWeight.Rotation) &&
				FMath::IsNearlyZero(InWeight.Scale - TargetWeight.Scale))
			{
				return false;
			}
			
			const ERigTransformType::Type LocalType = bInitial ? InitialLocal : CurrentLocal;
			const ERigTransformType::Type GlobalType = SwapLocalAndGlobal(LocalType);

			if(bAffectChildren)
			{
				GetParentTransform(MultiParentElement, LocalType);
				if(FRigControlElement* ControlElement = Cast<FRigControlElement>(MultiParentElement))
				{
					GetControlOffsetTransform(ControlElement, LocalType);
				}
				GetTransform(MultiParentElement, LocalType);
				MultiParentElement->Pose.MarkDirty(GlobalType);
			}
			else
			{
				GetParentTransform(MultiParentElement, GlobalType);
				if(FRigControlElement* ControlElement = Cast<FRigControlElement>(MultiParentElement))
				{
					GetControlOffsetTransform(ControlElement, GlobalType);
				}
				GetTransform(MultiParentElement, GlobalType);
				MultiParentElement->Pose.MarkDirty(LocalType);
			}

			TargetWeight = InWeight;
			MultiParentElement->Parent.MarkDirty(GlobalType);

			if(FRigControlElement* ControlElement = Cast<FRigControlElement>(MultiParentElement))
			{
				ControlElement->Offset.MarkDirty(GlobalType);
			}

			PropagateDirtyFlags(MultiParentElement, ERigTransformType::IsInitial(LocalType), bAffectChildren);
			
#if WITH_EDITOR
			if (ensure(!bPropagatingChange))
			{
				TGuardValue<bool> bPropagatingChangeGuardValue(bPropagatingChange, true);
			
				for(FRigHierarchyListener& Listener : ListeningHierarchies)
				{
					if(!bForcePropagation && !Listener.ShouldReactToChange(LocalType))
					{
						continue;
					}

					URigHierarchy* ListeningHierarchy = Listener.Hierarchy.Get();
					if (ListeningHierarchy)
					{
						if(FRigBaseElement* ListeningElement = ListeningHierarchy->Find(InChild->GetKey()))
						{
							ListeningHierarchy->SetParentWeight(ListeningElement, InParentIndex, InWeight, bInitial, bAffectChildren);
						}
					}
				}	
			}
#endif

			Notify(ERigHierarchyNotification::ParentWeightsChanged, MultiParentElement);
			return true;
		}
	}
	return false;
}

bool URigHierarchy::SetParentWeightArray(FRigElementKey InChild, TArray<FRigElementWeight> InWeights, bool bInitial,
	bool bAffectChildren)
{
	return SetParentWeightArray(Find(InChild), InWeights, bInitial, bAffectChildren);
}

bool URigHierarchy::SetParentWeightArray(FRigBaseElement* InChild, const TArray<FRigElementWeight>& InWeights,
	bool bInitial, bool bAffectChildren)
{
	if(InWeights.Num() == 0)
	{
		return false;
	}
	
	TArrayView<const FRigElementWeight> View(InWeights.GetData(), InWeights.Num());
	return SetParentWeightArray(InChild, View, bInitial, bAffectChildren);
}

bool URigHierarchy::SetParentWeightArray(FRigBaseElement* InChild,  const TArrayView<const FRigElementWeight>& InWeights,
	bool bInitial, bool bAffectChildren)
{
	using namespace ERigTransformType;

	if(FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(InChild))
	{
		if(MultiParentElement->ParentConstraints.Num() == InWeights.Num())
		{
			TArray<FRigElementWeight> InputWeights;
			InputWeights.Reserve(InWeights.Num());

			bool bFoundDifference = false;
			for(int32 WeightIndex=0; WeightIndex < InWeights.Num(); WeightIndex++)
			{
				FRigElementWeight InputWeight = InWeights[WeightIndex];
				InputWeight.Location = FMath::Max(InputWeight.Location, 0.f);
				InputWeight.Rotation = FMath::Max(InputWeight.Rotation, 0.f);
				InputWeight.Scale = FMath::Max(InputWeight.Scale, 0.f);
				InputWeights.Add(InputWeight);

				FRigElementWeight& TargetWeight = bInitial?
					MultiParentElement->ParentConstraints[WeightIndex].InitialWeight :
					MultiParentElement->ParentConstraints[WeightIndex].Weight;

				if(!FMath::IsNearlyZero(InputWeight.Location - TargetWeight.Location) ||
					!FMath::IsNearlyZero(InputWeight.Rotation - TargetWeight.Rotation) ||
					!FMath::IsNearlyZero(InputWeight.Scale - TargetWeight.Scale))
				{
					bFoundDifference = true;
				}
			}

			if(!bFoundDifference)
			{
				return false;
			}
			
			const ERigTransformType::Type LocalType = bInitial ? InitialLocal : CurrentLocal;
			const ERigTransformType::Type GlobalType = SwapLocalAndGlobal(LocalType);

			if(bAffectChildren)
			{
				GetTransform(MultiParentElement, LocalType);
				MultiParentElement->Pose.MarkDirty(GlobalType);
			}
			else
			{
				GetTransform(MultiParentElement, GlobalType);
				MultiParentElement->Pose.MarkDirty(LocalType);
			}

			for(int32 WeightIndex=0; WeightIndex < InWeights.Num(); WeightIndex++)
			{
				if(bInitial)
				{
					MultiParentElement->ParentConstraints[WeightIndex].InitialWeight = InputWeights[WeightIndex];
				}
				else
				{
					MultiParentElement->ParentConstraints[WeightIndex].Weight = InputWeights[WeightIndex];
				}
			}

			MultiParentElement->Parent.MarkDirty(GlobalType);

			if(FRigControlElement* ControlElement = Cast<FRigControlElement>(MultiParentElement))
			{
				ControlElement->Offset.MarkDirty(GlobalType);
			}

			PropagateDirtyFlags(MultiParentElement, ERigTransformType::IsInitial(LocalType), bAffectChildren);
			
#if WITH_EDITOR
			if (ensure(!bPropagatingChange))
			{
				TGuardValue<bool> bPropagatingChangeGuardValue(bPropagatingChange, true);
			
				for(FRigHierarchyListener& Listener : ListeningHierarchies)
				{
					if(!bForcePropagation && !Listener.ShouldReactToChange(LocalType))
					{
						continue;
					}

					URigHierarchy* ListeningHierarchy = Listener.Hierarchy.Get();
					if (ListeningHierarchy)
					{
						if(FRigBaseElement* ListeningElement = ListeningHierarchy->Find(InChild->GetKey()))
						{
							ListeningHierarchy->SetParentWeightArray(ListeningElement, InWeights, bInitial, bAffectChildren);
						}
					}
				}	
			}
#endif

			Notify(ERigHierarchyNotification::ParentWeightsChanged, MultiParentElement);

			return true;
		}
	}
	return false;
}

bool URigHierarchy::SwitchToParent(FRigElementKey InChild, FRigElementKey InParent, bool bInitial, bool bAffectChildren)
{
	return SwitchToParent(Find(InChild), Find(InParent), bInitial, bAffectChildren);
}

bool URigHierarchy::SwitchToParent(FRigBaseElement* InChild, FRigBaseElement* InParent, bool bInitial,
	bool bAffectChildren)
{
	if(const FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(InChild))
	{
		int32 ParentIndex = INDEX_NONE;
		if(const int32* ParentIndexPtr = MultiParentElement->IndexLookup.Find(InParent->GetKey()))
		{
			ParentIndex = *ParentIndexPtr;
		}
		else
		{
			if(URigHierarchyController* Controller = GetController(true))
			{
				if(Controller->AddParent(InChild, InParent, 0.f, true, false))
				{
					ParentIndex = MultiParentElement->IndexLookup.FindChecked(InParent->GetKey());
				}
			}
		}

		return SwitchToParent(InChild, ParentIndex, bInitial, bAffectChildren);
	}
	return false;
}

bool URigHierarchy::SwitchToParent(FRigBaseElement* InChild, int32 InParentIndex, bool bInitial, bool bAffectChildren)
{
	TArray<FRigElementWeight> Weights = GetParentWeightArray(InChild, bInitial);
	if(Weights.IsValidIndex(InParentIndex))
	{
		FMemory::Memzero(Weights.GetData(), Weights.GetAllocatedSize());
		Weights[InParentIndex] = 1.f;
		return SetParentWeightArray(InChild, Weights, bInitial, bAffectChildren);
	}
	return false;
}

bool URigHierarchy::SwitchToDefaultParent(FRigElementKey InChild, bool bInitial, bool bAffectChildren)
{
	return SwitchToDefaultParent(Find(InChild), bInitial, bAffectChildren);
}

bool URigHierarchy::SwitchToDefaultParent(FRigBaseElement* InChild, bool bInitial, bool bAffectChildren)
{
	// we assume that the first stored parent is the default parent
	return SwitchToParent(InChild, 0, bInitial, bAffectChildren);
}

bool URigHierarchy::SwitchToWorldSpace(FRigElementKey InChild, bool bInitial, bool bAffectChildren)
{
	return SwitchToWorldSpace(Find(InChild), bInitial, bAffectChildren);
}

bool URigHierarchy::SwitchToWorldSpace(FRigBaseElement* InChild, bool bInitial, bool bAffectChildren)
{
	FRigElementKey WorldSocket = GetOrAddWorldSpaceSocket();
	if(FRigBaseElement* Parent = Find(WorldSocket))
	{
		return SwitchToParent(InChild, Parent, bInitial, bAffectChildren);
	}
	return false;
}

FRigElementKey URigHierarchy::GetOrAddWorldSpaceSocket()
{
	const FRigElementKey WorldSpaceSocketKey = GetWorldSpaceSocketKey();

	FRigBaseElement* Parent = Find(WorldSpaceSocketKey);
	if(Parent)
	{
		return Parent->GetKey();
	}

	if(URigHierarchyController* Controller = GetController(true))
	{
		return Controller->AddSocket(
			WorldSpaceSocketKey.Name,
			FRigElementKey(),
			FRigSocketGetWorldTransformDelegate::CreateUObject(this, &URigHierarchy::GetWorldTransformForSocket),
			false);
	}

	return FRigElementKey();
}

FRigElementKey URigHierarchy::GetWorldSpaceSocketKey() const
{
	static const FName WorldSpaceSocketName = TEXT("WorldSpace");
	return FRigElementKey(WorldSpaceSocketName, ERigElementType::Socket); 
}

TArray<FRigElementKey> URigHierarchy::GetAllKeys(bool bTraverse, ERigElementType InElementType) const
{
	TArray<FRigElementKey> Keys;
	Keys.Reserve(Elements.Num());

	if(bTraverse)
	{
		TArray<bool> ElementVisited;
		ElementVisited.AddZeroed(Elements.Num());
		
		for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ElementIndex++)
		{
			FRigBaseElement* Element = Elements[ElementIndex];
			Traverse(Element, true, [&ElementVisited, &Keys, InElementType](FRigBaseElement* InElement, bool& bContinue)
            {
				bContinue = !ElementVisited[InElement->GetIndex()];

				if(bContinue)
				{
					if(InElement->IsTypeOf(InElementType))
					{
						Keys.Add(InElement->GetKey());
					}
					ElementVisited[InElement->GetIndex()] = true;
				}
            });
		}
	}
	else
	{
		for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ElementIndex++)
		{
			FRigBaseElement* Element = Elements[ElementIndex];
			if(Element->IsTypeOf(InElementType))
			{
				Keys.Add(Element->GetKey());
			}
		}
	}
	return Keys;
}

void URigHierarchy::Traverse(FRigBaseElement* InElement, bool bTowardsChildren,
                             TFunction<void(FRigBaseElement*, bool&)> PerElementFunction) const
{
	bool bContinue = true;
	PerElementFunction(InElement, bContinue);

	if(bContinue)
	{
		if(bTowardsChildren)
		{
			const FRigBaseElementChildrenArray& Children = GetChildren(InElement);
			for (FRigBaseElement* Child : Children)
			{
				Traverse(Child, true, PerElementFunction);
			}
		}
		else
		{
			FRigBaseElementParentArray Parents = GetParents(InElement);
			for (FRigBaseElement* Parent : Parents)
			{
				Traverse(Parent, false, PerElementFunction);
			}
		}
	}
}

void URigHierarchy::Traverse(TFunction<void(FRigBaseElement*, bool& /* continue */)> PerElementFunction, bool bTowardsChildren) const
{
	if(bTowardsChildren)
	{
		for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ElementIndex++)
		{
			FRigBaseElement* Element = Elements[ElementIndex];
			if(GetNumberOfParents(Element) == 0)
			{
				Traverse(Element, bTowardsChildren, PerElementFunction);
			}
        }
	}
	else
	{
		for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ElementIndex++)
		{
			FRigBaseElement* Element = Elements[ElementIndex];
			if(GetChildren(Element).Num() == 0)
			{
				Traverse(Element, bTowardsChildren, PerElementFunction);
			}
		}
	}
}

bool URigHierarchy::Undo()
{
#if WITH_EDITOR
	
	if(TransformUndoStack.IsEmpty())
	{
		return false;
	}

	const FRigTransformStackEntry Entry = TransformUndoStack.Pop();
	ApplyTransformFromStack(Entry, true);
	UndoRedoEvent.Broadcast(this, Entry.Key, Entry.TransformType, Entry.OldTransform, true);
	TransformRedoStack.Push(Entry);
	TransformStackIndex = TransformUndoStack.Num();
	return true;
	
#else
	
	return false;
	
#endif
}

bool URigHierarchy::Redo()
{
#if WITH_EDITOR

	if(TransformRedoStack.IsEmpty())
	{
		return false;
	}

	const FRigTransformStackEntry Entry = TransformRedoStack.Pop();
	ApplyTransformFromStack(Entry, false);
	UndoRedoEvent.Broadcast(this, Entry.Key, Entry.TransformType, Entry.NewTransform, false);
	TransformUndoStack.Push(Entry);
	TransformStackIndex = TransformUndoStack.Num();
	return true;
	
#else
	
	return false;
	
#endif
}

bool URigHierarchy::SetTransformStackIndex(int32 InTransformStackIndex)
{
#if WITH_EDITOR

	while(TransformUndoStack.Num() > InTransformStackIndex)
	{
		if(TransformUndoStack.Num() == 0)
		{
			return false;
		}

		if(!Undo())
		{
			return false;
		}
	}
	
	while(TransformUndoStack.Num() < InTransformStackIndex)
	{
		if(TransformRedoStack.Num() == 0)
		{
			return false;
		}

		if(!Redo())
		{
			return false;
		}
	}

	return InTransformStackIndex == TransformStackIndex;

#else
	
	return false;
	
#endif
}

#if WITH_EDITOR

void URigHierarchy::PostTransacted(const FTransactionObjectEvent& TransactionEvent)
{
	if (TransactionEvent.GetEventType() == ETransactionObjectEventType::UndoRedo)
	{
		const int32 DesiredStackIndex = TransformStackIndex;
		TransformStackIndex = TransformUndoStack.Num();
		if (DesiredStackIndex == TransformStackIndex)
		{
			return;
		}
		SetTransformStackIndex(DesiredStackIndex);
	}
}

#endif

void URigHierarchy::SendEvent(const FRigEventContext& InEvent, bool bAsynchronous)
{
	if(EventDelegate.IsBound())
	{
		TWeakObjectPtr<URigHierarchy> WeakThis = this;
		FRigEventDelegate& Delegate = EventDelegate;

		if (bAsynchronous)
		{
			FFunctionGraphTask::CreateAndDispatchWhenReady([WeakThis, Delegate, InEvent]()
            {
                Delegate.Broadcast(WeakThis.Get(), InEvent);
            }, TStatId(), NULL, ENamedThreads::GameThread);
		}
		else
		{
			Delegate.Broadcast(this, InEvent);
		}
	}

}

void URigHierarchy::SendAutoKeyEvent(FRigElementKey InElement, float InOffsetInSeconds, bool bAsynchronous)
{
	FRigEventContext Context;
	Context.Event = ERigEvent::RequestAutoKey;
	Context.Key = InElement;
	Context.LocalTime = InOffsetInSeconds;
	if(UControlRig* Rig = Cast<UControlRig>(GetOuter()))
	{
		Context.LocalTime += Rig->AbsoluteTime;
	}
	SendEvent(Context, bAsynchronous);
}

URigHierarchyController* URigHierarchy::GetController(bool bCreateIfNeeded)
{
	if(LastControllerPtr.IsValid())
	{
		return Cast<URigHierarchyController>(LastControllerPtr.Get());
	}
	else if(bCreateIfNeeded)
	{
		if(UObject* Outer = GetOuter())
		{
			if(!IsGarbageCollecting())
			{
				URigHierarchyController* Controller = NewObject<URigHierarchyController>(Outer);
				Controller->SetHierarchy(this);
				LastControllerPtr = Controller;
				return Controller;
			}
		}
	}
	return nullptr;
}

FRigPose URigHierarchy::GetPose(
	bool bInitial,
	ERigElementType InElementType,
	const FRigElementKeyCollection& InItems 
) const
{
	FRigPose Pose;
	Pose.HierarchyTopologyVersion = GetTopologyVersion();
	Pose.PoseHash = Pose.HierarchyTopologyVersion;

	for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ElementIndex++)
	{
		FRigBaseElement* Element = Elements[ElementIndex];

		// filter by type
		if (((uint8)InElementType & (uint8)Element->GetType()) == 0)
		{
			continue;
		}

		// filter by optional collection
		if(InItems.Num() > 0)
		{
			if(!InItems.Contains(Element->GetKey()))
			{
				continue;
			}
		}
		
		FRigPoseElement PoseElement;
		PoseElement.Index.UpdateCache(Element->GetKey(), this);
		
		if(FRigTransformElement* TransformElement = Cast<FRigTransformElement>(Element))
		{
			PoseElement.LocalTransform = GetTransform(TransformElement, bInitial ? ERigTransformType::InitialLocal : ERigTransformType::CurrentLocal);
			PoseElement.GlobalTransform = GetTransform(TransformElement, bInitial ? ERigTransformType::InitialGlobal : ERigTransformType::CurrentGlobal);
		}
		else if(FRigCurveElement* CurveElement = Cast<FRigCurveElement>(Element))
		{
			PoseElement.CurveValue = GetCurveValue(CurveElement);
		}
		else
		{
			continue;
		}
		Pose.Elements.Add(PoseElement);
		Pose.PoseHash = HashCombine(Pose.PoseHash, GetTypeHash(PoseElement.Index.GetKey()));
	}
	return Pose;
}

void URigHierarchy::SetPose(
	const FRigPose& InPose,
	ERigTransformType::Type InTransformType,
	ERigElementType InElementType,
	const FRigElementKeyCollection& InItems,
	float InWeight
)
{
	const float U = FMath::Clamp(InWeight, 0.f, 1.f);
	if(U < SMALL_NUMBER)
	{
		return;
	}
	
	for(const FRigPoseElement& PoseElement : InPose)
	{
		FCachedRigElement Index = PoseElement.Index;

		// filter by type
		if (((uint8)InElementType & (uint8)Index.GetKey().Type) == 0)
		{
			continue;
		}

		// filter by optional collection
		if(InItems.Num() > 0)
		{
			if(!InItems.Contains(Index.GetKey()))
			{
				continue;
			}
		}

		if(Index.UpdateCache(this))
		{
			FRigBaseElement* Element = Get(Index.GetIndex());
			if(FRigTransformElement* TransformElement = Cast<FRigTransformElement>(Element))
			{
				FTransform TransformToSet =
					ERigTransformType::IsLocal(InTransformType) ?
						PoseElement.LocalTransform :
						PoseElement.GlobalTransform;
				
				if(U < 1.f - SMALL_NUMBER)
				{
					const FTransform PreviousTransform = GetTransform(TransformElement, InTransformType);
					TransformToSet = FControlRigMathLibrary::LerpTransform(PreviousTransform, TransformToSet, U);
				}

				SetTransform(TransformElement, TransformToSet, InTransformType, true);
			}
			else if(FRigCurveElement* CurveElement = Cast<FRigCurveElement>(Element))
			{
				SetCurveValue(CurveElement, PoseElement.CurveValue);
			}
		}
	}
}

void URigHierarchy::Notify(ERigHierarchyNotification InNotifType, const FRigBaseElement* InElement)
{
	if(bSuspendNotifications)
	{
		return;
	}
	ModifiedEvent.Broadcast(InNotifType, this, InElement);
}

FTransform URigHierarchy::GetTransform(FRigTransformElement* InTransformElement,
	const ERigTransformType::Type InTransformType) const
{
	if(InTransformElement == nullptr)
	{
		return FTransform::Identity;
	}
	
	if(InTransformElement->Pose.IsDirty(InTransformType))
	{
		const ERigTransformType::Type OpposedType = SwapLocalAndGlobal(InTransformType);
		const ERigTransformType::Type GlobalType = MakeGlobal(InTransformType);
		ensure(!InTransformElement->Pose.IsDirty(OpposedType));

		FTransform ParentTransform;
		if(IsLocal(InTransformType))
		{
			if(FRigControlElement* ControlElement = Cast<FRigControlElement>(InTransformElement))
			{
				const FTransform NewTransform = ComputeLocalControlValue(ControlElement, ControlElement->Pose.Get(OpposedType), GlobalType);
				InTransformElement->Pose.Set(InTransformType, NewTransform);
			}
			else
			{
				ParentTransform = GetParentTransform(InTransformElement, GlobalType);

				FTransform NewTransform = InTransformElement->Pose.Get(OpposedType).GetRelativeTransform(ParentTransform);
				NewTransform.NormalizeRotation();
				InTransformElement->Pose.Set(InTransformType, NewTransform);
			}
		}
		else
		{
			if(FRigControlElement* ControlElement = Cast<FRigControlElement>(InTransformElement))
			{
				const FTransform NewTransform = SolveParentConstraints(
					ControlElement->ParentConstraints, InTransformType,
					ControlElement->Offset.Get(OpposedType), true,
					ControlElement->Pose.Get(OpposedType), true);
				ControlElement->Pose.Set(InTransformType, NewTransform);
			}
			else
			{
				ParentTransform = GetParentTransform(InTransformElement, GlobalType);

				FTransform NewTransform = InTransformElement->Pose.Get(OpposedType) * ParentTransform;
				NewTransform.NormalizeRotation();
				InTransformElement->Pose.Set(InTransformType, NewTransform);
			}
		}
	}
	return InTransformElement->Pose.Get(InTransformType);
}

void URigHierarchy::SetTransform(FRigTransformElement* InTransformElement, const FTransform& InTransform, const ERigTransformType::Type InTransformType, bool bAffectChildren, bool bSetupUndo, bool bForce, bool bPrintPythonCommands)
{
	if(InTransformElement == nullptr)
	{
		return;
	}

	if(IsGlobal(InTransformType))
	{
		if(FRigControlElement* ControlElement = Cast<FRigControlElement>(InTransformElement))
		{
			FTransform LocalTransform = ComputeLocalControlValue(ControlElement, InTransform, InTransformType);
			ControlElement->Settings.ApplyLimits(LocalTransform);
			SetTransform(ControlElement, LocalTransform, MakeLocal(InTransformType), bAffectChildren);
			return;
		}
	}

	if(!InTransformElement->Pose.IsDirty(InTransformType))
	{
		const FTransform PreviousTransform = InTransformElement->Pose.Get(InTransformType);
		if(!bForce && FRigComputedTransform::Equals(PreviousTransform, InTransform))
		{
			return;
		}
	}

	const FTransform PreviousTransform = GetTransform(InTransformElement, InTransformType);
	PropagateDirtyFlags(InTransformElement, ERigTransformType::IsInitial(InTransformType), bAffectChildren);

	const ERigTransformType::Type OpposedType = SwapLocalAndGlobal(InTransformType);
	InTransformElement->Pose.Set(InTransformType, InTransform);
	InTransformElement->Pose.MarkDirty(OpposedType);

	if(FRigControlElement* ControlElement = Cast<FRigControlElement>(InTransformElement))
	{
		ControlElement->Gizmo.MarkDirty(MakeGlobal(InTransformType));
	}

#if WITH_EDITOR
	if(bSetupUndo || IsTracingChanges())
	{
		PushTransformToStack(
			InTransformElement->GetKey(),
			ERigTransformStackEntryType::TransformPose,
			InTransformType,
			PreviousTransform,
			InTransformElement->Pose.Get(InTransformType),
			bAffectChildren,
			bSetupUndo);
	}

	if (ensure(!bPropagatingChange))
	{
		TGuardValue<bool> bPropagatingChangeGuardValue(bPropagatingChange, true);
			
		for(FRigHierarchyListener& Listener : ListeningHierarchies)
		{
			if(!bForcePropagation && !Listener.ShouldReactToChange(InTransformType))
			{
				continue;
			}

			URigHierarchy* ListeningHierarchy = Listener.Hierarchy.Get();
			if (ListeningHierarchy)
			{			
				if(FRigTransformElement* ListeningElement = Cast<FRigTransformElement>(ListeningHierarchy->Find(InTransformElement->GetKey())))
				{
					// bSetupUndo = false such that all listening hierarchies performs undo at the same time the root hierachy undos
					ListeningHierarchy->SetTransform(ListeningElement, InTransform, InTransformType, bAffectChildren, false, bForce);
				}
			}
		}
	}

	if (bPrintPythonCommands)
	{
		FString MethodName;
		switch (InTransformType)
		{
			case ERigTransformType::InitialLocal: 
			case ERigTransformType::CurrentLocal:
			{
				MethodName = TEXT("set_local_transform");
				break;
			}
			case ERigTransformType::InitialGlobal: 
			case ERigTransformType::CurrentGlobal:
			{
				MethodName = TEXT("set_global_transform");
				break;
			}
		}

		RigVMPythonUtils::Print(GetOuter()->GetFName().ToString(),
			FString::Printf(TEXT("hierarchy.%s(%s, %s, %s, %s)"),
			*MethodName,
			*InTransformElement->GetKey().ToPythonString(),
			*RigVMPythonUtils::TransformToPythonString(InTransform),
			(InTransformType == ERigTransformType::InitialGlobal || InTransformType == ERigTransformType::InitialLocal) ? TEXT("True") : TEXT("False"),
			(bAffectChildren) ? TEXT("True") : TEXT("False")));
	}
#endif
}

FTransform URigHierarchy::GetControlOffsetTransform(FRigControlElement* InControlElement,
	const ERigTransformType::Type InTransformType) const
{
	if(InControlElement == nullptr)
	{
		return FTransform::Identity;
	}
	
	if(InControlElement->Offset.IsDirty(InTransformType))
	{
		const ERigTransformType::Type OpposedType = SwapLocalAndGlobal(InTransformType);
		const ERigTransformType::Type GlobalType = MakeGlobal(InTransformType);
		ensure(!InControlElement->Offset.IsDirty(OpposedType));

		if(IsLocal(InTransformType))
		{
			const FTransform LocalTransform = InverseSolveParentConstraints(
				InControlElement->Offset.Get(OpposedType), 
				InControlElement->ParentConstraints, InTransformType, FTransform::Identity);
			InControlElement->Offset.Set(InTransformType, LocalTransform);
		}
		else
		{
			const FTransform GlobalTransform = SolveParentConstraints(
				InControlElement->ParentConstraints, InTransformType,
				InControlElement->Offset.Get(OpposedType), true,
				FTransform::Identity, false);
			InControlElement->Offset.Set(InTransformType, GlobalTransform);
		}
	}
	return InControlElement->Offset.Get(InTransformType);
}

void URigHierarchy::SetControlOffsetTransform(FRigControlElement* InControlElement, const FTransform& InTransform,
                                              const ERigTransformType::Type InTransformType, bool bAffectChildren, bool bSetupUndo, bool bForce, bool bPrintPythonCommands)
{
	if(InControlElement == nullptr)
	{
		return;
	}

	if(!InControlElement->Offset.IsDirty(InTransformType))
	{
		const FTransform PreviousTransform = InControlElement->Offset.Get(InTransformType);
		if(!bForce && FRigComputedTransform::Equals(PreviousTransform, InTransform))
		{
			return;
		}
	}

	const FTransform PreviousTransform = GetControlOffsetTransform(InControlElement, InTransformType);
	PropagateDirtyFlags(InControlElement, ERigTransformType::IsInitial(InTransformType), bAffectChildren);

	GetTransform(InControlElement, MakeLocal(InTransformType));
	InControlElement->Pose.MarkDirty(MakeGlobal(InTransformType));

	const ERigTransformType::Type OpposedType = SwapLocalAndGlobal(InTransformType);
	InControlElement->Offset.Set(InTransformType, InTransform);
	InControlElement->Offset.MarkDirty(OpposedType);
	InControlElement->Gizmo.MarkDirty(MakeGlobal(InTransformType));

	if (ERigTransformType::IsInitial(InTransformType))
	{
		// control's offset transform is considered a special type of transform
		// whenever its initial value is changed, we want to make sure the current is kept in sync
		// such that the viewport can reflect this change
		SetControlOffsetTransform(InControlElement, InTransform, ERigTransformType::MakeCurrent(InTransformType), bAffectChildren, false, bForce);
	}
	

#if WITH_EDITOR
	if(bSetupUndo || IsTracingChanges())
	{
		PushTransformToStack(
            InControlElement->GetKey(),
            ERigTransformStackEntryType::ControlOffset,
            InTransformType,
            PreviousTransform,
            InControlElement->Offset.Get(InTransformType),
            bAffectChildren,
            bSetupUndo);
	}

	if (ensure(!bPropagatingChange))
	{
		TGuardValue<bool> bPropagatingChangeGuardValue(bPropagatingChange, true);
			
		for(FRigHierarchyListener& Listener : ListeningHierarchies)
		{
			URigHierarchy* ListeningHierarchy = Listener.Hierarchy.Get();
			if (ListeningHierarchy)
			{	
				if(FRigControlElement* ListeningElement = Cast<FRigControlElement>(ListeningHierarchy->Find(InControlElement->GetKey())))
				{
					// bSetupUndo = false such that all listening hierarchies performs undo at the same time the root hierachy undos
					ListeningHierarchy->SetControlOffsetTransform(ListeningElement, InTransform, InTransformType, bAffectChildren, false, bForce);
				}
			}
		}
	}

	if (bPrintPythonCommands)
	{
		//FRigElementKey InKey, FTransform InTransform, bool bInitial = false, bool bAffectChildren = true, bool bSetupUndo = false, bool bPrintPythonCommands = false)
		RigVMPythonUtils::Print(GetOuter()->GetFName().ToString(),
			FString::Printf(TEXT("hierarchy.set_control_offset_transform(%s, %s, %s, %s)"),
			*InControlElement->GetKey().ToPythonString(),
			*RigVMPythonUtils::TransformToPythonString(InTransform),
			(ERigTransformType::IsInitial(InTransformType)) ? TEXT("True") : TEXT("False"),
			(bAffectChildren) ? TEXT("True") : TEXT("False")));
	}
#endif
}

FTransform URigHierarchy::GetControlGizmoTransform(FRigControlElement* InControlElement,
	const ERigTransformType::Type InTransformType) const
{
	if(InControlElement == nullptr)
	{
		return FTransform::Identity;
	}
	
	if(InControlElement->Gizmo.IsDirty(InTransformType))
	{
		const ERigTransformType::Type OpposedType = SwapLocalAndGlobal(InTransformType);
		const ERigTransformType::Type GlobalType = MakeGlobal(InTransformType);
		ensure(!InControlElement->Gizmo.IsDirty(OpposedType));

		const FTransform ParentTransform = GetTransform(InControlElement, GlobalType);
		if(IsLocal(InTransformType))
		{
			InControlElement->Gizmo.Set(InTransformType, InControlElement->Gizmo.Get(OpposedType).GetRelativeTransform(ParentTransform));
		}
		else
		{
			InControlElement->Gizmo.Set(InTransformType, InControlElement->Gizmo.Get(OpposedType) * ParentTransform);
		}
	}
	return InControlElement->Gizmo.Get(InTransformType);
}

void URigHierarchy::SetControlGizmoTransform(FRigControlElement* InControlElement, const FTransform& InTransform,
	const ERigTransformType::Type InTransformType, bool bSetupUndo, bool bForce)
{
	if(InControlElement == nullptr)
	{
		return;
	}

	if(!InControlElement->Gizmo.IsDirty(InTransformType))
	{
		const FTransform PreviousTransform = InControlElement->Gizmo.Get(InTransformType);
		if(!bForce && FRigComputedTransform::Equals(PreviousTransform, InTransform))
		{
			return;
		}
	}

	const FTransform PreviousTransform = GetControlGizmoTransform(InControlElement, InTransformType);
	const ERigTransformType::Type OpposedType = SwapLocalAndGlobal(InTransformType);
	InControlElement->Gizmo.Set(InTransformType, InTransform);
	InControlElement->Gizmo.MarkDirty(OpposedType);

	if (IsInitial(InTransformType))
	{
		// control's gizmo transform, similar to offset transform, is considered a special type of transform
		// whenever its initial value is changed, we want to make sure the current is kept in sync
		// such that the viewport can reflect this change
		SetControlGizmoTransform(InControlElement, InTransform, ERigTransformType::MakeCurrent(InTransformType), false, bForce);
	}
	
#if WITH_EDITOR
	if(bSetupUndo || IsTracingChanges())
	{
		PushTransformToStack(
            InControlElement->GetKey(),
            ERigTransformStackEntryType::ControlGizmo,
            InTransformType,
            PreviousTransform,
            InControlElement->Gizmo.Get(InTransformType),
            false,
            bSetupUndo);
	}
#endif

	if(IsLocal(InTransformType))
	{
		Notify(ERigHierarchyNotification::ControlGizmoTransformChanged, InControlElement);
	}

#if WITH_EDITOR
	if (ensure(!bPropagatingChange))
	{
		TGuardValue<bool> bPropagatingChangeGuardValue(bPropagatingChange, true);
			
		for(FRigHierarchyListener& Listener : ListeningHierarchies)
		{
			URigHierarchy* ListeningHierarchy = Listener.Hierarchy.Get();
			if (ListeningHierarchy)
			{	
				if(FRigControlElement* ListeningElement = Cast<FRigControlElement>(ListeningHierarchy->Find(InControlElement->GetKey())))
				{
					// bSetupUndo = false such that all listening hierarchies performs undo at the same time the root hierachy undos
					ListeningHierarchy->SetControlGizmoTransform(ListeningElement, InTransform, InTransformType, false, bForce);
				}
			}
		}
	}
#endif
}

void URigHierarchy::SetControlSettings(FRigControlElement* InControlElement, FRigControlSettings InSettings, bool bSetupUndo, bool bForce)
{
	if(InControlElement == nullptr)
	{
		return;
	}

	const FRigControlSettings PreviousSettings = InControlElement->Settings;
	if(!bForce && PreviousSettings == InSettings)
	{
		return;
	}

	InControlElement->Settings = InSettings;
	Notify(ERigHierarchyNotification::ControlSettingChanged, InControlElement);
	
#if WITH_EDITOR
	if (ensure(!bPropagatingChange))
	{
		TGuardValue<bool> bPropagatingChangeGuardValue(bPropagatingChange, true);
			
		for(FRigHierarchyListener& Listener : ListeningHierarchies)
		{
			URigHierarchy* ListeningHierarchy = Listener.Hierarchy.Get();
			if (ListeningHierarchy)
			{	
				if(FRigControlElement* ListeningElement = Cast<FRigControlElement>(ListeningHierarchy->Find(InControlElement->GetKey())))
				{
					// bSetupUndo = false such that all listening hierarchies performs undo at the same time the root hierachy undos
					ListeningHierarchy->SetControlSettings(ListeningElement, InSettings, false, bForce);
				}
			}
		}
	}
#endif
}

FTransform URigHierarchy::GetParentTransform(FRigBaseElement* InElement, const ERigTransformType::Type InTransformType) const
{
	if(FRigSingleParentElement* SingleParentElement = Cast<FRigSingleParentElement>(InElement))
	{
		return GetTransform(SingleParentElement->ParentElement, InTransformType);
	}
	else if(FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(InElement))
	{
		FRigComputedTransform& Output = MultiParentElement->Parent[InTransformType];

		if(Output.bDirty)
		{
			const FTransform OutputTransform = SolveParentConstraints(
				MultiParentElement->ParentConstraints,
				InTransformType,
				FTransform::Identity,
				false,
				FTransform::Identity,
				false
			);
			MultiParentElement->Parent.Set(InTransformType, OutputTransform);
		}
		return Output.Transform;
	}
	return FTransform::Identity;
}

FRigControlValue URigHierarchy::GetControlValue(FRigControlElement* InControlElement, ERigControlValueType InValueType) const
{
	using namespace ERigTransformType;

	FRigControlValue Value;

	if(InControlElement != nullptr)
	{
		switch(InValueType)
		{
			case ERigControlValueType::Current:
			{
				Value.SetFromTransform(
                    GetTransform(InControlElement, CurrentLocal),
                    InControlElement->Settings.ControlType,
                    InControlElement->Settings.PrimaryAxis
                );
				break;
			}
			case ERigControlValueType::Initial:
			{
				Value.SetFromTransform(
                    GetTransform(InControlElement, InitialLocal),
                    InControlElement->Settings.ControlType,
                    InControlElement->Settings.PrimaryAxis
                );
				break;
			}
			case ERigControlValueType::Minimum:
			{
				return InControlElement->Settings.MinimumValue;
			}
			case ERigControlValueType::Maximum:
			{
				return InControlElement->Settings.MaximumValue;
			}
		}
	}
	return Value;
}

void URigHierarchy::SetControlValue(FRigControlElement* InControlElement, const FRigControlValue& InValue, ERigControlValueType InValueType, bool bSetupUndo, bool bForce, bool bPrintPythonCommands)
{
	using namespace ERigTransformType;

	if(InControlElement != nullptr)
	{
		switch(InValueType)
		{
			case ERigControlValueType::Current:
			{
				FRigControlValue Value = InValue;
				InControlElement->Settings.ApplyLimits(Value);
					
				SetTransform(
					InControlElement,
					Value.GetAsTransform(
						InControlElement->Settings.ControlType,
						InControlElement->Settings.PrimaryAxis
					),
					CurrentLocal,
					true,
					bSetupUndo,
					bForce,
					bPrintPythonCommands
				); 
				break;
			}
			case ERigControlValueType::Initial:
			{
				FRigControlValue Value = InValue;
				InControlElement->Settings.ApplyLimits(Value);

				SetTransform(
					InControlElement,
					Value.GetAsTransform(
						InControlElement->Settings.ControlType,
						InControlElement->Settings.PrimaryAxis
					),
					InitialLocal,
					true,
					bSetupUndo,
					bForce,
					bPrintPythonCommands
				); 
				break;
			}
			case ERigControlValueType::Minimum:
			case ERigControlValueType::Maximum:
			{
				if(InValueType == ERigControlValueType::Minimum)
				{
					InControlElement->Settings.MinimumValue = InValue;
					InControlElement->Settings.ApplyLimits(InControlElement->Settings.MinimumValue);
				}
				else
				{
					InControlElement->Settings.MaximumValue = InValue;
					InControlElement->Settings.ApplyLimits(InControlElement->Settings.MaximumValue);
				}
				
				Notify(ERigHierarchyNotification::ControlSettingChanged, InControlElement);

#if WITH_EDITOR
				if (ensure(!bPropagatingChange))
				{
					TGuardValue<bool> bPropagatingChangeGuardValue(bPropagatingChange, true);
			
					for(FRigHierarchyListener& Listener : ListeningHierarchies)
					{
						URigHierarchy* ListeningHierarchy = Listener.Hierarchy.Get();
					
						if (ListeningHierarchy)
						{
							if(FRigControlElement* ListeningElement = Cast<FRigControlElement>(ListeningHierarchy->Find(InControlElement->GetKey())))
							{
								ListeningHierarchy->SetControlValue(ListeningElement, InValue, InValueType, false, bForce);
							}
						}
					}
				}
#endif
				break;
			}
		}	
	}
}

void URigHierarchy::SetControlVisibility(FRigControlElement* InControlElement, bool bVisibility)
{
	if(InControlElement == nullptr)
	{
		return;
	}

	InControlElement->Settings.bGizmoVisible = bVisibility;
	Notify(ERigHierarchyNotification::ControlVisibilityChanged, InControlElement);

#if WITH_EDITOR
	if (ensure(!bPropagatingChange))
	{
		TGuardValue<bool> bPropagatingChangeGuardValue(bPropagatingChange, true);
			
		for(FRigHierarchyListener& Listener : ListeningHierarchies)
		{
			URigHierarchy* ListeningHierarchy = Listener.Hierarchy.Get();
			if (ListeningHierarchy)
			{
				if(FRigControlElement* ListeningElement = Cast<FRigControlElement>(ListeningHierarchy->Find(InControlElement->GetKey())))
				{
					ListeningHierarchy->SetControlVisibility(ListeningElement, bVisibility);
				}
			}
		}
	}
#endif
}

float URigHierarchy::GetCurveValue(FRigCurveElement* InCurveElement) const
{
	if(InCurveElement == nullptr)
	{
		return 0.f;
	}
	return InCurveElement->Value;
}

void URigHierarchy::SetCurveValue(FRigCurveElement* InCurveElement, float InValue, bool bSetupUndo, bool bForce)
{
	if(InCurveElement == nullptr)
	{
		return;
	}

	const float PreviousValue = InCurveElement->Value;
	if(!bForce && FMath::IsNearlyZero(PreviousValue - InValue))
	{
		return;
	}

	InCurveElement->Value = InValue;

#if WITH_EDITOR
	if(bSetupUndo || IsTracingChanges())
	{
		PushCurveToStack(InCurveElement->GetKey(), PreviousValue, InCurveElement->Value, bSetupUndo);
	}

	if (ensure(!bPropagatingChange))
	{
		TGuardValue<bool> bPropagatingChangeGuardValue(bPropagatingChange, true);
			
		for(FRigHierarchyListener& Listener : ListeningHierarchies)
		{
			if(!Listener.Hierarchy.IsValid())
			{
				continue;
			}

			URigHierarchy* ListeningHierarchy = Listener.Hierarchy.Get();
			if (ListeningHierarchy)
			{
				if(FRigCurveElement* ListeningElement = Cast<FRigCurveElement>(ListeningHierarchy->Find(InCurveElement->GetKey())))
				{
					// bSetupUndo = false such that all listening hierarchies performs undo at the same time the root hierachy undos
					ListeningHierarchy->SetCurveValue(ListeningElement, InValue, false, bForce);
				}
			}
		}
	}
#endif
}

FName URigHierarchy::GetPreviousName(const FRigElementKey& InKey) const
{
	if(const FRigElementKey* OldKeyPtr = PreviousNameMap.Find(InKey))
	{
		return OldKeyPtr->Name;
	}
	return NAME_None;
}

FRigElementKey URigHierarchy::GetPreviousParent(const FRigElementKey& InKey) const
{
	if(const FRigElementKey* OldParentPtr = PreviousParentMap.Find(InKey))
	{
		return *OldParentPtr;
	}
	return FRigElementKey();
}

bool URigHierarchy::IsParentedTo(FRigBaseElement* InChild, FRigBaseElement* InParent) const
{
	if((InChild == nullptr) || (InParent == nullptr))
	{
		return false;
	}

	if(InChild == InParent)
	{
		return true;
	}

	if(FRigSingleParentElement* SingleParentElement = Cast<FRigSingleParentElement>(InChild))
	{
		if(SingleParentElement->ParentElement == InParent)
		{
			return true;
		}
		return IsParentedTo(SingleParentElement->ParentElement, InParent);
	}

	if(FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(InChild))
	{
		for(const FRigElementParentConstraint& ParentConstraint : MultiParentElement->ParentConstraints)
		{
			if(ParentConstraint.ParentElement == InParent)
			{
				return true;
			}
			if(IsParentedTo(ParentConstraint.ParentElement, InParent))
			{
				return true;
			}
		}
	}

	return false;
}

bool URigHierarchy::IsTracingChanges() const
{
#if WITH_EDITOR
	return (CVarControlRigHierarchyTraceAlways->GetInt() != 0) || (TraceFramesLeft > 0);
#else
	return false;
#endif
}

#if WITH_EDITOR

void URigHierarchy::ResetTransformStack()
{
	TransformUndoStack.Reset();
	TransformRedoStack.Reset();
	TransformStackIndex = TransformUndoStack.Num();

	if(IsTracingChanges())
	{
		TracePoses.Reset();
		StorePoseForTrace(TEXT("BeginOfFrame"));
	}
}

void URigHierarchy::StorePoseForTrace(const FString& InPrefix)
{
	check(!InPrefix.IsEmpty());
	
	FName InitialKey = *FString::Printf(TEXT("%s_Initial"), *InPrefix);
	FName CurrentKey = *FString::Printf(TEXT("%s_Current"), *InPrefix);
	TracePoses.FindOrAdd(InitialKey) = GetPose(true);
	TracePoses.FindOrAdd(CurrentKey) = GetPose(false);
}

void URigHierarchy::CheckTraceFormatIfRequired()
{
	if(sRigHierarchyLastTrace != CVarControlRigHierarchyTracePrecision->GetInt())
	{
		sRigHierarchyLastTrace = CVarControlRigHierarchyTracePrecision->GetInt();
		const FString Format = FString::Printf(TEXT("%%.%df"), sRigHierarchyLastTrace);
		check(Format.Len() < 16);
		sRigHierarchyTraceFormat[Format.Len()] = '\0';
		FMemory::Memcpy(sRigHierarchyTraceFormat, *Format, Format.Len() * sizeof(TCHAR));
	}
}

template <class CharType>
struct TRigHierarchyJsonPrintPolicy
	: public TPrettyJsonPrintPolicy<CharType>
{
	static inline void WriteDouble(  FArchive* Stream, double Value )
	{
		URigHierarchy::CheckTraceFormatIfRequired();
		TJsonPrintPolicy<CharType>::WriteString(Stream, FString::Printf(sRigHierarchyTraceFormat, Value));
	}
};

void URigHierarchy::DumpTransformStackToFile(FString* OutFilePath)
{
	if(IsTracingChanges())
	{
		StorePoseForTrace(TEXT("EndOfFrame"));
	}

	FString PathName = GetPathName();
	PathName.Split(TEXT(":"), nullptr, &PathName);
	PathName.ReplaceCharInline('.', '/');

	FString Suffix;
	if(TraceFramesLeft > 0)
	{
		Suffix = FString::Printf(TEXT("_Trace_%03d"), TraceFramesCaptured);
	}

	FString FileName = FString::Printf(TEXT("%sControlRig/%s%s.json"), *FPaths::ProjectLogDir(), *PathName, *Suffix);
	FString FullFilename = FPlatformFileManager::Get().GetPlatformFile().ConvertToAbsolutePathForExternalAppForWrite(*FileName);

	TSharedPtr<FJsonObject> JsonData = MakeShareable(new FJsonObject);
	JsonData->SetStringField(TEXT("PathName"), GetPathName());

	TSharedRef<FJsonObject> JsonTracedPoses = MakeShareable(new FJsonObject);
	for(const TPair<FName, FRigPose>& Pair : TracePoses)
	{
		TSharedRef<FJsonObject> JsonTracedPose = MakeShareable(new FJsonObject);
		if (FJsonObjectConverter::UStructToJsonObject(FRigPose::StaticStruct(), &Pair.Value, JsonTracedPose, 0, 0))
		{
			JsonTracedPoses->SetObjectField(Pair.Key.ToString(), JsonTracedPose);
		}
	}
	JsonData->SetObjectField(TEXT("TracedPoses"), JsonTracedPoses);

	TArray<TSharedPtr<FJsonValue>> JsonTransformStack;
	for (const FRigTransformStackEntry& TransformStackEntry : TransformUndoStack)
	{
		TSharedRef<FJsonObject> JsonTransformStackEntry = MakeShareable(new FJsonObject);
		if (FJsonObjectConverter::UStructToJsonObject(FRigTransformStackEntry::StaticStruct(), &TransformStackEntry, JsonTransformStackEntry, 0, 0))
		{
			JsonTransformStack.Add(MakeShareable(new FJsonValueObject(JsonTransformStackEntry)));
		}
	}
	JsonData->SetArrayField(TEXT("TransformStack"), JsonTransformStack);

	FString JsonText;
	const TSharedRef< TJsonWriter< TCHAR, TRigHierarchyJsonPrintPolicy<TCHAR> > > JsonWriter = TJsonWriterFactory< TCHAR, TRigHierarchyJsonPrintPolicy<TCHAR> >::Create(&JsonText);
	if (FJsonSerializer::Serialize(JsonData.ToSharedRef(), JsonWriter))
	{
		if ( FFileHelper::SaveStringToFile(JsonText, *FullFilename) )
		{
			UE_LOG(LogControlRig, Display, TEXT("Saved hierarchy trace to %s"), *FullFilename);

			if(OutFilePath)
			{
				*OutFilePath = FullFilename;
			}
		}
	}

	TraceFramesLeft = FMath::Max(0, TraceFramesLeft - 1);
	TraceFramesCaptured++;
}

void URigHierarchy::TraceFrames(int32 InNumFramesToTrace)
{
	TraceFramesLeft = InNumFramesToTrace;
	TraceFramesCaptured = 0;
	ResetTransformStack();
}

#endif

bool URigHierarchy::IsSelected(const FRigBaseElement* InElement) const
{
	if(InElement == nullptr)
	{
		return false;
	}
	if(URigHierarchy* HierarchyForSelection = HierarchyForSelectionPtr.Get())
	{
		return HierarchyForSelection->IsSelected(InElement->GetKey());
	}
	return InElement->IsSelected();
}

void URigHierarchy::ResetCachedChildren()
{
	for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ElementIndex++)
	{
		FRigBaseElement* Element = Elements[ElementIndex];
		Element->CachedChildren.Reset();
	}
}

void URigHierarchy::UpdateCachedChildren(const FRigBaseElement* InElement, bool bForce) const
{
	check(InElement);

	if(InElement->TopologyVersion == TopologyVersion && !bForce)
	{
		return;
	}
	
	InElement->CachedChildren.Reset();
	
	for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ElementIndex++)
	{
		FRigBaseElement* Element = Elements[ElementIndex];
		if(FRigSingleParentElement* SingleParentElement = Cast<FRigSingleParentElement>(Element))
		{
			if(SingleParentElement->ParentElement == InElement)
			{
				InElement->CachedChildren.Add(SingleParentElement);
			}
		}
		else if(FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(Element))
		{
			for(const FRigElementParentConstraint& ParentConstraint : MultiParentElement->ParentConstraints)
			{
				if(ParentConstraint.ParentElement == InElement)
				{
					InElement->CachedChildren.Add(MultiParentElement);
					break;
				}
			}
		}
	}

	InElement->TopologyVersion = TopologyVersion;
}

void URigHierarchy::UpdateAllCachedChildren() const
{
	TArray<bool> ParentVisited;
	ParentVisited.AddZeroed(Elements.Num());
	
	for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ElementIndex++)
	{
		FRigBaseElement* Element = Elements[ElementIndex];
		Element->TopologyVersion = TopologyVersion;
		
		if(FRigSingleParentElement* SingleParentElement = Cast<FRigSingleParentElement>(Element))
		{
			if(FRigTransformElement* ParentElement = SingleParentElement->ParentElement)
			{
				if(!ParentVisited[ParentElement->Index])
				{
					ParentElement->CachedChildren.Reset();
					ParentVisited[ParentElement->Index] = true;
				}
				ParentElement->CachedChildren.Add(Element);
			}
		}
		else if(FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(Element))
		{
			for(const FRigElementParentConstraint& ParentConstraint : MultiParentElement->ParentConstraints)
			{
				if(ParentConstraint.ParentElement)
				{
					if(!ParentVisited[ParentConstraint.ParentElement->Index])
					{
						ParentConstraint.ParentElement->CachedChildren.Reset();
						ParentVisited[ParentConstraint.ParentElement->Index] = true;
					}
					ParentConstraint.ParentElement->CachedChildren.Add(Element);
				}
			}
		}
	}
}

FRigBaseElement* URigHierarchy::MakeElement(ERigElementType InElementType, int32 InCount, int32* OutStructureSize)
{
	check(InCount > 0);
	
	FRigBaseElement* Element = nullptr;
	switch(InElementType)
	{
		case ERigElementType::Bone:
		{
			if(OutStructureSize)
			{
				*OutStructureSize = sizeof(FRigBoneElement);
			}
			FRigBoneElement* Elements = (FRigBoneElement*)FMemory::Malloc(sizeof(FRigBoneElement) * InCount);
			for(int32 Index=0;Index<InCount;Index++)
			{
				new(&Elements[Index]) FRigBoneElement(); 
			} 
			Element = Elements;
			break;
		}
		case ERigElementType::Null:
		{
			if(OutStructureSize)
			{
				*OutStructureSize = sizeof(FRigNullElement);
			}
			FRigNullElement* Elements = (FRigNullElement*)FMemory::Malloc(sizeof(FRigNullElement) * InCount);
			for(int32 Index=0;Index<InCount;Index++)
			{
				new(&Elements[Index]) FRigNullElement(); 
			} 
			Element = Elements;
			break;
		}
		case ERigElementType::Control:
		{
			if(OutStructureSize)
			{
				*OutStructureSize = sizeof(FRigControlElement);
			}
			FRigControlElement* Elements = (FRigControlElement*)FMemory::Malloc(sizeof(FRigControlElement) * InCount);
			for(int32 Index=0;Index<InCount;Index++)
			{
				new(&Elements[Index]) FRigControlElement(); 
			} 
			Element = Elements;
			break;
		}
		case ERigElementType::Curve:
		{
			if(OutStructureSize)
			{
				*OutStructureSize = sizeof(FRigCurveElement);
			}
			FRigCurveElement* Elements = (FRigCurveElement*)FMemory::Malloc(sizeof(FRigCurveElement) * InCount);
			for(int32 Index=0;Index<InCount;Index++)
			{
				new(&Elements[Index]) FRigCurveElement(); 
			} 
			Element = Elements;
			break;
		}
		case ERigElementType::RigidBody:
		{
			if(OutStructureSize)
			{
				*OutStructureSize = sizeof(FRigRigidBodyElement);
			}
			FRigRigidBodyElement* Elements = (FRigRigidBodyElement*)FMemory::Malloc(sizeof(FRigRigidBodyElement) * InCount);
			for(int32 Index=0;Index<InCount;Index++)
			{
				new(&Elements[Index]) FRigRigidBodyElement(); 
			} 
			Element = Elements;
			break;
		}
		case ERigElementType::Socket:
		{
			if(OutStructureSize)
			{
				*OutStructureSize = sizeof(FRigSocketElement);
			}
			FRigSocketElement* Elements = (FRigSocketElement*)FMemory::Malloc(sizeof(FRigSocketElement) * InCount);
			for(int32 Index=0;Index<InCount;Index++)
			{
				new(&Elements[Index]) FRigSocketElement(); 
			} 
			Element = Elements;
			break;
		}
		default:
		{
			ensure(false);
		}
	}

	if(Element)
	{
		Element->OwnedInstances = InCount;
	}
	return Element;
}

void URigHierarchy::DestroyElement(FRigBaseElement*& InElement)
{
	check(InElement != nullptr);

	if(InElement->OwnedInstances == 0)
	{
		return;
	}

	const int32 Count = InElement->OwnedInstances;
	switch(InElement->GetType())
	{
		case ERigElementType::Bone:
		{
			FRigBoneElement* Elements = Cast<FRigBoneElement>(InElement);
			for(int32 Index=0;Index<Count;Index++)
			{
				Elements[Index].~FRigBoneElement(); 
			}
			break;
		}
		case ERigElementType::Null:
		{
			FRigNullElement* Elements = Cast<FRigNullElement>(InElement);
			for(int32 Index=0;Index<Count;Index++)
			{
				Elements[Index].~FRigNullElement(); 
			}
			break;
		}
		case ERigElementType::Control:
		{
			FRigControlElement* Elements = Cast<FRigControlElement>(InElement);
			for(int32 Index=0;Index<Count;Index++)
			{
				Elements[Index].~FRigControlElement(); 
			}
			break;
		}
		case ERigElementType::Curve:
		{
			FRigCurveElement* Elements = Cast<FRigCurveElement>(InElement);
			for(int32 Index=0;Index<Count;Index++)
			{
				Elements[Index].~FRigCurveElement(); 
			}
			break;
		}
		case ERigElementType::RigidBody:
		{
			FRigRigidBodyElement* Elements = Cast<FRigRigidBodyElement>(InElement);
			for(int32 Index=0;Index<Count;Index++)
			{
				Elements[Index].~FRigRigidBodyElement(); 
			}
			break;
		}
		case ERigElementType::Socket:
		{
			FRigSocketElement* Elements = Cast<FRigSocketElement>(InElement);
			for(int32 Index=0;Index<Count;Index++)
			{
				Elements[Index].~FRigSocketElement(); 
			}
			break;
		}
		default:
		{
			ensure(false);
			return;
		}
	}

	FMemory::Free(InElement);
	InElement = nullptr;
}

#if URIGHIERARCHY_RECURSIVE_DIRTY_PROPAGATION
void URigHierarchy::PropagateDirtyFlags(FRigTransformElement* InTransformElement, bool bInitial, bool bAffectChildren, bool bComputeOpposed, bool bMarkDirty) const
#else
void URigHierarchy::PropagateDirtyFlags(FRigTransformElement* InTransformElement, bool bInitial, bool bAffectChildren) const
#endif
{
	if(!bEnableDirtyPropagation)
	{
		return;
	}
	
	check(InTransformElement);

	const ERigTransformType::Type LocalType = bInitial ? ERigTransformType::InitialLocal : ERigTransformType::CurrentLocal;
	const ERigTransformType::Type GlobalType = bInitial ? ERigTransformType::InitialGlobal : ERigTransformType::CurrentGlobal;
	const ERigTransformType::Type TypeToCompute = bAffectChildren ? LocalType : GlobalType;
	const ERigTransformType::Type TypeToDirty = SwapLocalAndGlobal(TypeToCompute);

#if URIGHIERARCHY_RECURSIVE_DIRTY_PROPAGATION
	if(bComputeOpposed)
#endif
	{
		for(const FRigTransformElement::FElementToDirty& ElementToDirty : InTransformElement->ElementsToDirty)
		{
#if URIGHIERARCHY_RECURSIVE_DIRTY_PROPAGATION
			if(FRigControlElement* ControlElement = Cast<FRigControlElement>(ElementToDirty.Element))
			{
				if(ERigTransformType::IsGlobal(TypeToDirty))
				{
					if(ControlElement->Parent.IsDirty(TypeToDirty) &&
						ControlElement->Offset.IsDirty(TypeToDirty) &&
						ControlElement->Pose.IsDirty(TypeToDirty))
					{
						continue;
					}
				}
				else
				{
					if(ControlElement->Parent.IsDirty(TypeToDirty))
					{
						continue;
					}
				}
			}
			else if(FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(ElementToDirty.Element))
			{
				if(ERigTransformType::IsGlobal(TypeToDirty))
				{
					if(MultiParentElement->Parent.IsDirty(TypeToDirty) &&
						MultiParentElement->Pose.IsDirty(TypeToDirty))
					{
						continue;
					}
				}
				else
				{
					if(MultiParentElement->Parent.IsDirty(TypeToDirty))
					{
						continue;
					}
				}
			}
			else
			{
				if(ElementToDirty.Element->Pose.IsDirty(TypeToDirty))
				{
					continue;
				}
			}
#else

			if(!bAffectChildren && ElementToDirty.HierarchyDistance > 1)
			{
				continue;
			}

#endif

			if(FRigControlElement* ControlElement = Cast<FRigControlElement>(ElementToDirty.Element))
			{
				GetControlOffsetTransform(ControlElement, LocalType);
			}
			GetTransform(ElementToDirty.Element, TypeToCompute); // make sure the local / global transform is up 2 date

#if URIGHIERARCHY_RECURSIVE_DIRTY_PROPAGATION
			PropagateDirtyFlags(ElementToDirty.Element, bInitial, bAffectChildren, true, false);
#endif
		}
	}
#if URIGHIERARCHY_RECURSIVE_DIRTY_PROPAGATION

	if(bMarkDirty)
#endif
	{
		for(const FRigTransformElement::FElementToDirty& ElementToDirty : InTransformElement->ElementsToDirty)
		{
#if URIGHIERARCHY_RECURSIVE_DIRTY_PROPAGATION

			if(FRigControlElement* ControlElement = Cast<FRigControlElement>(ElementToDirty.Element))
			{
				if(ERigTransformType::IsGlobal(TypeToDirty))
				{
					if(ControlElement->Parent.IsDirty(TypeToDirty) &&
						ControlElement->Offset.IsDirty(TypeToDirty) &&
						ControlElement->Pose.IsDirty(TypeToDirty))
					{
						continue;
					}
				}
				else
				{
					if(ControlElement->Parent.IsDirty(TypeToDirty))
					{
						continue;
					}
				}
			}
			else if(FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(ElementToDirty.Element))
			{
				if(ERigTransformType::IsGlobal(TypeToDirty))
				{
					if(MultiParentElement->Parent.IsDirty(TypeToDirty) &&
						MultiParentElement->Pose.IsDirty(TypeToDirty))
					{
						continue;
					}
				}
				else
				{
					if(MultiParentElement->Parent.IsDirty(TypeToDirty))
					{
						continue;
					}
				}
			}
			else
			{
				if(ElementToDirty.Element->Pose.IsDirty(TypeToDirty))
				{
					continue;
				}
			}
						
#else

			if(!bAffectChildren && ElementToDirty.HierarchyDistance > 1)
			{
				continue;
			}
			
#endif

			ElementToDirty.Element->Pose.MarkDirty(TypeToDirty);
		
			if(FRigMultiParentElement* MultiParentElement = Cast<FRigMultiParentElement>(ElementToDirty.Element))
			{
				MultiParentElement->Parent.MarkDirty(GlobalType);
			}
			if(FRigControlElement* ControlElement = Cast<FRigControlElement>(ElementToDirty.Element))
			{
				ControlElement->Offset.MarkDirty(GlobalType);
				ControlElement->Gizmo.MarkDirty(GlobalType);
			}

#if URIGHIERARCHY_RECURSIVE_DIRTY_PROPAGATION

			if(bAffectChildren)
			{
#if URIGHIERARCHY_RECURSIVE_DIRTY_PROPAGATION
				PropagateDirtyFlags(ElementToDirty.Element, bInitial, bAffectChildren, false, true);
#endif
			}
			
#endif
		}
	}
}

void URigHierarchy::PushTransformToStack(const FRigElementKey& InKey, ERigTransformStackEntryType InEntryType,
	ERigTransformType::Type InTransformType, const FTransform& InOldTransform, const FTransform& InNewTransform,
	bool bAffectChildren, bool bModify)
{
#if WITH_EDITOR

	if(GIsTransacting)
	{
		return;
	}

	static const FText TransformPoseTitle = NSLOCTEXT("RigHierarchy", "Set Pose Transform", "Set Pose Transform");
	static const FText ControlOffsetTitle = NSLOCTEXT("RigHierarchy", "Set Control Offset", "Set Control Offset");
	static const FText ControlGizmoTitle = NSLOCTEXT("RigHierarchy", "Set Control Gizo", "Set Control Gizo");
	static const FText CurveValueTitle = NSLOCTEXT("RigHierarchy", "Set Curve Value", "Set Curve Value");
	
	FText Title;
	switch(InEntryType)
	{
		case ERigTransformStackEntryType::TransformPose:
		{
			Title = TransformPoseTitle;
			break;
		}
		case ERigTransformStackEntryType::ControlOffset:
		{
			Title = TransformPoseTitle;
			break;
		}
		case ERigTransformStackEntryType::ControlGizmo:
		{
			Title = TransformPoseTitle;
			break;
		}
		case ERigTransformStackEntryType::CurveValue:
		{
			Title = TransformPoseTitle;
			break;
		}
	}

	TGuardValue<bool> TransactingGuard(bTransactingForTransformChange, true);

	TSharedPtr<FScopedTransaction> TransactionPtr;
	if(bModify)
	{
		TransactionPtr = MakeShareable(new FScopedTransaction(Title));
	}

	if(bIsInteracting)
	{
		bool bCanMerge = LastInteractedKey == InKey;

		FRigTransformStackEntry LastEntry;
		if(!TransformUndoStack.IsEmpty())
		{
			LastEntry = TransformUndoStack.Last();
		}

		if(bCanMerge && LastEntry.Key == InKey && LastEntry.EntryType == InEntryType && LastEntry.bAffectChildren == bAffectChildren)
		{
			// merge the entries on the stack
			TransformUndoStack.Last() = 
                FRigTransformStackEntry(InKey, InEntryType, InTransformType, LastEntry.OldTransform, InNewTransform, bAffectChildren);
		}
		else
		{
			Modify();

			TransformUndoStack.Add(
                FRigTransformStackEntry(InKey, InEntryType, InTransformType, InOldTransform, InNewTransform, bAffectChildren));
			TransformStackIndex = TransformUndoStack.Num();
		}

		TransformRedoStack.Reset();
		LastInteractedKey = InKey;
		return;
	}

	if(bModify)
	{
		Modify();
	}

	TArray<FString> Callstack;
	if(IsTracingChanges() && (CVarControlRigHierarchyTraceCallstack->GetInt() != 0))
	{
		FString JoinedCallStack;
		RigHierarchyCaptureCallStack(JoinedCallStack, 1);
		JoinedCallStack.ReplaceInline(TEXT("\r"), TEXT(""));

		FString Left, Right;
		do
		{
			if(!JoinedCallStack.Split(TEXT("\n"), &Left, &Right))
			{
				Left = JoinedCallStack;
				Right.Empty();
			}

			Left.TrimStartAndEndInline();
			if(Left.StartsWith(TEXT("0x")))
			{
				Left.Split(TEXT(" "), nullptr, &Left);
			}
			Callstack.Add(Left);
			JoinedCallStack = Right;
		}
		while(!JoinedCallStack.IsEmpty());
	}

	TransformUndoStack.Add(
		FRigTransformStackEntry(InKey, InEntryType, InTransformType, InOldTransform, InNewTransform, bAffectChildren, Callstack));
	TransformStackIndex = TransformUndoStack.Num();

	TransformRedoStack.Reset();
	
#endif
}

void URigHierarchy::PushCurveToStack(const FRigElementKey& InKey, float InOldCurveValue, float InNewCurveValue, bool bModify)
{
#if WITH_EDITOR

	FTransform OldTransform = FTransform::Identity;
	FTransform NewTransform = FTransform::Identity;

	OldTransform.SetTranslation(FVector(InOldCurveValue, 0.f, 0.f));
	NewTransform.SetTranslation(FVector(InNewCurveValue, 0.f, 0.f));

	PushTransformToStack(InKey, ERigTransformStackEntryType::CurveValue, ERigTransformType::CurrentLocal, OldTransform, NewTransform, false, bModify);

#endif
}

bool URigHierarchy::ApplyTransformFromStack(const FRigTransformStackEntry& InEntry, bool bUndo)
{
#if WITH_EDITOR

	bool bApplyInitialForCurrent = false;
	FRigBaseElement* Element = Find(InEntry.Key);
	if(Element == nullptr)
	{
		// this might be a transient control which had been removed.
		if(InEntry.Key.Type == ERigElementType::Control)
		{
			const FRigElementKey TargetKey = UControlRig::GetElementKeyFromTransientControl(InEntry.Key);
			Element = Find(TargetKey);
			bApplyInitialForCurrent = Element != nullptr;
		}

		if(Element == nullptr)
		{
			return false;
		}
	}

	const FTransform& Transform = bUndo ? InEntry.OldTransform : InEntry.NewTransform;
	
	switch(InEntry.EntryType)
	{
		case ERigTransformStackEntryType::TransformPose:
		{
			SetTransform(Cast<FRigTransformElement>(Element), Transform, InEntry.TransformType, InEntry.bAffectChildren, false);

			if(ERigTransformType::IsCurrent(InEntry.TransformType) && bApplyInitialForCurrent)
			{
				SetTransform(Cast<FRigTransformElement>(Element), Transform, ERigTransformType::MakeInitial(InEntry.TransformType), InEntry.bAffectChildren, false);
			}
			break;
		}
		case ERigTransformStackEntryType::ControlOffset:
		{
			SetControlOffsetTransform(Cast<FRigControlElement>(Element), Transform, InEntry.TransformType, InEntry.bAffectChildren, false); 
			break;
		}
		case ERigTransformStackEntryType::ControlGizmo:
		{
			SetControlGizmoTransform(Cast<FRigControlElement>(Element), Transform, InEntry.TransformType, false); 
			break;
		}
		case ERigTransformStackEntryType::CurveValue:
		{
			const float CurveValue = Transform.GetTranslation().X;
			SetCurveValue(Cast<FRigCurveElement>(Element), CurveValue, false);
			break;
		}
	}

	return true;

#endif

	return false;
}

void URigHierarchy::ComputeAllTransforms()
{
	for(int32 ElementIndex = 0; ElementIndex < Elements.Num(); ElementIndex++)
	{
		for(int32 TransformTypeIndex = 0; TransformTypeIndex < (int32) ERigTransformType::NumTransformTypes; TransformTypeIndex++)
		{
			const ERigTransformType::Type TransformType = (ERigTransformType::Type)TransformTypeIndex; 
			if(FRigTransformElement* TransformElement = Get<FRigTransformElement>(ElementIndex))
			{
				GetTransform(TransformElement, TransformType);
			}
			if(FRigControlElement* ControlElement = Get<FRigControlElement>(ElementIndex))
			{
				GetControlOffsetTransform(ControlElement, TransformType);
				GetControlGizmoTransform(ControlElement, TransformType);
			}
		}
	}
}

FTransform URigHierarchy::GetWorldTransformForSocket(const FRigUnitContext* InContext, const FRigElementKey& InKey, bool bInitial)
{
	if(const USceneComponent* OuterSceneComponent = GetTypedOuter<USceneComponent>())
	{
		return OuterSceneComponent->GetComponentToWorld().Inverse();
	}
	return FTransform::Identity;
}

FTransform URigHierarchy::ComputeLocalControlValue(FRigControlElement* ControlElement,
	const FTransform& InGlobalTransform, ERigTransformType::Type InTransformType) const
{
	check(ERigTransformType::IsGlobal(InTransformType));

	const FTransform OffsetTransform =
		GetControlOffsetTransform(ControlElement, ERigTransformType::MakeLocal(InTransformType));

	return InverseSolveParentConstraints(
		InGlobalTransform,
		ControlElement->ParentConstraints,
		InTransformType,
		OffsetTransform);
}

FTransform URigHierarchy::SolveParentConstraints(
	const FRigElementParentConstraintArray& InConstraints,
	const ERigTransformType::Type InTransformType,
	const FTransform& InLocalOffsetTransform,
	bool bApplyLocalOffsetTransform,
	const FTransform& InLocalPoseTransform,
	bool bApplyLocalPoseTransform) const
{
	FTransform Result = FTransform::Identity;
	const bool bInitial = IsInitial(InTransformType);

	// collect all of the weights
	FConstraintIndex FirstConstraint;
	FConstraintIndex SecondConstraint;
	FConstraintIndex NumConstraintsAffecting(0);
	FRigElementWeight TotalWeight(0.f);
	ComputeParentConstraintIndices(InConstraints, InTransformType, FirstConstraint, SecondConstraint, NumConstraintsAffecting, TotalWeight);

	if(NumConstraintsAffecting.Location == 0 ||
		NumConstraintsAffecting.Rotation == 0 ||
		NumConstraintsAffecting.Scale == 0)
	{
		if(bApplyLocalOffsetTransform)
		{
			Result = InLocalOffsetTransform;
		}
		
		if(bApplyLocalPoseTransform)
		{
			Result = InLocalPoseTransform * Result;
		}

		if(NumConstraintsAffecting.Location == 0 &&
			NumConstraintsAffecting.Rotation == 0 &&
			NumConstraintsAffecting.Scale == 0)
		{
			Result.NormalizeRotation();
			return Result;
		}
	}

	if(NumConstraintsAffecting.Location == 1)
	{
		check(FirstConstraint.Location != INDEX_NONE);

		const FRigElementParentConstraint& ParentConstraint = InConstraints[FirstConstraint.Location];
		const FRigElementWeight& Weight = ParentConstraint.GetWeight(bInitial);
		const FTransform Transform = LazilyComputeParentConstraint(InConstraints, FirstConstraint.Location, InTransformType,
			InLocalOffsetTransform, bApplyLocalOffsetTransform, InLocalPoseTransform, bApplyLocalPoseTransform);

		check(Weight.AffectsLocation());
		Result.SetLocation(Transform.GetLocation());
	}
	else if(NumConstraintsAffecting.Location == 2)
	{
		check(FirstConstraint.Location != INDEX_NONE);
		check(SecondConstraint.Location != INDEX_NONE);

		const FRigElementParentConstraint& ParentConstraintA = InConstraints[FirstConstraint.Location];
		const FRigElementParentConstraint& ParentConstraintB = InConstraints[SecondConstraint.Location];

		const FRigElementWeight& WeightA = ParentConstraintA.GetWeight(bInitial); 
		const FRigElementWeight& WeightB = ParentConstraintB.GetWeight(bInitial);
		check(WeightA.AffectsLocation());
		check(WeightB.AffectsLocation());
		const float Weight = GetWeightForLerp(WeightA.Location, WeightB.Location);

		const FTransform TransformA = LazilyComputeParentConstraint(InConstraints, FirstConstraint.Location, InTransformType,
			InLocalOffsetTransform, bApplyLocalOffsetTransform, InLocalPoseTransform, bApplyLocalPoseTransform);
		const FTransform TransformB = LazilyComputeParentConstraint(InConstraints, SecondConstraint.Location, InTransformType,
			InLocalOffsetTransform, bApplyLocalOffsetTransform, InLocalPoseTransform, bApplyLocalPoseTransform);

		const FVector ParentLocationA = TransformA.GetLocation();
		const FVector ParentLocationB = TransformB.GetLocation();
		Result.SetLocation(FMath::Lerp<FVector>(ParentLocationA, ParentLocationB, Weight));
	}
	else if(NumConstraintsAffecting.Location > 2)
	{
		check(TotalWeight.Location > SMALL_NUMBER);
		
		FVector Location = FVector::ZeroVector;
		
		for(int32 ConstraintIndex = 0; ConstraintIndex < InConstraints.Num(); ConstraintIndex++)
		{
			const FRigElementParentConstraint& ParentConstraint = InConstraints[ConstraintIndex];
			const FRigElementWeight& Weight = ParentConstraint.GetWeight(bInitial);
			if(!Weight.AffectsLocation())
			{
				continue;
			}

			const FTransform Transform = LazilyComputeParentConstraint(InConstraints, ConstraintIndex, InTransformType,
				InLocalOffsetTransform, bApplyLocalOffsetTransform, InLocalPoseTransform, bApplyLocalPoseTransform);

			IntegrateParentConstraintVector(Location, Transform, Weight.Location / TotalWeight.Location, true);
		}

		Result.SetLocation(Location);
	}

	if(NumConstraintsAffecting.Rotation == 1)
	{
		check(FirstConstraint.Rotation != INDEX_NONE);

		const FRigElementParentConstraint& ParentConstraint = InConstraints[FirstConstraint.Rotation];
		const FRigElementWeight& Weight = ParentConstraint.GetWeight(bInitial);
		const FTransform Transform = LazilyComputeParentConstraint(InConstraints, FirstConstraint.Rotation, InTransformType,
			InLocalOffsetTransform, bApplyLocalOffsetTransform, InLocalPoseTransform, bApplyLocalPoseTransform);
		check(Weight.AffectsRotation());
		Result.SetRotation(Transform.GetRotation());
	}
	else if(NumConstraintsAffecting.Rotation == 2)
	{
		check(FirstConstraint.Rotation != INDEX_NONE);
		check(SecondConstraint.Rotation != INDEX_NONE);

		const FRigElementParentConstraint& ParentConstraintA = InConstraints[FirstConstraint.Rotation];
		const FRigElementParentConstraint& ParentConstraintB = InConstraints[SecondConstraint.Rotation];

		const FRigElementWeight& WeightA = ParentConstraintA.GetWeight(bInitial); 
		const FRigElementWeight& WeightB = ParentConstraintB.GetWeight(bInitial);
		check(WeightA.AffectsRotation());
		check(WeightB.AffectsRotation());
		const float Weight = GetWeightForLerp(WeightA.Rotation, WeightB.Rotation);

		const FTransform TransformA = LazilyComputeParentConstraint(InConstraints, FirstConstraint.Rotation, InTransformType,
			InLocalOffsetTransform, bApplyLocalOffsetTransform, InLocalPoseTransform, bApplyLocalPoseTransform);
		const FTransform TransformB = LazilyComputeParentConstraint(InConstraints, SecondConstraint.Rotation, InTransformType,
			InLocalOffsetTransform, bApplyLocalOffsetTransform, InLocalPoseTransform, bApplyLocalPoseTransform);

		const FQuat ParentRotationA = TransformA.GetRotation();
		const FQuat ParentRotationB = TransformB.GetRotation();
		Result.SetRotation(FQuat::Slerp(ParentRotationA, ParentRotationB, Weight));
	}
	else if(NumConstraintsAffecting.Rotation > 2)
	{
		check(TotalWeight.Rotation > SMALL_NUMBER);
		
		int NumMixedRotations = 0;
		FQuat FirstRotation = FQuat::Identity;
		FQuat MixedRotation = FQuat(0.f, 0.f, 0.f, 0.f);

		for(int32 ConstraintIndex = 0; ConstraintIndex < InConstraints.Num(); ConstraintIndex++)
		{
			const FRigElementParentConstraint& ParentConstraint = InConstraints[ConstraintIndex];
			const FRigElementWeight& Weight = ParentConstraint.GetWeight(bInitial);
			if(!Weight.AffectsRotation())
			{
				continue;
			}

			const FTransform Transform = LazilyComputeParentConstraint(InConstraints, ConstraintIndex, InTransformType,
				InLocalOffsetTransform, bApplyLocalOffsetTransform, InLocalPoseTransform, bApplyLocalPoseTransform);

			IntegrateParentConstraintQuat(
				NumMixedRotations,
				FirstRotation,
				MixedRotation,
				Transform,
				Weight.Rotation / TotalWeight.Rotation);
		}

		Result.SetRotation(MixedRotation.GetNormalized());
	}

	if(NumConstraintsAffecting.Scale == 1)
	{
		check(FirstConstraint.Scale != INDEX_NONE);

		const FRigElementParentConstraint& ParentConstraint = InConstraints[FirstConstraint.Scale];
		const FRigElementWeight& Weight = ParentConstraint.GetWeight(bInitial);
		
		const FTransform Transform = LazilyComputeParentConstraint(InConstraints, FirstConstraint.Scale, InTransformType,
			InLocalOffsetTransform, bApplyLocalOffsetTransform, InLocalPoseTransform, bApplyLocalPoseTransform);

		check(Weight.AffectsScale());
		Result.SetScale3D(Transform.GetScale3D());
	}
	else if(NumConstraintsAffecting.Scale == 2)
	{
		check(FirstConstraint.Scale != INDEX_NONE);
		check(SecondConstraint.Scale != INDEX_NONE);

		const FRigElementParentConstraint& ParentConstraintA = InConstraints[FirstConstraint.Scale];
		const FRigElementParentConstraint& ParentConstraintB = InConstraints[SecondConstraint.Scale];

		const FRigElementWeight& WeightA = ParentConstraintA.GetWeight(bInitial); 
		const FRigElementWeight& WeightB = ParentConstraintB.GetWeight(bInitial);
		check(WeightA.AffectsScale());
		check(WeightB.AffectsScale());
		const float Weight = GetWeightForLerp(WeightA.Scale, WeightB.Scale);

		const FTransform TransformA = LazilyComputeParentConstraint(InConstraints, FirstConstraint.Scale, InTransformType,
			InLocalOffsetTransform, bApplyLocalOffsetTransform, InLocalPoseTransform, bApplyLocalPoseTransform);
		const FTransform TransformB = LazilyComputeParentConstraint(InConstraints, SecondConstraint.Scale, InTransformType,
			InLocalOffsetTransform, bApplyLocalOffsetTransform, InLocalPoseTransform, bApplyLocalPoseTransform);

		const FVector ParentScaleA = TransformA.GetScale3D();
		const FVector ParentScaleB = TransformB.GetScale3D();
		Result.SetScale3D(FMath::Lerp<FVector>(ParentScaleA, ParentScaleB, Weight));
	}
	else if(NumConstraintsAffecting.Scale > 2)
	{
		check(TotalWeight.Scale > SMALL_NUMBER);
		
		FVector Scale = FVector::ZeroVector;
		
		for(int32 ConstraintIndex = 0; ConstraintIndex < InConstraints.Num(); ConstraintIndex++)
		{
			const FRigElementParentConstraint& ParentConstraint = InConstraints[ConstraintIndex];
			const FRigElementWeight& Weight = ParentConstraint.GetWeight(bInitial);
			if(!Weight.AffectsScale())
			{
				continue;
			}

			const FTransform Transform = LazilyComputeParentConstraint(InConstraints, ConstraintIndex, InTransformType,
				InLocalOffsetTransform, bApplyLocalOffsetTransform, InLocalPoseTransform, bApplyLocalPoseTransform);

			IntegrateParentConstraintVector(Scale, Transform, Weight.Scale / TotalWeight.Scale, false);
		}

		Result.SetScale3D(Scale);
	}

	Result.NormalizeRotation();
	return Result;
}

FTransform URigHierarchy::InverseSolveParentConstraints(
	const FTransform& InGlobalTransform,
	const FRigElementParentConstraintArray& InConstraints,
	const ERigTransformType::Type InTransformType,
	const FTransform& InLocalOffsetTransform) const
{
	FTransform Result = FTransform::Identity;
	const bool bInitial = IsInitial(InTransformType);

	// collect all of the weights
	FConstraintIndex FirstConstraint;
	FConstraintIndex SecondConstraint;
	FConstraintIndex NumConstraintsAffecting(0);
	FRigElementWeight TotalWeight(0.f);
	ComputeParentConstraintIndices(InConstraints, InTransformType, FirstConstraint, SecondConstraint, NumConstraintsAffecting, TotalWeight);

	if(NumConstraintsAffecting.Location == 0 ||
		NumConstraintsAffecting.Rotation == 0 ||
		NumConstraintsAffecting.Scale == 0)
	{
		Result = InGlobalTransform.GetRelativeTransform(InLocalOffsetTransform);
		
		if(NumConstraintsAffecting.Location == 0 &&
			NumConstraintsAffecting.Rotation == 0 &&
			NumConstraintsAffecting.Scale == 0)
		{
			Result.NormalizeRotation();
			return Result;
		}
	}

	if(NumConstraintsAffecting.Location == 1)
	{
		check(FirstConstraint.Location != INDEX_NONE);

		const FRigElementParentConstraint& ParentConstraint = InConstraints[FirstConstraint.Location];
		const FRigElementWeight& Weight = ParentConstraint.GetWeight(bInitial);
		const FTransform Transform = LazilyComputeParentConstraint(InConstraints, FirstConstraint.Location, InTransformType,
			InLocalOffsetTransform, true, FTransform::Identity, false);

		check(Weight.AffectsLocation());
		Result.SetLocation(InGlobalTransform.GetRelativeTransform(Transform).GetLocation());
	}
	else if(NumConstraintsAffecting.Location == 2)
	{
		check(FirstConstraint.Location != INDEX_NONE);
		check(SecondConstraint.Location != INDEX_NONE);

		const FRigElementParentConstraint& ParentConstraintA = InConstraints[FirstConstraint.Location];
		const FRigElementParentConstraint& ParentConstraintB = InConstraints[SecondConstraint.Location];

		const FRigElementWeight& WeightA = ParentConstraintA.GetWeight(bInitial); 
		const FRigElementWeight& WeightB = ParentConstraintB.GetWeight(bInitial);
		check(WeightA.AffectsLocation());
		check(WeightB.AffectsLocation());
		const float Weight = GetWeightForLerp(WeightA.Location, WeightB.Location);

		const FTransform TransformA = LazilyComputeParentConstraint(InConstraints, FirstConstraint.Location, InTransformType,
			InLocalOffsetTransform, true, FTransform::Identity, false);
		const FTransform TransformB = LazilyComputeParentConstraint(InConstraints, SecondConstraint.Location, InTransformType,
			InLocalOffsetTransform, true, FTransform::Identity, false);

		const FTransform MixedTransform = FControlRigMathLibrary::LerpTransform(TransformA, TransformB, Weight);
		Result.SetLocation(InGlobalTransform.GetRelativeTransform(MixedTransform).GetLocation());
	}
	else if(NumConstraintsAffecting.Location > 2)
	{
		check(TotalWeight.Location > SMALL_NUMBER);
		
		FVector Location = FVector::ZeroVector;
		int NumMixedRotations = 0;
		FQuat FirstRotation = FQuat::Identity;
		FQuat MixedRotation = FQuat(0.f, 0.f, 0.f, 0.f);
		FVector Scale = FVector::ZeroVector;

		for(int32 ConstraintIndex = 0; ConstraintIndex < InConstraints.Num(); ConstraintIndex++)
		{
			const FRigElementParentConstraint& ParentConstraint = InConstraints[ConstraintIndex];
			const FRigElementWeight& Weight = ParentConstraint.GetWeight(bInitial);
			if(!Weight.AffectsLocation())
			{
				continue;
			}

			const FTransform Transform = LazilyComputeParentConstraint(InConstraints, ConstraintIndex, InTransformType,
				InLocalOffsetTransform, true, FTransform::Identity, false);

			const float NormalizedWeight = Weight.Location / TotalWeight.Location;
			IntegrateParentConstraintVector(Location, Transform, NormalizedWeight, true);
			IntegrateParentConstraintQuat(NumMixedRotations, FirstRotation, MixedRotation, Transform, NormalizedWeight);
			IntegrateParentConstraintVector(Scale, Transform, NormalizedWeight, false);
		}

		FTransform ParentTransform(MixedRotation.GetNormalized(), Location, Scale);
		Result.SetLocation(InGlobalTransform.GetRelativeTransform(ParentTransform).GetLocation());
	}

	if(NumConstraintsAffecting.Rotation == 1)
	{
		check(FirstConstraint.Rotation != INDEX_NONE);

		const FRigElementParentConstraint& ParentConstraint = InConstraints[FirstConstraint.Rotation];
		const FRigElementWeight& Weight = ParentConstraint.GetWeight(bInitial);
		const FTransform Transform = LazilyComputeParentConstraint(InConstraints, FirstConstraint.Rotation, InTransformType,
			InLocalOffsetTransform, true, FTransform::Identity, false);
		check(Weight.AffectsRotation());
		Result.SetRotation(InGlobalTransform.GetRelativeTransform(Transform).GetRotation());
	}
	else if(NumConstraintsAffecting.Rotation == 2)
	{
		check(FirstConstraint.Rotation != INDEX_NONE);
		check(SecondConstraint.Rotation != INDEX_NONE);

		const FRigElementParentConstraint& ParentConstraintA = InConstraints[FirstConstraint.Rotation];
		const FRigElementParentConstraint& ParentConstraintB = InConstraints[SecondConstraint.Rotation];

		const FRigElementWeight& WeightA = ParentConstraintA.GetWeight(bInitial); 
		const FRigElementWeight& WeightB = ParentConstraintB.GetWeight(bInitial);
		check(WeightA.AffectsRotation());
		check(WeightB.AffectsRotation());
		const float Weight = GetWeightForLerp(WeightA.Rotation, WeightB.Rotation);

		const FTransform TransformA = LazilyComputeParentConstraint(InConstraints, FirstConstraint.Rotation, InTransformType,
			InLocalOffsetTransform, true, FTransform::Identity, false);
		const FTransform TransformB = LazilyComputeParentConstraint(InConstraints, SecondConstraint.Rotation, InTransformType,
			InLocalOffsetTransform, true, FTransform::Identity, false);

		const FTransform MixedTransform = FControlRigMathLibrary::LerpTransform(TransformA, TransformB, Weight);
		Result.SetRotation(InGlobalTransform.GetRelativeTransform(MixedTransform).GetRotation());
	}
	else if(NumConstraintsAffecting.Rotation > 2)
	{
		check(TotalWeight.Rotation > SMALL_NUMBER);
		
		FVector Location = FVector::ZeroVector;
		int NumMixedRotations = 0;
		FQuat FirstRotation = FQuat::Identity;
		FQuat MixedRotation = FQuat(0.f, 0.f, 0.f, 0.f);
		FVector Scale = FVector::ZeroVector;

		for(int32 ConstraintIndex = 0; ConstraintIndex < InConstraints.Num(); ConstraintIndex++)
		{
			const FRigElementParentConstraint& ParentConstraint = InConstraints[ConstraintIndex];
			const FRigElementWeight& Weight = ParentConstraint.GetWeight(bInitial);
			if(!Weight.AffectsRotation())
			{
				continue;
			}

			const FTransform Transform = LazilyComputeParentConstraint(InConstraints, ConstraintIndex, InTransformType,
				InLocalOffsetTransform, true, FTransform::Identity, false);

			const float NormalizedWeight = Weight.Rotation / TotalWeight.Rotation;
			IntegrateParentConstraintVector(Location, Transform, NormalizedWeight, true);
			IntegrateParentConstraintQuat(NumMixedRotations, FirstRotation, MixedRotation, Transform, NormalizedWeight);
			IntegrateParentConstraintVector(Scale, Transform, NormalizedWeight, false);
		}

		FTransform ParentTransform(MixedRotation.GetNormalized(), Location, Scale);
		Result.SetRotation(InGlobalTransform.GetRelativeTransform(ParentTransform).GetRotation());
	}

	if(NumConstraintsAffecting.Scale == 1)
	{
		check(FirstConstraint.Scale != INDEX_NONE);

		const FRigElementParentConstraint& ParentConstraint = InConstraints[FirstConstraint.Scale];
		const FRigElementWeight& Weight = ParentConstraint.GetWeight(bInitial);
		
		const FTransform Transform = LazilyComputeParentConstraint(InConstraints, FirstConstraint.Scale, InTransformType,
			InLocalOffsetTransform, true, FTransform::Identity, false);

		check(Weight.AffectsScale());
		Result.SetScale3D(InGlobalTransform.GetRelativeTransform(Transform).GetScale3D());
	}
	else if(NumConstraintsAffecting.Scale == 2)
	{
		check(FirstConstraint.Scale != INDEX_NONE);
		check(SecondConstraint.Scale != INDEX_NONE);

		const FRigElementParentConstraint& ParentConstraintA = InConstraints[FirstConstraint.Scale];
		const FRigElementParentConstraint& ParentConstraintB = InConstraints[SecondConstraint.Scale];

		const FRigElementWeight& WeightA = ParentConstraintA.GetWeight(bInitial); 
		const FRigElementWeight& WeightB = ParentConstraintB.GetWeight(bInitial);
		check(WeightA.AffectsScale());
		check(WeightB.AffectsScale());
		const float Weight = GetWeightForLerp(WeightA.Scale, WeightB.Scale);

		const FTransform TransformA = LazilyComputeParentConstraint(InConstraints, FirstConstraint.Scale, InTransformType,
			InLocalOffsetTransform, true, FTransform::Identity, false);
		const FTransform TransformB = LazilyComputeParentConstraint(InConstraints, SecondConstraint.Scale, InTransformType,
			InLocalOffsetTransform, true, FTransform::Identity, false);

		const FTransform MixedTransform = FControlRigMathLibrary::LerpTransform(TransformA, TransformB, Weight);
		Result.SetScale3D(InGlobalTransform.GetRelativeTransform(MixedTransform).GetScale3D());
	}
	else if(NumConstraintsAffecting.Scale > 2)
	{
		check(TotalWeight.Scale > SMALL_NUMBER);
		
		FVector Location = FVector::ZeroVector;
		int NumMixedRotations = 0;
		FQuat FirstRotation = FQuat::Identity;
		FQuat MixedRotation = FQuat(0.f, 0.f, 0.f, 0.f);
		FVector Scale = FVector::ZeroVector;
		
		for(int32 ConstraintIndex = 0; ConstraintIndex < InConstraints.Num(); ConstraintIndex++)
		{
			const FRigElementParentConstraint& ParentConstraint = InConstraints[ConstraintIndex];
			const FRigElementWeight& Weight = ParentConstraint.GetWeight(bInitial);
			if(!Weight.AffectsScale())
			{
				continue;
			}

			const FTransform Transform = LazilyComputeParentConstraint(InConstraints, ConstraintIndex, InTransformType,
				InLocalOffsetTransform, true, FTransform::Identity, false);

			const float NormalizedWeight = Weight.Scale / TotalWeight.Scale;
			IntegrateParentConstraintVector(Location, Transform, NormalizedWeight, true);
			IntegrateParentConstraintQuat(NumMixedRotations, FirstRotation, MixedRotation, Transform, NormalizedWeight);
			IntegrateParentConstraintVector(Scale, Transform, NormalizedWeight, false);
		}

		FTransform ParentTransform(MixedRotation.GetNormalized(), Location, Scale);
		Result.SetScale3D(InGlobalTransform.GetRelativeTransform(ParentTransform).GetScale3D());
	}

	Result.NormalizeRotation();
	return Result;
}

FTransform URigHierarchy::LazilyComputeParentConstraint(
	const FRigElementParentConstraintArray& InConstraints,
	int32 InIndex,
	const ERigTransformType::Type InTransformType,
	const FTransform& InLocalOffsetTransform,
	bool bApplyLocalOffsetTransform,
	const FTransform& InLocalPoseTransform,
	bool bApplyLocalPoseTransform) const
{
	const FRigElementParentConstraint& Constraint = InConstraints[InIndex];
	if(Constraint.Cache.bDirty)
	{
		FTransform Transform = GetTransform(Constraint.ParentElement, InTransformType);
		if(bApplyLocalOffsetTransform)
		{
			Transform = InLocalOffsetTransform * Transform;
		}
		if(bApplyLocalPoseTransform)
		{
			Transform = InLocalPoseTransform * Transform;
		}

		Constraint.Cache.Transform = Transform;
		Constraint.Cache.bDirty = false;
	}
	return Constraint.Cache.Transform;
}

void URigHierarchy::ComputeParentConstraintIndices(
	const FRigElementParentConstraintArray& InConstraints,
	ERigTransformType::Type InTransformType,
	FConstraintIndex& OutFirstConstraint,
	FConstraintIndex& OutSecondConstraint,
	FConstraintIndex& OutNumConstraintsAffecting,
	FRigElementWeight& OutTotalWeight)
{
	const bool bInitial = IsInitial(InTransformType);
	
	// find all of the weights affecting this output
	for(int32 ConstraintIndex = 0; ConstraintIndex < InConstraints.Num(); ConstraintIndex++)
	{
		InConstraints[ConstraintIndex].Cache.bDirty = true;
		
		const FRigElementWeight& Weight = InConstraints[ConstraintIndex].GetWeight(bInitial);
		if(Weight.AffectsLocation())
		{
			OutNumConstraintsAffecting.Location++;
			OutTotalWeight.Location += Weight.Location;

			if(OutFirstConstraint.Location == INDEX_NONE)
			{
				OutFirstConstraint.Location = ConstraintIndex;
			}
			else if(OutSecondConstraint.Location == INDEX_NONE)
			{
				OutSecondConstraint.Location = ConstraintIndex;
			}
		}
		if(Weight.AffectsRotation())
		{
			OutNumConstraintsAffecting.Rotation++;
			OutTotalWeight.Rotation += Weight.Rotation;

			if(OutFirstConstraint.Rotation == INDEX_NONE)
			{
				OutFirstConstraint.Rotation = ConstraintIndex;
			}
			else if(OutSecondConstraint.Rotation == INDEX_NONE)
			{
				OutSecondConstraint.Rotation = ConstraintIndex;
			}
		}
		if(Weight.AffectsScale())
		{
			OutNumConstraintsAffecting.Scale++;
			OutTotalWeight.Scale += Weight.Scale;

			if(OutFirstConstraint.Scale == INDEX_NONE)
			{
				OutFirstConstraint.Scale = ConstraintIndex;
			}
			else if(OutSecondConstraint.Scale == INDEX_NONE)
			{
				OutSecondConstraint.Scale = ConstraintIndex;
			}
		}
	}
}

void URigHierarchy::IntegrateParentConstraintVector(
	FVector& OutVector,
	const FTransform& InTransform,
	float InWeight,
	bool bIsLocation)
{
	if(bIsLocation)
	{
		OutVector += InTransform.GetLocation() * InWeight;
	}
	else
	{
		OutVector += InTransform.GetScale3D() * InWeight;
	}
}

void URigHierarchy::IntegrateParentConstraintQuat(
	int32& OutNumMixedRotations,
	FQuat& OutFirstRotation,
	FQuat& OutMixedRotation,
	const FTransform& InTransform,
	float InWeight)
{
	FQuat ParentRotation = InTransform.GetRotation().GetNormalized();

	if(OutNumMixedRotations == 0)
	{
		OutFirstRotation = ParentRotation; 
	}
	else if ((ParentRotation | OutFirstRotation) <= 0.f)
	{
		InWeight = -InWeight;
	}

	OutMixedRotation.X += InWeight * ParentRotation.X;
	OutMixedRotation.Y += InWeight * ParentRotation.Y;
	OutMixedRotation.Z += InWeight * ParentRotation.Z;
	OutMixedRotation.W += InWeight * ParentRotation.W;
	OutNumMixedRotations++;
}
