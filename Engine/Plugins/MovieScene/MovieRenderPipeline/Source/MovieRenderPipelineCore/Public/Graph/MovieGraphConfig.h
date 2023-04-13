// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once
#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "Graph/MovieGraphNode.h"
#include "Graph/MovieGraphTraversalContext.h"
#include "MovieGraphValueContainer.h"

#include "MovieGraphConfig.generated.h"

// Forward Declare
class UMovieGraphNode;

#if WITH_EDITOR
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnMovieGraphVariableChanged, class UMovieGraphMember*);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnMovieGraphInputChanged, class UMovieGraphMember*);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnMovieGraphOutputChanged, class UMovieGraphMember*);
#endif

UCLASS(Abstract)
class MOVIERENDERPIPELINECORE_API UMovieGraphMember : public UMovieGraphValueContainer
{
	// The graph needs to set private flags during construction time
	friend class UMovieGraphConfig;
	
	GENERATED_BODY()

public:
	UMovieGraphMember() = default;

	/** Gets the GUID that uniquely identifies this member. */
	const FGuid& GetGuid() const { return Guid; }

	/** Sets the GUID that uniquely identifies this member. */
	void SetGuid(const FGuid& InGuid) { Guid = InGuid; }

	/** Determines if this member can be deleted. */
	virtual bool IsDeletable() const { return true; }

public:
	// TODO: Need a details customization that validates whether or not the name is valid/unique
	/** The name of this member, which is user-facing. */
	UPROPERTY(EditAnywhere, Category = "General", meta=(EditCondition="bIsEditable", HideEditConditionToggle))
	FString Name;

	/** The optional description of this member, which is user-facing. */
	UPROPERTY(EditAnywhere, Category = "General", meta=(EditCondition="bIsEditable", HideEditConditionToggle))
	FString Description;

private:
	/** A GUID that uniquely identifies this member within its graph. */
	UPROPERTY()
	FGuid Guid;

	// Note: This is a bool flag rather than a method (eg, IsEditable()) for now in order to allow it to drive the
	// EditCondition metadata on properties.
	/** Whether this member can be edited in the UI. */
	UPROPERTY()
	bool bIsEditable = true;
};

/**
 * A variable that can be used inside the graph. Most variables are created by the user, and can have their value
 * changed at the job level. Global variables, however, are not user-created and their values are provided when the
 * graph is evaluated. Overriding them at the job level is not possible.
 */
UCLASS(BlueprintType)
class MOVIERENDERPIPELINECORE_API UMovieGraphVariable : public UMovieGraphMember
{
	// The graph needs to set private flags during construction time
	friend class UMovieGraphConfig;
	
	GENERATED_BODY()

public:
	UMovieGraphVariable() = default;

	/** Returns true if this variable is a global variable. */
	bool IsGlobal() const { return bIsGlobal; }

	//~ Begin UMovieGraphMember interface
	virtual bool IsDeletable() const override { return !bIsGlobal; }
	//~ End UMovieGraphMember interface

public:
#if WITH_EDITOR
	FOnMovieGraphVariableChanged OnMovieGraphVariableChangedDelegate;

	//~ Begin UObject overrides
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	//~ End UObject overrides
#endif // WITH_EDITOR

private:
	/** Whether this variable represents a global variable. */
	UPROPERTY()
	bool bIsGlobal = false;
};

/**
 * An input exposed on the graph that will be available for nodes to connect to.
 */
UCLASS(BlueprintType)
class MOVIERENDERPIPELINECORE_API UMovieGraphInput : public UMovieGraphMember
{
	GENERATED_BODY()

public:
	UMovieGraphInput() = default;

	virtual bool IsDeletable() const override;

public:
#if WITH_EDITOR
	FOnMovieGraphInputChanged OnMovieGraphInputChangedDelegate;

	//~ Begin UObject overrides
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	//~ End UObject overrides
#endif
};

/**
 * An output exposed on the graph that will be available for nodes to connect to.
 */
UCLASS(BlueprintType)
class MOVIERENDERPIPELINECORE_API UMovieGraphOutput : public UMovieGraphMember
{
	GENERATED_BODY()

public:
	UMovieGraphOutput() = default;

	virtual bool IsDeletable() const override;

public:
#if WITH_EDITOR
	FOnMovieGraphOutputChanged OnMovieGraphOutputChangedDelegate;

	//~ Begin UObject overrides
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	//~ End UObject overrides
#endif
};

#if WITH_EDITOR
	DECLARE_MULTICAST_DELEGATE(FOnMovieGraphChanged);
	DECLARE_MULTICAST_DELEGATE(FOnMovieGraphVariablesChanged);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnMovieGraphInputAdded, UMovieGraphInput*);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnMovieGraphOutputAdded, UMovieGraphOutput*);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnMovieGraphNodesDeleted, TArray<UMovieGraphNode*>);
#endif // WITH_EDITOR

/**
* This is the runtime representation of the UMoviePipelineEdGraph which contains the actual strongly
* typed graph network that is read by the MoviePipeline. There is an editor-only representation of
* this graph (UMoviePipelineEdGraph).
*/
UCLASS()
class MOVIERENDERPIPELINECORE_API UMovieGraphConfig : public UObject
{
	GENERATED_BODY()

public:
	UMovieGraphConfig();

	//~ UObject interface
	virtual void PostLoad() override;
	//~ End UObject interface

	bool AddLabeledEdge(UMovieGraphNode* FromNode, const FName& FromPinLabel, UMovieGraphNode* ToNode, const FName& ToPinLabel);
	bool RemoveEdge(UMovieGraphNode* FromNode, const FName& FromPinName, UMovieGraphNode* ToNode, const FName& ToPinName);
	bool RemoveAllInboundEdges(UMovieGraphNode* InNode);
	bool RemoveAllOutboundEdges(UMovieGraphNode* InNode);
	bool RemoveInboundEdges(UMovieGraphNode* InNode, const FName& InPinName);
	bool RemoveOutboundEdges(UMovieGraphNode* InNode, const FName& InPinName);

	/** Removes the specified node from the graph. */
	bool RemoveNode(UMovieGraphNode* InNode);
	bool RemoveNodes(TArray<UMovieGraphNode*> InNodes);

	UMovieGraphNode* GetInputNode() const { return InputNode; }
	UMovieGraphNode* GetOutputNode() const { return OutputNode; }
	const TArray<TObjectPtr<UMovieGraphNode>>& GetNodes() const { return AllNodes; }

	/**
	 * Adds a new variable member with default values to the graph. The new variable will have a base name of
	 * "Variable" unless specified in InCustomBaseName. Returns the new variable on success, else nullptr.
	 */
	UFUNCTION(BlueprintCallable, Category="Experimental")
	UMovieGraphVariable* AddVariable(const FName InCustomBaseName = NAME_None);

	/** Adds a new input member to the graph. Returns the new input on success, else nullptr. */
	UMovieGraphInput* AddInput();

	/** Adds a new output member to the graph. Returns the new output on success, else nullptr. */
	UMovieGraphOutput* AddOutput();

	/** Gets the variable in the graph with the specified GUID, else nullptr if one could not be found. */
	UMovieGraphVariable* GetVariableByGuid(const FGuid& InGuid) const;

	/**
	 * Gets all variables that are available to be used in the graph. Global variables can optionally be included if
	 * bIncludeGlobal is set to true.
	 */
	UFUNCTION(BlueprintCallable, Category="Experimental")
	TArray<UMovieGraphVariable*> GetVariables(const bool bIncludeGlobal = false) const;

	/** Gets all inputs that have been defined on the graph. */
	TArray<UMovieGraphInput*> GetInputs() const;

	/** Gets all outputs that have been defined on the graph. */
	TArray<UMovieGraphOutput*> GetOutputs() const;

	/** Remove the specified member (input, output, variable) from the graph. */
	bool DeleteMember(UMovieGraphMember* MemberToDelete);

	/** Returns only the names of the root branches in the Output Node, with no depth information. */
	TArray<FMovieGraphBranch> GetOutputBranches() const;

#if WITH_EDITOR
	/** Gets the editor-only nodes in this graph. Editor-only nodes do not have an equivalent runtime node. */
	const TArray<TObjectPtr<UObject>>& GetEditorOnlyNodes() const { return EditorOnlyNodes; }

	/** Sets the editor-only nodes in this graph. */
	void SetEditorOnlyNodes(const TArray<TObjectPtr<const UObject>>& InNodes);
#endif

	template<typename NodeType>
	static NodeType* IterateGraphForClass(const FMovieGraphTraversalContext& InContext)
	{
		TArray<NodeType*> AllSettings = IterateGraphForClassAll<NodeType>(InContext);
		if (AllSettings.Num() > 0)
		{
			return AllSettings[0];
		}
		return nullptr;
	}
	
	template<typename NodeType>
	static TArray<NodeType*> IterateGraphForClassAll(const FMovieGraphTraversalContext& InContext)
	{
		TArray<NodeType*> TypedNodes;
		if (!ensureMsgf(InContext.RootGraph, TEXT("You must specify a RootGraph to traverse with")))
		{
			return TypedNodes;
		}

		const TArray<UMovieGraphNode*> FoundNodes = InContext.RootGraph->TraverseGraph(NodeType::StaticClass(), InContext);
		for (UMovieGraphNode* Node : FoundNodes)
		{
			TypedNodes.Add(CastChecked<NodeType>(Node));
		}

		return TypedNodes;
	}

	TArray<UMovieGraphNode*> TraverseGraph(TSubclassOf<UMovieGraphNode> InClassType, const FMovieGraphTraversalContext& InContext) const;

protected:
	void TraverseGraphRecursive(UMovieGraphNode* InNode, TSubclassOf<UMovieGraphNode> InClassType, const FMovieGraphTraversalContext& InContext, TArray<UMovieGraphNode*>& OutNodes) const;

public:
	// Names of global variables that are provided by the graph
	static FName GlobalVariable_ShotName;
	static FName GlobalVariable_SequenceName;
	static FName GlobalVariable_FrameNumber;
	static FName GlobalVariable_CameraName;
	static FName GlobalVariable_RenderLayerName;
	
#if WITH_EDITOR
	FOnMovieGraphChanged OnGraphChangedDelegate;
	FOnMovieGraphVariablesChanged OnGraphVariablesChangedDelegate;
	FOnMovieGraphInputAdded OnGraphInputAddedDelegate;
	FOnMovieGraphOutputAdded OnGraphOutputAddedDelegate;
	FOnMovieGraphNodesDeleted OnGraphNodesDeletedDelegate;
#endif
	
	UPROPERTY()
	TArray<TObjectPtr<UMovieGraphNode>> AllNodes;

	UPROPERTY()
	TObjectPtr<UMovieGraphNode> InputNode;

	UPROPERTY()
	TObjectPtr<UMovieGraphNode> OutputNode;

#if WITH_EDITORONLY_DATA
	// Not strongly typed to avoid a circular dependency between the editor only module
	// and the runtime module, but it should be a UMoviePipelineEdGraph.
	UPROPERTY(Transient)
	TObjectPtr<UEdGraph> PipelineEdGraph;
#endif

	template<class T>
	T* ConstructRuntimeNode(TSubclassOf<UMovieGraphNode> PipelineGraphNodeClass = T::StaticClass())
	{
		// Construct a new object with ourselves as the outer, then keep track of it.
		// ToDo: This is a runtime node, kept track in AllNodes, which is ultimately editor only. Probably
		// because the system it's based on (SoundCues) have a root node and links nodes together later?
		T* RuntimeNode = NewObject<T>(this, PipelineGraphNodeClass, NAME_None, RF_Transactional);
		RuntimeNode->UpdateDynamicProperties();
		RuntimeNode->UpdatePins();
		RuntimeNode->Guid = FGuid::NewGuid();
#if WITH_EDITOR
		AllNodes.Add(RuntimeNode);
#endif
		return RuntimeNode;
	}

private:
	/** Remove the specified variable member from the graph. */
	bool DeleteVariableMember(UMovieGraphVariable* VariableMemberToDelete);
	
	/** Remove the specified input member from the graph. */
	bool DeleteInputMember(UMovieGraphInput* InputMemberToDelete);

	/** Remove the specified output member from the graph. */
	bool DeleteOutputMember(UMovieGraphOutput* OutputMemberToDelete);
	
	/** Add a new member of type T to MemberArray, with a unique name that includes BaseName in it. */
	template<typename T>
	T* AddMember(TArray<TObjectPtr<T>>& InMemberArray, const FName& InBaseName);

	/** Adds a global variable to the graph with the provided name. */
	UMovieGraphVariable* AddGlobalVariable(const FName& InName);

	/** Adds members to the graph that should always be available. */
	void AddDefaultMembers();

private:
	/** All variables (user and global) which are available for use in the graph. */
	UPROPERTY()
	TArray<TObjectPtr<UMovieGraphVariable>> Variables;

	/** All inputs which have been defined on the graph. */
	UPROPERTY()
	TArray<TObjectPtr<UMovieGraphInput>> Inputs;

	/** All outputs which have been defined on the graph. */
	UPROPERTY()
	TArray<TObjectPtr<UMovieGraphOutput>> Outputs;

#if WITH_EDITORONLY_DATA
	/** Nodes which are only useful in the editor (like comments) and have no runtime equivalent */
	UPROPERTY()
	TArray<TObjectPtr<UObject>> EditorOnlyNodes;
#endif
};