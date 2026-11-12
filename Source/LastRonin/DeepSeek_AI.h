#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "RoninTypes.h" 
#include "DeepSeek_AI.generated.h"

USTRUCT(BlueprintType)
struct FNPCMemory
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadWrite) FString InteractionHistory = TEXT("");
    UPROPERTY(BlueprintReadWrite) int32 LocalReputation = 0;
};

// DELEGATES
DECLARE_DELEGATE_OneParam(FOnDeepSeekResponse, FString);
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnDeepSeekResponseDynamic, FString, ResponseText);
DECLARE_DYNAMIC_DELEGATE_SixParams(FOnGeminiDialogueResponse, FString, NPCLine, FString, LawfulGood, FString, ChaoticGood, FString, TrueNeutral, FString, ChaoticEvil, FString, LawfulEvil);

UCLASS()
class LASTRONIN_API UDeepSeekSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    UDeepSeekSubsystem();
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    void SendPromptToDeepSeek(const FString& Prompt, const FOnDeepSeekResponse& ResponseDelegate);

    // Session boundaries invalidate pending NPC callbacks/summary writes, not stored memory.
    void InvalidateNPCRequests(const FString& NPCID);

    // Same transport/schema; guards both delivery and summary writes by NPC revision.
    void SendNPCDialoguePrompt(const FString& NPCID, const FString& Prompt, const FOnDeepSeekResponse& ResponseDelegate);

    UFUNCTION(BlueprintCallable, Category = "AI|Dialogue")
    void GenerateSamuraiDialogue(const FHunterProfile& HunterDNA, FString RecentEvent, FOnGeminiDialogueResponse ResponseDelegate);

    UFUNCTION(BlueprintCallable, Category = "AI|Dialogue")
    void GenerateNPCReaction(FString NPCID, FString NPCName, FString NPCType, FString RecentEvent, FOnDeepSeekResponseDynamic ResponseDelegate);

    UFUNCTION(BlueprintCallable, Category = "AI|Dialogue")
    void GeneratePlayerChoices(FString NPCLine, FOnDeepSeekResponseDynamic ResponseDelegate);

    UFUNCTION(BlueprintCallable, Category = "AI|Memory")
    void UpdateNPCMemory(FString NPCID, FString PlayerAction, int32 ReputationChange);

    UFUNCTION(BlueprintCallable, Category = "DeepSeek")
    void GenerateCharacterBackground(FString Race, FString Class, FString WorldContext);

    UFUNCTION(BlueprintCallable, Category = "DeepSeek")
    void GenerateWorldEvent(FString Location, FString TimeOfDay);

    UFUNCTION(BlueprintCallable, Category = "DeepSeek")
    void GenerateCombatStrategy(FString EnemyType, TArray<FString> PlayerAbilities);

    // Schema helper exposed for internal translation layers
    TSharedPtr<FJsonObject> CreateSamuraiSchema();

protected:
    void CacheResponse(FString Prompt, FString Response);
    FString GetCachedResponse(FString Prompt);

private:
    TMap<FString, FString> CachedResponses;
    // FIFO, bounded by entry count and total TCHAR count (keys plus values).
    TArray<FString> CachedResponseOrder;
    TMap<FString, FNPCMemory> PersonalMemoryBank;
    TMap<FString, uint64> NPCMemoryRevisions;
    TMap<uint64, FHttpRequestPtr> ActiveRequests;
    uint64 LifecycleGeneration = 0;
    uint64 NextRequestID = 0;
    bool bDeinitializing = false;

    void SendPromptAttempt(const FString& Prompt, FOnDeepSeekResponse ResponseDelegate,
        uint64 Generation, bool bLocal, TSharedRef<bool> Completed);

    FString GeminiBaseURL = TEXT("https://generativelanguage.googleapis.com/v1beta/models/gemini-1.5-flash:generateContent?key=");
    FString GeminiAPIKey = TEXT("PASTE_YOUR_KEY_HERE");
};