#include "RoninGameMode.h" // MUST BE THE FIRST INCLUDE
#include "RoninCharacter.h"
#include "NemesisManager.h" 
#include "EngineUtils.h"
#include "AIController.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"
#include "Engine/DataTable.h"
#include "Engine/GameInstance.h"
#include "RoninPlayerController.h"
#include "BaseHunter.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Guid.h"
#include "Engine/LocalPlayer.h"
#include "RoninLocalIdentitySaveGame.h"

ARoninGameMode::ARoninGameMode()
{
    PrimaryActorTick.bCanEverTick = true;
    DefaultPawnClass = ARoninCharacter::StaticClass();
    PlayerControllerClass = ARoninPlayerController::StaticClass();
}

void ARoninGameMode::BeginPlay()
{
    Super::BeginPlay();
    if (!HasAuthority() || !GetGameInstance()) return;

    // =======================================================================
    // AUTHORITATIVE SUBSYSTEM DATA INJECTION PASS
    // =======================================================================
    // Automatically feeds your chosen editor assets into your memory manager subsystem
    if (UNemesisManager* NemesisSystem = GetGameInstance()->GetSubsystem<UNemesisManager>())
    {
        if (AuthoritativeHunterBlueprintClass)
        {
            NemesisSystem->BaseHunterClass = AuthoritativeHunterBlueprintClass;
            UE_LOG(LogTemp, Log, TEXT("GAME MODE INITIALIZATION: Successfully injected BaseHunter blueprint class down into Subsystem registers."));
        }

        if (AuthoritativeRegionSeedingTable && NemesisSystem->MapRegions.Num() == 0)
        {
            NemesisSystem->RegionSeedingTable = AuthoritativeRegionSeedingTable;
            UE_LOG(LogTemp, Log, TEXT("GAME MODE INITIALIZATION: Successfully injected Seeding Data Table down into Subsystem registers."));
        }
    }
}

UNemesisManager* ARoninGameMode::GetNemesisManager() const
{
    return GetGameInstance() ? GetGameInstance()->GetSubsystem<UNemesisManager>() : nullptr;
}

void ARoninGameMode::PostLogin(APlayerController* NewPlayer)
{
    Super::PostLogin(NewPlayer); // Base dispatches HandleStartingNewPlayer and may spawn here.
    BindPlayerAccount(NewPlayer);
}

void ARoninGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
    Super::HandleStartingNewPlayer_Implementation(NewPlayer);
    BindPlayerAccount(NewPlayer);
}

void ARoninGameMode::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!HasAuthority() || bTransferInProgress || !GetWorld() ||
        !FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.f) return;
    UNemesisManager* Manager = GetNemesisManager();
    if (!Manager) return;
    Manager->AdvanceWorldClock(DeltaSeconds,
        FMath::IsFinite(WorldTimeMultiplier) ? FMath::Max(0.f, WorldTimeMultiplier) : 1.f);
    PopulationScanElapsed += DeltaSeconds;
    const float Interval = FMath::IsFinite(PopulationScanInterval) ? FMath::Max(0.05f, PopulationScanInterval) : 1.f;
    if (PopulationScanElapsed < Interval) return;
    PopulationScanElapsed = FMath::Fmod(PopulationScanElapsed, Interval);

    TArray<TWeakObjectPtr<APawn>> PlayerPawns;
    for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
    {
        APlayerController* PC = It->Get();
        if (!IsValid(PC)) continue;
        BindPlayerAccount(PC); // Handles delayed/custom RestartPlayer spawns too.
        if (!GetAuthorizedCharacterID(PC).IsEmpty())
        {
            if (ARoninCharacter* Pawn = Cast<ARoninCharacter>(PC->GetPawn())) MergeLiveProfile(Pawn, Manager);
            if (IsValid(PC->GetPawn())) PlayerPawns.Add(PC->GetPawn());
        }
    }
    // Offline bodies still own their live vitals; the registry owns their social/economic state.
    for (TActorIterator<ARoninCharacter> It(GetWorld()); It; ++It)
        if (It->PlayerDNA.bIsRealPlayer && !It->IsPlayerControlled()) MergeLiveProfile(*It, Manager);
    for (const TWeakObjectPtr<APawn>& Pawn : PlayerPawns)
        if (Pawn.IsValid()) Manager->ScanAndManagePhysicalPopulation(Pawn.Get());
}

FString ARoninGameMode::ResolveAccount(APlayerController* PlayerPC)
{
    if (!HasAuthority() || !IsValid(PlayerPC) || PlayerPC->GetWorld() != GetWorld()) return FString();
    const TWeakObjectPtr<APlayerController> Key(PlayerPC);
    // Never switch an established account if the online subsystem becomes ready later.
    if (const FString* Existing = SessionAccounts.Find(Key)) return *Existing;
    FString Account;
    if (PlayerPC->PlayerState && PlayerPC->PlayerState->GetUniqueId().IsValid())
    {
        // Authentication of this ID belongs to the configured online login service.
        Account = PlayerPC->PlayerState->GetUniqueId().ToString();
    }
    else
    {
        ULocalPlayer* Local = PlayerPC->IsLocalController() ? PlayerPC->GetLocalPlayer() : nullptr;
        if (!Local) return FString(); // Remote GUID claims are not authentication.
        const int32 LocalSlot = Local->GetLocalPlayerIndex();
        const int32 UserIndex = Local->GetPlatformUserIndex();
        if (LocalSlot < 0 || UserIndex < 0) return FString();
        const FString Slot = FString::Printf(TEXT("RoninLocalIdentity_%d"), LocalSlot);
        URoninLocalIdentitySaveGame* Identity = nullptr;
        if (UGameplayStatics::DoesSaveGameExist(Slot, UserIndex))
        {
            Identity = Cast<URoninLocalIdentitySaveGame>(UGameplayStatics::LoadGameFromSlot(Slot, UserIndex));
            if (!Identity || !Identity->IsIdentityValid(LocalSlot))
            {
                UE_LOG(LogTemp, Error, TEXT("Local identity slot is unreadable or invalid; account binding denied."));
                return FString(); // Never overwrite an existing bad slot.
            }
        }
        else
        {
            Identity = Cast<URoninLocalIdentitySaveGame>(UGameplayStatics::CreateSaveGameObject(URoninLocalIdentitySaveGame::StaticClass()));
            if (!Identity) return FString();
            Identity->IdentityVersion = 1;
            Identity->LocalPlayerSlot = LocalSlot;
            Identity->AccountGuid = FGuid::NewGuid();
            if (!UGameplayStatics::SaveGameToSlot(Identity, Slot, UserIndex)) return FString();
            const FGuid Written = Identity->AccountGuid;
            Identity = Cast<URoninLocalIdentitySaveGame>(UGameplayStatics::LoadGameFromSlot(Slot, UserIndex));
            if (!Identity || !Identity->IsIdentityValid(LocalSlot) || Identity->AccountGuid != Written) return FString();
        }
        Account = FString(TEXT("LOCAL:")) + Identity->AccountGuid.ToString(EGuidFormats::DigitsWithHyphens);
    }
    if (Account.IsEmpty()) return FString();
    for (const auto& Pair : SessionAccounts)
        if (Pair.Key.IsValid() && Pair.Key != Key && Pair.Value == Account) return FString();
    SessionAccounts.Add(Key, Account);
    return Account;
}

FString ARoninGameMode::GetAuthorizedCharacterID(APlayerController* PlayerPC) const
{
    if (!HasAuthority() || !IsValid(PlayerPC) || PlayerPC->GetWorld() != GetWorld()) return FString();
    const TWeakObjectPtr<APlayerController> Key(PlayerPC);
    const FString* Account = SessionAccounts.Find(Key);
    const ARoninCharacter* Pawn = Cast<ARoninCharacter>(PlayerPC->GetPawn());
    if (!Account || Account->IsEmpty() || !IsValid(Pawn) || Pawn->GetController() != PlayerPC ||
        BoundPlayerPawns.FindRef(Key).Get() != Pawn || Pawn->PlayerDNA.UniquePlayerAccountID != *Account ||
        Pawn->PlayerDNA.UniqueID.IsEmpty() || Pawn->PlayerDNA.UniqueID == TEXT("PLAYER")) return FString();
    return Pawn->PlayerDNA.UniqueID;
}

bool ARoninGameMode::IsPlayerPossessedID(const FString& ID) const
{
    if (!GetWorld() || ID.IsEmpty()) return false;
    for (TActorIterator<ARoninCharacter> It(GetWorld()); It; ++It)
        if (IsValid(*It) && It->PlayerDNA.UniqueID == ID && It->IsPlayerControlled()) return true;
    for (TActorIterator<ABaseHunter> It(GetWorld()); It; ++It)
        if (IsValid(*It) && It->HunterDNA.UniqueID == ID && It->IsPlayerControlled()) return true;
    return false;
}

ARoninCharacter* ARoninGameMode::FindLivingRonin(const FString& ID) const
{
    if (!GetWorld() || ID.IsEmpty()) return nullptr;
    for (TActorIterator<ARoninCharacter> It(GetWorld()); It; ++It)
        if (IsValid(*It) && It->PlayerDNA.UniqueID == ID && It->PlayerDNA.bIsAlive &&
            !It->bHasInitiatedDeath && !It->bRestoringCombatState && !It->bReplayingCombat &&
            FMath::IsFinite(It->PlayerDNA.Blood) && It->PlayerDNA.Blood > 0.f && !It->IsPlayerControlled()) return *It;
    return nullptr;
}

void ARoninGameMode::MergeLiveProfile(ARoninCharacter* Pawn, UNemesisManager* Manager)
{
    if (!IsValid(Pawn) || !Manager || Pawn->bRestoringCombatState || Pawn->bReplayingCombat) return;
    const FHunterProfile Live = Pawn->GetPlayerDNA();
    if (Live.UniqueID.IsEmpty() || Live.UniqueID == TEXT("PLAYER")) return;
    Manager->SynchronizeSimulation();
    FHunterProfile Merged = Live;
    const bool bExisting = Manager->GetHunterProfile(Live.UniqueID, Merged);
    auto FiniteOr = [](float Value, float Fallback) { return FMath::IsFinite(Value) ? Value : Fallback; };
    Merged.Blood = FiniteOr(Live.Blood, Merged.Blood);
    Merged.Stamina = FiniteOr(Live.Stamina, Merged.Stamina);
    Merged.Potential = FiniteOr(Live.Potential, Merged.Potential);
    Merged.Hunger = FiniteOr(Live.Hunger, Merged.Hunger);
    Merged.Thirst = FiniteOr(Live.Thirst, Merged.Thirst);
    Merged.Karma = FMath::Clamp(FiniteOr(Pawn->CurrentKarma, Merged.Karma), -100.f, 100.f);
    Merged.Heat = FMath::Clamp(FiniteOr(Pawn->CurrentHeat, Merged.Heat), 0.f, 100.f);
    if (!Pawn->GetActorLocation().ContainsNaN()) Merged.WorldLocation = Pawn->GetActorLocation();
    Merged.bIsAlive = (!bExisting || Merged.bIsAlive) && Live.bIsAlive && !Pawn->bHasInitiatedDeath && Merged.Blood > 0.f;
    Merged.bIsRealPlayer = Live.bIsRealPlayer;
    Merged.bIsOfflineAI = Live.bIsOfflineAI;
    Merged.UniquePlayerAccountID = Live.UniquePlayerAccountID;
    Pawn->PlayerDNA = Merged;
    Manager->SyncPlayerProfile(Merged);
}

void ARoninGameMode::RetirePhysicalDuplicate(const FString& ID, UNemesisManager* Manager)
{
    if (!Manager) return;
    // Do not use DeactivateHunter here: even its location/death merge can overwrite
    // the freshly possessed successor. Remove only this tracked physical duplicate.
    ABaseHunter* Duplicate = Manager->SpawnedPhysicalHunters.FindRef(ID);
    if (IsValid(Duplicate) && Duplicate->IsPlayerControlled()) return;
    Manager->SpawnedPhysicalHunters.Remove(ID);
    if (IsValid(Duplicate))
    {
        if (AController* Controller = Duplicate->GetController())
        {
            Controller->UnPossess();
            if (AAIController* AI = Cast<AAIController>(Controller)) AI->Destroy();
        }
        Duplicate->Destroy();
    }
}

ARoninCharacter* ARoninGameMode::TransferToProfile(APlayerController* PlayerPC,
    const FHunterProfile& Profile, const FTransform& SpawnTransform)
{
    if (!IsValid(PlayerPC) || !GetWorld() || Profile.UniqueID.IsEmpty() ||
        !Profile.bIsAlive || !FMath::IsFinite(Profile.Blood) || Profile.Blood <= 0.f ||
        IsPlayerPossessedID(Profile.UniqueID) || SpawnTransform.ContainsNaN() ||
        !SpawnTransform.GetRotation().IsNormalized()) return nullptr;
    for (TActorIterator<ARoninCharacter> It(GetWorld()); It; ++It)
    {
        if (!IsValid(*It) || It->PlayerDNA.UniqueID != Profile.UniqueID) continue;
        const FHunterProfile Live = It->GetPlayerDNA();
        if (!Live.bIsAlive || It->bHasInitiatedDeath || It->bRestoringCombatState || It->bReplayingCombat ||
            !FMath::IsFinite(Live.Blood) || Live.Blood <= 0.f ||
            (!Live.UniquePlayerAccountID.IsEmpty() && Live.UniquePlayerAccountID != Profile.UniquePlayerAccountID) ||
            (Live.bIsRealPlayer && Live.UniquePlayerAccountID != Profile.UniquePlayerAccountID)) return nullptr;
    }
    APawn* PreviousPawn = PlayerPC->GetPawn();
    ARoninCharacter* Target = FindLivingRonin(Profile.UniqueID);
    const bool bSpawned = !Target;
    if (!Target)
    {
        UClass* PawnClass = SuccessorPawnClass.Get();
        if (!PawnClass && DefaultPawnClass && DefaultPawnClass->IsChildOf(ARoninCharacter::StaticClass()))
            PawnClass = DefaultPawnClass.Get();
        if (!PawnClass || PawnClass->HasAnyClassFlags(CLASS_Abstract)) return nullptr;
        Target = GetWorld()->SpawnActorDeferred<ARoninCharacter>(PawnClass, SpawnTransform, nullptr, nullptr,
            ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
        if (!IsValid(Target)) return nullptr;
        // No inherited identity during BeginPlay: it may auto-register nonempty DNA.
        Target->PlayerDNA = FHunterProfile();
        UGameplayStatics::FinishSpawningActor(Target, SpawnTransform);
        if (!IsValid(Target)) return nullptr;
    }
    if (Target->bHasInitiatedDeath || Target->bRestoringCombatState || Target->bReplayingCombat || Target->IsPlayerControlled())
    {
        if (bSpawned) Target->Destroy();
        return nullptr;
    }
    const FHunterProfile PreviousDNA = Target->GetPlayerDNA();
    TWeakObjectPtr<AController> PreviousController(Target->GetController());
    FHunterProfile Prepared = Profile;
    Prepared.WorldLocation = Target->GetActorLocation();
    Target->InitializePlayerDNA(Prepared);
    if (!IsValid(Target) || Target->PlayerDNA.UniqueID != Profile.UniqueID || Target->bHasInitiatedDeath)
    {
        if (IsValid(Target))
        {
            if (bSpawned) Target->Destroy();
            else Target->InitializePlayerDNA(PreviousDNA);
        }
        if (bSpawned && PreviousController.IsValid())
            if (AAIController* AI = Cast<AAIController>(PreviousController.Get())) AI->Destroy();
        return nullptr;
    }
    PlayerPC->Possess(Target);
    if (!IsValid(Target) || PlayerPC->GetPawn() != Target || Target->GetController() != PlayerPC)
    {
        if (IsValid(Target))
        {
            if (Target->GetController() == PlayerPC) PlayerPC->UnPossess();
            if (!bSpawned)
            {
                Target->InitializePlayerDNA(PreviousDNA);
                if (PreviousController.IsValid()) PreviousController->Possess(Target);
            }
            else Target->Destroy();
        }
        if (bSpawned && PreviousController.IsValid())
            if (AAIController* AI = Cast<AAIController>(PreviousController.Get())) AI->Destroy();
        if (IsValid(PreviousPawn) && PlayerPC->GetPawn() != PreviousPawn) PlayerPC->Possess(PreviousPawn);
        return nullptr;
    }
    if (PreviousController.IsValid() && PreviousController.Get() != PlayerPC)
        if (AAIController* AI = Cast<AAIController>(PreviousController.Get())) AI->Destroy();
    return Target;
}

void ARoninGameMode::BindPlayerAccount(APlayerController* PlayerPC)
{
    if (!HasAuthority() || bTransferInProgress || !IsValid(PlayerPC)) return;
    ARoninCharacter* Pawn = Cast<ARoninCharacter>(PlayerPC->GetPawn());
    UNemesisManager* Manager = GetNemesisManager();
    if (!IsValid(Pawn) || !Manager || Pawn->bRestoringCombatState || Pawn->bReplayingCombat ||
        Pawn->bHasInitiatedDeath || !Pawn->PlayerDNA.bIsAlive) return;
    const FString Account = ResolveAccount(PlayerPC);
    if (Account.IsEmpty()) return;
    const TWeakObjectPtr<APlayerController> Key(PlayerPC);
    if (BoundPlayerPawns.FindRef(Key).Get() == Pawn && Pawn->PlayerDNA.UniquePlayerAccountID == Account) return;
    TGuardValue<bool> TransferGuard(bTransferInProgress, true);
    Manager->SynchronizeSimulation();

    FHunterProfile Offline;
    bool bFoundOffline = false;
    for (const auto& Pair : Manager->ActiveNemeses)
    {
        const FHunterProfile& P = Pair.Value;
        if (Pair.Key == TEXT("PLAYER") || Pair.Key != P.UniqueID || P.UniqueID == Pawn->PlayerDNA.UniqueID ||
            P.UniquePlayerAccountID != Account || !P.bIsRealPlayer || !P.bIsAlive ||
            !FMath::IsFinite(P.Blood) || P.Blood <= 0.f || IsPlayerPossessedID(P.UniqueID)) continue;
        if (!bFoundOffline || P.UniqueID < Offline.UniqueID) { Offline = P; bFoundOffline = true; }
    }
    if (bFoundOffline)
    {
        if (ARoninCharacter* Existing = FindLivingRonin(Offline.UniqueID))
        {
            MergeLiveProfile(Existing, Manager);
            Manager->GetHunterProfile(Offline.UniqueID, Offline);
        }
        Offline.bIsRealPlayer = true;
        Offline.bIsOfflineAI = false;
        const FTransform RestoreTransform(Pawn->GetActorRotation(), Offline.WorldLocation);
        ARoninCharacter* Restored = TransferToProfile(PlayerPC, Offline, RestoreTransform);
        if (!Restored) return; // Retry without changing either canonical record.
        Manager->SyncPlayerProfile(Restored->GetPlayerDNA());
        RetirePhysicalDuplicate(Offline.UniqueID, Manager);
        BoundPlayerPawns.Add(Key, TWeakObjectPtr<ARoninCharacter>(Restored));
        // The temporary login pawn was never registered by this binding path.
        Pawn->Destroy();
        Manager->SaveWorldState();
        return;
    }

    FHunterProfile Profile = Pawn->GetPlayerDNA();
    if (IsPlayerPossessedID(Profile.UniqueID))
    {
        for (TActorIterator<ARoninCharacter> It(GetWorld()); It; ++It)
            if (*It != Pawn && It->IsPlayerControlled() && It->PlayerDNA.UniqueID == Profile.UniqueID) return;
    }
    FHunterProfile Canonical;
    const bool bHasCanonical = !Profile.UniqueID.IsEmpty() && Manager->GetHunterProfile(Profile.UniqueID, Canonical);
    if (bHasCanonical && !Canonical.UniquePlayerAccountID.IsEmpty() && Canonical.UniquePlayerAccountID != Account) return;
    if (Profile.UniqueID.IsEmpty() || Profile.UniqueID == TEXT("PLAYER"))
    {
        do { Profile.UniqueID = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens); }
        while (Manager->ActiveNemeses.Contains(Profile.UniqueID));
    }
    if (bHasCanonical)
    {
        const FString StableID = Profile.UniqueID;
        Profile = Canonical; // Keep inventory, relationships, clan and lineage.
        Profile.UniqueID = StableID;
    }
    Profile.UniquePlayerAccountID = Account;
    Profile.bIsRealPlayer = true;
    Profile.bIsOfflineAI = false;
    Profile.WorldLocation = Pawn->GetActorLocation();
    Pawn->InitializePlayerDNA(Profile);
    MergeLiveProfile(Pawn, Manager);
    BoundPlayerPawns.Add(Key, TWeakObjectPtr<ARoninCharacter>(Pawn));
    Manager->SaveWorldState();
}

TSubclassOf<AAIController> ARoninGameMode::SelectOfflineController(const FHunterProfile& Profile) const
{
    const FRoninPersona Persona = FRoninPersona::FromProfile(Profile);
    for (const FRoninOfflineControllerRule& Rule : OfflineControllerRules)
    {
        if (Rule.ControllerClass && !Rule.ControllerClass->HasAnyClassFlags(CLASS_Abstract) &&
            Persona.Karma >= Rule.MinKarma && Persona.Karma <= Rule.MaxKarma &&
            Persona.Heat >= Rule.MinHeat && Persona.Heat <= Rule.MaxHeat &&
            Persona.Fame >= Rule.MinFame && Persona.Fame <= Rule.MaxFame) return Rule.ControllerClass;
    }
    return NemesisAIControllerClass && !NemesisAIControllerClass->HasAnyClassFlags(CLASS_Abstract)
        ? NemesisAIControllerClass : TSubclassOf<AAIController>();
}

void ARoninGameMode::Logout(AController* Exiting)
{
    APlayerController* PC = Cast<APlayerController>(Exiting);
    PreparePlayerLogout(PC);
    if (PC) PreparedLogoutPlayers.Remove(TWeakObjectPtr<APlayerController>(PC));
    Super::Logout(Exiting);
}

void ARoninGameMode::PreparePlayerLogout(APlayerController* PlayerPC)
{
    if (!HasAuthority() || bTransferInProgress || !IsValid(PlayerPC)) return;
    const TWeakObjectPtr<APlayerController> PreparedKey(PlayerPC);
    if (PreparedLogoutPlayers.Contains(PreparedKey)) return;
    const FString* SessionAccount = SessionAccounts.Find(PreparedKey);
    if (!SessionAccount || SessionAccount->IsEmpty()) return;
    const FString LogoutAccount = *SessionAccount;
    PreparedLogoutPlayers.Add(PreparedKey);
    AController* Exiting = PlayerPC;
    APlayerController* PC = PlayerPC;
    ARoninCharacter* Pawn = IsValid(Exiting) ? Cast<ARoninCharacter>(Exiting->GetPawn()) : nullptr;
    if (!IsValid(Pawn) && PC)
        Pawn = BoundPlayerPawns.FindRef(TWeakObjectPtr<APlayerController>(PC)).Get();
    if (IsValid(Pawn) && Pawn->GetController() && Pawn->GetController() != Exiting)
        Pawn = nullptr; // Never take a body that another controller now owns.
    UNemesisManager* Manager = GetNemesisManager();
    if (HasAuthority() && !bTransferInProgress && PC && Manager && !IsValid(Pawn))
    {
        // Some engine teardown paths destroy the pawn before GameMode::Logout.
        // Preserve the last synchronized account record; never resurrect that body here.
        TGuardValue<bool> TransferGuard(bTransferInProgress, true);
        const FString Account = LogoutAccount;
        Manager->SynchronizeSimulation();
        TArray<FHunterProfile> Records;
        for (const auto& Pair : Manager->ActiveNemeses)
            if (Pair.Key != TEXT("PLAYER") && Pair.Value.UniquePlayerAccountID == Account && Pair.Value.bIsRealPlayer)
            {
                FHunterProfile Record = Pair.Value;
                ARoninCharacter* SurvivingBody = FindLivingRonin(Record.UniqueID);
                Record.bIsOfflineAI = SurvivingBody && Cast<AAIController>(SurvivingBody->GetController()) != nullptr;
                Records.Add(Record);
            }
        for (const FHunterProfile& Record : Records) Manager->SyncPlayerProfile(Record);
        Manager->SaveWorldState();
        UE_LOG(LogTemp, Warning, TEXT("Logout reached GameMode after pawn teardown; saved last synchronized account state. Earlier PlayerController handoff is required to retain the body."));
    }
    if (HasAuthority() && !bTransferInProgress && IsValid(Exiting) && IsValid(Pawn) && Manager &&
        !Pawn->bRestoringCombatState && !Pawn->bReplayingCombat)
    {
        TGuardValue<bool> TransferGuard(bTransferInProgress, true);
        if (BoundPlayerPawns.FindRef(PreparedKey).Get() != Pawn ||
            Pawn->PlayerDNA.UniquePlayerAccountID != LogoutAccount) return;
        Pawn->PlayerDNA.UniquePlayerAccountID = LogoutAccount;
        if (Pawn->PlayerDNA.UniqueID.IsEmpty() || Pawn->PlayerDNA.UniqueID == TEXT("PLAYER"))
        {
            Manager->SynchronizeSimulation();
            do { Pawn->PlayerDNA.UniqueID = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens); }
            while (Manager->ActiveNemeses.Contains(Pawn->PlayerDNA.UniqueID));
        }
        Pawn->PlayerDNA.bIsRealPlayer = true;
        MergeLiveProfile(Pawn, Manager);
        const TSubclassOf<AAIController> AIClass = SelectOfflineController(Pawn->GetPlayerDNA());
        AAIController* AI = nullptr;
        // Spawn before releasing the player. Never invent a default AI class.
        if (AIClass && Pawn->PlayerDNA.bIsAlive && !Pawn->bHasInitiatedDeath && GetWorld())
        {
            FActorSpawnParameters Params;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
            AI = GetWorld()->SpawnActor<AAIController>(AIClass, Pawn->GetActorLocation(), Pawn->GetActorRotation(), Params);
        }
        bool bAIHasPawn = false;
        if (IsValid(AI))
        {
            Exiting->UnPossess();
            AI->Possess(Pawn);
            bAIHasPawn = IsValid(Pawn) && AI->GetPawn() == Pawn && Pawn->GetController() == AI;
            if (!bAIHasPawn)
            {
                AI->Destroy();
                if (IsValid(Pawn)) Exiting->Possess(Pawn);
            }
        }
        if (IsValid(Pawn))
        {
            // No configured/working AI means an unpossessed persistent body, not an AI flag lie.
            Pawn->PlayerDNA.bIsOfflineAI = bAIHasPawn;
            MergeLiveProfile(Pawn, Manager);
            // Detach from the departing PC even after rollback. PlayerController teardown
            // must not treat the offline body as its PawnLeavingGame destruction target.
            if (Exiting->GetPawn() == Pawn) Exiting->UnPossess();
        }
        Manager->SaveWorldState();
    }
    if (PC)
    {
        const TWeakObjectPtr<APlayerController> Key(PC);
        BoundPlayerPawns.Remove(Key);
        SessionAccounts.Remove(Key);
    }
}

void ARoninGameMode::ReincarnateAsHeir(APlayerController* PlayerPC, FHunterProfile FallenDNA, FString ChosenHeirID)
{
    if (!HasAuthority() || bTransferInProgress || !IsValid(PlayerPC) || !GetWorld()) return;
    ARoninCharacter* FallenPawn = Cast<ARoninCharacter>(PlayerPC->GetPawn());
    UNemesisManager* Manager = GetNemesisManager();
    if (!IsValid(FallenPawn) || !Manager || FallenPawn->bRestoringCombatState || FallenPawn->bReplayingCombat ||
        (!FallenPawn->bHasInitiatedDeath && FallenPawn->PlayerDNA.bIsAlive)) return;
    TGuardValue<bool> TransferGuard(bTransferInProgress, true);
    Manager->SynchronizeSimulation();
    // Caller-supplied FallenDNA is compatibility input only, never authority.
    const FHunterProfile LiveFallen = FallenPawn->GetPlayerDNA();
    if (LiveFallen.UniqueID.IsEmpty() || LiveFallen.UniqueID == TEXT("PLAYER")) return;
    if (!Manager->GetHunterProfile(LiveFallen.UniqueID, FallenDNA) ||
        FallenDNA.UniqueID != LiveFallen.UniqueID) return;
    const FString Account = ResolveAccount(PlayerPC);
    if (Account.IsEmpty() || GetAuthorizedCharacterID(PlayerPC) != LiveFallen.UniqueID) return;
    if (FallenDNA.UniquePlayerAccountID != Account) return;
    FallenDNA.UniquePlayerAccountID = Account;
    FallenDNA.Karma = FMath::IsFinite(FallenPawn->CurrentKarma) ?
        FMath::Clamp(FallenPawn->CurrentKarma, -100.f, 100.f) : FRoninPersona::ClampSigned(FallenDNA.Karma);
    FallenDNA.bIsAlive = false;
    FallenDNA.bIsOfflineAI = false;
    FallenDNA.WorldLocation = FallenPawn->GetActorLocation();
    FallenDNA.Blood = FMath::IsFinite(LiveFallen.Blood) ? FMath::Max(0.f, LiveFallen.Blood) : 0.f;
    FallenDNA.Stamina = FMath::IsFinite(LiveFallen.Stamina) ? LiveFallen.Stamina : FallenDNA.Stamina;
    FallenDNA.Potential = FMath::IsFinite(LiveFallen.Potential) ? LiveFallen.Potential : FallenDNA.Potential;
    FallenDNA.Hunger = FMath::IsFinite(LiveFallen.Hunger) ? LiveFallen.Hunger : FallenDNA.Hunger;
    FallenDNA.Thirst = FMath::IsFinite(LiveFallen.Thirst) ? LiveFallen.Thirst : FallenDNA.Thirst;
    FallenDNA.Heat = FMath::IsFinite(FallenPawn->CurrentHeat) ? FMath::Clamp(FallenPawn->CurrentHeat, 0.f, 100.f) : 0.f;
    FallenDNA.bIsRealPlayer = true;

    auto ReadEligible = [this, Manager, &FallenDNA, &Account](const FString& ID, FHunterProfile& Out) -> bool
    {
        const FRelationshipData* Link = FallenDNA.RelationshipWeb.Find(ID);
        if (!Link || (Link->TieType != EFamilyTie::Student && Link->TieType != EFamilyTie::Spouse &&
            Link->TieType != EFamilyTie::Child) || ID.IsEmpty() || ID == TEXT("PLAYER") ||
            ID == FallenDNA.UniqueID || !Manager->GetHunterProfile(ID, Out) || Out.UniqueID != ID ||
            !Out.bIsAlive || !FMath::IsFinite(Out.Blood) || Out.Blood <= 0.f || IsPlayerPossessedID(ID)) return false;
        if ((!Out.UniquePlayerAccountID.IsEmpty() && Out.UniquePlayerAccountID != Account) ||
            (Out.bIsRealPlayer && Out.UniquePlayerAccountID != Account)) return false;
        // A loaded corpse or differently owned actor is not a living heir, even if its DB snapshot is stale.
        for (TActorIterator<ARoninCharacter> It(GetWorld()); It; ++It)
        {
            if (!IsValid(*It) || It->PlayerDNA.UniqueID != ID) continue;
            const FHunterProfile Live = It->GetPlayerDNA();
            if (!Live.bIsAlive || It->bHasInitiatedDeath || It->bRestoringCombatState || It->bReplayingCombat ||
                !FMath::IsFinite(Live.Blood) || Live.Blood <= 0.f ||
                (!Live.UniquePlayerAccountID.IsEmpty() && Live.UniquePlayerAccountID != Account)) return false;
            Out.Blood = Live.Blood;
            Out.Stamina = Live.Stamina;
            Out.Potential = Live.Potential;
            Out.Hunger = Live.Hunger;
            Out.Thirst = Live.Thirst;
            Out.WorldLocation = It->GetActorLocation();
        }
        if (ABaseHunter* Physical = Manager->SpawnedPhysicalHunters.FindRef(ID))
        {
            if (IsValid(Physical))
            {
                const FHunterProfile PhysicalDNA = Physical->HunterDNA;
                if (!PhysicalDNA.bIsAlive || !FMath::IsFinite(PhysicalDNA.Blood) || PhysicalDNA.Blood <= 0.f ||
                    Physical->IsPlayerControlled() ||
                    (!PhysicalDNA.UniquePlayerAccountID.IsEmpty() && PhysicalDNA.UniquePlayerAccountID != Account) ||
                    (PhysicalDNA.bIsRealPlayer && PhysicalDNA.UniquePlayerAccountID != Account)) return false;
                Out.Blood = PhysicalDNA.Blood;
                Out.Stamina = PhysicalDNA.Stamina;
                Out.Potential = PhysicalDNA.Potential;
                Out.Hunger = PhysicalDNA.Hunger;
                Out.Thirst = PhysicalDNA.Thirst;
                Out.WorldLocation = Physical->GetActorLocation();
            }
        }
        return true;
    };

    FString BestID;
    int32 BestAffinity = MIN_int32;
    FHunterProfile Candidate;
    for (const auto& Link : FallenDNA.RelationshipWeb)
    {
        FHunterProfile Eligible;
        if (ReadEligible(Link.Key, Eligible) && (BestID.IsEmpty() || Link.Value.Affinity > BestAffinity ||
            (Link.Value.Affinity == BestAffinity && Link.Key < BestID)))
        {
            BestID = Link.Key;
            BestAffinity = Link.Value.Affinity;
            Candidate = Eligible;
        }
    }
    const bool bDrifterRequested = ChosenHeirID == TEXT("Nameless_Drifter");
    if (!ChosenHeirID.IsEmpty() && !bDrifterRequested)
    {
        if (!ReadEligible(ChosenHeirID, Candidate)) return; // Explicit invalid selection never falls back.
    }
    else if (bDrifterRequested && !BestID.IsEmpty()) return; // Cannot bypass a living dynasty.
    else if (BestID.IsEmpty())
    {
        Candidate = FHunterProfile(); // Local only: generator would write the DB prematurely.
        do { Candidate.UniqueID = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens); }
        while (Manager->ActiveNemeses.Contains(Candidate.UniqueID));
        Candidate.HunterName = TEXT("Nameless Drifter");
        Candidate.Wealth = 5;
        Candidate.PowerLevel = 1;
        Candidate.Blood = 100.f;
        Candidate.Stamina = 100.f;
    }

    Candidate.Karma = FallenDNA.Karma;
    const float OwnFame = FMath::IsFinite(Candidate.InheritedFame) ? Candidate.InheritedFame : 0.f;
    const float AncestorFame = FMath::IsFinite(FallenDNA.InheritedFame) ? FallenDNA.InheritedFame : 0.f;
    const float AncestorNotoriety = FMath::IsFinite(FallenDNA.Notoriety) ? FallenDNA.Notoriety : 0.f;
    Candidate.InheritedFame = FMath::Max(OwnFame, FMath::Max(AncestorFame, AncestorNotoriety));
    Candidate.LegacyGeneration = FMath::Max(1, FallenDNA.LegacyGeneration);
    if (Candidate.LegacyGeneration < MAX_int32) ++Candidate.LegacyGeneration;
    Candidate.UniquePlayerAccountID = Account;
    Candidate.bIsRealPlayer = true;
    Candidate.bIsOfflineAI = false;
    Candidate.bIsAlive = true;
    FHunterMemory Memory;
    Memory.ActionLabel = FString::Printf(TEXT("I carry the legacy of %s."), *FallenDNA.HunterName);
    Memory.TimeStamp = FDateTime::Now();
    Memory.GenerationAtTime = Candidate.LegacyGeneration;
    Candidate.MemoryBank.Add(Memory);
    const FTransform SpawnTransform = bUseSuccessionShrine ? SuccessionShrineTransform : FallenPawn->GetActorTransform();
    ARoninCharacter* Heir = TransferToProfile(PlayerPC, Candidate, SpawnTransform);
    if (!Heir) return; // Neither registry nor old possession was committed.

    Manager->SynchronizeSimulation();
    // Snapshot alias ownership before inserting anything; no borrowed map pointer survives an insertion.
    const FHunterProfile Alias = Manager->ActiveNemeses.FindRef(TEXT("PLAYER"));
    const bool bTransferPrimaryAlias = Alias.UniqueID == FallenDNA.UniqueID;
    Manager->ActiveNemeses.Add(FallenDNA.UniqueID, FallenDNA);
    Manager->ActiveNemeses.Add(Candidate.UniqueID, Heir->GetPlayerDNA());
    if (bTransferPrimaryAlias) Manager->ActiveNemeses.Add(TEXT("PLAYER"), Heir->GetPlayerDNA());
    RetirePhysicalDuplicate(Candidate.UniqueID, Manager);
    BoundPlayerPawns.Add(TWeakObjectPtr<APlayerController>(PlayerPC), TWeakObjectPtr<ARoninCharacter>(Heir));
    if (IsValid(FallenPawn) && FallenPawn != Heir) FallenPawn->Destroy();
    Manager->SaveWorldState();
}