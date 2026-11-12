#include "DialogueSystem.h"
#include "DeepSeek_AI.h" 
#include "NemesisManager.h" 
#include "RoninCharacter.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/GameInstance.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"

UDialogueSystem::UDialogueSystem() {}

void UDialogueSystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    bDeinitializing = false;
    ++SessionToken;
}

void UDialogueSystem::Deinitialize()
{
    bDeinitializing = true;
    ++SessionToken;
    ++RequestRevision;
    if (UDeepSeekSubsystem* AI = GetAIBrain()) AI->InvalidateNPCRequests(CurrentEntityID);
    bSessionActive = false;
    bRequestPending = false;
    bInterrogating = false;
    CurrentEntityID.Empty();
    CurrentDialogueContext.Empty();
    AvailableOptions.Empty();
    ActiveDialogues.Empty();
    CachedOptions.Empty();
    Super::Deinitialize();
}

void UDialogueSystem::EndDialogue()
{
    ++SessionToken;
    ++RequestRevision;
    if (UDeepSeekSubsystem* AI = GetAIBrain()) AI->InvalidateNPCRequests(CurrentEntityID);
    bSessionActive = false;
    bRequestPending = false;
    bInterrogating = false;
    ActiveDialogues.Remove(CurrentEntityID);
    CurrentEntityID.Empty();
    CurrentDialogueContext.Empty();
    AvailableOptions.Empty();
    bHasKnownIntel = false;
    bHasRevealedIntel = false;
    bResolveBroken = false;
    KnownIntelLocation = FVector::ZeroVector;
    RevealedIntelLocation = FVector::ZeroVector;
    if (!bDeinitializing) OnDialogueClosed.Broadcast();
}

UDeepSeekSubsystem* UDialogueSystem::GetAIBrain() const
{
    return GetGameInstance() ? GetGameInstance()->GetSubsystem<UDeepSeekSubsystem>() : nullptr;
}

void UDialogueSystem::StartDialogue(FString EntityID, FString Context)
{
    if (bDeinitializing) return;
    if (EntityID.TrimStartAndEnd().IsEmpty()) { EndDialogue(); return; }
    if (UDeepSeekSubsystem* AI = GetAIBrain())
    {
        AI->InvalidateNPCRequests(CurrentEntityID);
        if (EntityID != CurrentEntityID) AI->InvalidateNPCRequests(EntityID);
    }
    ++SessionToken;
    ++RequestRevision;
    CurrentEntityID = EntityID;
    CurrentDialogueContext = Context;
    bSessionActive = true;
    bRequestPending = false;
    bInterrogating = false;
    bResolveBroken = false;
    bHasKnownIntel = false;
    bHasRevealedIntel = false;
    RevealedIntelLocation = FVector::ZeroVector;
    AvailableOptions.Empty();
    ActiveDialogues.Empty();
    ActiveDialogues.Add(EntityID, true);
    const uint64 StartedSession = SessionToken;
    const uint64 StartedRevision = RequestRevision;
    OnDialogueStart.Broadcast(Context);
    if (StartedSession != SessionToken || StartedRevision != RequestRevision || !bSessionActive || bDeinitializing) return;
    SendDialoguePrompt(FString::Printf(TEXT("NPC %s context: %s. Greeting:"), *EntityID, *Context));
}

void UDialogueSystem::StartInterrogation(FString NPCName, FString Personality, FString PlayerInput)
{
    if (bDeinitializing || !bSessionActive || CurrentEntityID.IsEmpty()) return;
    bool bIdentityMatches = NPCName.Equals(CurrentEntityID, ESearchCase::CaseSensitive);
    if (UNemesisManager* NM = GetGameInstance()->GetSubsystem<UNemesisManager>())
    {
        NM->SynchronizeSimulation();
        if (const FHunterProfile* Profile = NM->ActiveNemeses.Find(CurrentEntityID))
            bIdentityMatches = Profile->bIsAlive && (bIdentityMatches || NPCName.Equals(Profile->HunterName, ESearchCase::CaseSensitive));
    }
    if (!bIdentityMatches)
    {
        UE_LOG(LogTemp, Warning, TEXT("Interrogation rejected: name does not match the active identity."));
        return;
    }
    if (UDeepSeekSubsystem* AI = GetAIBrain()) AI->InvalidateNPCRequests(CurrentEntityID);
    ++SessionToken;
    ++RequestRevision;
    bInterrogating = true;
    bRequestPending = false; // The previous request belongs to the invalidated session.
    bResolveBroken = false;
    bHasKnownIntel = false;
    bHasRevealedIntel = false;
    RevealedIntelLocation = FVector::ZeroVector;
    AvailableOptions.Empty();
    CurrentDialogueContext = FString::Printf(
        TEXT("Interrogation of %s. Personality: %s. Determine resolve only, never authorize execution. "
             "Return ONLY JSON matching the shared samurai schema."), *NPCName, *Personality);
    const uint64 StartedSession = SessionToken;
    const uint64 StartedRevision = RequestRevision;
    OnDialogueStart.Broadcast(CurrentDialogueContext);
    if (StartedSession != SessionToken || StartedRevision != RequestRevision || !bSessionActive || bDeinitializing) return;
    SendDialoguePrompt(FString::Printf(TEXT("%s Player Threat: '%s'."), *CurrentDialogueContext, *PlayerInput));
}

void UDialogueSystem::SendDialoguePrompt(FString Prompt)
{
    if (bDeinitializing || !bSessionActive || CurrentEntityID.IsEmpty() || bRequestPending) return;
    const FString EntityID = CurrentEntityID;
    const uint64 Session = SessionToken;
    const uint64 Revision = ++RequestRevision;
    AvailableOptions.Empty();
    bRequestPending = true; // Before dispatch: cache completion may be inline.
    bHasKnownIntel = false;
    KnownIntelLocation = FVector::ZeroVector;
    UDeepSeekSubsystem* AI = GetAIBrain();
    if (!AI) { ParseDialogueResponse(TEXT("")); return; }

    FString CleanPrompt = Prompt;
    if (UNemesisManager* NM = GetGameInstance()->GetSubsystem<UNemesisManager>())
    {
        NM->SynchronizeSimulation();
        if (const FHunterProfile* Profile = NM->ActiveNemeses.Find(CurrentEntityID))
        {
            if (!Profile->bIsAlive) { ParseDialogueResponse(TEXT("")); return; }
            const FRoninPersona Persona = FRoninPersona::FromProfile(*Profile);
            const UEnum* AlignmentEnum = StaticEnum<EMoralAlignment>();
            const FString Alignment = AlignmentEnum ? AlignmentEnum->GetDisplayNameTextByValue(
                static_cast<int64>(Profile->Alignment)).ToString() : TEXT("unspecified");
            CleanPrompt += FString::Printf(
                TEXT(" NPC identity: %s. Explicit moral alignment: %s (authoritative). "
                     "Derived NPC persona: Karma=%.3f, Heat=%.3f, Fame=%.3f, HudHeat=%.3f. "
                     "Persona affects disposition/notoriety, never reclassifies moral alignment."),
                *Profile->HunterName, *Alignment, Persona.Karma, Persona.Heat, Persona.Fame, Persona.HudHeat);
            if (bInterrogating)
            {
                if (const FRegionData* Region = NM->MapRegions.Find(Profile->ClanID))
                {
                    if (Region->Structures.Num() > 0)
                    {
                        const FVector Target = Region->Structures[0].WorldLocation;
                        bHasKnownIntel = FMath::IsFinite(Target.X) && FMath::IsFinite(Target.Y) && FMath::IsFinite(Target.Z);
                        if (bHasKnownIntel) KnownIntelLocation = Target;
                    }
                }
                CleanPrompt += FString::Printf(TEXT(" Family connections: %d."), Profile->RelationshipWeb.Num());
            }
        }
    }
    CleanPrompt += TEXT(" Return every shared schema field and five distinct nonempty player choices lg/cg/tn/ce/le. "
        "did_break describes resolve only, never execution or permission to kill.");
    if (bInterrogating)
    {
        CleanPrompt += bHasKnownIntel ? FString::Printf(
            TEXT(" Known intel: reveal_x=%.17g, reveal_y=%.17g, reveal_z=%.17g. "
                 "Return these exact coordinates only if resolve breaks; otherwise return zeros."),
            KnownIntelLocation.X, KnownIntelLocation.Y, KnownIntelLocation.Z) :
            TEXT(" No known intel: return reveal coordinates as zero even if resolve breaks.");
    }
    else CleanPrompt += TEXT(" Normal dialogue: did_break=false and reveal coordinates zero.");
    const TWeakObjectPtr<UDialogueSystem> WeakThis(this);
    AI->SendNPCDialoguePrompt(EntityID, CleanPrompt, FOnDeepSeekResponse::CreateLambda(
        [WeakThis, EntityID, Session, Revision](FString Result)
        {
            if (!WeakThis.IsValid() || WeakThis->bDeinitializing || !WeakThis->bSessionActive ||
                WeakThis->SessionToken != Session || WeakThis->RequestRevision != Revision ||
                WeakThis->CurrentEntityID != EntityID) return;
            WeakThis->ParseDialogueResponse(Result);
        }));
}

void UDialogueSystem::ParseDialogueResponse(FString Response)
{
    if (bDeinitializing || !bSessionActive) return;
    // Session/revision guards reject stale callbacks before reaching this function.
    // Release before parsing or broadcasting so failure/retry choices remain usable.
    bRequestPending = false;
    TSharedPtr<FJsonObject> JsonObject;
    FString SpokenText;
    // This reflected parser is also a boundary: do not rely solely on transport validation.
    const auto HasValidSchema = [&JsonObject]()
    {
        for (const TCHAR* Key : { TEXT("npc_line"), TEXT("personality_summary"), TEXT("lg"), TEXT("cg"), TEXT("tn"), TEXT("ce"), TEXT("le") })
        {
            const TSharedPtr<FJsonValue>* Field = JsonObject->Values.Find(Key);
            FString Value;
            if (!Field || !Field->IsValid() || (*Field)->Type != EJson::String ||
                !JsonObject->TryGetStringField(Key, Value) || Value.TrimStartAndEnd().IsEmpty()) return false;
        }
        const TSharedPtr<FJsonValue>* Broken = JsonObject->Values.Find(TEXT("did_break"));
        if (!Broken || !Broken->IsValid() || (*Broken)->Type != EJson::Boolean) return false;
        for (const TCHAR* Key : { TEXT("reveal_x"), TEXT("reveal_y"), TEXT("reveal_z") })
        {
            const TSharedPtr<FJsonValue>* Field = JsonObject->Values.Find(Key);
            double Value = 0.0;
            if (!Field || !Field->IsValid() || (*Field)->Type != EJson::Number ||
                !JsonObject->TryGetNumberField(Key, Value) || !FMath::IsFinite(Value)) return false;
        }
        return true;
    };
    if (Response.Len() > 65536 ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response), JsonObject) || !JsonObject.IsValid() ||
        !HasValidSchema() || !JsonObject->TryGetStringField(TEXT("npc_line"), SpokenText))
    {
        AvailableOptions = { TEXT("Continue"), bInterrogating ? TEXT("End Interrogation") : TEXT("Leave") };
        OnDialogueUpdated.Broadcast(TEXT("No valid response received."), AvailableOptions);
        return;
    }

    AvailableOptions.Empty();
    if (bInterrogating)
    {
        bool bDidBreak = false;
        if (JsonObject->TryGetBoolField(TEXT("did_break"), bDidBreak) && bDidBreak)
        {
            bResolveBroken = true;
            double X = 0.0, Y = 0.0, Z = 0.0;
            const bool bComplete = JsonObject->TryGetNumberField(TEXT("reveal_x"), X) &&
                JsonObject->TryGetNumberField(TEXT("reveal_y"), Y) && JsonObject->TryGetNumberField(TEXT("reveal_z"), Z) &&
                FMath::IsFinite(X) && FMath::IsFinite(Y) && FMath::IsFinite(Z);
            // Accept only intel supplied by the authoritative context, including a valid origin.
            if (bComplete && bHasKnownIntel && FVector(X, Y, Z).Equals(KnownIntelLocation, 0.01))
            {
                RevealedIntelLocation = KnownIntelLocation;
                bHasRevealedIntel = true;
                const uint64 Session = SessionToken;
                const uint64 Revision = RequestRevision;
                OnIntelRevealed.Broadcast(CurrentEntityID, RevealedIntelLocation);
                if (bDeinitializing || !bSessionActive || SessionToken != Session || RequestRevision != Revision) return;
            }
        }
    }
    // Both conversation modes use the shared schema's generated player responses.
    for (const TCHAR* Key : { TEXT("lg"), TEXT("cg"), TEXT("tn"), TEXT("ce"), TEXT("le") })
    {
        FString Option;
        if (JsonObject->TryGetStringField(Key, Option) && !Option.TrimStartAndEnd().IsEmpty())
            AvailableOptions.Add(Option);
    }
    if (AvailableOptions.IsEmpty()) AvailableOptions.Add(TEXT("Continue"));
    // SubmitDialogueChoice identifies close by the final index, not a generated label.
    AvailableOptions.Add(bInterrogating ? TEXT("End Interrogation") : TEXT("Leave"));
    OnDialogueUpdated.Broadcast(SpokenText, AvailableOptions);
}

bool UDialogueSystem::CloseDialogueSession(int64 SessionID)
{
    if (bDeinitializing || !bSessionActive || SessionID != GetDialogueSessionID()) return false;
    EndDialogue();
    return true;
}

bool UDialogueSystem::SubmitDialogueChoice(int64 SessionID, int64 ChoiceRevision, int32 ChoiceIndex)
{
    if (bDeinitializing || !bSessionActive || bRequestPending || SessionID != GetDialogueSessionID() ||
        ChoiceRevision != GetDialogueChoiceRevision() || !AvailableOptions.IsValidIndex(ChoiceIndex)) return false;
    if (ChoiceIndex == AvailableOptions.Num() - 1) return CloseDialogueSession(SessionID);
    // Index, not label, identifies close: a model-authored line named Leave is still spoken.
    return SubmitDialogueText(SessionID, ChoiceRevision, AvailableOptions[ChoiceIndex]);
}

bool UDialogueSystem::SubmitDialogueText(int64 SessionID, int64 ChoiceRevision, FString Text)
{
    const FString CleanText = Text.TrimStartAndEnd();
    if (bDeinitializing || !bSessionActive || bRequestPending || CurrentEntityID.IsEmpty() ||
        SessionID != GetDialogueSessionID() || ChoiceRevision != GetDialogueChoiceRevision() ||
        CleanText.IsEmpty() || CleanText.Len() > 4096) return false;
    SendDialoguePrompt(FString::Printf(TEXT("NPC %s. Context: %s. Player said: '%s'. NPC reaction:"),
        *CurrentEntityID, *CurrentDialogueContext, *CleanText));
    return true;
}

void UDialogueSystem::ProcessPlayerResponse(FString Response)
{
    const FString Label = Response.TrimStartAndEnd();
    if (Label.Equals(TEXT("Leave"), ESearchCase::IgnoreCase) ||
        Label.Equals(TEXT("End Interrogation"), ESearchCase::IgnoreCase) ||
        Label.Equals(TEXT("End Dialogue"), ESearchCase::IgnoreCase) ||
        Label.Equals(TEXT("Exit"), ESearchCase::IgnoreCase))
    {
        CloseDialogueSession(GetDialogueSessionID());
        return;
    }
    SubmitDialogueText(GetDialogueSessionID(), GetDialogueChoiceRevision(), Response);
}