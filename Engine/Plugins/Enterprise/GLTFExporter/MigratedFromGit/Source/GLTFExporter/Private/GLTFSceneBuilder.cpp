// Copyright Epic Games, Inc. All Rights Reserved.

#include "GLTFSceneBuilder.h"
#include "GLTFConversionUtilities.h"
#include "GLTFExporterModule.h"

FGLTFNodeBuilder::FGLTFNodeBuilder(const USceneComponent* SceneComponent, bool bSelectedOnly, bool bTopLevel)
	: SceneComponent(SceneComponent)
	, bTopLevel(bTopLevel)
{
	const AActor* Owner = SceneComponent->GetOwner();
	Name = Owner->GetName() + TEXT("_") + SceneComponent->GetName();

	const TArray<USceneComponent*>& Children = SceneComponent->GetAttachChildren();
	for (const USceneComponent* ChildComponent : Children)
	{
		if (ChildComponent != nullptr && (!bSelectedOnly || ChildComponent->GetOwner()->IsSelected()))
		{
			AttachedComponents.Add(FGLTFNodeBuilder(ChildComponent, bSelectedOnly));
		}
	}
}

FGLTFJsonNodeIndex FGLTFNodeBuilder::AddNode(FGLTFContainerBuilder& Container) const
{
	FGLTFJsonNode Node;
	Node.Name = Name;

	FTransform Transform = bTopLevel ? SceneComponent->GetComponentTransform() : SceneComponent->GetRelativeTransform();
	Node.Translation = ConvertPosition(Transform.GetTranslation());
	Node.Rotation = ConvertRotation(Transform.GetRotation());
	Node.Scale = ConvertScale(Transform.GetScale3D());

	const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(SceneComponent);
	if (StaticMeshComponent != nullptr)
	{
		Node.Mesh = Container.AddMesh(StaticMeshComponent);
	}

	for (const FGLTFNodeBuilder& AttachedComponent : AttachedComponents)
	{
		FGLTFJsonNodeIndex ChildIndex = AttachedComponent.AddNode(Container);
		if (ChildIndex != INDEX_NONE) Node.Children.Add(ChildIndex);
	}

	return Container.AddNode(Node);
}

FGLTFSceneBuilder::FGLTFSceneBuilder(const UWorld* World, bool bSelectedOnly)
{
	World->GetName(Name);

	const ULevel* Level = World->PersistentLevel;

	// TODO: export Level->Model

	const TArray<AActor*>& Actors = Level->Actors;
	for (const AActor* Actor : Actors)
	{
		if (Actor != nullptr && (!bSelectedOnly || Actor->IsSelected()))
		{
			const USceneComponent* RootComponent = Actor->GetRootComponent();
			if (RootComponent != nullptr)
			{
				const USceneComponent* ParentComponent = RootComponent->GetAttachParent();
				if (ParentComponent == nullptr || (bSelectedOnly && !ParentComponent->GetOwner()->IsSelected()))
				{
					TopLevelComponents.Add(FGLTFNodeBuilder(RootComponent, bSelectedOnly, true));
				}
			}
		}
	}
}

FGLTFJsonSceneIndex FGLTFSceneBuilder::AddScene(FGLTFContainerBuilder& Container) const
{
	FGLTFJsonScene Scene;
	Scene.Name = Name;

	for (const FGLTFNodeBuilder& TopLevelComponent : TopLevelComponents)
	{
		FGLTFJsonNodeIndex NodeIndex = TopLevelComponent.AddNode(Container);
		if (NodeIndex != INDEX_NONE) Scene.Nodes.Add(NodeIndex);
	}

	return Container.AddScene(Scene);
}
