#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "RoninLocalIdentitySaveGame.generated.h"

/** Device-local identity only; never accepted from a remote controller or world save. */
UCLASS()
class LASTRONIN_API URoninLocalIdentitySaveGame : public USaveGame
{
    GENERATED_BODY()

public:
    // Defaults deliberately reject missing fields in a damaged or incompatible slot.
    UPROPERTY(SaveGame) int32 IdentityVersion = 0;
    UPROPERTY(SaveGame) int32 LocalPlayerSlot = INDEX_NONE;
    UPROPERTY(SaveGame) FGuid AccountGuid;

    bool IsIdentityValid(int32 ExpectedLocalPlayerSlot) const
    {
        return IdentityVersion == 1 && ExpectedLocalPlayerSlot >= 0 &&
            LocalPlayerSlot == ExpectedLocalPlayerSlot && AccountGuid.IsValid();
    }
};
