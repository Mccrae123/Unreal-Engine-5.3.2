// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetDefinition.h"
#include "AssetDefinitionRegistry.h"
#include "Misc/AssetFilterData.h"

#define LOCTEXT_NAMESPACE "UAssetDefinition"

FAssetCategoryPath EAssetCategoryPaths::Basic(LOCTEXT("Basic", "Basic"));
FAssetCategoryPath EAssetCategoryPaths::Animation(LOCTEXT("Animation", "Animation"));
FAssetCategoryPath EAssetCategoryPaths::Material(LOCTEXT("Material", "Material"));
FAssetCategoryPath EAssetCategoryPaths::Audio(LOCTEXT("Audio", "Audio"));
FAssetCategoryPath EAssetCategoryPaths::Physics(LOCTEXT("Physics", "Physics"));
FAssetCategoryPath EAssetCategoryPaths::UI(LOCTEXT("UserInterface", "User Interface"));
FAssetCategoryPath EAssetCategoryPaths::Misc(LOCTEXT("Miscellaneous", "Miscellaneous"));
FAssetCategoryPath EAssetCategoryPaths::Gameplay(LOCTEXT("Gameplay", "Gameplay"));
FAssetCategoryPath EAssetCategoryPaths::Blueprint(LOCTEXT("Blueprint", "Blueprint"));
FAssetCategoryPath EAssetCategoryPaths::Texture(LOCTEXT("Texture", "Texture"));

FAssetCategoryPath::FAssetCategoryPath(const FText InCategory)
{
	CategoryPath = { TPair<FName, FText>(FName(*FTextInspector::GetSourceString(InCategory)), InCategory) };
}

FAssetCategoryPath::FAssetCategoryPath(TConstArrayView<FText> InCategoryPath)
{
	check(InCategoryPath.Num() > 0);
	
	for (const FText& CategoryChunk : InCategoryPath)
	{
		CategoryPath.Add(TPair<FName, FText>(FName(*FTextInspector::GetSourceString(CategoryChunk)), CategoryChunk));
	}
}

UAssetDefinition::UAssetDefinition()
{
}

void UAssetDefinition::PostCDOContruct()
{
	Super::PostCDOContruct();

	if (CanRegisterStatically())
	{
		UAssetDefinitionRegistry::Get()->RegisterAssetDefinition(this);
	}
}

bool UAssetDefinition::CanRegisterStatically() const
{
	return !GetClass()->HasAnyClassFlags(CLASS_Abstract);
}

EAssetCommandResult UAssetDefinition::GetFilters(TArray<FAssetFilterData>& OutFilters) const
{
	const TSoftClassPtr<UObject> AssetClassPtr = GetAssetClass();

	if (const UClass* AssetClass = AssetClassPtr.Get())
	{
		// By default we don't advertise filtering if the class is abstract for the asset definition.  Odds are,
		// if they've registered an abstract class as an asset definition, they mean to use it for subclasses.
		if (!AssetClass->HasAnyClassFlags(CLASS_Abstract))
		{
			FAssetFilterData DefaultFilter;
			DefaultFilter.Name = AssetClassPtr.ToSoftObjectPath().ToString();
			DefaultFilter.DisplayText = GetAssetDisplayName();
			DefaultFilter.Filter.ClassPaths.Add(AssetClassPtr.ToSoftObjectPath().GetAssetPath());
			DefaultFilter.Filter.bRecursiveClasses = true;
			OutFilters.Add(MoveTemp(DefaultFilter));
	
			return EAssetCommandResult::Handled;
		}
	}
	
	return EAssetCommandResult::Unhandled;
}

#undef LOCTEXT_NAMESPACE