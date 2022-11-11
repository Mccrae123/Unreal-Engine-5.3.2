// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Array.h"
#include "Containers/BitArray.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Containers/SparseArray.h"
#include "Containers/UnrealString.h"
#include "Delegates/Delegate.h"
#include "HAL/Platform.h"
#include "Input/Reply.h"
#include "Internationalization/Text.h"
#include "Layout/Visibility.h"
#include "Misc/Optional.h"
#include "MuR/Image.h"
#include "MuR/MemoryPrivate.h"
#include "MuR/Mesh.h"
#include "MuR/Model.h"
#include "MuR/ModelPrivate.h"
#include "MuR/MutableMath.h"
#include "MuR/Operations.h"
#include "MuR/System.h"
#include "Templates/SharedPointer.h"
#include "Templates/TypeHash.h"
#include "Types/SlateConstants.h"
#include "Types/SlateEnums.h"
#include "UObject/GCObject.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STableViewBase.h"
#include "Widgets/Views/STreeView.h"
#include "Widgets/Input/SComboBox.h"


class FMutableCodeTreeElement;
class FMutableOperationElement;
class FReferenceCollector;
class ITableRow;
class SBorder;
class SMutableBoolViewer;
class SMutableColorViewer;
class SMutableConstantsWidget;
class SMutableCurveViewer;
class SMutableImageViewer;
class SMutableIntViewer;
class SMutableLayoutViewer;
class SMutableMeshViewer;
class SMutableParametersWidget;
class SMutableProjectorViewer;
class SMutableScalarViewer;
class SMutableSkeletonViewer;
class SMutableStringViewer;
class STextComboBox;
class SWidget;
namespace mu { struct Curve; }
namespace mu { struct FProjector; }
namespace mu { struct FShape; }
struct FGeometry;

/** This widget shows the internal Mutable Code for debugging purposes. 
 * This is not the Unreal source graph in the UCustomizableObject, but the actual Mutable virtual machine graph.
 */
class SMutableCodeViewer :
	public SCompoundWidget,
	public FGCObject
{
public:

	SLATE_BEGIN_ARGS(SMutableCodeViewer) {}

	/** User-visible tag to identify the source of the data shown. */
	SLATE_ARGUMENT(FString, DataTag)
		
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const mu::ModelPtr& InMutableModel /*, const TSharedPtr<SDockTab>& ConstructUnderMajorTab*/);

	// SWidget interface
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	FReply OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	FReply OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;

	// FGCObject interface
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override;

private:

	/** The Mutable Model that we are showing. */
	mu::ModelPtr MutableModel;
	
	/** Selected model operation for preview. */
	mu::OP::ADDRESS SelectedOperationAddress = 0;

	/** Mutable parameters used in the preview. */
	mu::ParametersPtr PreviewParameters;

	/** Widget showing the parameters that affect the current preview. */
	TSharedPtr<SMutableParametersWidget> ParametersWidget;

	/** Widget showing the constants found on the mutable Model program */
	TSharedPtr<SMutableConstantsWidget> ConstantsWidget;

	/** If true, the parameters have changed and we need to update the preview. */
	bool bIsPreviewPendingUpdate = false;

	/** Widget container where different previews will be created. */
	TSharedPtr<SBorder> PreviewBorder;

	/*
	* Preview windows for the data types exposed by Mutable
	*/

	/** Widget used to show the preview of layout operation results. Once created it is reused to preserve the settings. */
	TSharedPtr<SMutableLayoutViewer> PreviewLayoutViewer;

	/** Widget used to show the preview of image operation results. Once created it is reused to preserve the settings. */
	TSharedPtr<SMutableImageViewer> PreviewImageViewer;

	/** Widget used to show a preview of the mesh and the metadata it holds */
	TSharedPtr<SMutableMeshViewer> PreviewMeshViewer;

	/** Widget used to show a preview of a mutable bool value */
	TSharedPtr<SMutableBoolViewer> PreviewBoolViewer;

	/** Widget used to show a preview of a mutable int value */
	TSharedPtr<SMutableIntViewer> PreviewIntViewer;

	/** Widget used to show a preview of a mutable float value */
	TSharedPtr<SMutableScalarViewer> PreviewScalarViewer;

	/** Widget used to show a preview of a mutable string value */
	TSharedPtr<SMutableStringViewer> PreviewStringViewer;
	
	/** Widget used to show a preview of a mutable color value */
	TSharedPtr<SMutableColorViewer> PreviewColorViewer;

	/** Widget used to display the data held on the mutable projector objects */
	TSharedPtr<SMutableProjectorViewer> PreviewProjectorViewer;

	/** Widget used to display the data held on the mutable skeleton objects */
	TSharedPtr<SMutableSkeletonViewer> PreviewSkeletonViewer;

	/** Widget used to display the data held on the mutable curve objects */
	TSharedPtr<SMutableCurveViewer> PreviewCurveViewer;
	
	/** Tree widget showing the code hierarchically. */
	TSharedPtr<STreeView<TSharedPtr<FMutableCodeTreeElement>>> TreeView;

	/** Root nodes of the tree widget. */
	TArray<TSharedPtr<FMutableCodeTreeElement>> RootNodes;

	/** Cache of tree elements matching the operations that have been generated so far. 
	* We store both the parent and the operation in the key, because a single operation may appear multiple times if it has different parents.
	*/
	struct FItemCacheKey
	{
		mu::OP::ADDRESS Parent=0;
		mu::OP::ADDRESS Child=0;
		uint32 ChildIndexInParent=0;

		friend FORCEINLINE bool operator == (const FItemCacheKey& A, const FItemCacheKey& B)
		{
			return A.Parent == B.Parent && A.Child == B.Child && A.ChildIndexInParent == B.ChildIndexInParent;
		}

		friend FORCEINLINE uint32 GetTypeHash(const FItemCacheKey& Key)
		{
			return HashCombine(Key.Parent, HashCombine(Key.Child, Key.ChildIndexInParent));
		}
	};

	/*
	* Tree widget objects
	*/

	/** Map with all the generated elements of the tree. Unique and duplicated elements will be present on this list and
	 * also the children of the unique elements.
	 * @note The children of duplicated elements will only be present once, as children of the unique element they duplicate.
	 * This is to avoid having identical elements on the tree (witch would cause a crash) while also being pointless due to
	 * how we manage expansion of duplicated elements.
	 */
	TMap< FItemCacheKey, TSharedPtr<FMutableCodeTreeElement>> ItemCache;

	/** Main tree item for each op. An op can be represented with multiple tree nodes if it is reachable from different paths. */
	TMap< mu::OP::ADDRESS, TSharedPtr<FMutableCodeTreeElement>> MainItemPerOp;

	/** List with all the elements related to nodes displayed on the tree. */
	TArray<TSharedPtr<FMutableCodeTreeElement>> TreeElements;

	/** Array with all the elements that have been manually expanded by the user */
	TMap< mu::OP::ADDRESS, TSharedPtr<FMutableCodeTreeElement>> ExpandedElements;

	/** Prepare the widget for the given model. */
	void SetCurrentModel(const mu::ModelPtr&);

	/** Before any UI operation generate all the elements that may be navigable over the tree. No children of duplicated 
	 * addresses will be generated.
	 */
	void GenerateAllTreeElements();

	/** Support GenerateAllTreeElements by generating the elements recursively */
	void GenerateElementRecursive(mu::OP::ADDRESS InParentAddress, const mu::FProgram& InProgram);

	
	/** The addresses of the root operations. Cached when this object gets loaded on the Construct method of this slate */
	TArray<mu::OP::ADDRESS> RootNodeAddresses;
	
	/** Utility method that provides you with the addresses of the root nodes of the program */
	void CacheRootNodeAddresses();
	
	/** Control boolean to know if there are any highlighted elements on the tree */
	bool bIsElementHighlighted = false;

	/** Operation that is currently being highlighted */
	mu::OP::ADDRESS HighlightedOperation{};

	/*
	 * Operations computational cost reference collections
	 */

	/** Collection with all very expensive to run operation types */
	const TArray<mu::OP_TYPE> VeryExpensiveOperationTypes
	{
		mu::OP_TYPE::ME_BINDSHAPE,
		mu::OP_TYPE::ME_MASKCLIPMESH,
		mu::OP_TYPE::ME_FORMAT,
		mu::OP_TYPE::ME_DIFFERENCE,
		mu::OP_TYPE::IM_MAKEGROWMAP,
	};

	/** Collection with all expensive to run operation types */
	const TArray<mu::OP_TYPE> ExpensiveOperationTypes
	{
		mu::OP_TYPE::IM_PIXELFORMAT,
		mu::OP_TYPE::ME_PROJECT,
	};
	
	/** Enum designed to be able to notify the row generation of the type of operation being generated */
	enum class EOperationComputationalCost : uint8
	{
		Standard =			0,			// All other operation types
		Expensive =			1,			// The ones located on ExpensiveOperationTypes
		VeryExpensive =		2			// The ones located on VeryExpensiveOperationTypes
	};

	/** Array holding the relation between each computational cost category and the color to be used to display elements related
	 * with it. 
	 */
	const TArray<FSlateColor> ColorPerComputationalCost
	{
		FSlateColor(FLinearColor(1,1,1,1)),		// Standard cost color
		FSlateColor(FLinearColor(1,0.4,0.2,1)),	// Expensive cost color
		FSlateColor(FLinearColor(1,0.1,0.1,1))		// Very Expensive cost color
	};
	
	/** Provided an operation type it returns the category representing how much costs to run an operation of this type
	 * @param OperationType The operation type you want to know the computational cost of
	 * @return The enum value representing the computational cost of the operation type provided.
	 */
	EOperationComputationalCost GetOperationTypeComputationalCost(mu::OP_TYPE OperationType) const;
	
	
	/*
	 * Navigation : Operation type navigation Type selection object
	 */
	
	/** Slate object that provides the user a way of selecting what kind of operation it wants to navigate.*/
	TSharedPtr<SComboBox<TSharedPtr<FMutableOperationElement>>> TargetedTypeSelector;

	/** Data backend for the list displayed for the navigation type selection */
	TArray<TSharedPtr<FMutableOperationElement>> FoundModelOperationTypeElements;

	/** Currently selected element on the TargetedTypeSelector slate. Actively used by the ui */
	TSharedPtr<FMutableOperationElement> CurrentlySelectedOperationTypeElement;
	
	/** Operation type we are using to search for tree nodes. Driven primarily by the UI */
	mu::OP_TYPE OperationTypeToSearch = mu::OP_TYPE::NONE;

	/** Operation types present on the currently set mutable model. */
	TArray<mu::OP_TYPE> ModelOperationTypes;
	
	/** Array with all the names for each of the operations available on mutable. Used by the UI  */
	TArray<TSharedPtr<FString>> ModelOperationTypeStrings;
	
	/** Stores a list of strings based on the possible types of operations mutable define to be used by the UI */
	void GenerateNavigationOpTypeStrings();

	/** Generate a list of elements that will be used as backend for the navigation type selection dropdown */
	void GenerateNavigationDropdownElements();
	
	
	/** Fills ModelOperationTypes with all the types present on the current model.
	 * It also makes sure we have a NONE operation and that the operations are sorted alphabetically.
	 * @note Method designed to be called once per model.*/
	void CacheOperationTypesPresentOnModel();
	
	/** Method to scan over all the operations performed on the current model to produce a set of operation types present on it.
	 * @param InParentAddresses Addresses of the parent objects. Required to be able to perform recursive calls to this method (to get the data from the children)
	 * @param  InProgram Mutable program.
	 * @param OutLocatedOperations Output of the method : Set with all the operation types present on the model.
	 * @param AlreadyProcessedAddresses Addresses already processed. Used to avoid processing the same operation twice.
	 */
	void GetOperationTypesPresentOnModel(
		const TArray< mu::OP::ADDRESS>& InParentAddresses, const mu::FProgram& InProgram, TSet<mu::OP_TYPE>& OutLocatedOperations,
		TSet<mu::OP::ADDRESS>& AlreadyProcessedAddresses);

	
	/*
	 * Navigation : UI callback methods
	 */

	/** Generate the text to be used by the navigation operation selector */
	FText GetCurrentNavigationOpTypeText() const;

	/** Returns the color to be used by the text being currently displayed as selected on the operation selector*/
	FSlateColor GetCurrentNavigationOpTypeColor() const;

	/** Callback invoked by the ComboBox used for displaying and selecting operation types for navigation. it gets invoked
	 * each time the slate object requires to draw a line representing one of the elements set on FoundModelOperationTypeElements
	 */
	TSharedRef<SWidget> OnGenerateOpNavigationDropDownWidget(TSharedPtr<FMutableOperationElement> MutableOperationElement) const;

	/** Callback invoked each time the selected operation on our navigation slate changes. It can change due to UI interaction
	 * or also due to direct change by invoking the SetSelectedOption on the SComboBox TargetedTypeSelector
	 */
	void OnNavigationSelectedOperationChanged(TSharedPtr<FMutableOperationElement, ESPMode::ThreadSafe> MutableOperationElement, ESelectInfo::Type Arg);

	/** Callback used to print on screen the amount of operations found on the tree that share the same operation type that
	 * the one currently selected on the navigation system.
	 */
	FText OnPrintNavigableObjectAddressesCount() const;
	
	/** Method that tells the UI if the bottom to go to the previous element can be interacted */
	bool CanInteractWithPreviousOperationButton() const;

	/** Method that tells the UI if the bottom to go to the next element can be interacted */
	bool CanInteractWithNextOperationButton() const;
	

	/** Used by the UI and internally does what is necessary to get to the previous element of the targeted search type.
	 * It is designed to work alongside GoToPreviousOperationAfterRefresh(...)
	 * @return A reply to tell if the UI action has been handled or not.
	 */
	FReply OnGoToPreviousOperationButtonPressed();

	/** Used by the UI and internally does what is necessary to get to the next element of the targeted search type.
	 * It is designed to work alongside GoToNextOperationAfterRefresh(...)
	 * @return A reply to tell if the UI action has been handled or not.
	 */
	FReply OnGoToNextOperationButtonPressed();
	
	
	/** Callback method used to allow the user to directly set the type of operation to be scanning directly by selecting a
	 * operation on the tree and using that operation type as the type to search for
	 */
	void OnSelectedOperationTypeFromTree();
	
	/*
	 * Navigation : control flags
	 */
	
	/** Flag monitoring if we have requested a scroll operation to reach the targeted element */
	bool bWasScrollToTargetRequested = false;

	/** Flag designed to tell the system when the expansion of unique elements have been performed as part of the navigation operation */
	bool bWasUniqueExpansionInvokedForNavigation = false;
	
	/*
	 * Navigation : Shared objects between navigation search types 
	 */

	/** Array with all the elements of the type we are looking for (shared type of constant resource). */
	TArray<TSharedPtr<FMutableCodeTreeElement>> NavigationElements;
	
	/** Operation types present on the currently set mutable model. */
	int64 NavigationIndex = -1;
	
	/** Sort the contents of NavigationElements to follow a sequential pattern using the indices of the elements from 0 to +n*/
	void SortNavigationElements();

	/** Focus the current NavigationElement and places it into view. It also selects it so the previewer for the
	 * element gets invoked
	 */
	void FocusViewOnNavigationTarget();
	
	/** Wrapper struct designed to be used as cache for all elements found during the navigation system search for elements
	 * of X Type or relation with a targeted constant resource. It is designed to be used and then destroyed once the
	 * search operation has been completed */
	struct FElementsSearchCache
	{
		/** Set of addresses that have already been searched for relevant data */
		TSet<mu::OP::ADDRESS> ProcessedAddresses;

		/** Collection of elements that have been found during the search. They may be related by OP_Type or used constant resource*/
		TArray<TSharedPtr<FMutableCodeTreeElement>> FoundElements;
		
		/** Array containing all the next addresses to be processed.
		 * The child is the address itself to be later processed
		 * The parent is the parent address of the child.
		 * The ChildIndexInParent is the index (or child position) o the child address on Child as part of the child set of the parent address.
		 */
		TArray<FItemCacheKey> BatchData;
		

		/** Generates the structures to be able to start the search of elements. It uses the root addresses as the
		 * start of the search operation.
		 * @param InRootNodeAddresses The addresses of the root operations of the operations tree
		 */
		void SetupRootBatch(const TArray<mu::OP::ADDRESS>& InRootNodeAddresses)
		{
			check (InRootNodeAddresses.Num());
			// This method should only be called once when no data is present on this cache
			check (BatchData.IsEmpty());
			
			BatchData.Reserve(InRootNodeAddresses.Num());
			// The child address of each parent operation on it's parent
			for (int32 RootIndex = 0; RootIndex < InRootNodeAddresses.Num(); RootIndex++)
			{
				FItemCacheKey Key;
				{
					Key.Child = InRootNodeAddresses[RootIndex];
					// Store the parent of this object as 0 to have a "virtual" address witch all root addresses are
					// children of 
					Key.Parent = 0;
					// And also the index of the parent on it's parent "virtual" structure
					Key.ChildIndexInParent = RootIndex;		
				}

				// Add this new entry point for the search of addresses 
				BatchData.Add(Key);
			}
		}
		
		/** Method to cache the provided mutable address as one of the addresses that are of the type we are looking for or maybe
		* is related with the constant resource we are looking for operations related with.
		* @param OpAddress The address to save as one related with a operation type or constant resource
		* @param IndexAsChildOfInputAddress The index on BatchData that represents the parent of the provided operation address
		* @param InItemCache Cache with all the elements of the tree. Here is where we search for the element based on
		* the OpAddress and the parent and ChildIndexInParent provided thanks to IndexAsChildOfInputAddress
		*/
		void AddToFoundElements(const mu::OP::ADDRESS OpAddress,const int32 IndexAsChildOfInputAddress,
		                        const TMap<FItemCacheKey, TSharedPtr<FMutableCodeTreeElement>>& InItemCache)
		{
			FItemCacheKey Key;
			{
				Key.Child = OpAddress;
				// Store the parent address of this object
				Key.Parent = BatchData[IndexAsChildOfInputAddress].Parent;
				// And also the index of the parent on it's parent structure
				Key.ChildIndexInParent = BatchData[IndexAsChildOfInputAddress].ChildIndexInParent;		
			}
			// Generate a key for this element in order to search in on the map with all the elements
								
			// Find that element on the tree, (check error if not found since all elements should be there)
			const TSharedPtr<FMutableCodeTreeElement>* PtrFoundElement = InItemCache.Find(Key);
			check(PtrFoundElement);
			TSharedPtr<FMutableCodeTreeElement> FoundElement = *PtrFoundElement;
			check(FoundElement != nullptr);
			
			// Store this element on our temp Map of elements
			FoundElements.Add(FoundElement);
		}

		/** Caches the provided parent address to the Search payload so they can be later read and processed on another batch of the
		 * method tasked with finding related operations to operation type or constant resource.
		 * @param InParentAddress The parent address to search children of.
		 * @param InProgram The mutable program that will be used to perform the children search.
		 * @param OutFoundChildrenData Array with all the ItemCacheKeys that represent all the children found
		 * @note It will not add the children of any provided ParentAddress for the next batch if the parent address provided
		 * has already been processed and therefore whose children have already been searched or prepared for searching.
		 */
		void CacheChildrenOfAddressIfNotProcessed(mu::OP::ADDRESS InParentAddress,
		                                          const mu::FProgram& InProgram,
		                                          TArray<FItemCacheKey>& OutFoundChildrenData)
		{
			if (!ProcessedAddresses.Contains(InParentAddress))
			{
				// Cache to avoid processing it again later
				ProcessedAddresses.Add(InParentAddress);
	
				// Generic case for unnamed children traversal.
				uint32 ChildIndex = 0;
				mu::ForEachReference(InProgram, InParentAddress, [this, &InParentAddress, &ChildIndex,&OutFoundChildrenData]( mu::OP::ADDRESS ChildAddress)
				{
					// If the parent does have a child then process it 
					if (ChildAddress) 
					{
						FItemCacheKey Key;
						{
							Key.Child = ChildAddress;
							Key.Parent = InParentAddress;
							Key.ChildIndexInParent = ChildIndex;		
						}

						// Save it to the output so can later be placed onto BatchData safely 
						OutFoundChildrenData.Add(Key);
					}
					ChildIndex++;
				});
			}
		}
	};
	
	
	/*
	 * Navigation : Operation type based navigation
	 */
	
	/** Provided an Operation type store as navigation addresses all operations of the targeted navigation Op type
	 * @note The operations found get saved on the collection  NavigationOPAddresses.
	 */
	void CacheAddressesOfOperationsOfType();

	/** Fills an array with all the addresses of operations that do have in common the same targeted operation type
	 * @param TargetOperationType The operation type used to discriminate what operation addresses we want to retrieve.
	 * processing them more than once.
	 * @param InSearchPayload A caching structure designed to hold the data that gets passed from one recursive call to
	 * the other. It also stores the found elements and other data.
	 * @param InProgram The mutable program holding the data to be searched over
	 */
	void GetOperationsOfType(const mu::OP_TYPE& TargetOperationType,
	                         FElementsSearchCache& InSearchPayload,
	                         const mu::FProgram& InProgram);

	/*
	 * Navigation : Navigation based on constant resource relation with addresses
	 */

public:
	
	/**
	 * Provided a constant resource data type and index on its constant array this method sets all the operations that
	 * make usage of said constant to be navigable by the navigation system.
	 * @note The output of this operation will be cached onto NavigationOPAddresses so we can navigate over them.
	 * @param ConstantDataType The constant data type to locate operations referencing it: It is required for the
	 * system to know what operations should be checked for the usage of the provided constant resource. For example, a
	 * type of mu::DATATYPE::DT_Curve will try to locate operations that we know operate with constant curves.
	 * @param IndexOnConstantsArray The index of the constant on its constants array. If it is, for example, a constant mesh
	 * this value represents the index of the constant inside constantMeshes array on mu::program.
	 */
	void CacheAddressesRelatedWithConstantResource(const mu::DATATYPE ConstantDataType, const int32 IndexOnConstantsArray);

private:
	
	/**
	 * Provided a data type and the index of the constant object using said data type locate all operations that do
	 * directly make use of said constant resource.
	 * @param ConstantDataType The type of the resource we are providing an index of. 
	 * @param IndexOnConstantsArray The index of the constant resource we want to know what operations are using it.
	 * @param InSearchPayload Cache object that will end up containing all the elements related with the provided constant
	 * resource
	 * @param InProgram The mutable program containing all the operations of the graph.
	 */
	void GetOperationsReferencingConstantResource(const mu::DATATYPE ConstantDataType,
	                                              const int32 IndexOnConstantsArray,
	                                              FElementsSearchCache& InSearchPayload,
	                                              const mu::FProgram& InProgram);

	/**
	 * Provided an operation address this method tells us if that address is making use of our constant resource or not.
	 * It will take into account the datatype of the provided resource to discard or acknowledge the operation type of the
	 * provided operation address.
	 * @param IndexOnConstantsArray The index onto its respective constants array of the constant resource provided. We do
	 * not give a pointer to the constant resource but the index of it onto its hosting array.
	 * @param ConstantDataType The type of data we know the constant uses. It is used to check for ones or other operations since
	 * not all operations do access the same constant resources.
	 * @param OperationAddress The address of the operation currently being scanned for constant resources.
	 * @param InProgram The mutable program to use to check the type of operation that corresponds to that operation address.
	 * @return True if the operation at OperationAddress does make use of the provided constant resource. False otherwise
	 */
	bool IsConstantResourceUsedByOperation(const int32 IndexOnConstantsArray,
	                                       const mu::DATATYPE ConstantDataType, const mu::OP::ADDRESS OperationAddress,
	                                       const mu::FProgram& InProgram) const;
	
	
	/*
	* Main callbacks from the tree widget standard operation. 
	*/

	/** Callback that generates a new Row object based on a Tree Node*/
 	TSharedRef<ITableRow> GenerateRowForNodeTree(TSharedPtr<FMutableCodeTreeElement> InInfo, const TSharedRef<STableViewBase>& OwnerTable);
	
	/**
	* Provided an element it is returned a list with all the immediate children it has 
	* @param InInfo - The MutableCodeElement whose children objects we are searching
	* @param OutChildren - The children of the provided MutableCodeTreeElement
	*/
	void GetChildrenForInfo(TSharedPtr<FMutableCodeTreeElement> InInfo, TArray< TSharedPtr<FMutableCodeTreeElement> >& OutChildren);

	/** 
	* Callback invoked each time an element of the tree gets expanded or contracted 
	* @param InInfo - The MutableCodeElement to be contracted or expanded
	* @param bInExpanded - Determines if the action is of contraction (false) or expansion (true).
	*/
	void OnExpansionChanged(TSharedPtr<FMutableCodeTreeElement> InInfo, bool bInExpanded);


	/** 
	* Callback invoked each time the selected element changes used to generate the previews depending of the 
	* operation selected
	* @param InInfo - The MutableCodeElement that has been selected. Can be null if unselected
	* @param SelectInfo - Way in witch the selection changed
	*/
	void OnSelectionChanged(TSharedPtr<FMutableCodeTreeElement> InInfo, ESelectInfo::Type SelectInfo);

	/** 
	* Callback invoked when a tree row is getting removed 
	* @param InTreeRow - The tree row that is being removed from the tree view
	*/
	void OnRowReleased(const TSharedRef<ITableRow>& InTreeRow);

	/** Callback invoked when whe press the right mouse button. Useful for adding context menu objects */
	TSharedPtr<SWidget> OnTreeContextMenuOpening();

	void TreeExpandRecursive(TSharedPtr<FMutableCodeTreeElement> Info, bool bExpand );
	
	/** Temporal object designed to be used during the recursive operation of TreeExpandElements() and group
	 * strongly related data
	 */
	struct FProcessedOperationsBuffer
	{
		/** Array with the operation addresses of all original (not duplicates) expanded operations. */
		TArray<mu::OP::ADDRESS> ExpandedOriginalOperations;

		/** Array with the operation addresses of all duplicated expanded operations. */
		TArray<mu::OP::ADDRESS> ExpandedDuplicatedOperations;
	};
	
	/** 
	* Expands the provided elements and all the children they have. 
	* @note It performs the operation by going to the deepest children and expanding from then upwards to the origin before
	* proceeding to the next parent. This way we make sure that the expansion of the elements is following the order
	* of left to right and top to bottom in a "Z" like pattern like the way the tree gets read by humans.
	* @param InElementsToExpand - The root info objects where to start the expansion
	* @param bForceExpandDuplicates - (Optional) Determines if the duplicated objects must be expanded. 
	*	Used for certain situations where we want to expand them. By default they do not get expanded
	* @param FilteringDataType - (Optional) determines what kind of operations should be expanded. By default no
	* filtering is applied
	* @param InExpandedOperationsBuffer (Optional) Object containing all the duplicated and original elements already processed
	* during the subsequent recursive calls to this method.
	*/
	void TreeExpandElements(TArray<TSharedPtr<FMutableCodeTreeElement>>& InElementsToExpand,
		bool bForceExpandDuplicates = false,
		mu::DATATYPE FilteringDataType = mu::DATATYPE::DT_NONE,
		TSharedPtr<FProcessedOperationsBuffer> InExpandedOperationsBuffer = nullptr);
	

	/** Expand only the unique elements (mot duplicates) */
	void TreeExpandUnique();

	/** Expand only the elements using as operation type DT_INSTANCE */
	void TreeExpandInstance();
	
	/** Grabs the selected element and expands all the elements inside the selected branch. Duplicates are ignored */
	void TreeExpandSelected();
	
	/** 
	* Highlights all tree elements that share the same operation as the element provided
	* @param InTargetElement - The info object to be used as blueprint to search for similar objects and highlight them
	*/
	void HighlightDuplicatesOfEntry(const TSharedPtr<FMutableCodeTreeElement>& InTargetElement);

	/** Clear all the highlighted elements and set them to their default visual state */
	void ClearHighlightedItems();

	/** Called when any parameter value has changed, with the parameter index as argument.  */
	void OnPreviewParameterValueChanged( int32 ParamIndex );

	

	/*
	 * Control of "Skip Mips" for image operations.
	 */

	/** Operation type we are using to search for tree nodes. Driven by the UI */
	int32 MipsToSkip = 0;

	/** */
	bool bSelectedOperationIsImage = false;

	/** Stores a list of strings based on the possible types of operations mutable define to be used by the UI */
	EVisibility IsMipSkipVisible() const;

	/** Stores a list of strings based on the possible types of operations mutable define to be used by the UI */
	TOptional<int32> GetCurrentMipSkip() const;

	/** */
	void OnCurrentMipSkipChanged(int32 NewValue);

	/*
	 * Remote previewer invocation methods
	 */

public:
	void PreviewMutableImage (mu::ImagePtrConst InImagePtr);
	void PreviewMutableMesh (mu::MeshPtrConst InMeshPtr);
	void PreviewMutableLayout(mu::LayoutPtrConst Layout);
	void PreviewMutableSkeleton(mu::SkeletonPtrConst Skeleton);
	void PreviewMutableString(const mu::string* InStringPtr);
	void PreviewMutableProjector(const mu::FProjector* Projector);
	void PreviewMutableMatrix(const mu::mat4f* Mat);
	void PreviewMutableShape(const mu::FShape* Shape);
	void PreviewMutableCurve(const mu::Curve* Curve);
	
private:
	void PrepareStringViewer();
	void PrepareImageViewer();
	void PrepareMeshViewer();
	void PrepareLayoutViewer();
	void PrepareProjectorViewer();
};

/** The data of a row on the operation type dropdown. */
class FMutableOperationElement : public TSharedFromThis<FMutableOperationElement>
{
public:
	FMutableOperationElement(mu::OP_TYPE InOperationType, FText OperationTypeBeingRepresented,FSlateColor OperationColor)
	{
		OperationType = InOperationType;
		OperationTypeText = OperationTypeBeingRepresented;
		OperationTextColor = OperationColor;
	}

public:
	mu::OP_TYPE OperationType;
	FText OperationTypeText;
	FSlateColor OperationTextColor;
};



/** An row of the code tree in the SMutableCodeViewer. */
class FMutableCodeTreeElement : public TSharedFromThis<FMutableCodeTreeElement>
{
public:
	FMutableCodeTreeElement(int32 InIndexOnTree ,const mu::ModelPtr& InModel, mu::OP::ADDRESS InOperation, const FString& InCaption,const FSlateColor InLabelColor, const TSharedPtr<FMutableCodeTreeElement>* InDuplicatedOf = nullptr)
	{
		MutableModel = InModel;
		MutableOperation = InOperation;
		Caption = InCaption;
		LabelColor = InLabelColor;
		IndexOnTree = InIndexOnTree;
		if (InDuplicatedOf)
		{
			DuplicatedOf = *InDuplicatedOf;
		}

		// Check what type of operation is (state constant or dynamic resource)
		{
			// If duplicated then grab the already processed data on the original operation
			if (InDuplicatedOf)
			{
				bIsDynamicResource = DuplicatedOf->bIsDynamicResource;
				bIsStateConstant = DuplicatedOf->bIsStateConstant;

				// All required data has been processed so an early exit is required
				return;
			}
			
			// Iterate over all states and try to locate the operation
			const mu::FProgram& MutableProgram = InModel->GetPrivate()->m_program;
			for (const mu::FProgram::FState& CurrentState : MutableProgram.m_states)
			{
				// Check if it is a dynamic resource
				for (auto& DynamicResource : CurrentState.m_dynamicResources)
				{
					// If the operation gets located then mark it as dynamic resource
					if (DynamicResource.Key == MutableOperation)
					{
						bIsDynamicResource = true;
						break;
					}
				}
				
				// Early exit: A dynamic resource can not be at the same time a state constant
				if (bIsDynamicResource)
				{
					return;
				}
				
				// Check if it is a state constant
				bIsStateConstant = CurrentState.m_updateCache.Contains(MutableOperation);
			}
		}
		
	}

public:

	/** */
	mu::ModelPtr MutableModel;

	/** Mutable Graph Node represented in this tree row. */
	mu::OP::ADDRESS MutableOperation;

	/** If true means that it will not update when a runtime parameter on the state gets updated */
	bool bIsStateConstant = false;

	/** If true then the mesh or image of this operation may change during the state update */
	bool bIsDynamicResource = false;

	/** Label representing this operation. */
	FString Caption;

	/** If this tree element is a duplicated of another op, this is the op. */
	TSharedPtr<FMutableCodeTreeElement> DuplicatedOf;

	/** The color to be used by the row representing this object */
	FSlateColor LabelColor;

	/*
	 * Navigation metadata
	 */
	
	/** The current position of this element on the tree view. Used for navigation */
	int32 IndexOnTree;
};
