// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AddonTools.h"

#include "SyncDatabase.h"
#include "SyncData.h"
#include "Progression.h"
#include "StatsCounter.h"

class FDatasmithDirectLink;
class FDatasmithSceneExporter;

BEGIN_NAMESPACE_UE_AC

class FElementID;
class FSynchronizer;
class FProgression;

#define UE_AC_DO_STATS 1
#if UE_AC_DO_STATS
	#define UE_AC_STAT(X) X
#else
	#define UE_AC_STAT(X) (void)0
#endif

// Class for synchronization context
class FSyncContext
{
  public:
	// Collector of synchronization statistics
	class FStats
	{
	  public:
		FStats() { ResetAll(); }
		void ResetAll();
		void Print();

		TStatsCounter< 0, 3 > BodiesStats;
		TStatsCounter< 0, 3 > PolygonsStats;
		TStatsCounter< 2, 5 > EdgesStats;
		TAtomicInt			  PolygonsCurved;
		TAtomicInt			  PolygonsComplex;
		TAtomicInt			  PolygonsConvex;
		TAtomicInt			  TotalTrianglePts;
		TAtomicInt			  TotalUVPts;
		TAtomicInt			  TotalTriangles;
		TAtomicInt			  TotalElements;
		TAtomicInt			  TotalElementsWithGeometry;
		TAtomicInt			  TotalElementsModified;
		TAtomicInt			  TotalOwnerCreated;
		TAtomicInt			  TotalActorsCreated;
		TAtomicInt			  TotalEmptyActorsCreated;
		TAtomicInt			  TotalMeshesCreated;
		TAtomicInt			  TotalMeshesReused;
		TAtomicInt			  TotalBugsCount;
	};

	// Constructor
	FSyncContext(const ModelerAPI::Model& InModel, FSyncDatabase& InSyncDatabase, FProgression* InProgression);

	// Destructor
	~FSyncContext();

	// Accessors
	const ModelerAPI::Model& GetModel() const { return Model; }
	FSyncDatabase&			 GetSyncDatabase() const { return SyncDatabase; }
	IDatasmithScene&		 GetScene() const { return *SyncDatabase.GetScene(); }
	FMaterialsDatabase&		 GetMaterialsDatabase() const { return SyncDatabase.GetMaterialsDatabase(); }
	FTexturesCache&			 GetTexturesCache() const { return SyncDatabase.GetTexturesCache(); }

	// Progression - Start the next phase
	void NewPhase(EPhaseStrId InPhaseId, int InMaxValue = 0) const;

	// Progression - Advance progression bar to the current value
	void NewCurrentValue(int InCurrentValue = -1) const;

  private:
	// AC Model, can differ from one call to another
	const ModelerAPI::Model& Model;
	FProgression*			 Progression;
	FSyncDatabase&			 SyncDatabase;

  public:
	Geometry::Point3D ModelOrigin = {0.0, 0.0, 0.0};
	double			  ScaleLength = 100;
	bool			  bUseFingerPrint = true;
	FStats&			  Stats;
};

END_NAMESPACE_UE_AC
