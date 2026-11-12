#pragma once
#include "CoreMinimal.h"
#include "RoninCombatHistory.h"
#include "GameplayEffectTypes.h"
#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h" 
#include "InputActionValue.h"
#include "RoninTypes.h"
#include "NiagaraFunctionLibrary.h"
#include "DialogueSystem.h"
#include "Net/UnrealNetwork.h"
#include "NiagaraComponent.h"
#include "RoninCharacter.generated.h"

UENUM(BlueprintType)
enum class EAuraDriveStat : uint8
{
    Blood       UMETA(DisplayName = "Blood (Health)"),
    Stamina     UMETA(DisplayName = "Stamina"),
    Karma       UMETA(DisplayName = "Karma"),
    Potential   UMETA(DisplayName = "Potential (Magic)")
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FRoninHUDValueChanged, float, Value);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FRoninHeatChanged, float, Heat, int32, Tier);

UCLASS()
class LASTRONIN_API ARoninCharacter : public ACharacter, public IAbilitySystemInterface
{
    GENERATED_BODY()

public:
    ARoninCharacter();

    virtual class UAbilitySystemComponent* GetAbilitySystemComponent() const override { return AbilitySystemComponent; }

    // Expose safe accessors for stance so external classes (e.g. abilities) do not access protected members directly.
    UFUNCTION(BlueprintCallable, Category = "Ronin|Combat")
    void SetCurrentStance(ESamuraiStance NewStance);

    // Infrastructure only: does not roll back Chaos, GAS effects, montage state or networking.
    UFUNCTION(BlueprintCallable, Category = "Ronin|Combat")
    bool SubmitCombatInput(const FRoninCombatInput& Input);
    UFUNCTION(BlueprintCallable, Category = "Ronin|Combat")
    FRoninCombatSnapshot CaptureCombatSnapshot() const;
    UFUNCTION(BlueprintCallable, Category = "Ronin|Combat")
    bool RestoreCombatSnapshot(const FRoninCombatSnapshot& Snapshot);
    UFUNCTION(BlueprintCallable, Category = "Ronin|Combat")
    bool CopyCombatInput(int64 Frame, FRoninCombatInput& OutInput) const;
    UFUNCTION(BlueprintCallable, Category = "Ronin|Combat")
    bool CopyCombatSnapshot(int64 Frame, FRoninCombatSnapshot& OutSnapshot) const;
    UFUNCTION(BlueprintCallable, Category = "Ronin|Combat")
    bool ReplayCombatThroughFrame(int64 EndFrame);
    UFUNCTION(BlueprintCallable, Category = "Ronin|Combat")
    void RequestImpactFreeze(int32 LogicalSteps = 4);
    UFUNCTION(BlueprintCallable, Category = "Ronin|Combat")
    bool ConfirmCombatOutcome(int64 Frame, FName OutcomeID, bool bLethal = false);
    UFUNCTION(BlueprintImplementableEvent, Category = "Ronin|Combat")
    void OnLogicalCombatStep(const FRoninCombatInput& Input, float FixedDeltaSeconds, bool bReplay);

    UPROPERTY(BlueprintReadOnly, Category = "Ronin|Combat") int64 CombatFrame = 0;
    UPROPERTY(BlueprintReadOnly, Category = "Ronin|Combat") bool bRestoringCombatState = false;
    UPROPERTY(BlueprintReadOnly, Category = "Ronin|Combat") bool bReplayingCombat = false;
    UPROPERTY(EditDefaultsOnly, Category = "Ronin|Combat", meta = (ClampMin = "1", ClampMax = "32")) int32 MaxCombatStepsPerTick = 8;
    UPROPERTY(EditDefaultsOnly, Category = "Ronin|Combat", meta = (ClampMin = "8", ClampMax = "3600")) int32 CombatHistoryCapacity = 600;
    UPROPERTY(BlueprintReadWrite, Category = "Ronin|Combat") FRoninCombatSnapshot CombatWindowState;

    UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_HUDState, Category = "Ronin|HUD") float CurrentHeat = 0.f;
    UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_HUDState, Category = "Ronin|HUD") float KatanaDurability = 1.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|HUD") TArray<float> HeatThresholds = {0.f, 20.f, 40.f, 60.f, 80.f};
    UPROPERTY(BlueprintAssignable, Category = "Ronin|HUD") FRoninHeatChanged OnHeatChanged;
    UPROPERTY(BlueprintAssignable, Category = "Ronin|HUD") FRoninHUDValueChanged OnKatanaDurabilityChanged;
    UPROPERTY(BlueprintAssignable, Category = "Ronin|HUD") FRoninHUDValueChanged OnBloodChanged;
    UPROPERTY(BlueprintAssignable, Category = "Ronin|HUD") FRoninHUDValueChanged OnPotentialChanged;
    UPROPERTY(BlueprintAssignable, Category = "Ronin|HUD") FRoninHUDValueChanged OnStaminaChanged;
    UFUNCTION(BlueprintCallable, Category = "Ronin|HUD") void SetCurrentHeat(float Value);
    UFUNCTION(BlueprintCallable, Category = "Ronin|HUD") void SetKatanaDurability(float Value);
    UFUNCTION(BlueprintPure, Category = "Ronin|HUD") int32 GetHeatTier() const;
    UFUNCTION(BlueprintCallable, Category = "Ronin|HUD")
    bool BindKatanaDurabilityMaterial(class UMaterialInstanceDynamic* Material, FName ScalarParameter);
    UFUNCTION(BlueprintCallable, Category = "Ronin|HUD") void RefreshHUDState();
    UFUNCTION() void OnRep_HUDState();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Survival", meta = (ClampMin = "0")) float HungerDecayPerSecond = 0.02f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Survival", meta = (ClampMin = "0")) float ThirstDecayPerSecond = 0.03f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Survival", meta = (ClampMin = "0")) float RestStaminaPerSecond = 5.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Survival", meta = (ClampMin = "0", ClampMax = "1")) float LimpSpeedMultiplier = 0.5f;
    UFUNCTION(BlueprintCallable, Category = "Ronin|Survival") void SetResting(bool bNewResting);
    UPROPERTY(BlueprintReadOnly, Category = "Ronin|Survival") bool bResting = false;
    UPROPERTY(BlueprintReadOnly, Category = "Ronin|Survival") bool bIsLimping = false;

    // Caller supplies the actual weapon component/socket; no mesh, socket or material is guessed.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Effects") class USceneComponent* WeaponVFXAttachComponent = nullptr;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Effects") FName WeaponVFXSocket = NAME_None;
    // Opt-in owned single-body static meshes only. Ragdolls/constraints/cloth are not supported.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat") TArray<class UStaticMeshComponent*> ImpactFreezeRigidBodies;

    // One character owns the active stance passive and loose tag across ability instances.
    void ReplaceStancePassive(TSubclassOf<class UGameplayEffect> EffectClass, float Level);
    void UpdateExecutionVulnerability(bool bVulnerable);

    UFUNCTION(BlueprintCallable, Category = "Ronin|Combat")
    ESamuraiStance GetCurrentStance() const { return CurrentStance; }

    // =======================================================================
    // VFX & SYSTEMIC EFFECTS
    // =======================================================================
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Effects")
    class UNiagaraSystem* BloodKarmaEffect;

    UPROPERTY()
    class UNiagaraComponent* ActiveAuraVFX;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Effects")
    EAuraDriveStat ActiveAuraStat = EAuraDriveStat::Potential;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Effects")
    FName AuraColorParameter = NAME_None;

    UFUNCTION(BlueprintCallable, Category = "Ronin|Effects")
    void UpdateDynamicAura();

    UFUNCTION(BlueprintCallable, Category = "Ronin|Effects")
    void ActivateWeaponVFX(class UNiagaraSystem* NewEffect);

    // =======================================================================
    // WORLD INTERACTION & DIALOGUE
    // =======================================================================
    UFUNCTION(BlueprintCallable, Category = "Ronin|Interaction")
    void Interact();

    UFUNCTION(BlueprintCallable, Category = "Dialogue")
    void SelectDialogueOption(FString SelectedText);

    // =======================================================================
    // ECONOMY, INVENTORY, & CRAFTING WRAPPERS
    // =======================================================================
    UFUNCTION(BlueprintCallable, Category = "Ronin|Inventory")
    bool ConsumeLocalItem(FString ItemName);

    UFUNCTION(BlueprintCallable, Category = "Ronin|Inventory")
    bool CraftLocalItem(FString RecipeName);

    UFUNCTION(BlueprintCallable, Category = "Ronin|Settlement")
    bool DepositToLocalSettlement(FString RegionID, FString ItemName, int32 Quantity);

    UFUNCTION(BlueprintCallable, Category = "Ronin|Settlement")
    bool WithdrawFromLocalSettlement(FString RegionID, FString ItemName, int32 Quantity);

    // =======================================================================
    // STATS & KARMA
    // =======================================================================
    UPROPERTY(ReplicatedUsing = OnRep_CurrentKarma, BlueprintReadWrite, Category = "Ronin|Stats")
    float CurrentKarma = 0.0f;

    UFUNCTION()
    void OnRep_CurrentKarma();

    // Server gameplay must validate an occurrence before allowing a client report.
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Nemesis|Multiplayer")
    bool AuthorizeCrimeReport(const FString& CrimeCommitted, float KarmaShift);

    UFUNCTION(Server, Reliable, WithValidation, BlueprintCallable, Category = "Nemesis|Multiplayer")
    void Server_CommitCrime(float KarmaIncrease, const FString& CrimeCommitted);

    // =======================================================================
    // DNA & DYNASTY
    // =======================================================================
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|DNA")
    FHunterProfile PlayerDNA;

    UFUNCTION(BlueprintCallable, Category = "Ronin|DNA")
    void InitializePlayerDNA(FHunterProfile InheritedDNA);

    UFUNCTION(BlueprintCallable, Category = "Ronin|DNA")
    FHunterProfile GetPlayerDNA() const { return PlayerDNA; }

    UFUNCTION(BlueprintCallable, Category = "Ronin|Dynasty")
    void ReincarnateAsHeir(FString HeirID);

    UFUNCTION(Server, Reliable)
    void Server_RequestReincarnation(const FString& HeirID);

    UFUNCTION(BlueprintCallable, Category = "Ronin|Dynasty")
    TArray<FString> GetAvailableHeirs();

    UFUNCTION(BlueprintImplementableEvent, Category = "Ronin|Dynasty")
    void OnShowHeirUI();

    UFUNCTION(BlueprintImplementableEvent, Category = "Ronin|UI")
    void OnOpenSettlementUI(const FString& RegionID);

    UFUNCTION(BlueprintCallable, Category = "Ronin|Legacy")
    void HandleDeath();

    UPROPERTY(ReplicatedUsing = OnRep_LifeState, BlueprintReadOnly, Category = "Ronin|Status")
    bool bHasInitiatedDeath = false;
    UFUNCTION() void OnRep_LifeState();

protected:
    virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
    virtual void PossessedBy(AController* NewController) override;
    virtual void UnPossessed() override;
    virtual void OnRep_Controller() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ronin|Input") class UInputAction* InteractAction = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    ESamuraiStance CurrentStance;

    void HandleSurvivalStats(float DeltaTime);

    // --- INPUT ---
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ronin|Input")
    class UInputMappingContext* DefaultMappingContext;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ronin|Input")
    class UInputAction* MoveAction;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ronin|Input")
    class UInputAction* LookAction;

    // --- CAMERA ---
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ronin|Camera")
    class USpringArmComponent* CameraBoom;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ronin|Camera")
    class UCameraComponent* FollowCamera;

    UPROPERTY(BlueprintReadOnly, Category = "Ronin|Legacy")
    bool bIsDying = false;

    virtual void BeginPlay() override;
    virtual void Tick(float DeltaTime) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    // --- SYSTEMS ---
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ronin|GAS")
    class UAbilitySystemComponent* AbilitySystemComponent;

    UPROPERTY()
    class URoninAttributeSet* AttributeSet;

    void Move(const FInputActionValue& Value);
    void Look(const FInputActionValue& Value);

private:
    void SetAuraParameter(FName ParameterName, float Value, FLinearColor Color);
    void UpdateAuraVFX(FName ParameterName, float Value);
    void ProcessSurvivalMetrics(float DeltaTime);
    void StepCombat(const FRoninCombatInput& Input, bool bReplay);
    void AcquireImpactFreeze();
    void ReleaseImpactFreeze();
    void RebindAttributeDelegates();
    void UnbindAttributeDelegates();
    void HandleBloodChanged(const struct FOnAttributeChangeData& Data);
    void HandlePotentialChanged(const struct FOnAttributeChangeData& Data);
    void HandleStaminaChanged(const struct FOnAttributeChangeData& Data);
    void UpdateLimpState();
    void RefreshInputMapping();
    void RemoveInputMapping();
    bool PrepareEconomyProfile();
    void RefreshEconomyProfile();
    void SyncLiveVitals();
    void ClearOwnedCombatState();

    FRoninCombatHistory CombatHistory;
    FRoninCombatInput SampledCombatInput;
    TMap<FString, float> AuthorizedCrimeReports;
    double CombatAccumulator = 0.0;
    int64 CombatHighWaterFrame = 0;
    int64 FreezeEndFrame = 0;
    bool bInsideCombatStep = false;
    bool bOwnsImpactFreeze = false;
    bool bSavedPauseAnims = false;
    bool bSavedMovementTick = false;
    FVector SavedMovementVelocity = FVector::ZeroVector;
    TWeakObjectPtr<class USkeletalMeshComponent> FrozenMesh;
    TWeakObjectPtr<class UCharacterMovementComponent> FrozenMovement;
    struct FFrozenRigidBody {
        TWeakObjectPtr<class UStaticMeshComponent> Component;
        FVector LinearVelocity = FVector::ZeroVector;
        FVector AngularVelocity = FVector::ZeroVector;
        bool bWasAwake = false;
    };
    TArray<FFrozenRigidBody> FrozenRigidBodies;
    // Confirmed side effects are deliberately not part of speculative snapshots.
    TMap<FString, int64> ConfirmedCombatOutcomes;
    FDelegateHandle BloodChangedHandle;
    FDelegateHandle PotentialChangedHandle;
    FDelegateHandle StaminaChangedHandle;
    TWeakObjectPtr<class UAbilitySystemComponent> BoundAttributeASC;
    TWeakObjectPtr<class UEnhancedInputLocalPlayerSubsystem> BoundInputSubsystem;
    UPROPERTY(Transient) class UInputMappingContext* BoundMappingContext = nullptr;
    UPROPERTY(Transient) class UNiagaraComponent* ActiveWeaponVFX = nullptr;
    UPROPERTY(Transient) class UMaterialInstanceDynamic* DurabilityMaterial = nullptr;
    FName DurabilityScalarParameter = NAME_None;
    float BaselineWalkSpeed = 0.f;
    bool bHasLimpBaseline = false;
    FGameplayTag OwnedStanceTag;
    bool bOwnsVulnerabilityTag = false;
    FActiveGameplayEffectHandle OwnedStancePassive;
    UPROPERTY(Transient) TSubclassOf<class UGameplayEffect> OwnedStancePassiveClass;
    float OwnedStancePassiveLevel = 0.f;
};