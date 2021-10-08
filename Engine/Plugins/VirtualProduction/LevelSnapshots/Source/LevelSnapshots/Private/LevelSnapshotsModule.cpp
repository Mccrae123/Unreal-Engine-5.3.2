// Copyright Epic Games, Inc. All Rights Reserved.

#include "LevelSnapshotsModule.h"

#include "BlacklistRestorabilityOverrider.h"
#include "LevelSnapshotsEditorProjectSettings.h"
#include "LevelSnapshotsLog.h"
#include "Restorability/PropertyComparisonParams.h"
#include "Restorability/CollisionRestoration.h"

#include "Algo/AllOf.h"
#include "Components/ActorComponent.h"
#include "EngineUtils.h"
#include "Algo/Transform.h"
#include "Engine/Brush.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstance.h"
#include "Modules/ModuleManager.h"

namespace
{
	void AddSoftObjectPathSupport(FLevelSnapshotsModule& Module)
	{
		// FSnapshotRestorability::IsRestorableProperty requires properties to have the CPF_Edit specifier
		// FSoftObjectPath does not have this so we need to whitelist its properties

		UStruct* SoftObjectClassPath = FindObject<UStruct>(nullptr, TEXT("/Script/CoreUObject.SoftObjectPath"));
		if (!ensureMsgf(SoftObjectClassPath, TEXT("Investigate why this class could not be found")))
		{
			return;
		}

		TSet<const FProperty*> WhitelistedProperties;
		Algo::Transform(TFieldRange<const FProperty>(SoftObjectClassPath), WhitelistedProperties, [](const FProperty* Prop) { return Prop;} );
		Module.AddWhitelistedProperties(WhitelistedProperties);
	}

	void AddAttachParentSupport(FLevelSnapshotsModule& Module)
	{
		// These properties are not visible by default because they're not CPF_Edit
		const FProperty* AttachParent = USceneComponent::StaticClass()->FindPropertyByName(FName("AttachParent"));
		const FProperty* AttachSocketName = USceneComponent::StaticClass()->FindPropertyByName(FName("AttachSocketName"));
		if (ensure(AttachParent && AttachSocketName))
		{
			Module.AddWhitelistedProperties({ AttachParent, AttachSocketName });
		}
	}

	void DisableIrrelevantBrushSubobjects(FLevelSnapshotsModule& Module)
	{
#if WITH_EDITORONLY_DATA
		// ABrush::BrushBuilder is CPF_Edit but no user ever cares about it. We don't want it to make volumes to show up as changed.
		const FProperty* BrushBuilder = ABrush::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(ABrush, BrushBuilder));
		if (ensure(BrushBuilder))
		{
			Module.AddBlacklistedProperties({ BrushBuilder });
		}
#endif
	}

	void DisableIrrelevantWorldSettings(FLevelSnapshotsModule& Module)
	{
		// AWorldSettings::NavigationSystemConfig is CPF_Edit but no user ever cares about it.
		const FProperty* NavigationSystemConfig = AWorldSettings::StaticClass()->FindPropertyByName(FName("NavigationSystemConfig"));
		if (ensure(NavigationSystemConfig))
		{
			Module.AddBlacklistedProperties({ NavigationSystemConfig });
		}
	}

	void DisableIrrelevantMaterialInstanceProperties(FLevelSnapshotsModule& Module)
	{
		// This property causes diffs sometimes for unexplained reasons when creating in construction script... does not seem to be important
		const FProperty* BasePropertyOverrides = UMaterialInstance::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UMaterialInstance, BasePropertyOverrides));
		if (ensure(BasePropertyOverrides))
		{
			Module.AddBlacklistedProperties({ BasePropertyOverrides });
		}
		
	}
}

FLevelSnapshotsModule& FLevelSnapshotsModule::GetInternalModuleInstance()
{
	static FLevelSnapshotsModule& ModuleInstance = *[]() -> FLevelSnapshotsModule*
	{
		UE_CLOG(!FModuleManager::Get().IsModuleLoaded("LevelSnapshots"), LogLevelSnapshots, Fatal, TEXT("You called GetInternalModuleInstance before the module was initialised."));
		return &FModuleManager::GetModuleChecked<FLevelSnapshotsModule>("LevelSnapshots");
	}();
	return ModuleInstance;
}

void FLevelSnapshotsModule::StartupModule()
{
	// Hook up project settings blacklist
	const TSharedRef<FBlacklistRestorabilityOverrider> Blacklist = MakeShared<FBlacklistRestorabilityOverrider>(
		FBlacklistRestorabilityOverrider::FGetBlacklist::CreateLambda([]() -> const FRestorationBlacklist&
		{
			ULevelSnapshotsEditorProjectSettings* Settings = GetMutableDefault<ULevelSnapshotsEditorProjectSettings>();
			return Settings->Blacklist;
		})
	);
	RegisterRestorabilityOverrider(Blacklist);

	// Enable / disable troublesome properties
	AddSoftObjectPathSupport(*this);
	AddAttachParentSupport(*this);
	DisableIrrelevantBrushSubobjects(*this);
	DisableIrrelevantWorldSettings(*this);
	DisableIrrelevantMaterialInstanceProperties(*this);

	// Interact with special engine features
	FCollisionRestoration::Register(*this);
}

void FLevelSnapshotsModule::ShutdownModule()
{
	Overrides.Reset();
	PropertyComparers.Reset();
	CustomSerializers.Reset();
	RestorationListeners.Reset();
}

void FLevelSnapshotsModule::RegisterRestorabilityOverrider(TSharedRef<ISnapshotRestorabilityOverrider> Overrider)
{
	Overrides.AddUnique(Overrider);
}

void FLevelSnapshotsModule::UnregisterRestorabilityOverrider(TSharedRef<ISnapshotRestorabilityOverrider> Overrider)
{
	Overrides.RemoveSwap(Overrider);
}

void FLevelSnapshotsModule::AddBlacklistedSubobjectClasses(const TSet<UClass*>& Classes)
{
	for (UClass* Class : Classes)
	{
		check(Class);

		if (!Class
			|| !ensureAlwaysMsgf(!Class->IsChildOf(AActor::StaticClass()), TEXT("Invalid function input: Actors can never be subobjects. Check your code."))
			|| !ensureAlwaysMsgf(!Class->IsChildOf(UActorComponent::StaticClass()), TEXT("Invalid function input: Disallow components using RegisterRestorabilityOverrider and implementing ISnapshotRestorabilityOverrider::IsComponentDesirableForCapture instead.")))
		{
			continue;
		}

		BlacklistedSubobjectClasses.Add(Class);
	}
}

void FLevelSnapshotsModule::RemoveBlacklistedSubobjectClasses(const TSet<UClass*>& Classes)
{
	for (UClass* Class : Classes)
	{
		BlacklistedSubobjectClasses.Remove(Class);
	}
}

void FLevelSnapshotsModule::RegisterPropertyComparer(UClass* Class, TSharedRef<IPropertyComparer> Comparer)
{
	PropertyComparers.FindOrAdd(Class).AddUnique(Comparer);
}

void FLevelSnapshotsModule::UnregisterPropertyComparer(UClass* Class, TSharedRef<IPropertyComparer> Comparer)
{
	TArray<TSharedRef<IPropertyComparer>>* Comparers = PropertyComparers.Find(Class);
	if (!Comparers)
	{
		return;
	}
	Comparers->RemoveSwap(Comparer);

	if (Comparers->Num() == 0)
	{
		PropertyComparers.Remove(Class);
	}
}

void FLevelSnapshotsModule::RegisterCustomObjectSerializer(UClass* Class, TSharedRef<ICustomObjectSnapshotSerializer> CustomSerializer, bool bIncludeBlueprintChildClasses)
{
	if (!ensureAlways(Class))
	{
		return;
	}
	
	const bool bIsBlueprint = Class->IsInBlueprint();
	if (!ensureAlwaysMsgf(!bIsBlueprint, TEXT("Registering to Blueprint classes is unsupported because they can be reinstanced at any time")))
	{
		return;
	}

	FCustomSerializer* ExistingSerializer = CustomSerializers.Find(Class);
	if (!ensureAlwaysMsgf(!ExistingSerializer, TEXT("Class already registered")))
	{
		return;
	}

	CustomSerializers.Add(Class, { CustomSerializer, bIncludeBlueprintChildClasses });
}

void FLevelSnapshotsModule::UnregisterCustomObjectSerializer(UClass* Class)
{
	CustomSerializers.Remove(Class);
}

void FLevelSnapshotsModule::RegisterSnapshotLoader(TSharedRef<ISnapshotLoader> Loader)
{
	SnapshotLoaders.AddUnique(Loader);
}

void FLevelSnapshotsModule::UnregisterSnapshotLoader(TSharedRef<ISnapshotLoader> Loader)
{
	SnapshotLoaders.RemoveSingle(Loader);
}

void FLevelSnapshotsModule::RegisterRestorationListener(TSharedRef<IRestorationListener> Listener)
{
	RestorationListeners.AddUnique(Listener);
}

void FLevelSnapshotsModule::UnregisterRestorationListener(TSharedRef<IRestorationListener> Listener)
{
	RestorationListeners.RemoveSingle(Listener);
}

void FLevelSnapshotsModule::AddWhitelistedProperties(const TSet<const FProperty*>& Properties)
{
	for (const FProperty* Property : Properties)
	{
		WhitelistedProperties.Add(Property);
	}
}

void FLevelSnapshotsModule::RemoveWhitelistedProperties(const TSet<const FProperty*>& Properties)
{
	for (const FProperty* Property : Properties)
	{
		WhitelistedProperties.Remove(Property);
	}
}

void FLevelSnapshotsModule::AddBlacklistedProperties(const TSet<const FProperty*>& Properties)
{
	for (const FProperty* Property : Properties)
	{
		BlacklistedProperties.Add(Property);
	}
}

void FLevelSnapshotsModule::RemoveBlacklistedProperties(const TSet<const FProperty*>& Properties)
{
	for (const FProperty* Property : Properties)
	{
		BlacklistedProperties.Remove(Property);
	}
}

void FLevelSnapshotsModule::AddBlacklistedClassDefault(const UClass* Class)
{
	BlacklistedCDOs.Add(Class);
}

void FLevelSnapshotsModule::RemoveBlacklistedClassDefault(const UClass* Class)
{
	BlacklistedCDOs.Remove(Class);
}

bool FLevelSnapshotsModule::IsClassDefaultBlacklisted(const UClass* Class) const
{
	for (const UClass* CurrentClass = Class; CurrentClass; CurrentClass = CurrentClass->GetSuperClass())
	{
		if (BlacklistedCDOs.Contains(CurrentClass))
		{
			return true;
		}
	}

	return false;
}

bool FLevelSnapshotsModule::IsSubobjectClassBlacklisted(const UClass* Class) const
{
	if (Class->IsChildOf(UActorComponent::StaticClass()))
	{
		return false;
	}

	bool bFoundBlacklistedClass = false;
	for (const UClass* CurrentClass = Class; !bFoundBlacklistedClass && CurrentClass; CurrentClass = CurrentClass->GetSuperClass())
	{
		bFoundBlacklistedClass = BlacklistedSubobjectClasses.Contains(CurrentClass);
	}

	return bFoundBlacklistedClass;
}

const TArray<TSharedRef<ISnapshotRestorabilityOverrider>>& FLevelSnapshotsModule::GetOverrides() const
{
	return Overrides;
}

bool FLevelSnapshotsModule::IsPropertyWhitelisted(const FProperty* Property) const
{
	return WhitelistedProperties.Contains(Property);
}

bool FLevelSnapshotsModule::IsPropertyBlacklisted(const FProperty* Property) const
{
	return BlacklistedProperties.Contains(Property);
}

FPropertyComparerArray FLevelSnapshotsModule::GetPropertyComparerForClass(UClass* Class) const
{
	FPropertyComparerArray Result;
	for (UClass* CurrentClass = Class; CurrentClass; CurrentClass = CurrentClass->GetSuperClass())
	{
		const TArray<TSharedRef<IPropertyComparer>>* Comparers = PropertyComparers.Find(CurrentClass);
		if (Comparers)
		{
			Result.Append(*Comparers);
		}
	}
	return Result;
}

IPropertyComparer::EPropertyComparison FLevelSnapshotsModule::ShouldConsiderPropertyEqual(const FPropertyComparerArray& Comparers, const FPropertyComparisonParams& Params) const
{
	for (const TSharedRef<IPropertyComparer>& Comparer : Comparers)
	{
		const IPropertyComparer::EPropertyComparison Result = Comparer->ShouldConsiderPropertyEqual(Params);
		if (Result != IPropertyComparer::EPropertyComparison::CheckNormally)
		{
			return Result;
		}
	}
	return IPropertyComparer::EPropertyComparison::CheckNormally;
}

TSharedPtr<ICustomObjectSnapshotSerializer> FLevelSnapshotsModule::GetCustomSerializerForClass(UClass* Class) const
{
	// Walk to first native parent
	const bool bPassedInBlueprint = Class->IsInBlueprint();
	while (Class && Class->IsInBlueprint())
	{
		Class = Class->GetSuperClass();
	}

	if (ensureAlways(Class))
	{
		const FCustomSerializer* Result = CustomSerializers.Find(Class);
		return (Result && (!bPassedInBlueprint || Result->bIncludeBlueprintChildren)) ? Result->Serializer : TSharedPtr<ICustomObjectSnapshotSerializer>();
	}

	return nullptr;
}

void FLevelSnapshotsModule::AddCanTakeSnapshotDelegate(FName DelegateName, FCanTakeSnapshot Delegate)
{
	CanTakeSnapshotDelegates.FindOrAdd(DelegateName) = Delegate;
}

void FLevelSnapshotsModule::RemoveCanTakeSnapshotDelegate(FName DelegateName)
{
	CanTakeSnapshotDelegates.Remove(DelegateName);
}


bool FLevelSnapshotsModule::CanTakeSnapshot(const FPreTakeSnapshotEventData& Event) const
{
	return Algo::AllOf(CanTakeSnapshotDelegates, [&Event](const TTuple<FName,FCanTakeSnapshot>& Pair)
		{
			if (Pair.Get<1>().IsBound())
			{
				return Pair.Get<1>().Execute(Event);
			}
			return true;
		});
}

void FLevelSnapshotsModule::OnPostLoadSnapshotObject(const FPostLoadSnapshotObjectParams& Params)
{
	SCOPED_SNAPSHOT_CORE_TRACE(SnapshotLoaders);
	
	for (const TSharedRef<ISnapshotLoader>& Loader : SnapshotLoaders)
	{
		Loader->PostLoadSnapshotObject(Params);
	}
}

void FLevelSnapshotsModule::OnPreApplySnapshotProperties(const FApplySnapshotPropertiesParams& Params)
{
	SCOPED_SNAPSHOT_CORE_TRACE(RestorationListeners);
	
	for (const TSharedRef<IRestorationListener>& Listener : RestorationListeners)
	{
		Listener->PreApplySnapshotProperties(Params);
	}
}

void FLevelSnapshotsModule::OnPostApplySnapshotProperties(const FApplySnapshotPropertiesParams& Params)
{
	SCOPED_SNAPSHOT_CORE_TRACE(RestorationListeners);
	
	for (const TSharedRef<IRestorationListener>& Listener : RestorationListeners)
	{
		Listener->PostApplySnapshotProperties(Params);
	}
}

void FLevelSnapshotsModule::OnPreApplySnapshotToActor(const FApplySnapshotToActorParams& Params)
{
	SCOPED_SNAPSHOT_CORE_TRACE(RestorationListeners);
	
	for (const TSharedRef<IRestorationListener>& Listener : RestorationListeners)
	{
		Listener->PreApplySnapshotToActor(Params);
	}
}

void FLevelSnapshotsModule::OnPostApplySnapshotToActor(const FApplySnapshotToActorParams& Params)
{
	SCOPED_SNAPSHOT_CORE_TRACE(RestorationListeners);
	
	for (const TSharedRef<IRestorationListener>& Listener : RestorationListeners)
	{
		Listener->PostApplySnapshotToActor(Params);
	}
}

void FLevelSnapshotsModule::OnPreRecreateComponent(const FPreRecreateComponentParams& Params)
{
	SCOPED_SNAPSHOT_CORE_TRACE(RestorationListeners);
	
	for (const TSharedRef<IRestorationListener>& Listener : RestorationListeners)
	{
		Listener->PreRecreateComponent(Params);
	}
}

void FLevelSnapshotsModule::OnPostRecreateComponent(UActorComponent* RecreatedComponent)
{
	SCOPED_SNAPSHOT_CORE_TRACE(RestorationListeners);
	
	for (const TSharedRef<IRestorationListener>& Listener : RestorationListeners)
	{
		Listener->PostRecreateComponent(RecreatedComponent);
	}
}

void FLevelSnapshotsModule::OnPreRemoveComponent(UActorComponent* ComponentToRemove)
{
	SCOPED_SNAPSHOT_CORE_TRACE(RestorationListeners);

	for (const TSharedRef<IRestorationListener>& Listener : RestorationListeners)
	{
		Listener->PreRemoveComponent(ComponentToRemove);
	}
}

void FLevelSnapshotsModule::OnPostRemoveComponent(const FPostRemoveComponentParams& Params)
{
	SCOPED_SNAPSHOT_CORE_TRACE(RestorationListeners);

	for (const TSharedRef<IRestorationListener>& Listener : RestorationListeners)
	{
		Listener->PostRemoveComponent(Params);
	}
}

IMPLEMENT_MODULE(FLevelSnapshotsModule, LevelSnapshots)
