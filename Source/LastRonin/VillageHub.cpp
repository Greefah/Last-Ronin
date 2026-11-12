#include "VillageHub.h"
#include "NemesisManager.h"
#include "Engine/GameInstance.h"
#include "Components/StaticMeshComponent.h"
#include "Kismet/GameplayStatics.h"

AVillageHub::AVillageHub()
{
    VillageCenterStoneMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VillageCenterStoneMesh"));
    RootComponent = VillageCenterStoneMesh;
}

void AVillageHub::BeginPlay()
{
    Super::BeginPlay();
}

void AVillageHub::ReassignVillageOverlord(const FString& NewFortressRegionID, const FString& CapturingClanID)
{
    if (!HasAuthority()) return;
    UGameInstance* GI = GetGameInstance();
    UNemesisManager* NemesisSys = GI ? GI->GetSubsystem<UNemesisManager>() : nullptr;
    if (!NemesisSys || RegionID.IsEmpty() || NewFortressRegionID.IsEmpty() ||
        RegionID == NewFortressRegionID || CapturingClanID.IsEmpty()) return;
    NemesisSys->SynchronizeSimulation();
    const FRegionData* Village = NemesisSys->MapRegions.Find(RegionID);
    const FRegionData* Fortress = NemesisSys->MapRegions.Find(NewFortressRegionID);
    if (!Village || !Fortress || Village->RegionType == ESubRegionType::MilitaryFortress ||
        Fortress->RegionType != ESubRegionType::MilitaryFortress ||
        Fortress->ControllingClanID != CapturingClanID) return;
    TSet<FString> Visited;
    Visited.Add(RegionID);
    FString Cursor = NewFortressRegionID;
    while (!Cursor.IsEmpty())
    {
        if (Visited.Contains(Cursor)) return;
        Visited.Add(Cursor);
        const FRegionData* Node = NemesisSys->MapRegions.Find(Cursor);
        if (!Node) return;
        Cursor = Node->ParentFortressRegionID;
    }
    // Manager owns aggregation and persistence; refresh all loaded dependent caches here.
    if (!NemesisSys->ReassignVillageTributary(RegionID, NewFortressRegionID)) return;
    TArray<AActor*> Hubs;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), ASettlementHub::StaticClass(), Hubs);
    for (AActor* Actor : Hubs)
        if (ASettlementHub* Hub = Cast<ASettlementHub>(Actor)) Hub->SyncWithBackendDatabase();
    UE_LOG(LogTemp, Warning, TEXT("FEUDAL MATRIX: Village %s annexed to Fortress %s under clan %s!"), *RegionID, *NewFortressRegionID, *CapturingClanID);
}

void AVillageHub::OpenLocalNoticeBoardUI()
{
    UE_LOG(LogTemp, Log, TEXT("VILLAGE HUB: Accessing resource records for region %s"), *RegionID);
}