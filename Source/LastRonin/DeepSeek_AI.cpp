#include "DeepSeek_AI.h"
#include "NemesisManager.h" 
#include "LocalAISubsystem.h" 
#include "Engine/GameInstance.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    bool ReadAIObject(const FString& Text, TSharedPtr<FJsonObject>& Object)
    {
        return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) && Object.IsValid();
    }

    bool IsValidAIResponse(const FString& Text)
    {
        if (Text.Len() > 65536) return false;
        TSharedPtr<FJsonObject> Object;
        if (!ReadAIObject(Text, Object)) return false;
        // TryGetNumberField can coerce strings; check the actual JSON types first.
        for (const TCHAR* Key : { TEXT("npc_line"), TEXT("personality_summary"), TEXT("lg"), TEXT("cg"), TEXT("tn"), TEXT("ce"), TEXT("le") })
        {
            const TSharedPtr<FJsonValue>* Field = Object->Values.Find(Key);
            FString Value;
            if (!Field || !Field->IsValid() || (*Field)->Type != EJson::String ||
                !Object->TryGetStringField(Key, Value) || Value.TrimStartAndEnd().IsEmpty()) return false;
        }
        const TSharedPtr<FJsonValue>* Broken = Object->Values.Find(TEXT("did_break"));
        if (!Broken || !Broken->IsValid() || (*Broken)->Type != EJson::Boolean) return false;
        for (const TCHAR* Key : { TEXT("reveal_x"), TEXT("reveal_y"), TEXT("reveal_z") })
        {
            const TSharedPtr<FJsonValue>* Field = Object->Values.Find(Key);
            double Value = 0.0;
            if (!Field || !Field->IsValid() || (*Field)->Type != EJson::Number ||
                !Object->TryGetNumberField(Key, Value) || !FMath::IsFinite(Value)) return false;
        }
        return true;
    }

    bool IsCacheableAIResponse(const FString& Text)
    {
        return IsValidAIResponse(Text);
    }

    TSharedPtr<FJsonObject> FirstAIObject(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values || Values->IsEmpty() ||
            !(*Values)[0].IsValid() || (*Values)[0]->Type != EJson::Object) return nullptr;
        return (*Values)[0]->AsObject();
    }

    bool ExtractAIContent(const FString& Envelope, bool bLocal, FString& Text)
    {
        // Bound JSON parsing separately from the stricter generated-content limit.
        if (Envelope.Len() > 1048576) return false;
        TSharedPtr<FJsonObject> Root;
        if (!ReadAIObject(Envelope, Root)) return false;
        if (bLocal)
        {
            const TSharedPtr<FJsonObject> Choice = FirstAIObject(Root, TEXT("choices"));
            if (Choice.IsValid())
            {
                const TSharedPtr<FJsonObject>* Message = nullptr;
                if (Choice->TryGetObjectField(TEXT("message"), Message) && Message && Message->IsValid() &&
                    (*Message)->TryGetStringField(TEXT("content"), Text)) return IsValidAIResponse(Text);
                if (Choice->TryGetStringField(TEXT("text"), Text)) return IsValidAIResponse(Text);
            }
            return Root->TryGetStringField(TEXT("content"), Text) && IsValidAIResponse(Text);
        }
        const TSharedPtr<FJsonObject> Candidate = FirstAIObject(Root, TEXT("candidates"));
        const TSharedPtr<FJsonObject>* Content = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
        if (!Candidate.IsValid() || !Candidate->TryGetObjectField(TEXT("content"), Content) || !Content || !Content->IsValid() ||
            !(*Content)->TryGetArrayField(TEXT("parts"), Parts) || !Parts) return false;
        Text.Empty();
        for (const TSharedPtr<FJsonValue>& Part : *Parts)
        {
            FString Fragment;
            if (Part.IsValid() && Part->Type == EJson::Object && Part->AsObject().IsValid() &&
                Part->AsObject()->TryGetStringField(TEXT("text"), Fragment))
            {
                if (Fragment.Len() > 65536 - Text.Len()) return false;
                Text += Fragment;
            }
        }
        return IsValidAIResponse(Text);
    }
}

void UDeepSeekSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    ++LifecycleGeneration;
    bDeinitializing = false;
}

void UDeepSeekSubsystem::Deinitialize()
{
    bDeinitializing = true;
    ++LifecycleGeneration;
    TMap<uint64, FHttpRequestPtr> Requests = MoveTemp(ActiveRequests);
    ActiveRequests.Empty();
    for (auto& Pair : Requests)
    {
        if (Pair.Value.IsValid())
        {
            Pair.Value->OnProcessRequestComplete().Unbind();
            Pair.Value->CancelRequest();
        }
    }
    // Memory and its public struct shape are deliberately not reset.
    Super::Deinitialize();
}

UDeepSeekSubsystem::UDeepSeekSubsystem() { GConfig->GetString(TEXT("GenerativeAI"), TEXT("GeminiAPIKey"), GeminiAPIKey, GGameIni); }

TSharedPtr<FJsonObject> UDeepSeekSubsystem::CreateSamuraiSchema()
{
    TSharedPtr<FJsonObject> Schema = MakeShareable(new FJsonObject());
    Schema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject());

    auto AddProp = [&](FString Name, FString Desc) {
        TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
        Obj->SetStringField(TEXT("type"), TEXT("string"));
        Obj->SetStringField(TEXT("description"), Desc);
        Props->SetObjectField(Name, Obj);
        };

    auto AddFloatProp = [&](FString Name, FString Desc) {
        TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
        Obj->SetStringField(TEXT("type"), TEXT("number"));
        Obj->SetStringField(TEXT("description"), Desc);
        Props->SetObjectField(Name, Obj);
        };

    AddProp(TEXT("npc_line"), TEXT("The spoken dialogue response sentence from the prisoner."));
    AddProp(TEXT("personality_summary"), TEXT("Concise update of the NPC's internal state and fear metrics."));

    TSharedPtr<FJsonObject> BreakBool = MakeShareable(new FJsonObject());
    BreakBool->SetStringField(TEXT("type"), TEXT("boolean"));
    BreakBool->SetStringField(TEXT("description"), TEXT("True ONLY if the player successfully breaks their resolve."));
    Props->SetObjectField(TEXT("did_break"), BreakBool);

    AddFloatProp(TEXT("reveal_x"), TEXT("The target map X coordinate if broken. Default to 0.0"));
    AddFloatProp(TEXT("reveal_y"), TEXT("The target map Y coordinate if broken. Default to 0.0"));
    AddFloatProp(TEXT("reveal_z"), TEXT("The target map Z coordinate if broken. Default to 0.0"));

    AddProp(TEXT("lg"), TEXT("Lawful Good player option response template."));
    AddProp(TEXT("cg"), TEXT("Chaotic Good player option response template."));
    AddProp(TEXT("tn"), TEXT("True Neutral player option response template."));
    AddProp(TEXT("ce"), TEXT("Chaotic Evil player option response template."));
    AddProp(TEXT("le"), TEXT("Lawful Evil player option response template."));

    Schema->SetObjectField(TEXT("properties"), Props);
    return Schema;
}

void UDeepSeekSubsystem::SendPromptToDeepSeek(const FString& Prompt, const FOnDeepSeekResponse& ResponseDelegate)
{
    if (bDeinitializing) return;
    const FString Cached = GetCachedResponse(Prompt);
    if (!Cached.IsEmpty())
    {
        // Public API retains inline cache completion; callers must be reentrancy-safe.
        ResponseDelegate.ExecuteIfBound(Cached);
        return;
    }
    SendPromptAttempt(Prompt, ResponseDelegate, LifecycleGeneration, false, MakeShared<bool>(false));
}

void UDeepSeekSubsystem::SendPromptAttempt(const FString& Prompt, FOnDeepSeekResponse ResponseDelegate,
    uint64 Generation, bool bLocal, TSharedRef<bool> Completed)
{
    if (bDeinitializing || Generation != LifecycleGeneration || *Completed) return;
    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
    Request->SetTimeout(30.0f);
    Request->SetURL(bLocal ? TEXT("http://127.0.0.1:8080/v1/chat/completions") : GeminiBaseURL + GeminiAPIKey);
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    if (bLocal)
    {
        // Local chat endpoint takes messages, never Gemini contents/generationConfig.
        FString SchemaText;
        FJsonSerializer::Serialize(CreateSamuraiSchema().ToSharedRef(), TJsonWriterFactory<>::Create(&SchemaText));
        const TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
        Message->SetStringField(TEXT("role"), TEXT("user"));
        Message->SetStringField(TEXT("content"), Prompt + TEXT("\nReturn only JSON matching this schema: ") + SchemaText);
        TArray<TSharedPtr<FJsonValue>> Messages;
        Messages.Add(MakeShared<FJsonValueObject>(Message));
        Root->SetArrayField(TEXT("messages"), Messages);
        Root->SetBoolField(TEXT("stream"), false);
        Root->SetNumberField(TEXT("temperature"), 0.2);
    }
    else
    {
        const TSharedRef<FJsonObject> Content = MakeShared<FJsonObject>();
        const TSharedRef<FJsonObject> Part = MakeShared<FJsonObject>();
        Part->SetStringField(TEXT("text"), Prompt);
        TArray<TSharedPtr<FJsonValue>> Parts;
        Parts.Add(MakeShared<FJsonValueObject>(Part));
        Content->SetArrayField(TEXT("parts"), Parts);
        TArray<TSharedPtr<FJsonValue>> Contents;
        Contents.Add(MakeShared<FJsonValueObject>(Content));
        Root->SetArrayField(TEXT("contents"), Contents);
        const TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();
        Config->SetStringField(TEXT("response_mime_type"), TEXT("application/json"));
        Config->SetObjectField(TEXT("response_schema"), CreateSamuraiSchema());
        Config->SetNumberField(TEXT("temperature"), 0.2);
        Root->SetObjectField(TEXT("generationConfig"), Config);
    }
    FString Body;
    FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Body));
    Request->SetContentAsString(Body);
    const uint64 RequestID = ++NextRequestID;
    ActiveRequests.Add(RequestID, Request);
    const TSharedRef<bool> AttemptCompleted = MakeShared<bool>(false);
    const TWeakObjectPtr<UDeepSeekSubsystem> WeakThis(this);
    auto FinishAttempt = [WeakThis, Prompt, ResponseDelegate, Generation, bLocal, Completed, AttemptCompleted, RequestID]
        (FHttpRequestPtr Req, FHttpResponsePtr Response, bool bSuccessful)
    {
        if (*AttemptCompleted) return;
        *AttemptCompleted = true;
        if (!WeakThis.IsValid()) return;
        WeakThis->ActiveRequests.Remove(RequestID);
        if (WeakThis->bDeinitializing || WeakThis->LifecycleGeneration != Generation || *Completed) return;
        FString Result;
        const bool bValidResult = bSuccessful && Response.IsValid() && Response->GetResponseCode() >= 200 &&
            Response->GetResponseCode() < 300 && ExtractAIContent(Response->GetContentAsString(), bLocal, Result);
        if (bLocal && WeakThis->GetGameInstance())
        {
            if (ULocalAISubsystem* Local = WeakThis->GetGameInstance()->GetSubsystem<ULocalAISubsystem>())
                Local->RecordLocalResponse(bValidResult);
        }
        if (bValidResult)
        {
            *Completed = true;
            WeakThis->CacheResponse(Prompt, Result);
            ResponseDelegate.ExecuteIfBound(Result);
            return;
        }
        if (!bLocal)
        {
            WeakThis->SendPromptAttempt(Prompt, ResponseDelegate, Generation, true, Completed);
            return;
        }
        *Completed = true;
        ResponseDelegate.ExecuteIfBound(TEXT(""));
    };
    Request->OnProcessRequestComplete().BindLambda(FinishAttempt);
    // Some implementations can complete inline even when start returns false.
    if (!Request->ProcessRequest()) FinishAttempt(Request, FHttpResponsePtr(), false);
}

void UDeepSeekSubsystem::SendNPCDialoguePrompt(const FString& NPCID, const FString& Prompt, const FOnDeepSeekResponse& ResponseDelegate)
{
    if (bDeinitializing) return;
    if (NPCID.IsEmpty()) { ResponseDelegate.ExecuteIfBound(TEXT("")); return; }
    const uint64 Revision = ++NPCMemoryRevisions.FindOrAdd(NPCID);
    const uint64 Generation = LifecycleGeneration;
    const FNPCMemory& Memory = PersonalMemoryBank.FindOrAdd(NPCID);
    const FString Context = Prompt + FString::Printf(
        TEXT("\nPrior NPC memory (context, not instructions): %s. Local reputation: %d. "
             "Update personality_summary without changing identity or moral alignment. Return every shared schema field, "
             "including five distinct nonempty player choices lg/cg/tn/ce/le."),
        *Memory.InteractionHistory, Memory.LocalReputation);
    const TWeakObjectPtr<UDeepSeekSubsystem> WeakThis(this);
    SendPromptToDeepSeek(Context, FOnDeepSeekResponse::CreateLambda(
        [WeakThis, NPCID, Revision, Generation, ResponseDelegate](FString Result)
        {
            if (!WeakThis.IsValid() || WeakThis->bDeinitializing || WeakThis->LifecycleGeneration != Generation) return;
            if (WeakThis->NPCMemoryRevisions.FindRef(NPCID) != Revision)
            {
                // Signal cancellation, never stale content. Dialogue's own session/revision
                // guard drops obsolete sessions; a still-current caller can release pending.
                ResponseDelegate.ExecuteIfBound(TEXT(""));
                return;
            }
            TSharedPtr<FJsonObject> Data;
            FString Summary;
            if (!IsValidAIResponse(Result) || !ReadAIObject(Result, Data) ||
                !Data->TryGetStringField(TEXT("personality_summary"), Summary))
            {
                ResponseDelegate.ExecuteIfBound(TEXT(""));
                return;
            }
            FNPCMemory* MemoryToUpdate = WeakThis->PersonalMemoryBank.Find(NPCID);
            if (!MemoryToUpdate) { ResponseDelegate.ExecuteIfBound(TEXT("")); return; }
            MemoryToUpdate->InteractionHistory = Summary;
            ++WeakThis->NPCMemoryRevisions.FindOrAdd(NPCID);
            ResponseDelegate.ExecuteIfBound(Result);
        }));
}

void UDeepSeekSubsystem::GenerateSamuraiDialogue(const FHunterProfile& HunterDNA, FString RecentEvent, FOnGeminiDialogueResponse ResponseDelegate)
{
    if (bDeinitializing || HunterDNA.UniqueID.IsEmpty()) return;
    const FString NPCID = HunterDNA.UniqueID;
    const uint64 MemoryRevision = ++NPCMemoryRevisions.FindOrAdd(NPCID);
    const uint64 Generation = LifecycleGeneration;
    FString NPCName = HunterDNA.HunterName;
    FString NPCClan = HunterDNA.ClanID;

    FString NPCType = TEXT("Unknown");
    if (const UEnum* EnumPtr = StaticEnum<EHunterClass>())
    {
        NPCType = EnumPtr->GetDisplayNameTextByValue(static_cast<int64>(HunterDNA.HunterClass)).ToString();
    }

    FString NPCAlignmentStr;
    switch (HunterDNA.Alignment)
    {
    case EMoralAlignment::LawfulGood:
        NPCAlignmentStr = TEXT("Lawful Good. You are honorable, peaceful, adhere strictly to Bushido, and have very high Karma.");
        break;
    case EMoralAlignment::ChaoticGood:
        NPCAlignmentStr = TEXT("Chaotic Good. You do the right thing, but you are rebellious, ignore the rules of lords, and have positive Karma.");
        break;
    case EMoralAlignment::TrueNeutral:
        NPCAlignmentStr = TEXT("True Neutral. You are pragmatic, indifferent, focused only on survival, and have neutral Karma.");
        break;
    case EMoralAlignment::LawfulEvil:
        NPCAlignmentStr = TEXT("Lawful Evil. You are manipulative, greedy, use the law/tradition to exploit others, and have negative Karma.");
        break;
    case EMoralAlignment::ChaoticEvil:
        NPCAlignmentStr = TEXT("Chaotic Evil. You are cruel, unpredictable, ruthless, and have extremely negative Karma.");
        break;
    default:
        NPCAlignmentStr = TEXT("True Neutral. You are pragmatic and indifferent.");
        break;
    }

    FNPCMemory& Memory = PersonalMemoryBank.FindOrAdd(NPCID);
    FString WorldContext = TEXT("The province is currently quiet.");

    if (UNemesisManager* NemesisManager = GetGameInstance()->GetSubsystem<UNemesisManager>())
    {
        FRumorData ActiveRumor = NemesisManager->GetMostRelevantRumor(HunterDNA.WorldLocation);
        FString RumorTextToUse = ActiveRumor.Fact.IsEmpty() ? ActiveRumor.RawRumorText : ActiveRumor.Fact;

        if (!RumorTextToUse.IsEmpty())
        {
            if (HunterDNA.HunterClass >= EHunterClass::BanditChief) {
                WorldContext = FString::Printf(TEXT("CURRENT POLITICAL RUMOR: '%s'"), *RumorTextToUse);
            }
            else {
                FString FolkLoreText = ActiveRumor.Folklore.IsEmpty() ? RumorTextToUse : ActiveRumor.Folklore;
                WorldContext = FString::Printf(TEXT("LOCAL FOLKLORE/GOSSIP: '%s'"), *FolkLoreText);
            }
        }
    }

    FString Prompt = FString::Printf(
        TEXT("You are %s, a %s belonging to the '%s' Clan. "
            "CRITICAL: Your character alignment is strictly %s. All reactions MUST filter through this morality. "
            "%s "
            "Your CURRENT INTERNAL VIEW of the player: %s. "
            "THE PLAYER JUST: %s. "
            "TASK: Respond based on your alignment and knowledge of the world, then update your personality_summary to evolve your view of the player."),
        *NPCName, *NPCType, *NPCClan, *NPCAlignmentStr, *WorldContext, *Memory.InteractionHistory, *RecentEvent
    );

    const FRoninPersona Persona = FRoninPersona::FromProfile(HunterDNA);
    Prompt += FString::Printf(
        TEXT(" Derived NPC persona context: Karma=%.3f, Heat=%.3f, Fame=%.3f, HudHeat=%.3f. "
             "These describe disposition and notoriety only; the explicit moral alignment remains authoritative. "
             "Return every shared schema field and five distinct nonempty player choices."),
        Persona.Karma, Persona.Heat, Persona.Fame, Persona.HudHeat);
    const TWeakObjectPtr<UDeepSeekSubsystem> WeakThis(this);
    SendPromptToDeepSeek(Prompt, FOnDeepSeekResponse::CreateLambda(
        [WeakThis, NPCID, MemoryRevision, Generation, ResponseDelegate](FString RawJson)
        {
            if (!WeakThis.IsValid() || WeakThis->bDeinitializing || WeakThis->LifecycleGeneration != Generation ||
                WeakThis->NPCMemoryRevisions.FindRef(NPCID) != MemoryRevision) return;
            TSharedPtr<FJsonObject> Data;
            FString Summary, Line, LG, CG, TN, CE, LE;
            if (!IsValidAIResponse(RawJson) || !ReadAIObject(RawJson, Data) ||
                !Data->TryGetStringField(TEXT("personality_summary"), Summary) || Summary.TrimStartAndEnd().IsEmpty() ||
                !Data->TryGetStringField(TEXT("npc_line"), Line) ||
                !Data->TryGetStringField(TEXT("lg"), LG) || !Data->TryGetStringField(TEXT("cg"), CG) ||
                !Data->TryGetStringField(TEXT("tn"), TN) || !Data->TryGetStringField(TEXT("ce"), CE) ||
                !Data->TryGetStringField(TEXT("le"), LE))
            {
                ResponseDelegate.ExecuteIfBound(TEXT(""), TEXT(""), TEXT(""), TEXT(""), TEXT(""), TEXT(""));
                return;
            }
            FNPCMemory* MemoryToUpdate = WeakThis->PersonalMemoryBank.Find(NPCID);
            if (!MemoryToUpdate) return;
            MemoryToUpdate->InteractionHistory = Summary;
            ++WeakThis->NPCMemoryRevisions.FindOrAdd(NPCID);
            ResponseDelegate.ExecuteIfBound(Line, LG, CG, TN, CE, LE);
        }));
}

void UDeepSeekSubsystem::GenerateNPCReaction(FString NPCID, FString NPCName, FString NPCType, FString RecentEvent, FOnDeepSeekResponseDynamic ResponseDelegate)
{
    if (bDeinitializing || NPCID.IsEmpty()) return;
    const uint64 Revision = ++NPCMemoryRevisions.FindOrAdd(NPCID);
    const uint64 Generation = LifecycleGeneration;
    const TWeakObjectPtr<UDeepSeekSubsystem> WeakThis(this);
    FString Prompt = FString::Printf(TEXT("React as %s the %s to: %s"), *NPCName, *NPCType, *RecentEvent);
    SendPromptToDeepSeek(Prompt, FOnDeepSeekResponse::CreateLambda(
        [WeakThis, NPCID, Revision, Generation, ResponseDelegate](FString Res)
        {
            if (!WeakThis.IsValid() || WeakThis->bDeinitializing || WeakThis->LifecycleGeneration != Generation ||
                WeakThis->NPCMemoryRevisions.FindRef(NPCID) != Revision) return;
            ResponseDelegate.ExecuteIfBound(Res);
        }));
}

void UDeepSeekSubsystem::GeneratePlayerChoices(FString NPCLine, FOnDeepSeekResponseDynamic ResponseDelegate)
{
    if (bDeinitializing) return;
    const uint64 Generation = LifecycleGeneration;
    const TWeakObjectPtr<UDeepSeekSubsystem> WeakThis(this);
    FString Prompt = FString::Printf(TEXT("Provide 5 samurai responses to: %s"), *NPCLine);
    SendPromptToDeepSeek(Prompt, FOnDeepSeekResponse::CreateLambda(
        [WeakThis, Generation, ResponseDelegate](FString Res)
        {
            if (!WeakThis.IsValid() || WeakThis->bDeinitializing || WeakThis->LifecycleGeneration != Generation) return;
            ResponseDelegate.ExecuteIfBound(Res);
        }));
}

void UDeepSeekSubsystem::InvalidateNPCRequests(const FString& NPCID)
{
    if (!bDeinitializing && !NPCID.IsEmpty()) ++NPCMemoryRevisions.FindOrAdd(NPCID);
}

void UDeepSeekSubsystem::UpdateNPCMemory(FString NPCID, FString PlayerAction, int32 ReputationChange)
{
    if (bDeinitializing || NPCID.IsEmpty()) return;
    ++NPCMemoryRevisions.FindOrAdd(NPCID);
    FNPCMemory& Memory = PersonalMemoryBank.FindOrAdd(NPCID);
    Memory.InteractionHistory += FString::Printf(TEXT(" [Player said: %s]"), *PlayerAction);
    Memory.LocalReputation = static_cast<int32>(FMath::Clamp<int64>(static_cast<int64>(Memory.LocalReputation) + ReputationChange, -100, 100));
}

void UDeepSeekSubsystem::GenerateCombatStrategy(FString EnemyType, TArray<FString> PlayerAbilities) { SendPromptToDeepSeek(TEXT("Strategy Request"), FOnDeepSeekResponse()); }
void UDeepSeekSubsystem::GenerateCharacterBackground(FString Race, FString Class, FString WorldContext) { SendPromptToDeepSeek(TEXT("Background Request"), FOnDeepSeekResponse()); }
void UDeepSeekSubsystem::GenerateWorldEvent(FString Location, FString TimeOfDay) { SendPromptToDeepSeek(TEXT("Event Request"), FOnDeepSeekResponse()); }

void UDeepSeekSubsystem::CacheResponse(FString Prompt, FString Response)
{
    constexpr int32 MaxEntries = 64;
    constexpr int32 MaxCharacters = 262144;
    if (bDeinitializing || Prompt.Len() > 16384 || !IsCacheableAIResponse(Response)) return;
    CachedResponses.Remove(Prompt);
    CachedResponseOrder.Remove(Prompt);
    int32 Characters = Prompt.Len() + Response.Len();
    for (const auto& Pair : CachedResponses) Characters += Pair.Key.Len() + Pair.Value.Len();
    while (!CachedResponseOrder.IsEmpty() &&
        (CachedResponses.Num() >= MaxEntries || Characters > MaxCharacters))
    {
        const FString Oldest = CachedResponseOrder[0];
        if (const FString* OldResponse = CachedResponses.Find(Oldest))
            Characters -= Oldest.Len() + OldResponse->Len();
        CachedResponses.Remove(Oldest);
        CachedResponseOrder.RemoveAt(0);
    }
    if (Characters > MaxCharacters) return;
    CachedResponseOrder.Add(Prompt);
    CachedResponses.Add(Prompt, Response);
}

FString UDeepSeekSubsystem::GetCachedResponse(FString Prompt)
{
    if (const FString* Response = CachedResponses.Find(Prompt))
    {
        if (IsCacheableAIResponse(*Response)) return *Response;
        CachedResponses.Remove(Prompt);
        CachedResponseOrder.Remove(Prompt);
    }
    return TEXT("");
}