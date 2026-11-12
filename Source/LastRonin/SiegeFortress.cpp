#include "SiegeFortress.h"
#include "NemesisManager.h"
#include "BaseHunter.h"
#include "RoninCharacter.h"
#include "EconomySystem.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Controller.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/GameInstance.h"
#include "Net/UnrealNetwork.h"

ASiegeFortress::ASiegeFortress() {
    FortressMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FortressMesh"));
    RootComponent = FortressMesh;
}

void ASiegeFortress::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ASiegeFortress, FortressHealth);
    DOREPLIFETIME(ASiegeFortress, MaxFortressHealth);
}

void ASiegeFortress::BeginPlay() {
    Super::BeginPlay();
    if (!HasAuthority()) return;

    if (FRegionData* LiveData = GetLiveRegionData()) {
        OwningClanID = LiveData->ControllingClanID;
        MaxFortressHealth = LiveData->MaxFortressHealth;
        FortressHealth = LiveData->FortressHealth;
    }

    FString InitRumor = FString::Printf(TEXT("Stronghold checkpoint '%s' stands functional in %s."), *OwningClanID, *RegionID);
    UGameInstance* GI = GetGameInstance();
    UNemesisManager* DB = GI ? GI->GetSubsystem<UNemesisManager>() : nullptr;
    if (DB && FMath::IsFinite(HeatGenerationValue) && HeatGenerationValue >= 0.0f)
    {
        FRumorData Rumor;
        Rumor.Type = ERumorType::Folklore;
        Rumor.SubjectID = TEXT("Fortress_System");
        Rumor.LastKnownLocation = GetActorLocation();
        Rumor.SpreadLevel = HeatGenerationValue;
        Rumor.Fact = InitRumor;
        Rumor.RawRumorText = InitRumor;
        DB->ActiveRumors.Add(Rumor); // Announcement only; no crime/avenger route.
    }
}

float ASiegeFortress::TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, class AController* EventInstigator, AActor* DamageCauser)
{
    if (!HasAuthority() || !FMath::IsFinite(DamageAmount) || DamageAmount < 0.0f || !GetLiveRegionData()) return 0.0f;
    SyncWithBackendDatabase();
    auto ValidHealth = [this]()
    {
        return FMath::IsFinite(FortressHealth) && FMath::IsFinite(MaxFortressHealth) &&
            MaxFortressHealth > 0.0f && FortressHealth > 0.0f && FortressHealth <= MaxFortressHealth;
    };
    if (!ValidHealth()) return 0.0f;
    const float ReportedDamage = Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
    if (!FMath::IsFinite(ReportedDamage) || ReportedDamage <= 0.0f) return 0.0f;
    // Damage delegates may have touched backend health/ownership: reacquire after Super.
    FRegionData* LiveData = GetLiveRegionData();
    if (!LiveData || LiveData->RegionType != ESubRegionType::MilitaryFortress) return 0.0f;
    SyncWithBackendDatabase();
    if (!ValidHealth()) return 0.0f;
    const float ActualDamage = FMath::Min(ReportedDamage, FortressHealth);
    FortressHealth -= ActualDamage;
    LiveData->FortressHealth = FortressHealth;

    FString AttackerClan;
    FString AttackerName;
    auto ResolveAttacker = [this, &AttackerClan, &AttackerName](AActor* Candidate)
    {
        FString CharacterID;
        bool bRonin = false;
        if (const ABaseHunter* Hunter = Cast<ABaseHunter>(Candidate))
            CharacterID = Hunter->HunterDNA.UniqueID;
        else if (const ARoninCharacter* Ronin = Cast<ARoninCharacter>(Candidate))
        {
            CharacterID = Ronin->GetPlayerDNA().UniqueID; // Public API, no protected access.
            bRonin = true;
        }
        else return false;
        UGameInstance* GI = GetGameInstance();
        UNemesisManager* DB = GI ? GI->GetSubsystem<UNemesisManager>() : nullptr;
        if (!DB || CharacterID.IsEmpty()) return false;
        DB->SynchronizeSimulation();
        const FHunterProfile* Profile = DB->ActiveNemeses.Find(CharacterID);
        if (bRonin && !Profile)
        {
            const FHunterProfile* Player = DB->ActiveNemeses.Find(TEXT("PLAYER"));
            if (Player && Player->UniqueID == CharacterID) Profile = Player;
        }
        if (!Profile || !Profile->bIsAlive || Profile->ClanID.IsEmpty()) return false;
        AttackerClan = Profile->ClanID;
        AttackerName = Profile->HunterName;
        return true;
    };
    bool bKnownAttacker = ResolveAttacker(DamageCauser);
    if (!bKnownAttacker && EventInstigator) bKnownAttacker = ResolveAttacker(EventInstigator->GetPawn());
    if (bKnownAttacker && AttackerClan != OwningClanID && FMath::RandRange(0, 100) > 90)
    {
        const FString AttackRumor = FString::Printf(TEXT("The %s clan is actively attacking the %s walls at %s!"), *AttackerClan, *OwningClanID, *RegionID);
        BroadcastSiegeRumor(AttackRumor, 20.0f);
    }
    if (FortressHealth <= 0.0f)
        ClaimFortress(bKnownAttacker ? AttackerClan : TEXT("Neutral"), bKnownAttacker ? AttackerName : TEXT("Unknown Forces"));
    return ActualDamage;
}

void ASiegeFortress::ClaimFortress(const FString& NewClanID, const FString& ConquerorName)
{
    if (!HasAuthority() || NewClanID.IsEmpty()) return;
    UGameInstance* GI = GetGameInstance();
    UNemesisManager* NemesisSys = GI ? GI->GetSubsystem<UNemesisManager>() : nullptr;
    if (!NemesisSys) return;
    FRegionData* LiveData = GetLiveRegionData();
    if (!LiveData || LiveData->RegionType != ESubRegionType::MilitaryFortress) return;
    SyncWithBackendDatabase();
    if (!FMath::IsFinite(MaxFortressHealth) || MaxFortressHealth <= 0.0f ||
        !FMath::IsFinite(FortressHealth) || FortressHealth < 0.0f || FortressHealth > MaxFortressHealth) return;
    NemesisSys->ClaimRegion(RegionID, NewClanID);
    LiveData = GetLiveRegionData();
    if (!LiveData) return;
    // Capture restores the current backend maximum, including purchased additions.
    LiveData->FortressHealth = LiveData->MaxFortressHealth;
    LiveData->Status = ETerritoryStatus::Peace;
    if (NewClanID == TEXT("PLAYER") && FMath::IsFinite(HeatGenerationValue) && HeatGenerationValue >= 0.0f)
        NemesisSys->AddHeat(TEXT("PLAYER"), HeatGenerationValue);
    // Preserve direct tributary ownership propagation.
    for (auto& Pair : NemesisSys->MapRegions)
        if (Pair.Key != RegionID && Pair.Value.RegionType != ESubRegionType::MilitaryFortress &&
            Pair.Value.ParentFortressRegionID == RegionID)
            Pair.Value.ControllingClanID = NewClanID;
    NemesisSys->AggregateSubRegionDataToParents();
    TArray<AActor*> Hubs;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), ASettlementHub::StaticClass(), Hubs);
    for (AActor* Actor : Hubs)
        if (ASettlementHub* Hub = Cast<ASettlementHub>(Actor)) Hub->SyncWithBackendDatabase();
    SyncWithBackendDatabase();
    NemesisSys->SaveWorldState();
    const FString ConquestRumor = FString::Printf(TEXT("WARLORD %s HAS CAPTURED FORTRESS OVERLORD POSITION AT %s!"), *ConquerorName, *RegionID);
    BroadcastSiegeRumor(ConquestRumor, 100.0f);
    OnFortressCaptured(NewClanID);
}

void ASiegeFortress::ProcessStockpileFortificationUpgrades()
{
    if (!HasAuthority()) return;
    FRegionData* LiveData = GetLiveRegionData();
    if (!LiveData || LiveData->RegionType != ESubRegionType::MilitaryFortress) return;
    SyncWithBackendDatabase();
    if (!FMath::IsFinite(MaxFortressHealth) || MaxFortressHealth <= 0.0f ||
        !FMath::IsFinite(FortressHealth) || FortressHealth < 0.0f || FortressHealth > MaxFortressHealth) return;
    FRegionData Working = *LiveData;
    float* Iron = nullptr;
    float* Lumber = nullptr;
    if (!UEconomySystem::ResolvePrimaryResourceBalance(Working, TEXT("Iron"), Iron) ||
        !UEconomySystem::ResolvePrimaryResourceBalance(Working, TEXT("Lumber"), Lumber)) return;
    const double IronAfter = static_cast<double>(*Iron) - 50.0;
    const double LumberAfter = static_cast<double>(*Lumber) - 50.0;
    const double NewMax = static_cast<double>(MaxFortressHealth) + 250.0;
    const double NewHealth = FMath::Min(static_cast<double>(FortressHealth) + 250.0, NewMax);
    auto ExactNonnegative = [](double Value)
    {
        return FMath::IsFinite(Value) && Value >= 0.0 && Value <= MAX_flt &&
            static_cast<double>(static_cast<float>(Value)) == Value;
    };
    // Every debit and health result is checked before any live resource consumption.
    if (!ExactNonnegative(IronAfter) || !ExactNonnegative(LumberAfter) ||
        static_cast<double>(*Iron) - IronAfter != 50.0 || static_cast<double>(*Lumber) - LumberAfter != 50.0 ||
        !ExactNonnegative(NewMax) || NewMax - MaxFortressHealth != 250.0 || !ExactNonnegative(NewHealth)) return;
    *Iron = static_cast<float>(IronAfter);
    *Lumber = static_cast<float>(LumberAfter);
    LiveData->Stockpile = Working.Stockpile;
    LiveData->SharedStockpile = Working.SharedStockpile;
    LiveData->SharedStockpileMetadata = Working.SharedStockpileMetadata;
    LiveData->MaxFortressHealth = static_cast<float>(NewMax);
    LiveData->FortressHealth = static_cast<float>(NewHealth);
    SyncWithBackendDatabase();
    UE_LOG(LogTemp, Log, TEXT("FORTIFICATION: Overlord Fortress %s reinforced! Max Walls scaled to %f"), *RegionID, MaxFortressHealth);
}

void ASiegeFortress::BroadcastSiegeRumor(const FString& EventDescription, float SpreadLevel) {
    if (EventDescription.IsEmpty() || !FMath::IsFinite(SpreadLevel) || SpreadLevel < 0.0f) return;
    if (UGameInstance* GI = GetGameInstance()) {
        if (UNemesisManager* NemesisSys = GI->GetSubsystem<UNemesisManager>()) {
            FRumorData Rumor;
            Rumor.Type = ERumorType::Folklore;
            Rumor.SubjectID = TEXT("Fortress_System");
            Rumor.LastKnownLocation = GetActorLocation();
            Rumor.SpreadLevel = SpreadLevel;
            Rumor.Fact = EventDescription;
            Rumor.RawRumorText = EventDescription;
            NemesisSys->ActiveRumors.Add(Rumor); // Announcements never request an avenger.
        }
    }
}