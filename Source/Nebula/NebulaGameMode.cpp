// Copyright Epic Games, Inc. All Rights Reserved.

#include "NebulaGameMode.h"
#include "NebulaCharacter.h"
#include "UObject/ConstructorHelpers.h"

ANebulaGameMode::ANebulaGameMode()
{
	// set default pawn class to our Blueprinted character
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnBPClass(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"));
	if (PlayerPawnBPClass.Class != NULL)
	{
		DefaultPawnClass = PlayerPawnBPClass.Class;
	}
}
