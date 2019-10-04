// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "DynamicMesh3.h"

template<typename ParentType>
class TDynamicAttributeBase;

/**
* Generic base class for change tracking of an attribute layer
*/
template<typename ParentType>
class TDynamicAttributeChangeBase
{
public:
	virtual ~TDynamicAttributeChangeBase()
	{
	}

	// default do-nothing implementations are provided because many attribute layers will only care about some kinds of elements and won't implement all of these

	virtual void SaveInitialTriangle(const TDynamicAttributeBase<ParentType>* Attribute, int TriangleID)
	{
	}
	virtual void SaveInitialVertex(const TDynamicAttributeBase<ParentType>* Attribute, int VertexID)
	{
	}

	virtual void StoreAllFinalTriangles(const TDynamicAttributeBase<ParentType>* Attribute, const TArray<int>& TriangleIDs)
	{
	}
	virtual void StoreAllFinalVertices(const TDynamicAttributeBase<ParentType>* Attribute, const TArray<int>& VertexIDs)
	{
	}

	virtual bool Apply(TDynamicAttributeBase<ParentType>* Attribute, bool bRevert) const
	{
		return false;
	}
};

typedef TDynamicAttributeChangeBase<FDynamicMesh3> FDynamicMeshAttributeChangeBase;


/**
 * Base class for attributes that live on a dynamic mesh (or similar dynamic object)
 *
 * Subclasses can override the On* functions to ensure the attribute remains up to date through changes to the dynamic object
 */
template<typename ParentType>
class TDynamicAttributeBase
{

public:
	virtual ~TDynamicAttributeBase()
	{
	}

public:

	/** Allocate a new copy of the attribute layer, optionally with a different parent mesh */
	virtual TDynamicAttributeBase* MakeCopy(ParentType* ParentIn) const = 0;


	virtual void OnNewVertex(int VertexID, bool bInserted)
	{
	}

	virtual void OnRemoveVertex(int VertexID)
	{
	}

	virtual void OnNewTriangle(int TriangleID, bool bInserted)
	{
	}

	virtual void OnRemoveTriangle(int TriangleID)
	{
	}

	virtual void OnReverseTriOrientation(int TriangleID)
	{
	}

	/**
	* Check validity of attribute
	* 
	* @param bAllowNonmanifold Accept non-manifold topology as valid. Note that this should almost always be true for attributes; non-manifold overlays are generally valid.
	* @param FailMode Desired behavior if mesh is found invalid
	*/
	virtual bool CheckValidity(bool bAllowNonmanifold, EValidityCheckFailMode FailMode) const
	{
		// default impl just doesn't check anything; override with any useful sanity checks
		return true;
	}


	virtual TUniquePtr<TDynamicAttributeChangeBase<ParentType>> NewBlankChange() = 0;



	/** Update to reflect an edge split in the parent mesh */
	virtual void OnSplitEdge(const FDynamicMesh3::FEdgeSplitInfo& SplitInfo)
	{
	}

	/** Update to reflect an edge flip in the parent mesh */
	virtual void OnFlipEdge(const FDynamicMesh3::FEdgeFlipInfo& FlipInfo)
	{
	}

	/** Update to reflect an edge collapse in the parent mesh */
	virtual void OnCollapseEdge(const FDynamicMesh3::FEdgeCollapseInfo& CollapseInfo)
	{
	}

	/** Update to reflect a face poke in the parent mesh */
	virtual void OnPokeTriangle(const FDynamicMesh3::FPokeTriangleInfo& PokeInfo)
	{
	}

	/** Update to reflect an edge merge in the parent mesh */
	virtual void OnMergeEdges(const FDynamicMesh3::FMergeEdgesInfo& MergeInfo)
	{
	}

};


typedef TDynamicAttributeBase<FDynamicMesh3> FDynamicMeshAttributeBase;


/**
* Generic base class for managing a set of registered attributes that must all be kept up to date
*/
template<typename ParentType>
class TDynamicAttributeSetBase
{
protected:
	// not managed by the base class; we should be able to register any attributes here that we want to be automatically updated
	TArray<TDynamicAttributeBase<ParentType>*> RegisteredAttributes;

	/**
	 * Stores the given attribute pointer in the attribute register, so that it will be updated with mesh changes, but does not take ownership of the attribute memory.
	 */
	void RegisterExternalAttribute(TDynamicAttributeBase<ParentType>* Attribute)
	{
		RegisteredAttributes.Add(Attribute);
	}

	void ResetRegisteredAttributes()
	{
		RegisteredAttributes.Reset();
	}

public:
	virtual ~TDynamicAttributeSetBase()
	{
	}

	int NumRegisteredAttributes() const
	{
		return RegisteredAttributes.Num();
	}

	TDynamicAttributeBase<ParentType>* GetRegisteredAttribute(int Idx) const
	{
		return RegisteredAttributes[Idx];
	}

	// These functions are called by the FDynamicMesh3 to update the various
	// attributes when the parent mesh topology has been modified.
	virtual void OnNewTriangle(int TriangleID, bool bInserted)
	{
		for (TDynamicAttributeBase<ParentType>* A : RegisteredAttributes)
		{
			A->OnNewTriangle(TriangleID, bInserted);
		}
	}
	virtual void OnNewVertex(int VertexID, bool bInserted)
	{
		for (TDynamicAttributeBase<ParentType>* A : RegisteredAttributes)
		{
			A->OnNewVertex(VertexID, bInserted);
		}
	}
	virtual void OnRemoveTriangle(int TriangleID)
	{
		for (TDynamicAttributeBase<ParentType>* A : RegisteredAttributes)
		{
			A->OnRemoveTriangle(TriangleID);
		}
	}
	virtual void OnRemoveVertex(int VertexID)
	{
		for (TDynamicAttributeBase<ParentType>* A : RegisteredAttributes)
		{
			A->OnRemoveTriangle(VertexID);
		}
	}
	virtual void OnReverseTriOrientation(int TriangleID)
	{
		for (TDynamicAttributeBase<ParentType>* A : RegisteredAttributes)
		{
			A->OnReverseTriOrientation(TriangleID);
		}
	}

	/**
	* Check validity of attributes
	* 
	* @param bAllowNonmanifold Accept non-manifold topology as valid. Note that this should almost always be true for attributes; non-manifold overlays are generally valid.
	* @param FailMode Desired behavior if mesh is found invalid
	*/
	virtual bool CheckValidity(bool bAllowNonmanifold, EValidityCheckFailMode FailMode) const
	{
		bool bValid = true;
		for (TDynamicAttributeBase<ParentType>* A : RegisteredAttributes)
		{
			bValid = A->CheckValidity(bAllowNonmanifold, FailMode) && bValid;
		}
		return bValid;
	}


	// mesh-specific on* functions; may be split out
public:

	virtual void OnSplitEdge(const FDynamicMesh3::FEdgeSplitInfo& SplitInfo)
	{
		for (TDynamicAttributeBase<ParentType>* A : RegisteredAttributes)
		{
			A->OnSplitEdge(SplitInfo);
		}
	}
	virtual void OnFlipEdge(const FDynamicMesh3::FEdgeFlipInfo& FlipInfo)
	{
		for (TDynamicAttributeBase<ParentType>* A : RegisteredAttributes)
		{
			A->OnFlipEdge(FlipInfo);
		}
	}
	virtual void OnCollapseEdge(const FDynamicMesh3::FEdgeCollapseInfo& CollapseInfo)
	{
		for (TDynamicAttributeBase<ParentType>* A : RegisteredAttributes)
		{
			A->OnCollapseEdge(CollapseInfo);
		}
	}
	virtual void OnPokeTriangle(const FDynamicMesh3::FPokeTriangleInfo& PokeInfo)
	{
		for (TDynamicAttributeBase<ParentType>* A : RegisteredAttributes)
		{
			A->OnPokeTriangle(PokeInfo);
		}
	}
	virtual void OnMergeEdges(const FDynamicMesh3::FMergeEdgesInfo& MergeInfo)
	{
		for (TDynamicAttributeBase<ParentType>* A : RegisteredAttributes)
		{
			A->OnMergeEdges(MergeInfo);
		}
	}
};

typedef TDynamicAttributeSetBase<FDynamicMesh3> FDynamicMeshAttributeSetBase;
