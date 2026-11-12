#pragma once

#include "CoreMinimal.h"
#include "SettlementHub.h" // Inherits safely from our unified abstract base hub
#include "SiegeFortress.generated.h"

UCLASS(BlueprintType, Blueprintable)
class LASTRONIN_API ASiegeFortress : public ASettlementHub
{
    GENERATED_BODY()

public:
    ASiegeFortress();
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    UPROPERTY(Replicated, EditAnywhere, BlueprintReadWrite, Category = "Siege|Stats")
    float FortressHealth = 2000.0f;

    UPROPERTY(Replicated, EditAnywhere, BlueprintReadWrite, Category = "Siege|Stats")
    float MaxFortressHealth = 2000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Siege|Stats")
    float HeatGenerationValue = 50.0f;

    virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, class AController* EventInstigator, AActor* DamageCauser) override;

    UFUNCTION(BlueprintCallable, Category = "Siege|Faction")
    void ClaimFortress(const FString& NewClanID, const FString& ConquerorName);

    // Iterates across structural stockpiles and converts gathered lumber/iron into raw Max Health upgrades
    UFUNCTION(BlueprintCallable, Category = "Siege|Economy")
    void ProcessStockpileFortificationUpgrades();

    // FIXED: Swapped out UPROPERTY for a clean reflection UFUNCTION to permit Event routing passes
    UFUNCTION(BlueprintImplementableEvent, Category = "Siege|Visuals")
    void OnFortressCaptured(const FString& NewClanID);

protected:
    virtual void BeginPlay() override;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Siege|Mesh")
    UStaticMeshComponent* FortressMesh;

    void BroadcastSiegeRumor(const FString& EventDescription, float SpreadLevel);
};