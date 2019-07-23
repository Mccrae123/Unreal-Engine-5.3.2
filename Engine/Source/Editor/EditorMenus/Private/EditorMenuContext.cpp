// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "EditorMenuContext.h"
#include "EditorMenuSubsystem.h"
#include "IEditorMenusModule.h"

#include "Textures/SlateIcon.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/MultiBox/MultiBox.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Internationalization/Internationalization.h"
#include "Toolkits/AssetEditorToolkit.h"

#include "Editor.h"

FEditorMenuContext::FEditorMenuContext()
{

}

FEditorMenuContext::FEditorMenuContext(UObject* InContext)
{
	if (InContext)
	{
		ContextObjects.Add(InContext);
	}
}

FEditorMenuContext::FEditorMenuContext(TSharedPtr<FUICommandList> InCommandList, TSharedPtr<FExtender> InExtender, UObject* InContext)
{
	if (InContext)
	{
		ContextObjects.Add(InContext);
	}

	if (InExtender.IsValid())
	{
		AddExtender(InExtender);
	}

	AppendCommandList(InCommandList);
}

UObject* FEditorMenuContext::FindByClass(UClass* InClass) const
{
	for (UObject* ContextObject : ContextObjects)
	{
		if (ContextObject && ContextObject->IsA(InClass))
		{
			return ContextObject;
		}
	}

	return nullptr;
}

void FEditorMenuContext::AppendCommandList(const TSharedRef<FUICommandList>& InCommandList)
{
	const TSharedPtr<FUICommandList> List = InCommandList;
	AppendCommandList(List);
}

void FEditorMenuContext::AppendCommandList(const TSharedPtr<FUICommandList>& InCommandList)
{
	if (InCommandList.IsValid())
	{
		CommandLists.Add(InCommandList);

		if (CommandLists.Num() == 1)
		{
			CommandList = InCommandList;
		}
		else if (CommandLists.Num() == 2)
		{
			CommandList = MakeShared<FUICommandList>();
			CommandList->Append(CommandLists[0].ToSharedRef());
			CommandList->Append(InCommandList.ToSharedRef());
		}
		else
		{
			CommandList->Append(InCommandList.ToSharedRef());
		}
	}
}

const FUIAction* FEditorMenuContext::GetActionForCommand(TSharedPtr<const FUICommandInfo> Command, TSharedPtr<const FUICommandList>& OutCommandList) const
{
	for (const TSharedPtr<FUICommandList>& CommandListIter : CommandLists)
	{
		if (CommandListIter.IsValid())
		{
			if (const FUIAction* Result = CommandListIter->GetActionForCommand(Command))
			{
				OutCommandList = CommandListIter;
				return Result;
			}
		}
	}

	return nullptr;
}

void FEditorMenuContext::AddExtender(const TSharedPtr<FExtender>& InExtender)
{
	if (!ExtensibilityManager.IsValid())
	{
		ExtensibilityManager = MakeShared<FExtensibilityManager>();
	}

	ExtensibilityManager->AddExtender(InExtender);
}

TSharedPtr<FExtender> FEditorMenuContext::GetAllExtenders()
{
	if (ExtensibilityManager.IsValid())
	{
		return ExtensibilityManager->GetAllExtenders();
	}

	return TSharedPtr<FExtender>();
}

void FEditorMenuContext::ResetExtenders()
{
	if (ExtensibilityManager.IsValid())
	{
		ExtensibilityManager.Reset();
	}
}

void FEditorMenuContext::AppendObjects(const TArray<UObject*>& InObjects)
{
	for (UObject* Object : InObjects)
	{
		AddObject(Object);
	}
}

void FEditorMenuContext::AddObject(UObject* InObject)
{
	ContextObjects.AddUnique(InObject);
}
