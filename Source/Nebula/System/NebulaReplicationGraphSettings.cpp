// Copyright Epic Games, Inc. All Rights Reserved.


#include "NebulaReplicationGraphSettings.h"
#include "Misc/App.h"
#include "NebulaReplicationGraph.h"

UNebulaReplicationGraphSettings::UNebulaReplicationGraphSettings()
{
	CategoryName = TEXT("Game");
	DefaultReplicationGraphClass = UNebulaReplicationGraph::StaticClass();
}