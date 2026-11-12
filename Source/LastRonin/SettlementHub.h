#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RoninTypes.h"
#include "SettlementHub.generated.h"

UCLASS(Abstract, BlueprintType, Blueprintable)
class LASTRONIN_API ASettlementHub : public AActor
{
    GENERATED_BODY()

public:
    ASettlementHub();
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    // The master mapping string lookup key matching DT_RegionSeeding
    UPROPERTY(Replicated, EditAnywhere, BlueprintReadWrite, Category = "Settlement|Identity")
    FString RegionID;

    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Settlement|Faction")
    FString OwningClanID = TEXT("Neutral");

    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Settlement|Feudal Anchor")
    FString ParentFortressID;

    UFUNCTION(BlueprintCallable, Category = "Settlement|Data")
    void SyncWithBackendDatabase();

protected:
    virtual void BeginPlay() override;

    FRegionData* GetLiveRegionData() const;
};