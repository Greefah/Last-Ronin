#include "RoninCharacter.h"

#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "AbilitySystemComponent.h"
#include "RoninAttributeSet.h"
#include "DialogueSystem.h"
#include "Engine/GameInstance.h"
#include "NemesisManager.h"
#include "EconomySystem.h"
#include "RoninEconomyAccess.h"
#include "BaseHunter.h"
#include "SiegeFortress.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraComponent.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Net/UnrealNetwork.h"
#include "RoninGameMode.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameplayEffect.h"

ARoninCharacter::ARoninCharacter() {
    PrimaryActorTick.bCanEverTick = true;
    CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
    CameraBoom->SetupAttachment(RootComponent);
    CameraBoom->TargetArmLength = 400.0f;
    CameraBoom->bUsePawnControlRotation = true;

    FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
    FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);

    bReplicates = true;
    AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
    AbilitySystemComponent->SetIsReplicated(true);
    AttributeSet = CreateDefaultSubobject<URoninAttributeSet>(TEXT("AttributeSet"));
}

void ARoninCharacter::BeginPlay() {
    Super::BeginPlay();

    if (AbilitySystemComponent) {
        AbilitySystemComponent->InitAbilityActorInfo(this, this);
        AbilitySystemComponent->AddSpawnedAttribute(AttributeSet);
    }

    CombatHistory.Configure(FMath::Clamp(CombatHistoryCapacity, 8, 3600));
    CombatHistory.RecordSnapshot(CaptureCombatSnapshot());
    RebindAttributeDelegates();
    RefreshInputMapping();
    SetCurrentStance(CurrentStance);
    RefreshHUDState();

    if (HasAuthority() && GetGameInstance()) {
        if (UNemesisManager* Manager = GetGameInstance()->GetSubsystem<UNemesisManager>()) {
            Manager->SynchronizeSimulation();
            SyncLiveVitals();
            Manager->SyncPlayerProfile(PlayerDNA);
        }
    }
}

void ARoninCharacter::InitializePlayerDNA(FHunterProfile InheritedDNA) {
    if (!HasAuthority() || bRestoringCombatState || bReplayingCombat || InheritedDNA.UniqueID.IsEmpty()) return;
    PlayerDNA = InheritedDNA;
    if (AttributeSet) {
        AttributeSet->SetBlood(FMath::IsFinite(PlayerDNA.Blood) ? FMath::Clamp(PlayerDNA.Blood, 0.f, 1000.f) : 100.f);
        AttributeSet->SetStamina(FMath::IsFinite(PlayerDNA.Stamina) ? FMath::Clamp(PlayerDNA.Stamina, 0.f, FMath::Max(0.f, AttributeSet->GetMaxStamina())) : 0.f);
        AttributeSet->SetPotential(FMath::IsFinite(PlayerDNA.Potential) ? FMath::Clamp(PlayerDNA.Potential, 0.f, 100.f) : 0.f);
        AttributeSet->SetPosture(0.f);
    }
    CurrentKarma = FMath::IsFinite(PlayerDNA.Karma) ? FMath::Clamp(PlayerDNA.Karma, -100.f, 100.f) : 0.f;
    SetCurrentHeat(FMath::IsFinite(PlayerDNA.Heat) ? PlayerDNA.Heat : 0.f);
    SyncLiveVitals();
    RebindAttributeDelegates();
    RefreshHUDState();
    // Registration belongs to GameMode after successful possession, not a half-built body.
}

void ARoninCharacter::Tick(float DeltaTime) {
    Super::Tick(DeltaTime);

    HandleSurvivalStats(DeltaTime);
    UpdateDynamicAura();
    UpdateLimpState();
    PlayerDNA.WorldLocation = GetActorLocation();

    // GameMode must drive world clock once on authority, and scan its server player set.
    // No pawn-local world tick: multiple players must not multiply world time.
    if (FMath::IsFinite(DeltaTime) && DeltaTime > 0.f && !bHasInitiatedDeath) {
        CombatAccumulator += static_cast<double>(DeltaTime);
        constexpr double FixedStep = 1.0 / 60.0;
        const int32 Budget = FMath::Clamp(MaxCombatStepsPerTick, 1, 32);
        for (int32 Count = 0; Count < Budget && CombatAccumulator >= FixedStep && CombatFrame < MAX_int64 - 1; ++Count) {
            FRoninCombatInput Input;
            if (!CombatHistory.FindInput(CombatFrame + 1, Input)) {
                Input = SampledCombatInput;
                Input.Frame = CombatFrame + 1;
                Input.RequestedStance = CurrentStance;
                CombatHistory.RecordInput(Input);
                SampledCombatInput.Look = FVector2D::ZeroVector;
            }
            CombatAccumulator -= FixedStep; // retain every unprocessed step, including hitch backlog
            StepCombat(Input, false);
        }
    }
}

bool ARoninCharacter::SubmitCombatInput(const FRoninCombatInput& Input)
{
    const int64 Capacity = CombatHistory.GetCapacity();
    if (bInsideCombatStep || bRestoringCombatState || bReplayingCombat || bHasInitiatedDeath ||
        Input.Frame <= CombatFrame || Capacity <= 0 || Input.Frame - CombatFrame >= Capacity ||
        Input.Move.ContainsNaN() || Input.Look.ContainsNaN() ||
        static_cast<uint8>(Input.RequestedStance) > static_cast<uint8>(ESamuraiStance::Defensive)) return false;
    return CombatHistory.RecordInput(Input);
}

bool ARoninCharacter::CopyCombatInput(int64 Frame, FRoninCombatInput& OutInput) const
{
    return CombatHistory.FindInput(Frame, OutInput);
}

bool ARoninCharacter::CopyCombatSnapshot(int64 Frame, FRoninCombatSnapshot& OutSnapshot) const
{
    return CombatHistory.FindSnapshot(Frame, OutSnapshot);
}

FRoninCombatSnapshot ARoninCharacter::CaptureCombatSnapshot() const
{
    FRoninCombatSnapshot Result = CombatWindowState;
    Result.Frame = CombatFrame;
    Result.Transform = GetActorTransform();
    Result.Velocity = bOwnsImpactFreeze ? SavedMovementVelocity : GetVelocity();
    Result.Stance = CurrentStance;
    if (AttributeSet) {
        Result.Blood = AttributeSet->GetBlood();
        Result.Stamina = AttributeSet->GetStamina();
        Result.Potential = AttributeSet->GetPotential();
        Result.Posture = AttributeSet->GetPosture();
    }
    Result.FreezeFrames = static_cast<int32>(FMath::Clamp<int64>(FreezeEndFrame - CombatFrame, 0, MAX_int32));
    return Result;
}

bool ARoninCharacter::RestoreCombatSnapshot(const FRoninCombatSnapshot& Snapshot)
{
    // Explicit restore is required for old frames; SubmitCombatInput never silently rewinds.
    if (bInsideCombatStep || bRestoringCombatState || bReplayingCombat || bHasInitiatedDeath || !AttributeSet ||
        !CombatHistory.FindSnapshot(Snapshot.Frame) || Snapshot.Frame > CombatHighWaterFrame ||
        Snapshot.Frame <= CombatHighWaterFrame - CombatHistory.GetCapacity() ||
        Snapshot.Transform.ContainsNaN() || !Snapshot.Transform.GetRotation().IsNormalized() || Snapshot.Velocity.ContainsNaN() ||
        !FMath::IsFinite(Snapshot.Blood) || !FMath::IsFinite(Snapshot.Stamina) ||
        !FMath::IsFinite(Snapshot.Potential) || !FMath::IsFinite(Snapshot.Posture) || Snapshot.FreezeFrames < 0 ||
        Snapshot.FreezeFrames > 3600 || Snapshot.Frame > MAX_int64 - Snapshot.FreezeFrames ||
        static_cast<uint8>(Snapshot.Stance) > static_cast<uint8>(ESamuraiStance::Defensive)) return false;
    TGuardValue<bool> RestoreGuard(bRestoringCombatState, true);
    ReleaseImpactFreeze();
    CombatFrame = Snapshot.Frame;
    CombatWindowState = Snapshot;
    SetActorTransform(Snapshot.Transform, false, nullptr, ETeleportType::TeleportPhysics);
    if (GetCharacterMovement()) GetCharacterMovement()->Velocity = Snapshot.Velocity;
    SetCurrentStance(Snapshot.Stance);
    AttributeSet->SetBlood(FMath::Clamp(Snapshot.Blood, 0.f, 1000.f));
    AttributeSet->SetStamina(FMath::Clamp(Snapshot.Stamina, 0.f, AttributeSet->GetMaxStamina()));
    AttributeSet->SetPotential(FMath::Clamp(Snapshot.Potential, 0.f, 100.f));
    AttributeSet->SetPosture(FMath::Clamp(Snapshot.Posture, 0.f, 100.f));
    UpdateExecutionVulnerability(AttributeSet->GetPosture() >= 100.f);
    FreezeEndFrame = CombatFrame + Snapshot.FreezeFrames;
    if (Snapshot.FreezeFrames > 0) AcquireImpactFreeze();
    // Do not touch death flags, persistent profile, confirmed outcomes, or accumulated real time.
    RefreshHUDState();
    return true;
}

void ARoninCharacter::StepCombat(const FRoninCombatInput& Input, bool bReplay)
{
    TGuardValue<bool> StepGuard(bInsideCombatStep, true);
    const bool bFrozenStep = FreezeEndFrame > CombatFrame;
    CombatFrame = Input.Frame;
    CombatHighWaterFrame = FMath::Max(CombatHighWaterFrame, CombatFrame);
    CombatWindowState.DeterministicSeed = Input.DeterministicSeed;
    if (!bFrozenStep && !bHasInitiatedDeath) {
        // Input commands and seed are explicit hook data; movement/GAS are not resimulated here.
        OnLogicalCombatStep(Input, 1.f / 60.f, bReplay);
    }
    if (FreezeEndFrame <= CombatFrame) ReleaseImpactFreeze();
    CombatHistory.RecordSnapshot(CaptureCombatSnapshot());
}

bool ARoninCharacter::ReplayCombatThroughFrame(int64 EndFrame)
{
    if (bInsideCombatStep || bRestoringCombatState || bReplayingCombat || bHasInitiatedDeath ||
        EndFrame <= CombatFrame || EndFrame > CombatHighWaterFrame ||
        EndFrame - CombatFrame >= CombatHistory.GetCapacity()) return false;
    TArray<FRoninCombatInput> ReplayInputs;
    for (int64 Frame = CombatFrame + 1; Frame <= EndFrame; ++Frame) {
        FRoninCombatInput Input;
        if (!CombatHistory.FindInput(Frame, Input)) return false; // preflight before changing anything
        ReplayInputs.Add(Input);
    }
    TGuardValue<bool> ReplayGuard(bReplayingCombat, true);
    for (const FRoninCombatInput& Input : ReplayInputs) StepCombat(Input, true);
    return true;
}

bool ARoninCharacter::ConfirmCombatOutcome(int64 Frame, FName OutcomeID, bool bLethal)
{
    if (!HasAuthority() || bRestoringCombatState || bReplayingCombat || OutcomeID.IsNone() ||
        Frame > CombatFrame || !CombatHistory.FindSnapshot(Frame)) return false;
    const FString Key = FString::Printf(TEXT("%lld:%s"), static_cast<long long>(Frame), *OutcomeID.ToString());
    if (ConfirmedCombatOutcomes.Contains(Key) || (bLethal && (!AttributeSet || AttributeSet->GetBlood() > 0.f))) return false;
    ConfirmedCombatOutcomes.Add(Key, Frame);
    for (auto It = ConfirmedCombatOutcomes.CreateIterator(); It; ++It)
        if (It.Value() <= CombatHighWaterFrame - CombatHistory.GetCapacity()) It.RemoveCurrent();
    if (bLethal) HandleDeath();
    return true;
}

void ARoninCharacter::RequestImpactFreeze(int32 LogicalSteps)
{
    if (LogicalSteps <= 0 || bHasInitiatedDeath || CombatFrame > MAX_int64 - LogicalSteps) return;
    FreezeEndFrame = FMath::Max(FreezeEndFrame, CombatFrame + static_cast<int64>(LogicalSteps));
    AcquireImpactFreeze();
}

void ARoninCharacter::AcquireImpactFreeze()
{
    if (bOwnsImpactFreeze) return; // extensions never overwrite the original baseline
    bOwnsImpactFreeze = true;
    FrozenMesh = GetMesh();
    if (USkeletalMeshComponent* TargetSkelMesh = FrozenMesh.Get()) {
        bSavedPauseAnims = TargetSkelMesh->bPauseAnims;
        TargetSkelMesh->bPauseAnims = true;
    }
    FrozenMovement = GetCharacterMovement();
    if (UCharacterMovementComponent* Movement = FrozenMovement.Get()) {
        bSavedMovementTick = Movement->IsComponentTickEnabled();
        SavedMovementVelocity = Movement->Velocity;
        Movement->Velocity = FVector::ZeroVector;
        Movement->SetComponentTickEnabled(false);
    }
    FrozenRigidBodies.Reset();
    for (UStaticMeshComponent* Body : ImpactFreezeRigidBodies) {
        if (!IsValid(Body) || Body->GetOwner() != this || !Body->IsSimulatingPhysics()) continue;
        FFrozenRigidBody State;
        State.Component = Body;
        State.LinearVelocity = Body->GetPhysicsLinearVelocity();
        State.AngularVelocity = Body->GetPhysicsAngularVelocityInRadians();
        State.bWasAwake = Body->IsAnyRigidBodyAwake();
        FrozenRigidBodies.Add(State);
        Body->SetSimulatePhysics(false);
    }
}

void ARoninCharacter::ReleaseImpactFreeze()
{
    if (!bOwnsImpactFreeze) return;
    bOwnsImpactFreeze = false;
    if (USkeletalMeshComponent* TargetSkelMesh = FrozenMesh.Get()) TargetSkelMesh->bPauseAnims = bSavedPauseAnims;
    if (UCharacterMovementComponent* Movement = FrozenMovement.Get()) {
        Movement->Velocity = SavedMovementVelocity;
        Movement->SetComponentTickEnabled(bSavedMovementTick);
    }
    for (const FFrozenRigidBody& State : FrozenRigidBodies) {
        if (UStaticMeshComponent* Body = State.Component.Get()) {
            Body->SetSimulatePhysics(true);
            Body->SetPhysicsLinearVelocity(State.LinearVelocity);
            Body->SetPhysicsAngularVelocityInRadians(State.AngularVelocity);
            if (!State.bWasAwake) Body->PutRigidBodyToSleep();
        }
    }
    FrozenRigidBodies.Reset();
    FrozenMovement.Reset();
    FrozenMesh.Reset();
}

void ARoninCharacter::Interact()
{
    if (!FollowCamera) return;

    FVector Start = FollowCamera->GetComponentLocation();
    FVector ForwardVector = FollowCamera->GetForwardVector();
    FVector End = Start + (ForwardVector * 1500.f);

    FHitResult HitResult;
    FCollisionQueryParams CollisionParams;
    CollisionParams.AddIgnoredActor(this);

    if (GetWorld()->LineTraceSingleByChannel(HitResult, Start, End, ECC_Visibility, CollisionParams))
    {
        AActor* HitActor = HitResult.GetActor();
        if (!HitActor) return;

        if (ABaseHunter* HitHunter = Cast<ABaseHunter>(HitActor))
        {
            UE_LOG(LogTemp, Warning, TEXT("INTERACTION: Initiating Dialogue with %s"), *HitHunter->HunterDNA.HunterName);
            HitHunter->BeginDialogue();
        }
        else if (ASiegeFortress* HitFortress = Cast<ASiegeFortress>(HitActor))
        {
            UE_LOG(LogTemp, Warning, TEXT("INTERACTION: Accessing Settlement %s"), *HitFortress->RegionID);
            OnOpenSettlementUI(HitFortress->RegionID);
        }
    }
}

bool ARoninCharacter::ConsumeLocalItem(FString ItemName)
{
    if (!PrepareEconomyProfile()) return false;
    UEconomySystem* Economy = GetGameInstance()->GetSubsystem<UEconomySystem>();
    if (!Economy || !Economy->ConsumeItem(PlayerDNA.UniqueID, ItemName)) return false;
    UNemesisManager* Manager = GetGameInstance()->GetSubsystem<UNemesisManager>();
    FHunterProfile Updated;
    if (Manager && Manager->GetHunterProfile(PlayerDNA.UniqueID, Updated)) {
        // Consumption intentionally modifies these vitals, from a preflighted live baseline.
        if (AttributeSet) {
            AttributeSet->SetBlood(Updated.Blood);
            AttributeSet->SetStamina(Updated.Stamina);
            AttributeSet->SetPotential(Updated.Potential);
        }
    }
    RefreshEconomyProfile();
    return true;
}

bool ARoninCharacter::CraftLocalItem(FString RecipeName)
{
    if (!PrepareEconomyProfile()) return false;
    UEconomySystem* Economy = GetGameInstance()->GetSubsystem<UEconomySystem>();
    const bool bSuccess = Economy && Economy->CraftItem(Cast<APlayerController>(GetController()), RecipeName);
    if (bSuccess) RefreshEconomyProfile();
    return bSuccess;
}

bool ARoninCharacter::DepositToLocalSettlement(FString RegionID, FString ItemName, int32 Quantity)
{
    if (Quantity <= 0 || !PrepareEconomyProfile()) return false;
    UEconomySystem* Economy = GetGameInstance()->GetSubsystem<UEconomySystem>();
    const bool bSuccess = Economy && Economy->DepositToSettlementStockpile(Cast<APlayerController>(GetController()), RegionID, ItemName, Quantity);
    if (bSuccess) RefreshEconomyProfile();
    return bSuccess;
}

bool ARoninCharacter::WithdrawFromLocalSettlement(FString RegionID, FString ItemName, int32 Quantity)
{
    if (Quantity <= 0 || !PrepareEconomyProfile()) return false;
    UEconomySystem* Economy = GetGameInstance()->GetSubsystem<UEconomySystem>();
    const bool bSuccess = Economy && Economy->WithdrawFromSettlementStockpile(Cast<APlayerController>(GetController()), RegionID, ItemName, Quantity);
    if (bSuccess) RefreshEconomyProfile();
    return bSuccess;
}

void ARoninCharacter::HandleDeath() {
    if (!HasAuthority() || bRestoringCombatState || bReplayingCombat || bHasInitiatedDeath || bIsDying) return;

    bHasInitiatedDeath = true;
    bIsDying = true;
    bResting = false;
    ReleaseImpactFreeze();
    FreezeEndFrame = CombatFrame;
    if (AbilitySystemComponent) AbilitySystemComponent->CancelAllAbilities();
    ClearOwnedCombatState();
    OnRep_LifeState();
    SyncLiveVitals();
    PlayerDNA.bIsAlive = false;

    if (GEngine)
    {
        GEngine->AddOnScreenDebugMessage(-1, 10.f, FColor::Red, TEXT("CRITICAL: The Ronin has fallen. Your legacy begins now."));
    }

    if (UNemesisManager* Manager = GetGameInstance()->GetSubsystem<UNemesisManager>()) {
        Manager->SynchronizeSimulation();
        PlayerDNA.Blood = 0.0f;
        if (AttributeSet) AttributeSet->SetBlood(0.f);
        Manager->SyncPlayerProfile(PlayerDNA);
        Manager->RecordPlayerDeath(TEXT("Unknown Assailant"));
    }

    DisableInput(Cast<APlayerController>(GetController()));

    if (PlayerDNA.bIsRealPlayer && IsLocallyControlled()) {
        OnShowHeirUI();
    }
}

void ARoninCharacter::RemoveInputMapping()
{
    if (UEnhancedInputLocalPlayerSubsystem* Subsystem = BoundInputSubsystem.Get())
        if (BoundMappingContext) Subsystem->RemoveMappingContext(BoundMappingContext);
    BoundInputSubsystem.Reset();
    BoundMappingContext = nullptr;
}

void ARoninCharacter::RefreshInputMapping()
{
    RemoveInputMapping();
    APlayerController* PC = Cast<APlayerController>(GetController());
    ULocalPlayer* LocalPlayer = PC ? PC->GetLocalPlayer() : nullptr;
    if (!LocalPlayer || !DefaultMappingContext) return;
    if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer)) {
        Subsystem->AddMappingContext(DefaultMappingContext, 0);
        BoundInputSubsystem = Subsystem;
        BoundMappingContext = DefaultMappingContext;
    }
}

void ARoninCharacter::PossessedBy(AController* NewController)
{
    Super::PossessedBy(NewController);
    if (AbilitySystemComponent) AbilitySystemComponent->InitAbilityActorInfo(this, this);
    RebindAttributeDelegates();
    RefreshInputMapping();
}

void ARoninCharacter::UnPossessed()
{
    UnbindAttributeDelegates();
    RemoveInputMapping();
    Super::UnPossessed();
}

void ARoninCharacter::OnRep_Controller()
{
    Super::OnRep_Controller();
    if (AbilitySystemComponent) AbilitySystemComponent->InitAbilityActorInfo(this, this);
    RebindAttributeDelegates();
    RefreshInputMapping();
}

void ARoninCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UnbindAttributeDelegates();
    RemoveInputMapping();
    ReleaseImpactFreeze();
    ClearOwnedCombatState();
    if (bIsLimping && bHasLimpBaseline && GetCharacterMovement()) GetCharacterMovement()->MaxWalkSpeed = BaselineWalkSpeed;
    if (ActiveWeaponVFX) { ActiveWeaponVFX->DestroyComponent(); ActiveWeaponVFX = nullptr; }
    DurabilityMaterial = nullptr;
    Super::EndPlay(EndPlayReason);
}

void ARoninCharacter::UnbindAttributeDelegates()
{
    if (UAbilitySystemComponent* ASC = BoundAttributeASC.Get()) {
        ASC->GetGameplayAttributeValueChangeDelegate(URoninAttributeSet::GetBloodAttribute()).Remove(BloodChangedHandle);
        ASC->GetGameplayAttributeValueChangeDelegate(URoninAttributeSet::GetPotentialAttribute()).Remove(PotentialChangedHandle);
        ASC->GetGameplayAttributeValueChangeDelegate(URoninAttributeSet::GetStaminaAttribute()).Remove(StaminaChangedHandle);
    }
    BloodChangedHandle.Reset(); PotentialChangedHandle.Reset(); StaminaChangedHandle.Reset();
    BoundAttributeASC.Reset();
}

void ARoninCharacter::RebindAttributeDelegates()
{
    UnbindAttributeDelegates();
    if (!AbilitySystemComponent) return;
    BoundAttributeASC = AbilitySystemComponent;
    BloodChangedHandle = AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(URoninAttributeSet::GetBloodAttribute()).AddUObject(this, &ARoninCharacter::HandleBloodChanged);
    PotentialChangedHandle = AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(URoninAttributeSet::GetPotentialAttribute()).AddUObject(this, &ARoninCharacter::HandlePotentialChanged);
    StaminaChangedHandle = AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(URoninAttributeSet::GetStaminaAttribute()).AddUObject(this, &ARoninCharacter::HandleStaminaChanged);
    RefreshHUDState();
}

void ARoninCharacter::HandleBloodChanged(const FOnAttributeChangeData& Data)
{
    if (!bRestoringCombatState && !bReplayingCombat) PlayerDNA.Blood = Data.NewValue;
    OnBloodChanged.Broadcast(Data.NewValue);
    UpdateLimpState();
}
void ARoninCharacter::HandlePotentialChanged(const FOnAttributeChangeData& Data)
{
    if (!bRestoringCombatState && !bReplayingCombat) PlayerDNA.Potential = Data.NewValue;
    OnPotentialChanged.Broadcast(Data.NewValue);
}
void ARoninCharacter::HandleStaminaChanged(const FOnAttributeChangeData& Data)
{
    if (!bRestoringCombatState && !bReplayingCombat) PlayerDNA.Stamina = Data.NewValue;
    OnStaminaChanged.Broadcast(Data.NewValue);
}

int32 ARoninCharacter::GetHeatTier() const
{
    // Invalid threshold configuration is not silently sorted or used for indexing.
    if (HeatThresholds.Num() != 5) return 0;
    int32 Tier = 0;
    float Previous = -1.f;
    for (int32 Index = 0; Index < 5; ++Index) {
        const float Threshold = HeatThresholds[Index];
        if (!FMath::IsFinite(Threshold) || Threshold < 0.f || Threshold > 100.f || Threshold <= Previous) return 0;
        if (CurrentHeat >= Threshold) Tier = Index;
        Previous = Threshold;
    }
    return Tier;
}

void ARoninCharacter::SetCurrentHeat(float Value)
{
    if (!HasAuthority() || bRestoringCombatState || bReplayingCombat || !FMath::IsFinite(Value)) return;
    CurrentHeat = FMath::Clamp(Value, 0.f, 100.f);
    PlayerDNA.Heat = CurrentHeat;
    OnHeatChanged.Broadcast(CurrentHeat, GetHeatTier());
}
void ARoninCharacter::SetKatanaDurability(float Value)
{
    if (!HasAuthority() || bRestoringCombatState || bReplayingCombat || !FMath::IsFinite(Value)) return;
    KatanaDurability = FMath::Clamp(Value, 0.f, 1.f);
    OnRep_HUDState();
}

bool ARoninCharacter::BindKatanaDurabilityMaterial(UMaterialInstanceDynamic* Material, FName ScalarParameter)
{
    if (Material && ScalarParameter.IsNone()) return false;
    DurabilityMaterial = Material;
    DurabilityScalarParameter = Material ? ScalarParameter : NAME_None;
    OnRep_HUDState();
    return true;
}

void ARoninCharacter::OnRep_HUDState()
{
    CurrentHeat = FMath::Clamp(CurrentHeat, 0.f, 100.f);
    KatanaDurability = FMath::Clamp(KatanaDurability, 0.f, 1.f);
    if (DurabilityMaterial && !DurabilityScalarParameter.IsNone())
        DurabilityMaterial->SetScalarParameterValue(DurabilityScalarParameter, KatanaDurability);
    OnHeatChanged.Broadcast(CurrentHeat, GetHeatTier());
    OnKatanaDurabilityChanged.Broadcast(KatanaDurability);
}

void ARoninCharacter::RefreshHUDState()
{
    OnRep_HUDState();
    if (AttributeSet) {
        OnBloodChanged.Broadcast(AttributeSet->GetBlood());
        OnPotentialChanged.Broadcast(AttributeSet->GetPotential());
        OnStaminaChanged.Broadcast(AttributeSet->GetStamina());
    }
}

void ARoninCharacter::SetResting(bool bNewResting)
{
    if (HasAuthority() && !bRestoringCombatState && !bReplayingCombat && !bIsDying) bResting = bNewResting;
}

void ARoninCharacter::UpdateLimpState()
{
    UCharacterMovementComponent* Movement = GetCharacterMovement();
    if (!Movement || !AttributeSet) return;
    const bool bShouldLimp = AttributeSet->GetBlood() < 50.f && !bIsDying;
    if (bShouldLimp && !bIsLimping) {
        BaselineWalkSpeed = Movement->MaxWalkSpeed;
        bHasLimpBaseline = true;
        Movement->MaxWalkSpeed = BaselineWalkSpeed * FMath::Clamp(LimpSpeedMultiplier, 0.f, 1.f);
    } else if (!bShouldLimp && bIsLimping && bHasLimpBaseline) {
        Movement->MaxWalkSpeed = BaselineWalkSpeed;
        bHasLimpBaseline = false;
    }
    bIsLimping = bShouldLimp;
}

void ARoninCharacter::SetCurrentStance(ESamuraiStance NewStance)
{
    if (static_cast<uint8>(NewStance) > static_cast<uint8>(ESamuraiStance::Defensive)) return;
    static const FName Names[] = {TEXT("State.Stance.Flash"), TEXT("State.Stance.Heavy"), TEXT("State.Stance.Forbidden"), TEXT("State.Stance.Defensive")};
    const FGameplayTag NewTag = FGameplayTag::RequestGameplayTag(Names[static_cast<uint8>(NewStance)], false);
    if (AbilitySystemComponent && NewTag != OwnedStanceTag) {
        if (OwnedStanceTag.IsValid()) {
            AbilitySystemComponent->RemoveLooseGameplayTag(OwnedStanceTag);
            if (HasAuthority()) AbilitySystemComponent->RemoveReplicatedLooseGameplayTag(OwnedStanceTag);
        }
        OwnedStanceTag = NewTag;
        if (OwnedStanceTag.IsValid()) {
            AbilitySystemComponent->AddLooseGameplayTag(OwnedStanceTag);
            if (HasAuthority()) AbilitySystemComponent->AddReplicatedLooseGameplayTag(OwnedStanceTag);
        }
    }
    CurrentStance = NewStance;
}

void ARoninCharacter::ReplaceStancePassive(TSubclassOf<UGameplayEffect> EffectClass, float Level)
{
    if (!HasAuthority() || !AbilitySystemComponent || bRestoringCombatState || bReplayingCombat) return;
    if (EffectClass == OwnedStancePassiveClass && Level == OwnedStancePassiveLevel && OwnedStancePassive.IsValid()) return;
    if (OwnedStancePassive.IsValid()) AbilitySystemComponent->RemoveActiveGameplayEffect(OwnedStancePassive);
    OwnedStancePassive = FActiveGameplayEffectHandle();
    OwnedStancePassiveClass = EffectClass;
    OwnedStancePassiveLevel = Level;
    if (EffectClass) {
        const FGameplayEffectSpecHandle Spec = AbilitySystemComponent->MakeOutgoingSpec(EffectClass, Level, AbilitySystemComponent->MakeEffectContext());
        if (Spec.IsValid()) OwnedStancePassive = AbilitySystemComponent->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
    }
}

void ARoninCharacter::UpdateExecutionVulnerability(bool bVulnerable)
{
    if (!AbilitySystemComponent || bVulnerable == bOwnsVulnerabilityTag) return;
    const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(TEXT("State.Combat.IsExecutionVulnerable"), false);
    if (!Tag.IsValid()) return;
    bOwnsVulnerabilityTag = bVulnerable;
    if (bVulnerable) {
        AbilitySystemComponent->AddLooseGameplayTag(Tag);
        if (HasAuthority()) AbilitySystemComponent->AddReplicatedLooseGameplayTag(Tag);
    } else {
        AbilitySystemComponent->RemoveLooseGameplayTag(Tag);
        if (HasAuthority()) AbilitySystemComponent->RemoveReplicatedLooseGameplayTag(Tag);
    }
}

void ARoninCharacter::ClearOwnedCombatState()
{
    if (!AbilitySystemComponent) return;
    UpdateExecutionVulnerability(false);
    if (OwnedStanceTag.IsValid()) {
        AbilitySystemComponent->RemoveLooseGameplayTag(OwnedStanceTag);
        if (HasAuthority()) AbilitySystemComponent->RemoveReplicatedLooseGameplayTag(OwnedStanceTag);
        OwnedStanceTag = FGameplayTag();
    }
    if (OwnedStancePassive.IsValid()) AbilitySystemComponent->RemoveActiveGameplayEffect(OwnedStancePassive);
    OwnedStancePassive = FActiveGameplayEffectHandle();
    OwnedStancePassiveClass = nullptr;
}

void ARoninCharacter::SyncLiveVitals()
{
    if (!AttributeSet || bRestoringCombatState || bReplayingCombat) return;
    PlayerDNA.Blood = AttributeSet->GetBlood();
    PlayerDNA.Stamina = AttributeSet->GetStamina();
    PlayerDNA.Potential = AttributeSet->GetPotential();
}

bool ARoninCharacter::PrepareEconomyProfile()
{
    if (!HasAuthority() || bIsDying || bRestoringCombatState || bReplayingCombat || !GetGameInstance()) return false;
    ARoninCharacter* AuthorizedPawn = nullptr;
    FHunterProfile* Profile = nullptr;
    if (!FRoninEconomyAccess::ResolveCaller(Cast<APlayerController>(GetController()), GetGameInstance(),
        AuthorizedPawn, Profile) || AuthorizedPawn != this) return false;
    SyncLiveVitals();
    Profile->Blood = PlayerDNA.Blood;
    Profile->Stamina = PlayerDNA.Stamina;
    Profile->Potential = PlayerDNA.Potential;
    Profile->Hunger = PlayerDNA.Hunger;
    Profile->Thirst = PlayerDNA.Thirst;
    return true;
}

void ARoninCharacter::RefreshEconomyProfile()
{
    if (!GetGameInstance()) return;
    if (UNemesisManager* Manager = GetGameInstance()->GetSubsystem<UNemesisManager>()) {
        Manager->SynchronizeSimulation();
        FHunterProfile Updated;
        if (Manager->GetHunterProfile(PlayerDNA.UniqueID, Updated)) {
            // Economy does not own GAS vital attributes. Never restore stale DB values into GAS.
            PlayerDNA = Updated;
            SyncLiveVitals();
            SetCurrentHeat(PlayerDNA.Heat);
        }
    }
}

void ARoninCharacter::HandleSurvivalStats(float DeltaTime) {
    if (!bIsDying) {
        ProcessSurvivalMetrics(DeltaTime);
    }
}

void ARoninCharacter::ProcessSurvivalMetrics(float DeltaTime) {
    if (!HasAuthority() || bRestoringCombatState || bReplayingCombat || !FMath::IsFinite(DeltaTime) || DeltaTime <= 0.f) return;
    const float HungerRate = FMath::IsFinite(HungerDecayPerSecond) ? FMath::Max(0.f, HungerDecayPerSecond) : 0.f;
    const float ThirstRate = FMath::IsFinite(ThirstDecayPerSecond) ? FMath::Max(0.f, ThirstDecayPerSecond) : 0.f;
    PlayerDNA.Hunger = FMath::Clamp(PlayerDNA.Hunger - HungerRate * DeltaTime, 0.f, 100.f);
    PlayerDNA.Thirst = FMath::Clamp(PlayerDNA.Thirst - ThirstRate * DeltaTime, 0.f, 100.f);
    const float RestRate = FMath::IsFinite(RestStaminaPerSecond) ? FMath::Max(0.f, RestStaminaPerSecond) : 0.f;
    if (bResting && AttributeSet && GetVelocity().SizeSquared() < 1.f && PlayerDNA.Hunger > 0.f && PlayerDNA.Thirst > 0.f)
        AttributeSet->SetStamina(FMath::Min(AttributeSet->GetMaxStamina(), AttributeSet->GetStamina() + RestRate * DeltaTime));
}

void ARoninCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) {
    Super::SetupPlayerInputComponent(PlayerInputComponent);
    if (UEnhancedInputComponent* EI = Cast<UEnhancedInputComponent>(PlayerInputComponent)) {
        if (MoveAction) {
            EI->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ARoninCharacter::Move);
            EI->BindAction(MoveAction, ETriggerEvent::Completed, this, &ARoninCharacter::Move);
            EI->BindAction(MoveAction, ETriggerEvent::Canceled, this, &ARoninCharacter::Move);
        }
        if (LookAction) {
            EI->BindAction(LookAction, ETriggerEvent::Triggered, this, &ARoninCharacter::Look);
            EI->BindAction(LookAction, ETriggerEvent::Completed, this, &ARoninCharacter::Look);
            EI->BindAction(LookAction, ETriggerEvent::Canceled, this, &ARoninCharacter::Look);
        }
        if (InteractAction) EI->BindAction(InteractAction, ETriggerEvent::Started, this, &ARoninCharacter::Interact);
    }
}

void ARoninCharacter::Move(const FInputActionValue& Value) {
    const FVector2D MoveVec = Value.Get<FVector2D>();
    if (MoveVec.ContainsNaN()) return;
    SampledCombatInput.Move = FVector2D(FMath::Clamp(MoveVec.X, -1.0, 1.0), FMath::Clamp(MoveVec.Y, -1.0, 1.0));
    if (Controller && !bIsDying && !bOwnsImpactFreeze) {
        const FRotator Rotation = Controller->GetControlRotation();
        const FRotator YawRot(0, Rotation.Yaw, 0);
        const FVector Forward = FRotationMatrix(YawRot).GetUnitAxis(EAxis::X);
        const FVector Right = FRotationMatrix(YawRot).GetUnitAxis(EAxis::Y);
        AddMovementInput(Forward, SampledCombatInput.Move.Y);
        AddMovementInput(Right, SampledCombatInput.Move.X);
    }
}

void ARoninCharacter::Look(const FInputActionValue& Value) {
    const FVector2D LookVec = Value.Get<FVector2D>();
    if (LookVec.ContainsNaN()) return;
    SampledCombatInput.Look = LookVec;
    if (Controller && !bIsDying) {
        AddControllerYawInput(LookVec.X);
        AddControllerPitchInput(LookVec.Y);
    }
}

void ARoninCharacter::SelectDialogueOption(FString SelectedText) {
    if (UGameInstance* GI = GetGameInstance()) {
        if (UDialogueSystem* DialogueSystem = GI->GetSubsystem<UDialogueSystem>()) {
            DialogueSystem->ProcessPlayerResponse(SelectedText);
        }
    }
}

bool ARoninCharacter::AuthorizeCrimeReport(const FString& CrimeCommitted, float KarmaShift)
{
    if (!HasAuthority() || bIsDying || bRestoringCombatState || bReplayingCombat ||
        CrimeCommitted.TrimStartAndEnd().IsEmpty() || CrimeCommitted.Len() > 256 ||
        !FMath::IsFinite(KarmaShift) || FMath::Abs(KarmaShift) > 100.f || AuthorizedCrimeReports.Num() >= 64) return false;
    AuthorizedCrimeReports.Add(CrimeCommitted, KarmaShift);
    return true;
}

bool ARoninCharacter::Server_CommitCrime_Validate(float KarmaIncrease, const FString& CrimeCommitted)
{
    return FMath::IsFinite(KarmaIncrease) && FMath::Abs(KarmaIncrease) <= 100.f &&
        !CrimeCommitted.TrimStartAndEnd().IsEmpty() && CrimeCommitted.Len() <= 256;
}
void ARoninCharacter::Server_CommitCrime_Implementation(float KarmaIncrease, const FString& CrimeCommitted)
{
    if (!HasAuthority() || bIsDying || bRestoringCombatState || bReplayingCombat) return;
    float ConfirmedShift = 0.f;
    // The client magnitude is transport-only; the server's confirmed occurrence owns the value.
    if (!AuthorizedCrimeReports.RemoveAndCopyValue(CrimeCommitted, ConfirmedShift)) return;
    CurrentKarma = FMath::Clamp(CurrentKarma + ConfirmedShift, -100.f, 100.f);
    PlayerDNA.Karma = CurrentKarma;
    if (GetGameInstance()) {
        if (UNemesisManager* Manager = GetGameInstance()->GetSubsystem<UNemesisManager>()) {
            Manager->SynchronizeSimulation();
            if (FHunterProfile* Profile = Manager->ActiveNemeses.Find(PlayerDNA.UniqueID)) Profile->Karma = CurrentKarma;
            if (FHunterProfile* Legacy = Manager->ActiveNemeses.Find(TEXT("PLAYER")))
                if (Legacy->UniqueID == PlayerDNA.UniqueID) Legacy->Karma = CurrentKarma;
        }
    }
    OnRep_CurrentKarma();
}
void ARoninCharacter::OnRep_CurrentKarma()
{
    PlayerDNA.Karma = CurrentKarma;
    UpdateDynamicAura();
}

void ARoninCharacter::OnRep_LifeState()
{
    if (!bHasInitiatedDeath) return;
    const bool bWasDying = bIsDying;
    bIsDying = true;
    bResting = false;
    ReleaseImpactFreeze();
    FreezeEndFrame = CombatFrame;
    SampledCombatInput = FRoninCombatInput();
    AuthorizedCrimeReports.Empty();
    if (UCharacterMovementComponent* Movement = GetCharacterMovement()) {
        Movement->Velocity = FVector::ZeroVector;
        Movement->SetComponentTickEnabled(false);
    }
    if (AbilitySystemComponent) AbilitySystemComponent->CancelAllAbilities();
    ClearOwnedCombatState();
    if (APlayerController* PC = Cast<APlayerController>(GetController()))
        if (PC->IsLocalController()) DisableInput(PC);
    if (!bWasDying && IsLocallyControlled()) OnShowHeirUI();
}

void ARoninCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const {
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ARoninCharacter, CurrentKarma);
    DOREPLIFETIME(ARoninCharacter, CurrentHeat);
    DOREPLIFETIME(ARoninCharacter, KatanaDurability);
    DOREPLIFETIME(ARoninCharacter, bHasInitiatedDeath);
}

TArray<FString> ARoninCharacter::GetAvailableHeirs() {
    TArray<FString> Heirs;

    for (auto& Relation : PlayerDNA.RelationshipWeb) {
        EFamilyTie Tie = Relation.Value.TieType;

        if (Tie == EFamilyTie::Child || Tie == EFamilyTie::Spouse || Tie == EFamilyTie::Student) {
            if (UGameInstance* GI = GetGameInstance()) {
                if (UNemesisManager* NM = GI->GetSubsystem<UNemesisManager>()) {
                    FHunterProfile CompanionProfile;
                    if (NM->GetHunterProfile(Relation.Key, CompanionProfile)) {
                        if (CompanionProfile.bIsAlive) {
                            Heirs.Add(Relation.Key);
                        }
                    }
                }
            }
        }
    }

    if (Heirs.Num() == 0) {
        Heirs.Add(TEXT("Nameless_Drifter"));
    }

    return Heirs;
}

void ARoninCharacter::UpdateAuraVFX(FName ParameterName, float Value)
{
    if (IsValid(ActiveAuraVFX) && !ParameterName.IsNone() && FMath::IsFinite(Value))
        ActiveAuraVFX->SetVariableFloat(ParameterName, Value);
}

void ARoninCharacter::SetAuraParameter(FName ParameterName, float Value, FLinearColor Color)
{
    UpdateAuraVFX(ParameterName, Value);
    if (IsValid(ActiveAuraVFX) && !AuraColorParameter.IsNone())
        ActiveAuraVFX->SetVariableLinearColor(AuraColorParameter, Color);
}

void ARoninCharacter::UpdateDynamicAura() {
    if (!IsValid(ActiveAuraVFX) || !AttributeSet) return;
    float Intensity = 0.f;
    switch (ActiveAuraStat) {
        case EAuraDriveStat::Blood: Intensity = AttributeSet->GetBlood() / 100.f; break;
        case EAuraDriveStat::Stamina: Intensity = AttributeSet->GetStamina() / FMath::Max(1.f, AttributeSet->GetMaxStamina()); break;
        case EAuraDriveStat::Karma: Intensity = FMath::Abs(CurrentKarma) / 100.f; break;
        case EAuraDriveStat::Potential: Intensity = AttributeSet->GetPotential() / 100.f; break;
        default: break;
    }
    UpdateAuraVFX(TEXT("MasterIntensity"), FMath::Clamp(Intensity, 0.f, 1.f));
    if (PlayerDNA.ElementalAffinities.Num() > 0)
        UpdateAuraVFX(TEXT("User_ElementIndex"), static_cast<float>(PlayerDNA.ElementalAffinities[0]));
    UpdateAuraVFX(TEXT("User_VelocityBlur"), FMath::Clamp(static_cast<float>(GetVelocity().Size2D() / 800.0), 0.f, 1.f));
}

void ARoninCharacter::ActivateWeaponVFX(class UNiagaraSystem* NewEffect)
{
    if (IsValid(ActiveWeaponVFX)) ActiveWeaponVFX->DestroyComponent();
    ActiveWeaponVFX = nullptr;
    if (!NewEffect) return;
    USceneComponent* Attachment = IsValid(WeaponVFXAttachComponent) && WeaponVFXAttachComponent->GetOwner() == this
        ? WeaponVFXAttachComponent : GetMesh();
    if (!IsValid(Attachment)) return;
    ActiveWeaponVFX = UNiagaraFunctionLibrary::SpawnSystemAttached(NewEffect, Attachment,
        WeaponVFXSocket, FVector::ZeroVector, FRotator::ZeroRotator, EAttachLocation::KeepRelativeOffset,
        false, true, ENCPoolMethod::None, false);
}

void ARoninCharacter::ReincarnateAsHeir(FString HeirID)
{
    if (HeirID.Len() > 256 || !bHasInitiatedDeath || bRestoringCombatState || bReplayingCombat || !GetWorld()) return;
    if (!HasAuthority()) {
        if (IsLocallyControlled()) Server_RequestReincarnation(HeirID);
        return;
    }
    APlayerController* PC = Cast<APlayerController>(GetController());
    ARoninGameMode* Mode = Cast<ARoninGameMode>(GetWorld()->GetAuthGameMode());
    if (PC && Mode) Mode->ReincarnateAsHeir(PC, PlayerDNA, HeirID);
}

void ARoninCharacter::Server_RequestReincarnation_Implementation(const FString& HeirID)
{
    // GameMode resolves the owning controller and canonical fallen profile, and
    // validates the selected living relative before any durable mutation.
    ReincarnateAsHeir(HeirID);
}