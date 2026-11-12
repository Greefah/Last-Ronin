#pragma once

#include "CoreMinimal.h"
#include "RoninCharacter.h"
#include "RoninGameMode.h"
#include "NemesisManager.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

/** Native authority proof shared by commerce mutation boundaries. No client-supplied IDs. */
struct LASTRONIN_API FRoninEconomyAccess
{
    static constexpr double InteractionRange = 500.0;
    static constexpr double BuildingRange = 1500.0;

    static bool IsWithinRange(const FVector& A, const FVector& B, double Range)
    {
        if (!FMath::IsFinite(A.X) || !FMath::IsFinite(A.Y) || !FMath::IsFinite(A.Z) ||
            !FMath::IsFinite(B.X) || !FMath::IsFinite(B.Y) || !FMath::IsFinite(B.Z) ||
            !FMath::IsFinite(Range) || Range <= 0.0) return false;
        const double DistanceSquared = FVector::DistSquared(A, B);
        return FMath::IsFinite(DistanceSquared) && DistanceSquared <= Range * Range;
    }

    static bool HasSettlementRights(const FHunterProfile& Profile, const FRegionData& Region)
    {
        return Profile.bIsAlive && !Profile.ClanID.IsEmpty() &&
            !Profile.ClanID.Equals(TEXT("Neutral"), ESearchCase::IgnoreCase) &&
            !Region.ControllingClanID.IsEmpty() &&
            !Region.ControllingClanID.Equals(TEXT("Neutral"), ESearchCase::IgnoreCase) &&
            Profile.ClanID == Region.ControllingClanID;
    }

    // Synchronizes simulation before borrowing canonical data. No insertion/removal of profiles follows.
    static bool ResolveCaller(APlayerController* Caller, UGameInstance* GI,
        ARoninCharacter*& OutPawn, FHunterProfile*& OutProfile)
    {
        OutPawn = nullptr;
        OutProfile = nullptr;
        if (!IsValid(Caller) || !GI || !Caller->HasAuthority() || Caller->GetGameInstance() != GI ||
            !Caller->GetWorld() || Caller->IsActorBeingDestroyed()) return false;
        const ARoninGameMode* Mode = Cast<ARoninGameMode>(Caller->GetWorld()->GetAuthGameMode());
        ARoninCharacter* Pawn = Cast<ARoninCharacter>(Caller->GetPawn());
        if (!Mode || !IsValid(Pawn) || Pawn->GetWorld() != Caller->GetWorld() ||
            Pawn->GetController() != Caller || !Pawn->HasAuthority() || Pawn->IsActorBeingDestroyed() ||
            Pawn->bHasInitiatedDeath || Pawn->bRestoringCombatState || Pawn->bReplayingCombat ||
            !Pawn->PlayerDNA.bIsAlive || !FMath::IsFinite(Pawn->PlayerDNA.Blood) || Pawn->PlayerDNA.Blood <= 0.f) return false;
        const FString ID = Mode->GetAuthorizedCharacterID(Caller);
        if (ID.IsEmpty() || ID == TEXT("PLAYER") || ID != Pawn->PlayerDNA.UniqueID) return false;
        UNemesisManager* DB = GI->GetSubsystem<UNemesisManager>();
        if (!DB) return false;
        DB->SynchronizeSimulation();
        FHunterProfile* Profile = DB->ActiveNemeses.Find(ID);
        if (!Profile || Profile->UniqueID != ID || !Profile->bIsAlive || !Profile->bIsRealPlayer ||
            Profile->UniquePlayerAccountID.IsEmpty() ||
            Profile->UniquePlayerAccountID != Pawn->PlayerDNA.UniquePlayerAccountID ||
            !FMath::IsFinite(Profile->Blood) || Profile->Blood <= 0.f) return false;
        OutPawn = Pawn;
        OutProfile = Profile;
        return true;
    }
};
