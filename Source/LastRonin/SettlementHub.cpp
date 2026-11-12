#include "SettlementHub.h"
#include "NemesisManager.h"
#include "Engine/GameInstance.h"
#include "SiegeFortress.h"
#include "Net/UnrealNetwork.h"

ASettlementHub::ASettlementHub()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = true;
}

void ASettlementHub::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ASettlementHub, RegionID);
    DOREPLIFETIME(ASettlementHub, OwningClanID);
    DOREPLIFETIME(ASettlementHub, ParentFortressID);
}

void ASettlementHub::BeginPlay()
{
    Super::BeginPlay();
    SyncWithBackendDatabase();
}

void ASettlementHub::SyncWithBackendDatabase()
{
    if (FRegionData* LiveData = GetLiveRegionData())
    {
        OwningClanID = LiveData->ControllingClanID;
        ParentFortressID = LiveData->ParentFortressRegionID;
        if (ASiegeFortress* Fortress = Cast<ASiegeFortress>(this))
        {
            Fortress->FortressHealth = LiveData->FortressHealth;
            Fortress->MaxFortressHealth = LiveData->MaxFortressHealth;
        }
    }
}

FRegionData* ASettlementHub::GetLiveRegionData() const
{
    // Clients retain replicated cache values, never overwrite them from a local subsystem.
    if (!HasAuthority()) return nullptr;
    if (UGameInstance* GI = GetGameInstance())
    {
        if (UNemesisManager* NemesisSys = GI->GetSubsystem<UNemesisManager>())
        {
            NemesisSys->SynchronizeSimulation();
            if (NemesisSys->MapRegions.Contains(RegionID))
            {
                return &NemesisSys->MapRegions[RegionID];
            }
        }
    }
    return nullptr;
}