#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "RoninTypes.h" // Explicitly required for FHunterProfile structural variable parsing
#include "RoninGameMode.generated.h"

// Forward declare to isolate preprocessor header sweeps and speed up compile tasks
class ARoninCharacter;
class APlayerController;
class AAIController;
class UDataTable;
class UNemesisManager;

/** First matching inclusive persona range wins; empty classes are skipped. */
USTRUCT(BlueprintType)
struct LASTRONIN_API FRoninOfflineControllerRule
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Persona") float MinKarma = -100.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Persona") float MaxKarma = 100.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Persona") float MinHeat = -100.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Persona") float MaxHeat = 100.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Persona") float MinFame = -100.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Persona") float MaxFame = 100.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Persona") TSubclassOf<AAIController> ControllerClass;
};

UCLASS()
class LASTRONIN_API ARoninGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    ARoninGameMode();

    // Server-only identity proof for gameplay mutation boundaries; no caller-selected ID.
    FString GetAuthorizedCharacterID(APlayerController* PlayerPC) const;

    // Fires the exact frame a client successfully joins the server
    virtual void PostLogin(APlayerController* NewPlayer) override;

    virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer) override;
    virtual void Tick(float DeltaSeconds) override;

    // Fires the exact frame a client hits "Quit Game" or disconnects
    virtual void Logout(AController* Exiting) override;

    // Called before PlayerController's base PawnLeavingGame destroys its pawn.
    void PreparePlayerLogout(APlayerController* PlayerPC);

    // Physically spawns the next generation and forces the player to possess them
    UFUNCTION(BlueprintCallable, Category = "Nemesis|Dynasty")
    void ReincarnateAsHeir(APlayerController* PlayerPC, FHunterProfile FallenDNA, FString ChosenHeirID);

protected:
    virtual void BeginPlay() override;

    // The specific AI Controller class we want to possess the player's body when they leave
    UPROPERTY(EditDefaultsOnly, Category = "Nemesis|Multiplayer")
    TSubclassOf<AAIController> NemesisAIControllerClass;

    // The Authoritative Blueprint asset template we want to materialize into open-world grid cells
    UPROPERTY(EditDefaultsOnly, Category = "Nemesis|Spawning Configuration")
    TSubclassOf<ACharacter> AuthoritativeHunterBlueprintClass;

    // Reference configuration pin layer table passed to the population subsystem layout
    UPROPERTY(EditDefaultsOnly, Category = "Nemesis|Spawning Configuration")
    UDataTable* AuthoritativeRegionSeedingTable;

    UPROPERTY(EditDefaultsOnly, Category = "Nemesis|Multiplayer")
    TArray<FRoninOfflineControllerRule> OfflineControllerRules;

    UPROPERTY(EditDefaultsOnly, Category = "Nemesis|Simulation", meta = (ClampMin = "0.05"))
    float PopulationScanInterval = 1.f;

    UPROPERTY(EditDefaultsOnly, Category = "Nemesis|Simulation", meta = (ClampMin = "0"))
    float WorldTimeMultiplier = 1.f;

    UPROPERTY(EditDefaultsOnly, Category = "Nemesis|Dynasty")
    TSubclassOf<ARoninCharacter> SuccessorPawnClass;

    UPROPERTY(EditDefaultsOnly, Category = "Nemesis|Dynasty")
    bool bUseSuccessionShrine = false;

    UPROPERTY(EditDefaultsOnly, Category = "Nemesis|Dynasty", meta = (EditCondition = "bUseSuccessionShrine"))
    FTransform SuccessionShrineTransform = FTransform::Identity;

private:
    UNemesisManager* GetNemesisManager() const;
    FString ResolveAccount(APlayerController* PlayerPC);
    void BindPlayerAccount(APlayerController* PlayerPC);
    void MergeLiveProfile(ARoninCharacter* Pawn, UNemesisManager* Manager);
    bool IsPlayerPossessedID(const FString& ID) const;
    ARoninCharacter* FindLivingRonin(const FString& ID) const;
    ARoninCharacter* TransferToProfile(APlayerController* PlayerPC, const FHunterProfile& Profile,
        const FTransform& SpawnTransform);
    void RetirePhysicalDuplicate(const FString& ID, UNemesisManager* Manager);
    TSubclassOf<AAIController> SelectOfflineController(const FHunterProfile& Profile) const;

    UPROPERTY(Transient)
    bool bTransferInProgress = false;

    float PopulationScanElapsed = 0.f;
    TSet<TWeakObjectPtr<APlayerController>> PreparedLogoutPlayers;
    TMap<TWeakObjectPtr<APlayerController>, FString> SessionAccounts;
    TMap<TWeakObjectPtr<APlayerController>, TWeakObjectPtr<ARoninCharacter>> BoundPlayerPawns;
};