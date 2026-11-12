#pragma once

#include "CoreMinimal.h"
#include "SettlementHub.h"
#include "VillageHub.generated.h"

UCLASS(BlueprintType, Blueprintable)
class LASTRONIN_API AVillageHub : public ASettlementHub
{
    GENERATED_BODY()

public:
    AVillageHub();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Village|Economy")
    float LocalTaxRateModifier = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Village|Economy")
    float BaseProductionRate = 25.0f;

    UFUNCTION(BlueprintCallable, Category = "Village|Interactions")
    void OpenLocalNoticeBoardUI();

    UFUNCTION(BlueprintCallable, Category = "Village|Conquest")
    void ReassignVillageOverlord(const FString& NewFortressRegionID, const FString& CapturingClanID);

protected:
    virtual void BeginPlay() override;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Village|Mesh")
    UStaticMeshComponent* VillageCenterStoneMesh;
};