// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Chaos/ClusterUnionManager.h"
#include "Chaos/PBDRigidClusteredParticles.h"
#include "Chaos/PBDCollisionConstraints.h"
#include "Chaos/Transform.h"
#include "Chaos/CollisionResolutionTypes.h"
#include "Chaos/ExternalCollisionData.h"
#include "Chaos/TriangleMesh.h"
#include "Chaos/ClusterCreationParameters.h"
#include "Chaos/GeometryParticlesfwd.h"
#include "Framework/BufferedData.h"
#include "Chaos/PBDRigidClusteringTypes.h"

namespace Chaos
{
	class FPBDCollisionConstraints;

struct CHAOS_API FClusterDestoryParameters {
	bool bReturnInternalOnly : true;
};

/****
*
*   FRigidClustering
* 
*   The Chaos Destruction System allows artists to define exactly how geometry 
*   will break and separate during the simulations. Artists construct the 
*	simulation assets using pre-fractured geometry and utilize dynamically 
*   generated rigid constraints to model the structural connections during the
*   simulation. The resulting objects within the simulation can separate from 
*   connected structures based on interactions with environmental elements, 
*   like fields and collisions.
*
*   The destruction system relies on an internal clustering model 
*   (aka Clustering) which controls how the rigidly attached geometry is 
*   simulated. Clustering allows artists to initialize sets of geometry as 
*   a single rigid body, then dynamically break the objects during the 
*   simulation. At its core, the clustering system will simply join the mass
*   and inertia of each connected element into one larger single rigid body.
* 
*   At the beginning of the simulation a connection graph is initialized 
*   based on the rigid body’s nearest neighbors. Each connection between the
*   bodies represents a rigid constraint within the cluster and is given 
*   initial strain values. During the simulation, the strains within the 
*   connection graph are evaluated. The connections can be broken when collision
*   constraints, or field evaluations, impart an impulse on the rigid body that
*   exceeds the connections limit. Fields can also be used to decrease the 
*   internal strain values of the connections, resulting in a weakening of the
*   internal structure
*
*/
class CHAOS_API FRigidClustering
{
public:

	typedef FPBDRigidsEvolutionGBF								FRigidEvolution;
	typedef FPBDRigidParticleHandle*							FRigidHandle;
	typedef TArray<FRigidHandle>								FRigidHandleArray;
	typedef FPBDRigidClusteredParticleHandle*					FClusterHandle;
	typedef TMap<FClusterHandle, FRigidHandleArray>				FClusterMap;
	typedef TFunction<void(FRigidClustering&, FRigidHandle)>	FVisitorFunction;

	FRigidClustering(FRigidEvolution& InEvolution, FPBDRigidClusteredParticles& InParticles, const TArray<ISimCallbackObject*>* InStrainModifiers);
	~FRigidClustering();

	//
	// Initialization
	//

	/**
	 *  Initialize a cluster with the specified children.
	 *
	 *	\p ClusterGroupIndex - Index to join cluster into.
	 *	\p Children - List of children that should belong to the cluster
	 *	\p Parameters - ClusterParticleHandle must be valid, this is the parent cluster body.
	 *	ProxyGeometry : Collision default for the cluster, automatically generated otherwise.
	 *		ForceMassOrientation : Inertial alignment into mass space.
	 *	
	 */
	Chaos::FPBDRigidClusteredParticleHandle* CreateClusterParticle(
		const int32 ClusterGroupIndex, 
		TArray<Chaos::FPBDRigidParticleHandle*>&& Children, 
		const FClusterCreationParameters& Parameters = FClusterCreationParameters(),
		TSharedPtr<Chaos::FImplicitObject, ESPMode::ThreadSafe> ProxyGeometry = nullptr,
		const FRigidTransform3* ForceMassOrientation = nullptr,
		const FUniqueIdx* ExistingIndex = nullptr);

	/**
	 *  CreateClusterParticleFromClusterChildren
	 *    Children : Rigid body ID to include in the cluster.
	 */
	Chaos::FPBDRigidClusteredParticleHandle* CreateClusterParticleFromClusterChildren(
		TArray<FPBDRigidParticleHandle*>&& Children, 
		FPBDRigidClusteredParticleHandle* Parent,
		const FRigidTransform3& ClusterWorldTM, 
		const FClusterCreationParameters& Parameters/* = FClusterCreationParameters()*/);

	/**
	 * Manually add a set of particles to a cluster after the cluster has already been created.
	 *	ChildToParentMap: A map that may contain a pointer to one of the child particles as a key, and a pointer to its old parent particle (prior to be being released for example).
	 *					  If a child particle exists in this map, its parent (or whatever is specified as the value) will be used to determine the correct proxy to add to the parent cluster.
	 */
	void AddParticlesToCluster(
		FPBDRigidClusteredParticleHandle* Cluster,
		const TArray<FPBDRigidParticleHandle*>& InChildren,
		const TMap<FPBDRigidParticleHandle*, FPBDRigidParticleHandle*>& ChildToParentMap);

	/**
	 * Manually remove a set of particles from the cluster.
	 */
	void RemoveParticlesFromCluster(
		FPBDRigidClusteredParticleHandle* Cluster,
		const TArray<FPBDRigidParticleHandle*>& InChildren);
	/**
	 *  UnionClusterGroups
	 *    Clusters that share a group index should be unioned into a single cluster prior to simulation.
	 *    The GroupIndex should be set on creation, and never touched by the client again.
	 */
	void UnionClusterGroups();

	//
	// Releasing
	//

	/*
	*  DeactivateClusterParticle
	*    Release all the particles within the cluster particle
	*/
	TSet<FPBDRigidParticleHandle*> DeactivateClusterParticle(FPBDRigidClusteredParticleHandle* ClusteredParticle);

	/*
	*  ReleaseClusterParticles (BasedOnStrain)
	*    Release clusters based on the passed in \p ExternalStrainArray, or the 
	*    particle handle's current \c CollisionImpulses() value. Any cluster bodies 
	*    that have a strain value less than this valid will be released from the 
	*    cluster.
	*/
	TSet<FPBDRigidParticleHandle*> ReleaseClusterParticles(
		FPBDRigidClusteredParticleHandle* ClusteredParticle, 
		bool bForceRelease = false);

	TSet<FPBDRigidParticleHandle*> ReleaseClusterParticlesNoInternalCluster(
		FPBDRigidClusteredParticleHandle* ClusteredParticle,
		bool bForceRelease = false);

	/*
	*  ReleaseClusterParticles
	*    Release all rigid body IDs passed,
	*/
	TSet<FPBDRigidParticleHandle*> ReleaseClusterParticles(
		TArray<FPBDRigidParticleHandle*> ChildrenParticles, bool bTriggerBreakEvents = false);

	/** 
	* Force Release a particle at any level by making sure their parent particles are also release if necessary 
	* @Warning this will force all particles including the ones in the parent chain to be made breakable 
	*/
	void ForceReleaseChildParticleAndParents(FPBDRigidClusteredParticleHandle* ChildClusteredParticle, bool bTriggerBreakEvents);

	/*
	*  DestroyClusterParticle
	*    Disable the cluster particle and remove from all internal clustering
	*    structures. This will not activate children.
	* 
	*    Returns the active parent cluster that might need to be rebuilt because
	*    its geometry might be pointing to deleted particle handels. 
	*/
	FPBDRigidClusteredParticleHandle* DestroyClusterParticle(
		FPBDRigidClusteredParticleHandle* ClusteredParticle,
		const FClusterDestoryParameters& Parameters = FClusterDestoryParameters());

	/*
	*  BreakCluster
	*    Breaks a cluster (internal or not) by applying max external strain to all its children 
	* 
	*    @param ClusteredParticle handle of the cluster to break
	*    @return true if the cluster was successfully found and act upon 
	*/
	bool BreakCluster(FPBDRigidClusteredParticleHandle* ClusteredParticle);

	/*
	*  BreakClustersByProxy
	*    Breaks clusters (internal or not) own by a specific proxy by applying max external strain to all its children
	*
	*    @param Proxy proxy owning the clusters to break
	*    @return true if any cluster was successfully found and act upon
	*/
	bool BreakClustersByProxy(const IPhysicsProxyBase* Proxy);
	
	//
	// Operational 
	//

	/*
	*  AdvanceClustering
	*   Advance the cluster forward in time;
	*   ... Union unprocessed geometry.
	*   ... Release bodies based collision impulses.
	*   ... Updating properties as necessary.
	*/
	void AdvanceClustering(const FReal dt, FPBDCollisionConstraints& CollisionRule);

	/**
	*  BreakingModel
	*    Implements the promotion breaking model, where strain impulses are
	*    summed onto the cluster body, and released if greater than the
	*    encoded strain. The remainder strains are propagated back down to
	*    the children clusters.
	*/
	void BreakingModel();
	void BreakingModel(TArray<FPBDRigidClusteredParticleHandle*>& InParticles);
	
	//
	// Access
	//

	/**
	*
	*  Visitor
	*   Walk all the decendents of the current cluster and execute FVisitorFunction.
	*   FVisitorFunction = [](FRigidClustering& Clustering, FRigidHandle RigidHandle){}
	*/
	void Visitor(FClusterHandle Cluster, FVisitorFunction Function);

	/*
	*  GetActiveClusterIndex
	*    Get the current childs active cluster. Returns INDEX_NONE if
	*    not active or driven.
	*/
	FPBDRigidParticleHandle* GetActiveClusterIndex(FPBDRigidParticleHandle* Child);

	/*
	*  GetClusterIdsArray
	*    The cluster ids provide a mapping from the rigid body index
	*    to its parent cluster id. The parent id might not be the
	*    active id, see the GetActiveClusterIndex to find the active cluster.
	*    INDEX_NONE represents a non-clustered body.
	*/
	TArrayCollectionArray<ClusterId>& GetClusterIdsArray() { return MParticles.ClusterIdsArray(); }
	const TArrayCollectionArray<ClusterId>& GetClusterIdsArray() const { return MParticles.ClusterIdsArray(); }

	/*
	*  GetRigidClusteredFlagsArray
	*    The RigidClusteredFlags array contains various flag related to clustered particles
	*/
	const TArrayCollectionArray<FRigidClusteredFlags>& GetRigidClusteredFlagsArray() const { return MParticles.RigidClusteredFlags(); }

	/*
	*  GetChildToParentMap
	*    This map stores the relative transform from a child to its cluster parent.
	*/
	const TArrayCollectionArray<FRigidTransform3>& GetChildToParentMap() const { return MParticles.ChildToParentArray(); }

	/*
	*  GetStrainArray
	*    The strain array is used to store the maximum strain allowed for a individual
	*    body in the simulation. This attribute is initialized during the creation of
	*    the cluster body, can be updated during the evaluation of the simulation.
	*/
	TArrayCollectionArray<FRealSingle>& GetStrainArray() { return MParticles.StrainsArray(); }
	const TArrayCollectionArray<FRealSingle>& GetStrainArray() const { return MParticles.StrainsArray(); }
		
	/**
	*  GetParentToChildren
	*    The parent to children map stores the currently active cluster ids (Particle Indices) as
	*    the keys of the map. The value of the map is a pointer to an array  constrained
	*    rigid bodies.
	*/
	FClusterMap & GetChildrenMap() { return MChildren; }
	const FClusterMap & GetChildrenMap() const { return MChildren; }

	/*
	*  GetClusterGroupIndexArray
	*    The group index is used to automatically bind disjoint clusters. This attribute it set
	*    during the creation of cluster to a positive integer value. During UnionClusterGroups (which
	*    is called during AdvanceClustering) the positive bodies are joined with a negative pre-existing
	*    body, then set negative. Zero entries are ignored within the union.
	*/
	TArrayCollectionArray<int32>& GetClusterGroupIndexArray() { return MParticles.ClusterGroupIndexArray(); }

	/*
	* Reset all events ( this include breaking, crumbling event and tracking data 
	*/
	void ResetAllEvents();

	/*
	*  Cluster Break Data
	*     The cluster breaks can be used to seed particle emissions. 
	*/
	const TArray<FBreakingData>& GetAllClusterBreakings() const { return MAllClusterBreakings; }
	void SetGenerateClusterBreaking(bool DoGenerate) { DoGenerateBreakingData = DoGenerate; }
	bool GetDoGenerateBreakingData() const { return DoGenerateBreakingData; }
	void ResetAllClusterBreakings() { MAllClusterBreakings.Reset(); }

	/*
	*  Cluster crumbling Data
	*     triggered when all the children of a cluster are released all at once
	*     event is generated only if the owning proxy allows it
	*/
	const TArray<FCrumblingData>& GetAllClusterCrumblings() const { return MAllClusterCrumblings; }
	void ResetAllClusterCrumblings() { MAllClusterCrumblings.Reset(); }

	/*
	* GetConnectivityEdges
	*    Provides a list of each rigid body's current siblings and associated strain within the cluster.
	*/
	const TArrayCollectionArray<TArray<TConnectivityEdge<FReal>>>& GetConnectivityEdges() const { return MParticles.ConnectivityEdgesArray(); }

	/*
	*  FindClosestChild
	*    Find the closest child of an active cluster from a world position
	*    current implementation will pick the closest based on the distance from the center
	*    future implementation may expose option for more precise queries
	*    @param ClusteredParticle active cluster handle to query the children from
	*    @param WorldLocation world space location to find the closest child from   
	*/
	FPBDRigidParticleHandle* FindClosestChild(const FPBDRigidClusteredParticleHandle* ClusteredParticle, const FVec3& WorldLocation) const;

	/*
	*  FindClosest
	*    Find the closest particle from an array of  particle
	*    current implementation will pick the closest based on the distance from the center
	*    future implementation may expose option for more precise queries
	*    @param Particles array of clustered particles to select from  
	*    @param WorldLocation world space location to find the closest particle from   
	*/
	static FPBDRigidParticleHandle* FindClosestParticle(const TArray<FPBDRigidParticleHandle*>& Particles, const FVec3& WorldLocation);

	/*
	*  FindChildrenWithinRadius
	*    Find the children of an active cluster from a world position and a radius
	*    current implementation will pick the closest based on the distance from the center
	*    future implementation may expose option for more precise queries
	*    if bAlwaysReturnClosest is checked this will always return  the closest from the location even if the radius does not encompass any center of mass  
	*    @param ClusteredParticle active cluster handle to query the children from
	*    @param WorldLocation world space location to find the closest child from
	*    @param Radius Radius to use from the WorldLocation
	*    @param bAlwaysReturnClosest if radius query does not return anything still return the closest from the point
	*/
	TArray<FPBDRigidParticleHandle*> FindChildrenWithinRadius(const FPBDRigidClusteredParticleHandle* ClusteredParticle, const FVec3& WorldLocation, FReal Radius, bool bAlwaysReturnClosest) const;

	/*
	*  FindParticlesWithinRadius
	*    Find the closest particle from an array of  particle from a world position and a radius 
	*    current implementation will pick the closest based on the distance from the center
	*    future implementation may expose option for more precise queries
	*    if bAlwaysReturnClosest is checked this will always return  the closest from the location even if the radius does not encompass any center of mass
	*    @param Particles array of clustered particles to select from  
	*    @param WorldLocation world space location to find the closest particle from
	*    @param bAlwaysReturnClosest if radius query does not return anything still return the closest from the point
	*/
	static TArray<FPBDRigidParticleHandle*> FindParticlesWithinRadius(const TArray<FPBDRigidParticleHandle*>& Particles, const FVec3& WorldLocation, FReal Radius, bool bAlwaysReturnClosest);
	
	/**
	* GenerateConnectionGraph
	*   Creates a connection graph for the given index using the creation parameters. This will not
	*   clear the existing graph.
	*/
	void SetClusterConnectionFactor(FReal ClusterConnectionFactorIn) { MClusterConnectionFactor = ClusterConnectionFactorIn; }
	void SetClusterUnionConnectionType(FClusterCreationParameters::EConnectionMethod ClusterConnectionType) { MClusterUnionConnectionType = ClusterConnectionType; }
	FClusterCreationParameters::EConnectionMethod GetClusterUnionConnectionType() const { return MClusterUnionConnectionType; }

	void GenerateConnectionGraph(
		TArray<FPBDRigidParticleHandle*> Particles,
		const FClusterCreationParameters& Parameters = FClusterCreationParameters(),
		const TSet<FPBDRigidParticleHandle*>* FromParticles = nullptr,
		const TSet<FPBDRigidParticleHandle*>* ToParticles = nullptr);

	void GenerateConnectionGraph(
		Chaos::FPBDRigidClusteredParticleHandle* Parent,
		const FClusterCreationParameters & Parameters = FClusterCreationParameters());

	void ClearConnectionGraph(FPBDRigidClusteredParticleHandle* Parent);

	const TSet<Chaos::FPBDRigidClusteredParticleHandle*>& GetTopLevelClusterParents() const { return TopLevelClusterParents; }
	TSet<Chaos::FPBDRigidClusteredParticleHandle*>& GetTopLevelClusterParents() { return TopLevelClusterParents; }

	FRigidEvolution& GetEvolution() { return MEvolution; }
	const FRigidEvolution& GetEvolution() const { return MEvolution; }

	void SetInternalStrain(FPBDRigidClusteredParticleHandle* Particle, FRealSingle Strain);
	void SetExternalStrain(FPBDRigidClusteredParticleHandle* Particle, FRealSingle Strain);

	static bool ShouldUnionsHaveCollisionParticles();

	FClusterUnionManager& GetClusterUnionManager() { return ClusterUnionManager; }
	const FClusterUnionManager& GetClusterUnionManager() const { return ClusterUnionManager; }

	const TSet<Chaos::FPBDRigidClusteredParticleHandle*>& GetTopLevelClusterParentsStrained() const { return TopLevelClusterParentsStrained; }

	// Remove connectivity edges for specified particles
	void RemoveNodeConnections(FPBDRigidParticleHandle* Child);
	void RemoveNodeConnections(FPBDRigidClusteredParticleHandle* Child);

	template<typename ParticleHandleTypeA, typename ParticleHandleTypeB>
	void CreateNodeConnection(ParticleHandleTypeA* A, ParticleHandleTypeB* B)
	{
		if(A && B)
		{
			CreateNodeConnection(A->CastToClustered(), B->CastToClustered());
		}
		else
		{
			ensureMsgf(false, TEXT("CreateNodeConnection asked to connect a null particle, ignoring connection."));
		}
	}

	void CreateNodeConnection(FPBDRigidClusteredParticleHandle* A, FPBDRigidClusteredParticleHandle* B);

 protected:

	void ComputeStrainFromCollision(const FPBDCollisionConstraints& CollisionRule, const FReal Dt);
	void ResetCollisionImpulseArray();
	void DisableCluster(FPBDRigidClusteredParticleHandle* ClusteredParticle);
	void ApplyStrainModifiers();

	/*
	* Connectivity
	*/
	void UpdateConnectivityGraphUsingPointImplicit(
		const TArray<FPBDRigidParticleHandle*>& Particles,
		FReal CollisionThicknessPercent,
		const TSet<FPBDRigidParticleHandle*>* FromParticles = nullptr,
		const TSet<FPBDRigidParticleHandle*>* ToParticles = nullptr);
	void UpdateConnectivityGraphUsingPointImplicit(
		Chaos::FPBDRigidClusteredParticleHandle* Parent,
		const FClusterCreationParameters& Parameters = FClusterCreationParameters());

	void FixConnectivityGraphUsingDelaunayTriangulation(
		const TArray<FPBDRigidParticleHandle*>& Particles,
		const FClusterCreationParameters& Parameters = FClusterCreationParameters(),
		const TSet<FPBDRigidParticleHandle*>* FromParticles = nullptr,
		const TSet<FPBDRigidParticleHandle*>* ToParticles = nullptr);
	void FixConnectivityGraphUsingDelaunayTriangulation(
		Chaos::FPBDRigidClusteredParticleHandle* Parent,
		const FClusterCreationParameters& Parameters = FClusterCreationParameters());

	void UpdateConnectivityGraphUsingDelaunayTriangulation(
		const TArray<FPBDRigidParticleHandle*>& Particles,
		const FClusterCreationParameters& Parameters = FClusterCreationParameters(),
		const TSet<FPBDRigidParticleHandle*>* FromParticles = nullptr,
		const TSet<FPBDRigidParticleHandle*>* ToParticles = nullptr);
	void UpdateConnectivityGraphUsingDelaunayTriangulation(
		const Chaos::FPBDRigidClusteredParticleHandle* Parent,
		const FClusterCreationParameters& Parameters = FClusterCreationParameters());

	void UpdateConnectivityGraphUsingDelaunayTriangulationWithBoundsOverlaps(
		const TArray<FPBDRigidParticleHandle*>& Particles,
		const FClusterCreationParameters& Parameters = FClusterCreationParameters(),
		const TSet<FPBDRigidParticleHandle*>* FromParticles = nullptr,
		const TSet<FPBDRigidParticleHandle*>* ToParticles = nullptr);
	void UpdateConnectivityGraphUsingDelaunayTriangulationWithBoundsOverlaps(
		const Chaos::FPBDRigidClusteredParticleHandle* Parent,
		const FClusterCreationParameters& Parameters = FClusterCreationParameters());
	
	void RemoveChildFromParent(FPBDRigidParticleHandle* Child, const FPBDRigidClusteredParticleHandle* ClusteredParent);
	void RemoveChildFromParentAndChildrenArray(FPBDRigidParticleHandle* Child, const FPBDRigidClusteredParticleHandle* ClusteredParent);

	// When a body has broken due to contact resolution, record an entry in a set
	// for the collision and the particle who's momentum should be restored.
	void TrackBreakingCollision(FPBDRigidClusteredParticleHandle* ClusteredParticle);

	// Restore some percentage of momenta for objects which were involved in collisions
	// with destroyed GCs
	void RestoreBreakingMomentum();

	void SendBreakingEvent(FPBDRigidClusteredParticleHandle* ClusteredParticle, bool bFromCrumble);
	void SendCrumblingEvent(FPBDRigidClusteredParticleHandle* ClusteredParticle);

	TSet<FPBDRigidParticleHandle*> ReleaseClusterParticlesImpl(
		FPBDRigidClusteredParticleHandle* ClusteredParticle, 
		bool bForceRelease,
		bool bCreateNewClusters);

	/**
	 * Handles leveraging the connectivity edges on the children of the clustered particle to produce the desired effects.
	 */
	TSet<FPBDRigidParticleHandle*> HandleConnectivityOnReleaseClusterParticle(FPBDRigidClusteredParticleHandle* ClusteredParticle, bool bCreateNewClusters);
	
	using FParticleIsland = TArray<FPBDRigidParticleHandle*>;
	TArray<FParticleIsland> FindIslandsInChildren(const FPBDRigidClusteredParticleHandle* ClusteredParticle);
	TArray<FPBDRigidParticleHandle*> CreateClustersFromNewIslands(TArray<FParticleIsland>& Islands, FPBDRigidClusteredParticleHandle* ClusteredParent);

	void UpdateTopLevelParticle(FPBDRigidClusteredParticleHandle* Particle);

	/**
	 * This function is a bit more versatile than the name suggests. This function can either be used to update the cluster properties
	 * incrementally or entirely rebuild the properties all over again. This all depends on whether the input children is
	 * either 1) the new children or 2) all the children as well as what those initial properties are set to.
	 */
	void UpdateClusterParticlePropertiesFromChildren(
		FPBDRigidClusteredParticleHandle* Cluster,
		const FRigidHandleArray& Children,
		const TMap<FPBDRigidParticleHandle*, FPBDRigidParticleHandle*>& ChildToParentMap);
private:

	FRigidEvolution& MEvolution;
	FPBDRigidClusteredParticles& MParticles;
	TSet<Chaos::FPBDRigidClusteredParticleHandle*> TopLevelClusterParents;
	TSet<Chaos::FPBDRigidClusteredParticleHandle*> TopLevelClusterParentsStrained;

	// Cluster data
	FClusterMap MChildren;

	/**
	 * The old cluster union map has been replaced by the cluster union manager to allow for more
	 * dynamic behavior of adding and removing particles from a cluster instead of being just restricted
	 * to unioning particles together at construction.
	 */
	FClusterUnionManager ClusterUnionManager;

	// Collision Impulses
	bool MCollisionImpulseArrayDirty;
	
	// Breaking data
	bool DoGenerateBreakingData;
	TArray<FBreakingData> MAllClusterBreakings;

	TArray<FCrumblingData> MAllClusterCrumblings;

	TSet<FPBDRigidClusteredParticleHandle*> CrumbledSinceLastUpdate;

	// Pairs of collision constraints and rigid particle handles of particles which collided with
	// rigid clusters which broken. Some portion of the momentum change due to the constraint
	// will be restored to each of the corresponding particles.
	TSet<TPair<FPBDCollisionConstraint*, FPBDRigidParticleHandle*>> BreakingCollisions;

	FReal MClusterConnectionFactor;
	FClusterCreationParameters::EConnectionMethod MClusterUnionConnectionType;

	// Sim callback objects which implement cluster modification steps
	const TArray<ISimCallbackObject*>* StrainModifiers;
};

} // namespace Chaos
