// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "NebularGameState.generated.h"

/**
 * 
 */
UCLASS()
class NEBULA_API ANebularGameState : public AGameStateBase
{
	GENERATED_BODY()
	
protected:
	virtual void BeginPlay() override;

private:
	void DrawDebugGridCells(const float InCellSize, const FVector2D& InSpatialBias);
};
