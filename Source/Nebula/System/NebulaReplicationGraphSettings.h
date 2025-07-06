// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "Engine/DeveloperSettingsBackedByCVars.h"
#include "NebulaReplicationGraphTypes.h"
#include "NebulaReplicationGraphSettings.generated.h"

#define bUseFastPath 0

/**
 * Default settings for the Nebula replication graph
 */
UCLASS(config = Game, MinimalAPI)
class UNebulaReplicationGraphSettings : public UDeveloperSettingsBackedByCVars
{
	GENERATED_BODY()
	
public:
	UNebulaReplicationGraphSettings();

public:

	UPROPERTY(config, EditAnywhere, Category = ReplicationGraph)
	bool bDisableReplicationGraph = true;

	UPROPERTY(config, EditAnywhere, Category = ReplicationGraph, meta = (MetaClass = "/Script/Nebula.NebulaReplicationGraph"))
	FSoftClassPath DefaultReplicationGraphClass;

#if bUseFastPath // TODO : FastShared Settings
//	UPROPERTY(EditAnywhere, Category = FastSharedPath, meta = (ConsoleVariable = "Nebula.RepGraph.EnableFastSharedPath"))
	bool bEnableFastSharedPath = true;

	// How much bandwidth to use for FastShared movement updates. This is counted independently of the NetDriver's target bandwidth.
//	UPROPERTY(EditAnywhere, Category = FastSharedPath, meta = (ForceUnits = Kilobytes, ConsoleVariable = "Nebula.RepGraph.TargetKBytesSecFastSharedPath"))
	int32 TargetKBytesSecFastSharedPath = 10;

//	UPROPERTY(EditAnywhere, Category = FastSharedPath, meta = (ConsoleVariable = "Nebula.RepGraph.FastSharedPathCullDistPct"))
	float FastSharedPathCullDistPct = 0.80f;
#endif
	UPROPERTY(EditAnywhere, Category = DestructionInfo, meta = (ForceUnits = cm, ConsoleVariable = "Nebula.RepGraph.DestructInfo.MaxDist"))
	float DestructionInfoMaxDist = 30000.f;

	// CellSize(length of square) for "PrecomputedVisibilityGrid2D Node".
	UPROPERTY(EditAnywhere, Category = SpatialGrid, meta = (ForceUnits = cm, ConsoleVariable = "Nebula.RepGraph.PVSCellSize"))
	float PVSSCellSize = 200.0f;

	// Essentially "Min X" for "PrecomputedVisibilityGrid2D Node". This is just an initial value. The system will reset itself if actors appears outside of this.
	UPROPERTY(EditAnywhere, Category = SpatialGrid, meta = (ForceUnits = cm, ConsoleVariable = "Nebula.RepGraph.PVSSpatialBiasX"))
	float PVSSpatialBiasX = -600.f;

	// Essentially "Min Y" for"PrecomputedVisibilityGrid2D Node". This is just an initial value. The system will reset itself if actors appears outside of this.
	UPROPERTY(EditAnywhere, Category = SpatialGrid, meta = (ForceUnits = cm, ConsoleVariable = "Nebula.RepGraph.PVSSpatialBiasY"))
	float PVSSpatialBiasY = -600.f;

	UPROPERTY(EditAnywhere, Category = SpatialGrid, meta = (ConsoleVariable = "Nebula.RepGraph.DisableSpatialRebuilds"))
	bool bDisableSpatialRebuilds = true;

	// How many buckets to spread dynamic, spatialized actors across.
	// High number = more buckets = smaller effective replication frequency.
	// This happens before individual actors do their own NetUpdateFrequency check.
	UPROPERTY(EditAnywhere, Category = DynamicSpatialFrequency, meta = (ConsoleVariable = "Nebula.RepGraph.DynamicActorFrequencyBuckets"))
	int32 DynamicActorFrequencyBuckets = 3;

	// Array of Custom Settings for Specific Classes 
	UPROPERTY(config, EditAnywhere, Category = ReplicationGraph)
	TArray<FRepGraphActorClassSettings> ClassSettings;
};
