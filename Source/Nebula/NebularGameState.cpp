// Copyright Epic Games, Inc. All Rights Reserved.


#include "NebularGameState.h"
#include "System/NebulaReplicationGraphSettings.h"

void ANebularGameState::BeginPlay()
{
	Super::BeginPlay();
	
	
	if (!HasAuthority())
	{
		if (const UNebulaReplicationGraphSettings* NebulaRepGraphSettings = GetDefault<UNebulaReplicationGraphSettings>())
		{
			const FVector2D SpatialBias(NebulaRepGraphSettings->PVSSpatialBiasX, NebulaRepGraphSettings->PVSSpatialBiasY);
			DrawDebugGridCells(NebulaRepGraphSettings->PVSSCellSize, SpatialBias);
		}
	}
}

void ANebularGameState::DrawDebugGridCells(float InCellSize, const FVector2D& InSpatialBias)
{
	UWorld* World = GetWorld();
	check(World);

	// $TODO : ���⼱ ������ �������� ������� DebugLine�� �׷�����.
	// index�� �ʿ����, WorldLocation�� �ʿ��ϱ� ����
	// �׷��Ƿ� SpatialBias �� CellSize ������ �ٽ� WorldLocation���� ����ȯ�ؾ���.
	/// -> CellSize�� �ٽ� ���ѵ�, SpatialBias�� �ٽ� ���ؾ� ��.
	const int32 MaxX = (-InSpatialBias.X) * 2 / InCellSize;
	const int32 MaxY = (-InSpatialBias.Y) * 2 / InCellSize;

	for (int32 X = 0; X <= MaxX; X++)
	{
		for (int32 Y = 0; Y <= MaxY; Y++)
		{
			const FVector Center(((X * InCellSize) + InSpatialBias.X) + InCellSize / 2, ((Y * InCellSize) + InSpatialBias.Y) + InCellSize / 2, 10.f);

			const FVector TopLeft = Center + FVector(-InCellSize / 2, InCellSize / 2, 0.f);
			const FVector TopRight = Center + FVector(InCellSize / 2, InCellSize / 2, 0.f);

			const FVector BottomLeft = Center + FVector(-InCellSize / 2, -InCellSize / 2, 0.f);
			const FVector BottomRight = Center + FVector(InCellSize / 2, -InCellSize / 2, 0.f);

			if (X == 0 && Y == 0)
			{
				DrawDebugLine(World, TopLeft, BottomRight, FColor::Black, true, -1.f, 0U, 10.f);
				DrawDebugLine(World, TopRight, BottomLeft, FColor::White, true, -1.f, 0U, 10.f);
			}

			if (X == MaxX && Y == MaxY)
			{
				DrawDebugLine(World, TopLeft, BottomRight, FColor::Black, true, -1.f, 0U, 10.f);
				DrawDebugLine(World, TopRight, BottomLeft, FColor::White, true, -1.f, 0U, 10.f);
			}

			DrawDebugLine(World, TopLeft, TopRight, FColor::Red, true, -1.f, 0U, 2.f);
			DrawDebugLine(World, TopLeft, BottomLeft, FColor::Green, true, -1.f, 0U, 2.f);
			DrawDebugLine(World, BottomRight, TopRight, FColor::Blue, true, -1.f, 0U, 2.f);
			DrawDebugLine(World, BottomRight, BottomLeft, FColor::Yellow, true, -1.f, 0U, 2.f);
		}
	}
}
