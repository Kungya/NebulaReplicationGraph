// Copyright Epic Games, Inc. All Rights Reserved.

/**
*
*	===================== NebulaReplicationGraph Replication =====================
*
*	Overview
*
*		This changes the way actor relevancy works. AActor::IsNetRelevantFor is NOT used in this system!
*
*		Instead, The UNebulaReplicationGraph contains UReplicationGraphNodes. These nodes are responsible for generating lists of actors to replicate for each connection.
*		Most of these lists are persistent across frames. This enables most of the gathering work ("which actors should be considered for replication) to be shared/reused.
*		Nodes may be global (used by all connections), connection specific (each connection gets its own node), or shared (e.g, teams: all connections on the same team share).
*		Actors can be in multiple nodes! For example a pawn may be in the spatialization node but also in the always-relevant-for-team node. It will be returned twice for
*		teammates. This is ok though should be minimized when possible.
*
*		UNebulaReplicationGraph is intended to not be directly used by the game code. That is, you should not have to include NebulaReplicationGraph.h anywhere else.
*		Rather, UNebulaReplicationGraph depends on the game code and registers for events that the game code broadcasts (e.g., events for players joining/leaving teams).
*		This choice was made because it gives UNebulaReplicationGraph a complete holistic view of actor replication. Rather than exposing generic public functions that any
*		place in game code can invoke, all notifications are explicitly registered in UNebulaReplicationGraph::InitGlobalActorClassSettings.
*
*	Nebula Nodes
*
*		These are the top level nodes currently used:
*
*		UReplicationGraphNode_GridSpatialization2D:
*		This is the spatialization node. All "distance based relevant" actors will be routed here. This node divides the map into a 2D grid. Each cell in the grid contains
*		children nodes that hold lists of actors based on how they update/go dormant. Actors are put in multiple cells. Connections pull from the single cell they are in.
*
*		UReplicationGraphNode_ActorList
*		This is an actor list node that contains the always relevant actors. These actors are always relevant to every connection.
*
*		UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection
*		This is the node for connection specific always relevant actors. This node does not maintain a persistent list but builds it each frame. This is possible because (currently)
*		these actors are all easily accessed from the PlayerController. A persistent list would require notifications to be broadcast when these actors change, which would be possible
*		but currently not necessary.
*
*		UNebulaReplicationGraphNode_PlayerStateFrequencyLimiter
*		A custom node for handling player state replication. This replicates a small rolling set of player states (currently 2/frame). This is so player states replicate
*		to simulated connections at a low, steady frequency, and to take advantage of serialization sharing. Auto proxy player states are replicated at higher frequency (to the
*		owning connection only) via UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection.
*
*		UReplicationGraphNode_TearOff_ForConnection
*		Connection specific node for handling tear off actors. This is created and managed in the base implementation of Replication Graph.
*
*		Added Soon
*		UNebulaReplicationGraphNode_VisibilityCheck_ForConnection
* 
*		Added Soon
*		UNebulaReplicationGraphNode_DynamicSpatialFrequency_VisibilityCheck
* 
*		WIP
*		UReplicationGraphNode_PrecomputedVisibilityGrid2D
*		
* 
*	How To Use
*
*		Making something always relevant: Please avoid if you can :) If you must, just setting AActor::bAlwaysRelevant = true in the class defaults will do it.
*
*		Making something always relevant to connection: You will need to modify UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection::GatherActorListsForConnection. You will also want
*		to make sure the actor does not get put in one of the other nodes. The safest way to do this is by setting its EClassRepNodeMapping to NotRouted in UNebulaReplicationGraph::InitGlobalActorClassSettings.
*
*	How To Debug
*
*		Its a good idea to just disable rep graph to see if your problem is specific to this system or just general replication/game play problem.
*
*		If it is replication graph related, there are several useful commands that can be used: see ReplicationGraph_Debugging.cpp. The most useful are below. Use the 'cheat' command to run these on the server from a client.
*
*		"Net.RepGraph.PrintGraph" - this will print the graph to the log: each node and actor.
*		"Net.RepGraph.PrintGraph class" - same as above but will group by class.
*		"Net.RepGraph.PrintGraph nclass" - same as above but will group by native classes (hides blueprint noise)
*
*		Net.RepGraph.PrintAll <Frames> <ConnectionIdx> <"Class"/"Nclass"> -  will print the entire graph, the gathered actors, and how they were prioritized for a given connection for X amount of frames.
*
*		Net.RepGraph.PrintAllActorInfo <ActorMatchString> - will print the class, global, and connection replication info associated with an actor/class. If MatchString is empty will print everything. Call directly from client.
*
*		Nebula.RepGraph.PrintRouting - will print the EClassRepNodeMapping for each class. That is, how a given actor class is routed (or not) in the Replication Graph.
*
*/

#include "NebulaReplicationGraph.h"

#include "Net/UnrealNetwork.h"
#include "Engine/LevelStreaming.h"
#include "EngineUtils.h"
#include "CoreGlobals.h"

#if 0//WITH_GAMEPLAY_DEBUGGER
#include "GameplayDebuggerCategoryReplicator.h"
#endif

#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameState.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/Pawn.h"
#include "Engine/LevelScriptActor.h"
#include "Engine/NetConnection.h"
#include "UObject/UObjectIterator.h"

#include "NebulaReplicationGraphSettings.h"
#include "Nebula/NebulaCharacter.h"
#include "Nebula/NebulaPlayerController.h"
#include "Nebula/NebularGameState.h"

DEFINE_LOG_CATEGORY(LogNebulaRepGraph);

#define bUseFastPath 0

using namespace UE::Net::Private;

namespace UE::PreVisGrid2D::Private
{
	// Maintain pre-large world bounds for now, since the GridSpatialization2D node
	// uses a densely-stored grid and risks very high memory usage at large coordinates.
	constexpr double RepGraphWorldMax = UE_OLD_WORLD_MAX; // TODO : 
	constexpr double RepGraphHalfWorldMax = UE_OLD_HALF_WORLD_MAX; // TODO : 
}

namespace Nebula::RepGraph
{
	float DestructionInfoMaxDist = 30000.f;
	static FAutoConsoleVariableRef CVarNebulaRepGraphDestructMaxDist(TEXT("Nebula.RepGraph.DestructInfo.MaxDist"), DestructionInfoMaxDist, TEXT("Max distance (not squared) to rep destruct infos at"), ECVF_Default);

	int32 DisplayClientLevelStreaming = 0;
	static FAutoConsoleVariableRef CVarNebulaRepGraphDisplayClientLevelStreaming(TEXT("Nebula.RepGraph.DisplayClientLevelStreaming"), DisplayClientLevelStreaming, TEXT(""), ECVF_Default);

	float CellSize = 10000.f;
	static FAutoConsoleVariableRef CVarNebulaRepGraphCellSize(TEXT("Nebula.RepGraph.CellSize"), CellSize, TEXT(""), ECVF_Default);

	// Essentially "Min X" for replication. This is just an initial value. The system will reset itself if actors appears outside of this.
	float SpatialBiasX = -150000.f;
	static FAutoConsoleVariableRef CVarNebulaRepGraphSpatialBiasX(TEXT("Nebula.RepGraph.SpatialBiasX"), SpatialBiasX, TEXT(""), ECVF_Default);

	// Essentially "Min Y" for replication. This is just an initial value. The system will reset itself if actors appears outside of this.
	float SpatialBiasY = -200000.f;
	static FAutoConsoleVariableRef CVarNebulaRepSpatialBiasY(TEXT("Nebula.RepGraph.SpatialBiasY"), SpatialBiasY, TEXT(""), ECVF_Default);

	// How many buckets to spread dynamic, spatialized actors across. High number = more buckets = smaller effective replication frequency. This happens before individual actors do their own NetUpdateFrequency check.
	int32 DynamicActorFrequencyBuckets = 3;
	static FAutoConsoleVariableRef CVarNebulaRepDynamicActorFrequencyBuckets(TEXT("Nebula.RepGraph.DynamicActorFrequencyBuckets"), DynamicActorFrequencyBuckets, TEXT(""), ECVF_Default);

	int32 DisableSpatialRebuilds = 1;
	static FAutoConsoleVariableRef CVarNebulaRepDisableSpatialRebuilds(TEXT("Nebula.RepGraph.DisableSpatialRebuilds"), DisableSpatialRebuilds, TEXT(""), ECVF_Default);

	int32 LogLazyInitClasses = 0;
	static FAutoConsoleVariableRef CVarNebulaRepLogLazyInitClasses(TEXT("Nebula.RepGraph.LogLazyInitClasses"), LogLazyInitClasses, TEXT(""), ECVF_Default);

	// How much bandwidth to use for FastShared movement updates. This is counted independently of the NetDriver's target bandwidth.
	int32 TargetKBytesSecFastSharedPath = 10;
	static FAutoConsoleVariableRef CVarNebulaRepTargetKBytesSecFastSharedPath(TEXT("Nebula.RepGraph.TargetKBytesSecFastSharedPath"), TargetKBytesSecFastSharedPath, TEXT(""), ECVF_Default);

	float FastSharedPathCullDistPct = 0.80f;
	static FAutoConsoleVariableRef CVarNebulaRepFastSharedPathCullDistPct(TEXT("Nebula.RepGraph.FastSharedPathCullDistPct"), FastSharedPathCullDistPct, TEXT(""), ECVF_Default);

	int32 EnableFastSharedPath = 1;
	static FAutoConsoleVariableRef CVarNebulaRepEnableFastSharedPath(TEXT("Nebula.RepGraph.EnableFastSharedPath"), EnableFastSharedPath, TEXT(""), ECVF_Default);

	UReplicationDriver* ConditionalCreateReplicationDriver(UNetDriver* ForNetDriver, UWorld* World)
	{
		// Only create for GameNetDriver
		if (World && ForNetDriver && ForNetDriver->NetDriverName == NAME_GameNetDriver)
		{
			const UNebulaReplicationGraphSettings* NebulaRepGraphSettings = GetDefault<UNebulaReplicationGraphSettings>();

			// Enable/Disable via developer settings
			if (NebulaRepGraphSettings && NebulaRepGraphSettings->bDisableReplicationGraph)
			{
				UE_LOG(LogNebulaRepGraph, Display, TEXT("Replication graph is disabled via NebulaReplicationGraphSettings."));
				return nullptr;
			}

			UE_LOG(LogNebulaRepGraph, Display, TEXT("Replication graph is enabled for %s in world %s."), *GetNameSafe(ForNetDriver), *GetPathNameSafe(World));

			TSubclassOf<UNebulaReplicationGraph> GraphClass = NebulaRepGraphSettings->DefaultReplicationGraphClass.TryLoadClass<UNebulaReplicationGraph>();
			if (GraphClass.Get() == nullptr)
			{
				GraphClass = UNebulaReplicationGraph::StaticClass();
			}

			UNebulaReplicationGraph* NebulaReplicationGraph = NewObject<UNebulaReplicationGraph>(GetTransientPackage(), GraphClass.Get());
			return NebulaReplicationGraph;
		}

		return nullptr;
	}
};

// ------------------------------------------------------------------------------------------------------------------------------------------------
// Codes From ReplicationGraph.cpp, because below functions are non-member function. For using Custom DynamicSpatialFrequency GraphNode in future. 
// ------------------------------------------------------------------------------------------------------------------------------------------------
REPGRAPH_DEVCVAR_SHIPCONST(int32, "Net.RepGraph.EnableRPCSendPolicy", CVar_RepGraph_EnableRPCSendPolicy, 1, "Enables RPC send policy (e.g, force certain functions to send immediately rather than be queued)");

static TAutoConsoleVariable<FString> CVarRepGraphConditionalBreakpointActorName(TEXT("Net.RepGraph.ConditionalBreakpointActorName"), TEXT(""),
	TEXT("Helper CVar for debugging. Set this string to conditionally log/breakpoint various points in the repgraph pipeline. Useful for bugs like 'why is this actor channel closing'"), ECVF_Default);

FActorConnectionPair DebugActorConnectionPair;

FORCEINLINE bool RepGraphConditionalActorBreakpoint(AActor* Actor, UNetConnection* NetConnection)
{
#if !(UE_BUILD_SHIPPING)
	if (CVarRepGraphConditionalBreakpointActorName.GetValueOnGameThread().Len() > 0 && GetNameSafe(Actor).Contains(CVarRepGraphConditionalBreakpointActorName.GetValueOnGameThread()))
	{
		return true;
	}

	// Alternatively, DebugActorConnectionPair can be set by code to catch a specific actor/connection pair 
	if (DebugActorConnectionPair.Actor.Get() == Actor && (DebugActorConnectionPair.Connection == nullptr || DebugActorConnectionPair.Connection == NetConnection))
	{
		return true;
	}
#endif
	return false;
}

FORCEINLINE bool ReadyForNextReplication(FConnectionReplicationActorInfo& ConnectionData, FGlobalActorReplicationInfo& GlobalData, const uint32 FrameNum)
{
	return (ConnectionData.NextReplicationFrameNum <= FrameNum || GlobalData.ForceNetUpdateFrame > ConnectionData.LastRepFrameNum);
}

FORCEINLINE bool ReadyForNextReplication_FastPath(FConnectionReplicationActorInfo& ConnectionData, FGlobalActorReplicationInfo& GlobalData, const uint32 FrameNum)
{
	return (ConnectionData.FastPath_NextReplicationFrameNum <= FrameNum || GlobalData.ForceNetUpdateFrame > ConnectionData.FastPath_LastRepFrameNum);
}
// ----------------------------------------------------------------------------------------------------------




// ----------------------------------------------------------------------------------------------------------

UNebulaReplicationGraph::UNebulaReplicationGraph()
{
	if (!UReplicationDriver::CreateReplicationDriverDelegate().IsBound())
	{
		UReplicationDriver::CreateReplicationDriverDelegate().BindLambda(
			[](UNetDriver* ForNetDriver, const FURL& URL, UWorld* World) -> UReplicationDriver*
			{
				return Nebula::RepGraph::ConditionalCreateReplicationDriver(ForNetDriver, World);
			});
	}
}

void UNebulaReplicationGraph::ResetGameWorldState()
{
	Super::ResetGameWorldState();

	AlwaysRelevantStreamingLevelActors.Empty();

	for (UNetReplicationGraphConnection* ConnManager : Connections)
	{
		for (UReplicationGraphNode* ConnectionNode : ConnManager->GetConnectionGraphNodes())
		{
			if (UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection* AlwaysRelevantConnectionNode = Cast<UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection>(ConnectionNode))
			{
				AlwaysRelevantConnectionNode->ResetGameWorldState();
			}
		}
	}

	for (UNetReplicationGraphConnection* ConnManager : PendingConnections)
	{
		for (UReplicationGraphNode* ConnectionNode : ConnManager->GetConnectionGraphNodes())
		{
			if (UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection* AlwaysRelevantConnectionNode = Cast<UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection>(ConnectionNode))
			{
				AlwaysRelevantConnectionNode->ResetGameWorldState();
			}
		}
	}
}

EClassRepNodeMapping UNebulaReplicationGraph::GetClassNodeMapping(UClass* Class) const
{
	if (!Class)
	{
		return EClassRepNodeMapping::NotRouted;
	}

	if (const EClassRepNodeMapping* Ptr = ClassRepNodePolicies.FindWithoutClassRecursion(Class))
	{
		return *Ptr;
	}

	if (Class->IsChildOf(ANebulaCharacter::StaticClass()))
	{
		return EClassRepNodeMapping::PrecomputedVisibility;
	}

	AActor* ActorCDO = Cast<AActor>(Class->GetDefaultObject());
	if (!ActorCDO || !ActorCDO->GetIsReplicated())
	{
		return EClassRepNodeMapping::NotRouted;
	}

	auto ShouldSpatialize = [](const AActor* CDO)
		{
			return CDO->GetIsReplicated() && (!(CDO->bAlwaysRelevant || CDO->bOnlyRelevantToOwner || CDO->bNetUseOwnerRelevancy));
		};

	auto GetLegacyDebugStr = [](const AActor* CDO)
		{
			return FString::Printf(TEXT("%s [%d/%d/%d]"), *CDO->GetClass()->GetName(), CDO->bAlwaysRelevant, CDO->bOnlyRelevantToOwner, CDO->bNetUseOwnerRelevancy);
		};

	// Only handle this class if it differs from its super. There is no need to put every child class explicitly in the graph class mapping
	UClass* SuperClass = Class->GetSuperClass();
	if (AActor* SuperCDO = Cast<AActor>(SuperClass->GetDefaultObject()))
	{
		if (SuperCDO->GetIsReplicated() == ActorCDO->GetIsReplicated()
			&& SuperCDO->bAlwaysRelevant == ActorCDO->bAlwaysRelevant
			&& SuperCDO->bOnlyRelevantToOwner == ActorCDO->bOnlyRelevantToOwner
			&& SuperCDO->bNetUseOwnerRelevancy == ActorCDO->bNetUseOwnerRelevancy
			)
		{
			return GetClassNodeMapping(SuperClass);
		}
	}

	if (ShouldSpatialize(ActorCDO))
	{
		return EClassRepNodeMapping::Spatialize_Dynamic;
	}
	else if (ActorCDO->bAlwaysRelevant && !ActorCDO->bOnlyRelevantToOwner)
	{
		return EClassRepNodeMapping::RelevantAllConnections;
	}

	return EClassRepNodeMapping::NotRouted;
}

void UNebulaReplicationGraph::RegisterClassRepNodeMapping(UClass* Class)
{
	EClassRepNodeMapping Mapping = GetClassNodeMapping(Class);
	ClassRepNodePolicies.Set(Class, Mapping);
}

void UNebulaReplicationGraph::InitClassReplicationInfo(FClassReplicationInfo& Info, UClass* Class, bool Spatialize) const
{
	AActor* CDO = Class->GetDefaultObject<AActor>();
	if (Spatialize)
	{
		Info.SetCullDistanceSquared(CDO->GetNetCullDistanceSquared());
		UE_LOG(LogNebulaRepGraph, Log, TEXT("Setting cull distance for %s to %f (%f)"), *Class->GetName(), Info.GetCullDistanceSquared(), Info.GetCullDistance());
	}

	Info.ReplicationPeriodFrame = GetReplicationPeriodFrameForFrequency(CDO->GetNetUpdateFrequency());

	UClass* NativeClass = Class;
	while (!NativeClass->IsNative() && NativeClass->GetSuperClass() && NativeClass->GetSuperClass() != AActor::StaticClass())
	{
		NativeClass = NativeClass->GetSuperClass();
	}

	UE_LOG(LogNebulaRepGraph, Log, TEXT("Setting replication period for %s (%s) to %d frames (%.2f)"), *Class->GetName(), *NativeClass->GetName(), Info.ReplicationPeriodFrame, CDO->GetNetUpdateFrequency());
}

bool UNebulaReplicationGraph::ConditionalInitClassReplicationInfo(UClass* ReplicatedClass, FClassReplicationInfo& ClassInfo)
{
	if (ExplicitlySetClasses.FindByPredicate([&](const UClass* SetClass) { return ReplicatedClass->IsChildOf(SetClass); }) != nullptr)
	{
		return false;
	}

	bool ClassIsSpatialized = IsSpatialized(ClassRepNodePolicies.GetChecked(ReplicatedClass));
	InitClassReplicationInfo(ClassInfo, ReplicatedClass, ClassIsSpatialized);
	return true;
}

void UNebulaReplicationGraph::AddClassRepInfo(UClass* Class, EClassRepNodeMapping Mapping)
{
	if (IsSpatialized(Mapping))
	{
		if (Class->GetDefaultObject<AActor>()->bAlwaysRelevant)
		{
			UE_LOG(LogNebulaRepGraph, Warning, TEXT("Replicated Class %s is AlwaysRelevant but is initialized into a spatialized node (%s)"), *Class->GetName(), *StaticEnum<EClassRepNodeMapping>()->GetNameStringByValue((int64)Mapping));
		}
	}

	ClassRepNodePolicies.Set(Class, Mapping);
}

void UNebulaReplicationGraph::RegisterClassReplicationInfo(UClass* ReplicatedClass)
{
	FClassReplicationInfo ClassInfo;
	if (ConditionalInitClassReplicationInfo(ReplicatedClass, ClassInfo))
	{
		GlobalActorReplicationInfoMap.SetClassInfo(ReplicatedClass, ClassInfo);
		UE_LOG(LogNebulaRepGraph, Log, TEXT("Setting %s - %.2f"), *GetNameSafe(ReplicatedClass), ClassInfo.GetCullDistance());
	}
}

void UNebulaReplicationGraph::InitGlobalActorClassSettings()
{
	UE_LOG(LogNebulaRepGraph, Warning, TEXT("UNebulaReplicationGraph::InitGlobalActorClassSettings()"));
	// Setup our lazy init function for classes that are not currently loaded.
	GlobalActorReplicationInfoMap.SetInitClassInfoFunc(
		[this](UClass* Class, FClassReplicationInfo& ClassInfo)
		{
			RegisterClassRepNodeMapping(Class); // This needs to run before RegisterClassReplicationInfo.

			const bool bHandled = ConditionalInitClassReplicationInfo(Class, ClassInfo);

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			if (Nebula::RepGraph::LogLazyInitClasses != 0)
			{
				if (bHandled)
				{
					EClassRepNodeMapping Mapping = ClassRepNodePolicies.GetChecked(Class);
					UE_LOG(LogNebulaRepGraph, Warning, TEXT("%s was Lazy Initialized. (Parent: %s) %d."), *GetNameSafe(Class), *GetNameSafe(Class->GetSuperClass()), (int32)Mapping);

					FClassReplicationInfo& ParentRepInfo = GlobalActorReplicationInfoMap.GetClassInfo(Class->GetSuperClass());
					if (ClassInfo.BuildDebugStringDelta() != ParentRepInfo.BuildDebugStringDelta())
					{
						UE_LOG(LogNebulaRepGraph, Warning, TEXT("Differences Found!"));
						FString DebugStr = ParentRepInfo.BuildDebugStringDelta();
						UE_LOG(LogNebulaRepGraph, Warning, TEXT("  Parent: %s"), *DebugStr);

						DebugStr = ClassInfo.BuildDebugStringDelta();
						UE_LOG(LogNebulaRepGraph, Warning, TEXT("  Class : %s"), *DebugStr);
					}
				}
				else
				{
					UE_LOG(LogNebulaRepGraph, Warning, TEXT("%s skipped Lazy Initialization because it does not differ from its parent. (Parent: %s)"), *GetNameSafe(Class), *GetNameSafe(Class->GetSuperClass()));

				}
			}
#endif

			return bHandled;
		});

	ClassRepNodePolicies.InitNewElement = [this](UClass* Class, EClassRepNodeMapping& NodeMapping)
		{
			NodeMapping = GetClassNodeMapping(Class);
			return true;
		};

	const UNebulaReplicationGraphSettings* NebulaRepGraphSettings = GetDefault<UNebulaReplicationGraphSettings>();
	check(NebulaRepGraphSettings);

	// Set Classes Node Mappings
	for (const FRepGraphActorClassSettings& ActorClassSettings : NebulaRepGraphSettings->ClassSettings)
	{
		if (ActorClassSettings.bAddClassRepInfoToMap)
		{
			if (UClass* StaticActorClass = ActorClassSettings.GetStaticActorClass())
			{
				UE_LOG(LogNebulaRepGraph, Log, TEXT("ActorClassSettings -- AddClassRepInfo - %s :: %i"), *StaticActorClass->GetName(), int(ActorClassSettings.ClassNodeMapping));
				AddClassRepInfo(StaticActorClass, ActorClassSettings.ClassNodeMapping);
			}
		}
	}

	TArray<UClass*> AllReplicatedClasses;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		AActor* ActorCDO = Cast<AActor>(Class->GetDefaultObject());
		if (!ActorCDO || !ActorCDO->GetIsReplicated())
		{
			continue;
		}

		// Skip SKEL and REINST classes. I don't know a better way to do this.
		if (Class->GetName().StartsWith(TEXT("SKEL_")) || Class->GetName().StartsWith(TEXT("REINST_")))
		{
			continue;
		}

		// --------------------------------------------------------------------
		// This is a replicated class. Save this off for the second pass below
		// --------------------------------------------------------------------

		AllReplicatedClasses.Add(Class);

		RegisterClassRepNodeMapping(Class);
	}

	// -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
	// Setup FClassReplicationInfo. This is essentially the per class replication settings. Some we set explicitly, the rest we are setting via looking at the legacy settings on AActor.
	// -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

	auto SetClassInfo = [&](UClass* Class, const FClassReplicationInfo& Info) { GlobalActorReplicationInfoMap.SetClassInfo(Class, Info); ExplicitlySetClasses.Add(Class); };
	ExplicitlySetClasses.Reset();

	FClassReplicationInfo CharacterClassRepInfo;
	CharacterClassRepInfo.DistancePriorityScale = 1.f;
	CharacterClassRepInfo.StarvationPriorityScale = 1.f;
	CharacterClassRepInfo.ActorChannelFrameTimeout = 4;
	CharacterClassRepInfo.SetCullDistanceSquared(ANebulaCharacter::StaticClass()->GetDefaultObject<ANebulaCharacter>()->GetNetCullDistanceSquared());

	SetClassInfo(ANebulaCharacter::StaticClass(), CharacterClassRepInfo);

#if bUseFastPath
	{
		// Sanity check our FSharedRepMovement type has the same quantization settings as the default character.
		FRepMovement DefaultRepMovement = ANebulaCharacter::StaticClass()->GetDefaultObject<ANebulaCharacter>()->GetReplicatedMovement(); // Use the same quantization settings as our default replicatedmovement
		FSharedRepMovement SharedRepMovement;
		ensureMsgf(SharedRepMovement.RepMovement.LocationQuantizationLevel == DefaultRepMovement.LocationQuantizationLevel, TEXT("LocationQuantizationLevel mismatch. %d != %d"), (uint8)SharedRepMovement.RepMovement.LocationQuantizationLevel, (uint8)DefaultRepMovement.LocationQuantizationLevel);
		ensureMsgf(SharedRepMovement.RepMovement.VelocityQuantizationLevel == DefaultRepMovement.VelocityQuantizationLevel, TEXT("VelocityQuantizationLevel mismatch. %d != %d"), (uint8)SharedRepMovement.RepMovement.VelocityQuantizationLevel, (uint8)DefaultRepMovement.VelocityQuantizationLevel);
		ensureMsgf(SharedRepMovement.RepMovement.RotationQuantizationLevel == DefaultRepMovement.RotationQuantizationLevel, TEXT("RotationQuantizationLevel mismatch. %d != %d"), (uint8)SharedRepMovement.RepMovement.RotationQuantizationLevel, (uint8)DefaultRepMovement.RotationQuantizationLevel);
	}


	// ------------------------------------------------------------------------------------------------------
	//	Setup FastShared replication for pawns. This is called up to once per frame per pawn to see if it wants
	//	to send a FastShared update to all relevant connections.
	// ------------------------------------------------------------------------------------------------------
	CharacterClassRepInfo.FastSharedReplicationFunc = [](AActor* Actor)
		{
			bool bSuccess = false;
			if (ANebulaCharacter* Character = Cast<ANebulaCharacter>(Actor))
			{
				bSuccess = Character->UpdateSharedReplication();
			}
			return bSuccess;
		};

	CharacterClassRepInfo.FastSharedReplicationFuncName = FName(TEXT("FastSharedReplication"));

	FastSharedPathConstants.MaxBitsPerFrame = (int32)((float)(Nebula::RepGraph::TargetKBytesSecFastSharedPath * 1024 * 8) / NetDriver->GetNetServerMaxTickRate());
	FastSharedPathConstants.DistanceRequirementPct = Nebula::RepGraph::FastSharedPathCullDistPct;
#endif
	SetClassInfo(ANebulaCharacter::StaticClass(), CharacterClassRepInfo);

	// ---------------------------------------------------------------------
	UReplicationGraphNode_ActorListFrequencyBuckets::DefaultSettings.ListSize = 12;
	UReplicationGraphNode_ActorListFrequencyBuckets::DefaultSettings.NumBuckets = Nebula::RepGraph::DynamicActorFrequencyBuckets;
	UReplicationGraphNode_ActorListFrequencyBuckets::DefaultSettings.BucketThresholds.Reset();
	// FastPath off now
	//UReplicationGraphNode_ActorListFrequencyBuckets::DefaultSettings.EnableFastPath = (Nebula::RepGraph::EnableFastSharedPath > 0);
	//UReplicationGraphNode_ActorListFrequencyBuckets::DefaultSettings.FastPathFrameModulo = 1;

	RPCSendPolicyMap.Reset();

	// Set FClassReplicationInfo based on legacy settings from all replicated classes
	for (UClass* ReplicatedClass : AllReplicatedClasses)
	{
		RegisterClassReplicationInfo(ReplicatedClass);
	}

	// Print out what we came up with
	UE_LOG(LogNebulaRepGraph, Log, TEXT(""));
	UE_LOG(LogNebulaRepGraph, Log, TEXT("Class Routing Map: "));
	for (auto ClassMapIt = ClassRepNodePolicies.CreateIterator(); ClassMapIt; ++ClassMapIt)
	{
		UClass* Class = CastChecked<UClass>(ClassMapIt.Key().ResolveObjectPtr());
		EClassRepNodeMapping Mapping = ClassMapIt.Value();

		// Only print if different than native class
		UClass* ParentNativeClass = GetParentNativeClass(Class);

		EClassRepNodeMapping* ParentMapping = ClassRepNodePolicies.Get(ParentNativeClass);
		if (ParentMapping && Class != ParentNativeClass && Mapping == *ParentMapping)
		{
			continue;
		}

		UE_LOG(LogNebulaRepGraph, Log, TEXT("  %s (%s) -> %s"), *Class->GetName(), *GetNameSafe(ParentNativeClass), *StaticEnum<EClassRepNodeMapping>()->GetNameStringByValue((int64)Mapping));
	}

	UE_LOG(LogNebulaRepGraph, Log, TEXT(""));
	UE_LOG(LogNebulaRepGraph, Log, TEXT("Class Settings Map: "));
	FClassReplicationInfo DefaultValues;
	for (auto ClassRepInfoIt = GlobalActorReplicationInfoMap.CreateClassMapIterator(); ClassRepInfoIt; ++ClassRepInfoIt)
	{
		UClass* Class = CastChecked<UClass>(ClassRepInfoIt.Key().ResolveObjectPtr());
		const FClassReplicationInfo& ClassInfo = ClassRepInfoIt.Value();
		UE_LOG(LogNebulaRepGraph, Log, TEXT("  %s (%s) -> %s"), *Class->GetName(), *GetNameSafe(GetParentNativeClass(Class)), *ClassInfo.BuildDebugStringDelta());
	}


	// Rep destruct infos based on CVar value
	DestructInfoMaxDistanceSquared = Nebula::RepGraph::DestructionInfoMaxDist * Nebula::RepGraph::DestructionInfoMaxDist;

	// Add to RPC_Multicast_OpenChannelForClass map
	RPC_Multicast_OpenChannelForClass.Reset();
	RPC_Multicast_OpenChannelForClass.Set(AActor::StaticClass(), true); // Open channels for multicast RPCs by default
	RPC_Multicast_OpenChannelForClass.Set(AController::StaticClass(), false); // multicasts should never open channels on Controllers since opening a channel on a non-owner breaks the Controller's replication.
	RPC_Multicast_OpenChannelForClass.Set(AServerStatReplicator::StaticClass(), false);

	for (const FRepGraphActorClassSettings& ActorClassSettings : NebulaRepGraphSettings->ClassSettings)
	{
		if (ActorClassSettings.bAddToRPC_Multicast_OpenChannelForClassMap)
		{
			if (UClass* StaticActorClass = ActorClassSettings.GetStaticActorClass())
			{
				UE_LOG(LogNebulaRepGraph, Log, TEXT("ActorClassSettings -- RPC_Multicast_OpenChannelForClass - %s"), *StaticActorClass->GetName());
				RPC_Multicast_OpenChannelForClass.Set(StaticActorClass, ActorClassSettings.bRPC_Multicast_OpenChannelForClass);
			}
		}
	}
}

void UNebulaReplicationGraph::InitGlobalGraphNodes()
{
	// -----------------------------------------------
	//	Spatial Actors, maybe this node isn't used now
	// -----------------------------------------------
	GridNode = CreateNewNode<UReplicationGraphNode_GridSpatialization2D>();
	GridNode->CellSize = Nebula::RepGraph::CellSize;
	GridNode->SpatialBias = FVector2D(Nebula::RepGraph::SpatialBiasX, Nebula::RepGraph::SpatialBiasY);

	if (Nebula::RepGraph::DisableSpatialRebuilds)
	{
		GridNode->AddToClassRebuildDenyList(AActor::StaticClass()); // Disable All spatial rebuilding
	}

	AddGlobalGraphNode(GridNode);

	// -----------------------------------------------
	//	Always Relevant (to everyone) Actors
	// -----------------------------------------------
	AlwaysRelevantNode = CreateNewNode<UReplicationGraphNode_ActorList>();
	AddGlobalGraphNode(AlwaysRelevantNode);

	// -----------------------------------------------
	//	Player State specialization. This will return a rolling subset of the player states to replicate
	// -----------------------------------------------
	UNebulaReplicationGraphNode_PlayerStateFrequencyLimiter* PlayerStateNode = CreateNewNode<UNebulaReplicationGraphNode_PlayerStateFrequencyLimiter>();
	AddGlobalGraphNode(PlayerStateNode);

	// -----------------------------------------------
	//	Precomputed Visibility Grid2D Actors
	// -----------------------------------------------
	PVSGridNode = CreateNewNode<UNebularReplicationGraphNode_PrecomputedVisibilityGrid2D>();

	const UNebulaReplicationGraphSettings* RepGraphSettings = GetDefault<UNebulaReplicationGraphSettings>();
	PVSGridNode->CellSize = RepGraphSettings->PVSSCellSize;
	PVSGridNode->SpatialBias = FVector2D(RepGraphSettings->PVSSpatialBiasX, RepGraphSettings->PVSSpatialBiasY);

	PVSGridNode->GenerateLookupTable();

	AddGlobalGraphNode(PVSGridNode);

	UE_LOG(LogNebulaRepGraph, Warning, TEXT("UReplicationGraphNode_GridCell's size : %d bytes"), sizeof(UReplicationGraphNode_GridCell));
}

void UNebulaReplicationGraph::InitConnectionGraphNodes(UNetReplicationGraphConnection* RepGraphConnection)
{
	Super::InitConnectionGraphNodes(RepGraphConnection);

	UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection* AlwaysRelevantConnectionNode = CreateNewNode<UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection>();

	// This node needs to know when client levels go in and out of visibility
	RepGraphConnection->OnClientVisibleLevelNameAdd.AddUObject(AlwaysRelevantConnectionNode, &UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection::OnClientLevelVisibilityAdd);
	RepGraphConnection->OnClientVisibleLevelNameRemove.AddUObject(AlwaysRelevantConnectionNode, &UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection::OnClientLevelVisibilityRemove);

	AddConnectionGraphNode(AlwaysRelevantConnectionNode, RepGraphConnection);
}

EClassRepNodeMapping UNebulaReplicationGraph::GetMappingPolicy(UClass* Class)
{
	EClassRepNodeMapping* PolicyPtr = ClassRepNodePolicies.Get(Class);
	EClassRepNodeMapping Policy = PolicyPtr ? *PolicyPtr : EClassRepNodeMapping::NotRouted;
	return Policy;
}

void UNebulaReplicationGraph::RouteAddNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalInfo)
{
	EClassRepNodeMapping Policy = GetMappingPolicy(ActorInfo.Class);
	switch (Policy)
	{
	case EClassRepNodeMapping::NotRouted:
	{
		break;
	}

	// now only support dynamic, $todo : add case of static, dormancy
	case EClassRepNodeMapping::PrecomputedVisibility:
	{
		PVSGridNode->AddActor_Dynamic(ActorInfo, GlobalInfo);
		break;
	}
	// $TODO : precompueted visibility - static
	// $TODO : precomputed visibility - dormancy

	case EClassRepNodeMapping::RelevantAllConnections:
	{
		if (ActorInfo.StreamingLevelName == NAME_None)
		{
			AlwaysRelevantNode->NotifyAddNetworkActor(ActorInfo);
		}
		else
		{
			FActorRepListRefView& RepList = AlwaysRelevantStreamingLevelActors.FindOrAdd(ActorInfo.StreamingLevelName);
			RepList.ConditionalAdd(ActorInfo.Actor);
		}
		break;
	}

	case EClassRepNodeMapping::Spatialize_Static:
	{
		GridNode->AddActor_Static(ActorInfo, GlobalInfo);
		break;
	}

	case EClassRepNodeMapping::Spatialize_Dynamic:
	{
		GridNode->AddActor_Dynamic(ActorInfo, GlobalInfo);
		break;
	}

	case EClassRepNodeMapping::Spatialize_Dormancy:
	{
		GridNode->AddActor_Dormancy(ActorInfo, GlobalInfo);
		break;
	}
	};
}

void UNebulaReplicationGraph::RouteRemoveNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo)
{
	EClassRepNodeMapping Policy = GetMappingPolicy(ActorInfo.Class);
	switch (Policy)
	{
	case EClassRepNodeMapping::NotRouted:
	{
		break;
	}

	case EClassRepNodeMapping::PrecomputedVisibility:
	{
		// now only support dynamic, $todo : add case of static, dormancy
		PVSGridNode->RemoveActor_Dynamic(ActorInfo);
		break;
	}

	// $TODO : precompueted visibility - static

	// $TODO : precomputed visibility - dormancy

	case EClassRepNodeMapping::RelevantAllConnections:
	{
		if (ActorInfo.StreamingLevelName == NAME_None)
		{
			AlwaysRelevantNode->NotifyRemoveNetworkActor(ActorInfo);
		}
		else
		{
			FActorRepListRefView& RepList = AlwaysRelevantStreamingLevelActors.FindChecked(ActorInfo.StreamingLevelName);
			if (RepList.RemoveFast(ActorInfo.Actor) == false)
			{
				UE_LOG(LogNebulaRepGraph, Warning, TEXT("Actor %s was not found in AlwaysRelevantStreamingLevelActors list. LevelName: %s"), *GetActorRepListTypeDebugString(ActorInfo.Actor), *ActorInfo.StreamingLevelName.ToString());
			}
		}

		SetActorDestructionInfoToIgnoreDistanceCulling(ActorInfo.GetActor());

		break;
	}

	case EClassRepNodeMapping::Spatialize_Static:
	{
		GridNode->RemoveActor_Static(ActorInfo);
		break;
	}

	case EClassRepNodeMapping::Spatialize_Dynamic:
	{
		GridNode->RemoveActor_Dynamic(ActorInfo);
		break;
	}

	case EClassRepNodeMapping::Spatialize_Dormancy:
	{
		GridNode->RemoveActor_Dormancy(ActorInfo);
		break;
	}
	};
}

// Since we listen to global (static) events, we need to watch out for cross world broadcasts (PIE)
#if WITH_EDITOR
#define CHECK_WORLDS(X) if(X->GetWorld() != GetWorld()) return;
#else
#define CHECK_WORLDS(X)
#endif

#undef CHECK_WORLDS

// Swap Weapon - @see ShooterReplicationGraph

// ------------------------------------------------------------------------------

void UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection::ResetGameWorldState()
{
	ReplicationActorList.Reset();
	AlwaysRelevantStreamingLevelsNeedingReplication.Empty();
}

void UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	UNebulaReplicationGraph* NebulaGraph = CastChecked<UNebulaReplicationGraph>(GetOuter());

	ReplicationActorList.Reset();

	for (const FNetViewer& CurViewer : Params.Viewers)
	{
		ReplicationActorList.ConditionalAdd(CurViewer.InViewer);
		ReplicationActorList.ConditionalAdd(CurViewer.ViewTarget);

		if (ANebulaPlayerController* PC = Cast<ANebulaPlayerController>(CurViewer.InViewer))
		{
			// 50% throttling of PlayerStates.
			const bool bReplicatePS = (Params.ConnectionManager.ConnectionOrderNum % 2) == (Params.ReplicationFrameNum % 2);
			if (bReplicatePS)
			{
				// Always return the player state to the owning player. Simulated proxy player states are handled by UNebulaReplicationGraphNode_PlayerStateFrequencyLimiter
				if (APlayerState* PS = PC->PlayerState)
				{
					if (!bInitializedPlayerState)
					{
						bInitializedPlayerState = true;
						FConnectionReplicationActorInfo& ConnectionActorInfo = Params.ConnectionManager.ActorInfoMap.FindOrAdd(PS);
						ConnectionActorInfo.ReplicationPeriodFrame = 1;
					}

					ReplicationActorList.ConditionalAdd(PS);
				}
			}

			FCachedAlwaysRelevantActorInfo& LastData = PastRelevantActorMap.FindOrAdd(CurViewer.Connection);

			if (ANebulaCharacter* Pawn = Cast<ANebulaCharacter>(PC->GetPawn()))
			{
				UpdateCachedRelevantActor(Params, Pawn, LastData.LastViewer);

				if (Pawn != CurViewer.ViewTarget)
				{
					ReplicationActorList.ConditionalAdd(Pawn);
				}
			}

			if (ANebulaCharacter* ViewTargetPawn = Cast<ANebulaCharacter>(CurViewer.ViewTarget))
			{
				UpdateCachedRelevantActor(Params, ViewTargetPawn, LastData.LastViewTarget);
			}
		}
	}

	CleanupCachedRelevantActors(PastRelevantActorMap);

	// Always relevant streaming level actors.
	FPerConnectionActorInfoMap& ConnectionActorInfoMap = Params.ConnectionManager.ActorInfoMap;

	TMap<FName, FActorRepListRefView>& AlwaysRelevantStreamingLevelActors = NebulaGraph->AlwaysRelevantStreamingLevelActors;

	for (int32 Idx = AlwaysRelevantStreamingLevelsNeedingReplication.Num() - 1; Idx >= 0; --Idx)
	{
		const FName& StreamingLevel = AlwaysRelevantStreamingLevelsNeedingReplication[Idx];

		FActorRepListRefView* Ptr = AlwaysRelevantStreamingLevelActors.Find(StreamingLevel);
		if (Ptr == nullptr)
		{
			// No always relevant lists for that level
			UE_CLOG(Nebula::RepGraph::DisplayClientLevelStreaming > 0, LogNebulaRepGraph, Display, TEXT("CLIENTSTREAMING Removing %s from AlwaysRelevantStreamingLevelActors because FActorRepListRefView is null. %s "), *StreamingLevel.ToString(), *Params.ConnectionManager.GetName());
			AlwaysRelevantStreamingLevelsNeedingReplication.RemoveAtSwap(Idx, EAllowShrinking::No);
			continue;
		}

		FActorRepListRefView& RepList = *Ptr;

		if (RepList.Num() > 0)
		{
			bool bAllDormant = true;
			for (FActorRepListType Actor : RepList)
			{
				FConnectionReplicationActorInfo& ConnectionActorInfo = ConnectionActorInfoMap.FindOrAdd(Actor);
				if (ConnectionActorInfo.bDormantOnConnection == false)
				{
					bAllDormant = false;
					break;
				}
			}

			if (bAllDormant)
			{
				UE_CLOG(Nebula::RepGraph::DisplayClientLevelStreaming > 0, LogNebulaRepGraph, Display, TEXT("CLIENTSTREAMING All AlwaysRelevant Actors Dormant on StreamingLevel %s for %s. Removing list."), *StreamingLevel.ToString(), *Params.ConnectionManager.GetName());
				AlwaysRelevantStreamingLevelsNeedingReplication.RemoveAtSwap(Idx, EAllowShrinking::No);
			}
			else
			{
				UE_CLOG(Nebula::RepGraph::DisplayClientLevelStreaming > 0, LogNebulaRepGraph, Display, TEXT("CLIENTSTREAMING Adding always Actors on StreamingLevel %s for %s because it has at least one non dormant actor"), *StreamingLevel.ToString(), *Params.ConnectionManager.GetName());
				Params.OutGatheredReplicationLists.AddReplicationActorList(RepList);
			}
		}
		else
		{
			UE_LOG(LogNebulaRepGraph, Warning, TEXT("UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection::GatherActorListsForConnection - empty RepList %s"), *Params.ConnectionManager.GetName());
		}

	}

	Params.OutGatheredReplicationLists.AddReplicationActorList(ReplicationActorList);
}

void UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection::OnClientLevelVisibilityAdd(FName LevelName, UWorld* StreamingWorld)
{
	UE_CLOG(Nebula::RepGraph::DisplayClientLevelStreaming > 0, LogNebulaRepGraph, Display, TEXT("CLIENTSTREAMING ::OnClientLevelVisibilityAdd - %s"), *LevelName.ToString());
	AlwaysRelevantStreamingLevelsNeedingReplication.Add(LevelName);
}

void UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection::OnClientLevelVisibilityRemove(FName LevelName)
{
	UE_CLOG(Nebula::RepGraph::DisplayClientLevelStreaming > 0, LogNebulaRepGraph, Display, TEXT("CLIENTSTREAMING ::OnClientLevelVisibilityRemove - %s"), *LevelName.ToString());
	AlwaysRelevantStreamingLevelsNeedingReplication.Remove(LevelName);
}

void UNebulaReplicationGraphNode_AlwaysRelevant_ForConnection::LogNode(FReplicationGraphDebugInfo& DebugInfo, const FString& NodeName) const
{
	DebugInfo.Log(NodeName);
	DebugInfo.PushIndent();
	LogActorRepList(DebugInfo, NodeName, ReplicationActorList);

	for (const FName& LevelName : AlwaysRelevantStreamingLevelsNeedingReplication)
	{
		UNebulaReplicationGraph* NebulaGraph = CastChecked<UNebulaReplicationGraph>(GetOuter());
		if (FActorRepListRefView* RepList = NebulaGraph->AlwaysRelevantStreamingLevelActors.Find(LevelName))
		{
			LogActorRepList(DebugInfo, FString::Printf(TEXT("AlwaysRelevant StreamingLevel List: %s"), *LevelName.ToString()), *RepList);
		}
	}

	DebugInfo.PopIndent();
}

// ------------------------------------------------------------------------------

UNebulaReplicationGraphNode_PlayerStateFrequencyLimiter::UNebulaReplicationGraphNode_PlayerStateFrequencyLimiter()
{
	bRequiresPrepareForReplicationCall = true;
}

void UNebulaReplicationGraphNode_PlayerStateFrequencyLimiter::PrepareForReplication()
{
	ReplicationActorLists.Reset();
	ForceNetUpdateReplicationActorList.Reset();

	ReplicationActorLists.AddDefaulted();
	FActorRepListRefView* CurrentList = &ReplicationActorLists[0];

	// We rebuild our lists of player states each frame. This is not as efficient as it could be but its the simplest way
	// to handle players disconnecting and keeping the lists compact. If the lists were persistent we would need to defrag them as players left.

	for (TActorIterator<APlayerState> It(GetWorld()); It; ++It)
	{
		APlayerState* PS = *It;
		if (IsActorValidForReplicationGather(PS) == false)
		{
			continue;
		}

		if (CurrentList->Num() >= TargetActorsPerFrame)
		{
			ReplicationActorLists.AddDefaulted();
			CurrentList = &ReplicationActorLists.Last();
		}

		CurrentList->Add(PS);
	}
}

void UNebulaReplicationGraphNode_PlayerStateFrequencyLimiter::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	const int32 ListIdx = Params.ReplicationFrameNum % ReplicationActorLists.Num();
	Params.OutGatheredReplicationLists.AddReplicationActorList(ReplicationActorLists[ListIdx]);

	if (ForceNetUpdateReplicationActorList.Num() > 0)
	{
		Params.OutGatheredReplicationLists.AddReplicationActorList(ForceNetUpdateReplicationActorList);
	}
}

void UNebulaReplicationGraphNode_PlayerStateFrequencyLimiter::LogNode(FReplicationGraphDebugInfo& DebugInfo, const FString& NodeName) const
{
	DebugInfo.Log(NodeName);
	DebugInfo.PushIndent();

	int32 i = 0;
	for (const FActorRepListRefView& List : ReplicationActorLists)
	{
		LogActorRepList(DebugInfo, FString::Printf(TEXT("Bucket[%d]"), i++), List);
	}

	DebugInfo.PopIndent();
}

void UNebulaReplicationGraph::PrintRepNodePolicies()
{
	UEnum* Enum = StaticEnum<EClassRepNodeMapping>();
	if (!Enum)
	{
		return;
	}

	GLog->Logf(TEXT("===================================="));
	GLog->Logf(TEXT("Nebula Replication Routing Policies"));
	GLog->Logf(TEXT("===================================="));

	for (auto It = ClassRepNodePolicies.CreateIterator(); It; ++It)
	{
		FObjectKey ObjKey = It.Key();

		EClassRepNodeMapping Mapping = It.Value();

		GLog->Logf(TEXT("%-40s --> %s"), *GetNameSafe(ObjKey.ResolveObjectPtr()), *Enum->GetNameStringByValue(static_cast<uint32>(Mapping)));
	}
}

// ------------------------------------------------------------------------------

UNebularReplicationGraphNode_PrecomputedVisibilityGrid2D::UNebularReplicationGraphNode_PrecomputedVisibilityGrid2D()
	: CellSize(0.f)
	, SpatialBias(ForceInitToZero)
{
	bRequiresPrepareForReplicationCall = true;
}

void UNebularReplicationGraphNode_PrecomputedVisibilityGrid2D::NotifyAddNetworkActor(const FNewReplicatedActorInfo& ActorInfo)
{
	ensureAlwaysMsgf(false, TEXT("UNebulaReplicationGraphNode_PrecomputedVisibilityGrid2D::NotifyAddNetworkActor should not be called directly"));
}

bool UNebularReplicationGraphNode_PrecomputedVisibilityGrid2D::NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound)
{
	ensureAlwaysMsgf(false, TEXT("UNebulaReplicationGraphNode_PrecomputedVisibilityGrid2D::NotifyRemoveNetworkActor should not be called directly"));
	return false;
}

bool UNebularReplicationGraphNode_PrecomputedVisibilityGrid2D::NotifyActorRenamed(const FRenamedReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound)
{
	ensureAlwaysMsgf(false, TEXT("UNebulaReplicationGraphNode_PrecomputedVisibilityGrid2D::NotifyActorRenamed should not be called directly"));
	return false;
}

void UNebularReplicationGraphNode_PrecomputedVisibilityGrid2D::PrepareForReplication()
{
	FGlobalActorReplicationInfoMap* GlobalRepMap = GraphGlobals.IsValid() ? GraphGlobals->GlobalActorReplicationInfoMap : nullptr;
	check(GlobalRepMap);

	for (auto& MapIt : DynamicSpatializedActors)
	{
		FActorRepListType& DynamicActor = MapIt.Key;
		FCachedDynamicActorInfo& DynamicActorInfo = MapIt.Value;
		FActorCellInfo& PreviousCellInfo = DynamicActorInfo.CellInfo;
		FNewReplicatedActorInfo& ActorInfo = DynamicActorInfo.ActorInfo;

		FGlobalActorReplicationInfo& ActorRepInfo = GlobalRepMap->Get(DynamicActor);
		ActorRepInfo.WorldLocation = DynamicActor->GetActorLocation();

		const int32 GridCellX = UE::LWC::FloatToIntCastChecked<int32>((ActorRepInfo.WorldLocation.X - SpatialBias.X) / CellSize);
		const int32 GridCellY = UE::LWC::FloatToIntCastChecked<int32>((ActorRepInfo.WorldLocation.Y - SpatialBias.Y) / CellSize);

		FActorCellInfo NewCellInfo;
		NewCellInfo.CellIndex.X = GridCellX;
		NewCellInfo.CellIndex.Y = GridCellY;

		const FIntPoint& PreviousCell = PreviousCellInfo.CellIndex;

		if (PreviousCellInfo.IsValid())
		{
			if (UNLIKELY((PreviousCell.X != GridCellX) || (PreviousCell.Y != GridCellY)))
			{
				UE_LOG(LogNebulaRepGraph, Warning, TEXT("Dynamic Actor %s : {%d, %d} -> {%d, %d}"),
					*DynamicActor->GetName(), PreviousCell.X, PreviousCell.Y, GridCellX, GridCellY);

				if (UReplicationGraphNode_GridCell* PreviousGridCell = GetCell(PreviousCell.X, PreviousCell.Y))
				{
					PreviousGridCell->RemoveDynamicActor(ActorInfo);
				}

				if (UReplicationGraphNode_GridCell* CurrentGridCell = GetCellNode(GetCell(GridCellX, GridCellY)))
				{
					CurrentGridCell->AddDynamicActor(ActorInfo);
				}

				PreviousCellInfo = NewCellInfo;
			}
#if 0
			else
			{
				// NOP : nothing has changed. 
			}
#endif
		}
		else
		{
			// First time - Just add
			if (UReplicationGraphNode_GridCell* CurrentGridCell = GetCellNode(GetCell(GridCellX, GridCellY)))
			{
				CurrentGridCell->AddDynamicActor(ActorInfo);
			}

			PreviousCellInfo = NewCellInfo;
		}
	}
}

/**
* 1) Get ViewTarget's Grid index from WorldLocation (imagine First Person, but use ViewLocation in Params.Viewers if Third Person)
* 2) Find visible GridCells from LookupTable
* 3) Gather by iterating visible GridCells
*/
void UNebularReplicationGraphNode_PrecomputedVisibilityGrid2D::GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params)
{
	// $TODO : replace DynamicSpatializedActor with ViewTarget, but in prototype, viewtarget is ok
	if (AActor* ViewTarget = Params.ConnectionManager.NetConnection->ViewTarget)
	{
		FGlobalActorReplicationInfoMap* GlobalRepMap = GraphGlobals.IsValid() ? GraphGlobals->GlobalActorReplicationInfoMap : nullptr;
		FGlobalActorReplicationInfo& ActorRepInfo = GlobalRepMap->Get(ViewTarget);
		
		const int32 GridCellX = UE::LWC::FloatToIntCastChecked<int32>((ActorRepInfo.WorldLocation.X - SpatialBias.X) / CellSize);
		const int32 GridCellY = UE::LWC::FloatToIntCastChecked<int32>((ActorRepInfo.WorldLocation.Y - SpatialBias.Y) / CellSize);
		
		if (const TArray<FIntPoint>* VisibleCells = PVSLookupTable.Find(FIntPoint(GridCellX, GridCellY)))
		{
			for (const FIntPoint& Cell : *VisibleCells)
			{
				// $TODO : iter/calling GatherActorListsForConnection on all visible cells per Connection would heavy?
				if (UReplicationGraphNode_GridCell* GridCell = GetCellNode(GetCell(Cell.X, Cell.Y)))
				{
					GridCell->GatherActorListsForConnection(Params);
				}
			}
		}
	}
}

void UNebularReplicationGraphNode_PrecomputedVisibilityGrid2D::AddActorInternal_Dynamic(const FNewReplicatedActorInfo& ActorInfo)
{
	UE_LOG(LogNebulaRepGraph, Warning, TEXT("Dynamic Actor : %s is Added in PrecomputedVisibilityGrid2D Node."), *ActorInfo.Actor->GetName());
	DynamicSpatializedActors.Emplace(ActorInfo.Actor, ActorInfo);
}

void UNebularReplicationGraphNode_PrecomputedVisibilityGrid2D::RemoveActorInternal_Dynamic(const FNewReplicatedActorInfo& ActorInfo)
{
	if (FCachedDynamicActorInfo* DynamicActorInfo = DynamicSpatializedActors.Find(ActorInfo.Actor))
	{
		if (DynamicActorInfo->CellInfo.IsValid())
		{
			const int32 GridCellX = DynamicActorInfo->CellInfo.CellIndex.X;
			const int32 GridCellY = DynamicActorInfo->CellInfo.CellIndex.Y;

			if (UReplicationGraphNode_GridCell* GridCell = GetCell(GetGridX(GridCellX), GridCellY))
			{
				GridCell->RemoveDynamicActor(ActorInfo);
			}
		}
		DynamicSpatializedActors.Remove(ActorInfo.Actor);
	}
	UE_LOG(LogNebulaRepGraph, Warning, TEXT("Dynamic Actor : %s is Removed in PrecomputedVisibilityGrid2D Node."), *ActorInfo.Actor->GetName());
}

void UNebularReplicationGraphNode_PrecomputedVisibilityGrid2D::GenerateLookupTable()
{
	PVSLookupTable.Reset();
	// ------------------------------------
	// TestCase
	// SpatialBias = -600, CellSize : 200
	// ------------------------------------
	const int32 NumToReserve = (SpatialBias.X * 2 / CellSize) * (SpatialBias.Y * 2 / CellSize);
	PVSLookupTable.Reserve(NumToReserve);

	// max memory usage : O(N^3) * 8bytes?, need to compress data structure (ex : FIntPoint to uint16, ...)
	// cuz memory footprint...
	
	// --------------------------------
	//	grid cells brief specification
	//					  (6, 6)
	//		□□□□□□■
	//		□□□□□□□
	//		□□□□□□□
	//		□□□□□□□
	//		□□□□□□□
	//		□□□□□□□
	//		■□□□□□□
	// (0,0)
	// --------------------------------

	// ---------------------------------------------------------------------------------
	// | ViewTarget in key(0, 0) can see value({0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}) |
	// | $TODO : Generate all visibility info.                                         |
	// ---------------------------------------------------------------------------------
	PVSLookupTable.Add({ FIntPoint(0, 0), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // ■□□□□□□
	PVSLookupTable.Add({ FIntPoint(0, 1), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // ■□□□□□□
	PVSLookupTable.Add({ FIntPoint(0, 2), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // ■□□□□□□
	PVSLookupTable.Add({ FIntPoint(0, 3), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // ■□□□□□□
	PVSLookupTable.Add({ FIntPoint(0, 4), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // ■□□□□□□
	PVSLookupTable.Add({ FIntPoint(0, 5), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // ■□□□□□□
	PVSLookupTable.Add({ FIntPoint(0, 6), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // ■□□□□□□

	PVSLookupTable.Add({ FIntPoint(1, 0), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □■□□□□□
	PVSLookupTable.Add({ FIntPoint(1, 1), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □■□□□□□
	PVSLookupTable.Add({ FIntPoint(1, 2), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □■□□□□□
	PVSLookupTable.Add({ FIntPoint(1, 3), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □■□□□□□
	PVSLookupTable.Add({ FIntPoint(1, 4), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □■□□□□□
	PVSLookupTable.Add({ FIntPoint(1, 5), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □■□□□□□
	PVSLookupTable.Add({ FIntPoint(1, 6), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □■□□□□□

	PVSLookupTable.Add({ FIntPoint(2, 0), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□■□□□□
	PVSLookupTable.Add({ FIntPoint(2, 1), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□■□□□□
	PVSLookupTable.Add({ FIntPoint(2, 2), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□■□□□□
	PVSLookupTable.Add({ FIntPoint(2, 3), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□■□□□□
	PVSLookupTable.Add({ FIntPoint(2, 4), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□■□□□□
	PVSLookupTable.Add({ FIntPoint(2, 5), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□■□□□□
	PVSLookupTable.Add({ FIntPoint(2, 6), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□■□□□□

	PVSLookupTable.Add({ FIntPoint(3, 0), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□■□□□
	PVSLookupTable.Add({ FIntPoint(3, 1), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□■□□□
	PVSLookupTable.Add({ FIntPoint(3, 2), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□■□□□
	PVSLookupTable.Add({ FIntPoint(3, 3), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□■□□□
	PVSLookupTable.Add({ FIntPoint(3, 4), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□■□□□
	PVSLookupTable.Add({ FIntPoint(3, 5), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□■□□□
	PVSLookupTable.Add({ FIntPoint(3, 6), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□■□□□

	PVSLookupTable.Add({ FIntPoint(4, 0), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□■□□
	PVSLookupTable.Add({ FIntPoint(4, 1), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□■□□
	PVSLookupTable.Add({ FIntPoint(4, 2), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□■□□
	PVSLookupTable.Add({ FIntPoint(4, 3), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□■□□
	PVSLookupTable.Add({ FIntPoint(4, 4), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□■□□
	PVSLookupTable.Add({ FIntPoint(4, 5), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□■□□
	PVSLookupTable.Add({ FIntPoint(4, 6), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□■□□

	PVSLookupTable.Add({ FIntPoint(5, 0), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□□■□
	PVSLookupTable.Add({ FIntPoint(5, 1), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□□■□
	PVSLookupTable.Add({ FIntPoint(5, 2), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□□■□
	PVSLookupTable.Add({ FIntPoint(5, 3), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□□■□
	PVSLookupTable.Add({ FIntPoint(5, 4), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□□■□
	PVSLookupTable.Add({ FIntPoint(5, 5), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□□■□
	PVSLookupTable.Add({ FIntPoint(5, 6), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□□■□

	PVSLookupTable.Add({ FIntPoint(6, 0), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□□□■
	PVSLookupTable.Add({ FIntPoint(6, 1), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□□□■
	PVSLookupTable.Add({ FIntPoint(6, 2), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□□□■
	PVSLookupTable.Add({ FIntPoint(6, 3), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□□□■
	PVSLookupTable.Add({ FIntPoint(6, 4), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□□□■
	PVSLookupTable.Add({ FIntPoint(6, 5), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□□□■
	PVSLookupTable.Add({ FIntPoint(6, 6), TArray<FIntPoint>({ {0, 0}, {0, 1}, {0, 2}, {0, 3}, {0, 4}}) }); // □□□□□□■
}