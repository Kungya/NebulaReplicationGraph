// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ReplicationGraph.h"
#include "NebulaReplicationGraphTypes.h"
#include "NebulaReplicationGraph.generated.h"


DECLARE_LOG_CATEGORY_EXTERN(LogNebulaRepGraph, Display, All);

/** Nebula Replication Graph implementation. See additional notes in NebulaReplicationGraph.cpp! */
UCLASS(transient, config = Engine)
class UNebulaReplicationGraph : public UReplicationGraph
{
	GENERATED_BODY()

	public:
	UNebulaReplicationGraph();

	virtual void ResetGameWorldState() override;

	virtual void InitGlobalActorClassSettings() override;
	virtual void InitGlobalGraphNodes() override;
	virtual void InitConnectionGraphNodes(UNetReplicationGraphConnection* RepGraphConnection) override;
	virtual void RouteAddNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalInfo) override;
	virtual void RouteRemoveNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo) override;

	UPROPERTY()
	TArray<TObjectPtr<UClass>>	AlwaysRelevantClasses;

	UPROPERTY()
	TObjectPtr<UReplicationGraphNode_GridSpatialization2D> GridNode;

	UPROPERTY()
	TObjectPtr<UReplicationGraphNode_ActorList> AlwaysRelevantNode;

	// 1) VisibilityCheck - line trace for relevancy

	// 2) DynamicSpatialFrequency_VisibilityCheck - Optimized line trace for relevancy

	// 3) Precomputed Visibility Grid FogOfWar - WIP
	UPROPERTY()
	TObjectPtr<UNebularReplicationGraphNode_PrecomputedVisibilityGrid2D> PVSGridNode;


	TMap<FName, FActorRepListRefView> AlwaysRelevantStreamingLevelActors;

	void PrintRepNodePolicies();

private:
	void AddClassRepInfo(UClass* Class, EClassRepNodeMapping Mapping);
	void RegisterClassRepNodeMapping(UClass* Class);
	EClassRepNodeMapping GetClassNodeMapping(UClass* Class) const;

	void RegisterClassReplicationInfo(UClass* Class);
	bool ConditionalInitClassReplicationInfo(UClass* Class, FClassReplicationInfo& ClassInfo);
	void InitClassReplicationInfo(FClassReplicationInfo& Info, UClass* Class, bool Spatialize) const;

	EClassRepNodeMapping GetMappingPolicy(UClass* Class);

	bool IsSpatialized(EClassRepNodeMapping Mapping) const { return Mapping >= EClassRepNodeMapping::Spatialize_Static; }

	TClassMap<EClassRepNodeMapping> ClassRepNodePolicies;

	/** Classes that had their replication settings explictly set by code in UNebulaReplicationGraph::InitGlobalActorClassSettings */
	TArray<UClass*> ExplicitlySetClasses;
};

UCLASS()
class UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection : public UReplicationGraphNode_AlwaysRelevant_ForConnection
{
	GENERATED_BODY()

	public:
	virtual void NotifyAddNetworkActor(const FNewReplicatedActorInfo& Actor) override {}
	virtual bool NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override { return false; }
	virtual void NotifyResetAllNetworkActors() override {}

	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;

	virtual void LogNode(FReplicationGraphDebugInfo& DebugInfo, const FString& NodeName) const override;

	void OnClientLevelVisibilityAdd(FName LevelName, UWorld* StreamingWorld);
	void OnClientLevelVisibilityRemove(FName LevelName);

	void ResetGameWorldState();

private:
	TArray<FName, TInlineAllocator<64> > AlwaysRelevantStreamingLevelsNeedingReplication;

	bool bInitializedPlayerState = false;
};

/**
	This is a specialized node for handling PlayerState replication in a frequency limited fashion. It tracks all player states but only returns a subset of them to the replication driver each frame.
	This is an optimization for large player connection counts, and not a requirement.
*/
UCLASS()
class UNebulaReplicationGraphNode_PlayerStateFrequencyLimiter : public UReplicationGraphNode
{
	GENERATED_BODY()

	UNebulaReplicationGraphNode_PlayerStateFrequencyLimiter();

	virtual void NotifyAddNetworkActor(const FNewReplicatedActorInfo& Actor) override {}
	virtual bool NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override { return false; }
	virtual bool NotifyActorRenamed(const FRenamedReplicatedActorInfo& Actor, bool bWarnIfNotFound = true) override { return false; }

	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;

	virtual void PrepareForReplication() override;

	virtual void LogNode(FReplicationGraphDebugInfo& DebugInfo, const FString& NodeName) const override;

	/** How many actors we want to return to the replication driver per frame. Will not suppress ForceNetUpdate. */
	int32 TargetActorsPerFrame = 2;

private:

	TArray<FActorRepListRefView> ReplicationActorLists;
	FActorRepListRefView ForceNetUpdateReplicationActorList;
};

/**
* Precomputed Visibility GridCells (like Valorant FogOfWar) based on GridSpatialization2D, 
* but this works a little differently.
*/ 
UCLASS()
class UNebularReplicationGraphNode_PrecomputedVisibilityGrid2D : public UReplicationGraphNode
{
	GENERATED_BODY()
	
public:
	
	UNebularReplicationGraphNode_PrecomputedVisibilityGrid2D();

	// Pure Virtual~, but not used
	virtual void NotifyAddNetworkActor(const FNewReplicatedActorInfo& Actor) override;
	virtual bool NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override;
	virtual bool NotifyActorRenamed(const FRenamedReplicatedActorInfo& Actor, bool bWarnIfNotFound = true) override;
	 // ~Pure VIrtual, but not used

	//void AddActor_Static(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo) { AddActorInternal_Static(ActorInfo, ActorRepInfo, false); }
	void AddActor_Dynamic(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo) { AddActorInternal_Dynamic(ActorInfo); }
	//void AddActor_Dormancy(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo);

	//void RemoveActor_Static(const FNewReplicatedActorInfo& ActorInfo);
	void RemoveActor_Dynamic(const FNewReplicatedActorInfo& ActorInfo) { RemoveActorInternal_Dynamic(ActorInfo); }
	//void RemoveActor_Dormancy(const FNewReplicatedActorInfo& ActorInfo);

	// trace dynmaic actors' location and cell index per frame.
	virtual void PrepareForReplication() override;
	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;

	// same purpose with GridSpatialization2D, to map negative index to positive.
	// @See UNebulaReplicationGraphSettings
	float		CellSize;
	FVector2D	SpatialBias;
	
	void GenerateLookupTable();

protected:

	//
	void AddActorInternal_Dynamic(const FNewReplicatedActorInfo& ActorInfo);
	//

	//
	void RemoveActorInternal_Dynamic(const FNewReplicatedActorInfo& ActorInfo);
	//

private:

	// ----------------------
	// |   Dynamic Actors   |
	// ----------------------
	struct FActorCellInfo
	{
		bool IsValid() const { return CellIndex.X != -1; }
		void Reset() { CellIndex.X = -1; }
		FIntPoint CellIndex = {-1, -1};
	};

	struct FCachedDynamicActorInfo
	{
		FCachedDynamicActorInfo(const FNewReplicatedActorInfo& InInfo) : ActorInfo(InInfo) {}
		FNewReplicatedActorInfo ActorInfo;
		FActorCellInfo CellInfo;
	};


	TMap<FActorRepListType, FCachedDynamicActorInfo> DynamicSpatializedActors;
	// $TODO : Static Actors
	// $TODO : Dormant Actors

	UReplicationGraphNode_GridCell* GetCellNode(UReplicationGraphNode_GridCell*& NodePtr)
	{
		if (NodePtr == nullptr)
		{
			NodePtr = CreateChildNode<UReplicationGraphNode_GridCell>();
		}

		return NodePtr;
	}

	TArray<TArray<UReplicationGraphNode_GridCell*>> Grid;

	TArray<UReplicationGraphNode_GridCell*>& GetGridX(int32 X)
	{
		if (Grid.Num() <= X)
		{
			Grid.SetNum(X + 1);
		}
		return Grid[X];
	}

	UReplicationGraphNode_GridCell*& GetCell(TArray<UReplicationGraphNode_GridCell*>& GridX, int32 Y)
	{
		if (GridX.Num() <= Y)
		{
			GridX.SetNum(Y + 1);
		}
		return GridX[Y];
	}

	UReplicationGraphNode_GridCell*& GetCell(int32 X, int32 Y)
	{
		TArray<UReplicationGraphNode_GridCell*>& GridX = GetGridX(X);
		return GetCell(GridX, Y);
	}

	// $TODO : compress each coordinates, for memory footprint - FIntPoint is 4bytes
	TMap<FIntPoint, TArray<FIntPoint>> PVSLookupTable;
};