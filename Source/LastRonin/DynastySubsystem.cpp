#include "DynastySubsystem.h"
#include "NemesisManager.h"
#include "Engine/GameInstance.h"

void UDynastySubsystem::ConceiveHeir(FString MotherID) {
    UNemesisManager* DB = GetGameInstance()->GetSubsystem<UNemesisManager>();
    if (!DB || MotherID.IsEmpty()) return;
    DB->SynchronizeSimulation();
    MotherID = DB->ResolveCharacterID(MotherID);
    if (!DB->ActiveNemeses.Contains(MotherID)) return;

    // TMap insertion below can relocate its values. Keep an owned parent snapshot.
    const FHunterProfile Mother = DB->ActiveNemeses[MotherID];
    if (!Mother.bIsAlive || Mother.Gender != ERoninGender::Female || Mother.Age < 18) return;

    FHunterProfile BabyDNA;
    BabyDNA.HunterName = Mother.HunterName + TEXT("'s Heir");
    BabyDNA.LegacyGeneration = FMath::Max(1, Mother.LegacyGeneration);
    if (BabyDNA.LegacyGeneration < MAX_int32) ++BabyDNA.LegacyGeneration;
    BabyDNA.Age = 0;
    BabyDNA.HunterClass = EHunterClass::Child;
    BabyDNA.ClanID = Mother.ClanID;
    BabyDNA.WorldLocation = Mother.WorldLocation;
    BabyDNA.AssignedSpatialGridCell = Mother.AssignedSpatialGridCell;
    BabyDNA.MotherID = MotherID;
    BabyDNA.RelationshipWeb.FindOrAdd(MotherID).TieType = EFamilyTie::Parent;

    for (const auto& Relation : Mother.RelationshipWeb)
    {
        if (Relation.Key.IsEmpty() || Relation.Key == MotherID || Relation.Value.TieType != EFamilyTie::Spouse) continue;
        const FString FatherID = DB->ResolveCharacterID(Relation.Key);
        const FHunterProfile* Spouse = DB->ActiveNemeses.Find(FatherID);
        if (FatherID == MotherID || !Spouse || !Spouse->bIsAlive || Spouse->Age < 18 || Spouse->Gender != ERoninGender::Male) continue;
        const FRelationshipData* Reciprocal = Spouse->RelationshipWeb.Find(MotherID);
        if (!Reciprocal || Reciprocal->TieType != EFamilyTie::Spouse) continue;
        BabyDNA.FatherID = FatherID;
        BabyDNA.RelationshipWeb.FindOrAdd(BabyDNA.FatherID).TieType = EFamilyTie::Parent;
        break;
    }

    if (Mother.ElementalAffinities.Num() > 0) {
        if (BabyDNA.ElementalAffinities.Num() == 0) BabyDNA.ElementalAffinities.Add(Mother.ElementalAffinities[0]);
        else BabyDNA.ElementalAffinities[0] = Mother.ElementalAffinities[0];
    }

    BabyDNA.InheritedTraits.AppendTags(Mother.InheritedTraits);
    if (FMath::RandRange(0, 100) > 85)
    {
        BabyDNA.InheritedTraits.AddTag(FGameplayTag::RequestGameplayTag(TEXT("Trait.Hereditary.Pyromaniac")));
    }

    BabyDNA.UniqueID = FGuid::NewGuid().ToString();
    DB->ActiveNemeses.Add(BabyDNA.UniqueID, BabyDNA);

    // Reacquire after insertion rather than retaining a reference into the map.
    DB->ActiveNemeses[MotherID].RelationshipWeb.FindOrAdd(BabyDNA.UniqueID).TieType = EFamilyTie::Child;
    if (!BabyDNA.FatherID.IsEmpty())
    {
        DB->ActiveNemeses[BabyDNA.FatherID].RelationshipWeb.FindOrAdd(BabyDNA.UniqueID).TieType = EFamilyTie::Child;
    }

    DB->RefreshPlayerAlias();
    UE_LOG(LogTemp, Warning, TEXT("SUCCESSION: A new heir (%s) has been born to the %s line with genetic trait inheritance passes."), *BabyDNA.HunterName, *Mother.HunterName);
}

void UDynastySubsystem::UpdateRelationshipTier(FString CharacterA, FString CharacterB, int32 NewAffinity) {
    UNemesisManager* DB = GetGameInstance()->GetSubsystem<UNemesisManager>();
    if (!DB) return;
    DB->SynchronizeSimulation();
    CharacterA = DB->ResolveCharacterID(CharacterA);
    CharacterB = DB->ResolveCharacterID(CharacterB);
    if (CharacterA == CharacterB || !DB->ActiveNemeses.Contains(CharacterA) ||
        !DB->ActiveNemeses.Contains(CharacterB)) return;

    FRelationshipData& Relation = DB->ActiveNemeses[CharacterA].RelationshipWeb.FindOrAdd(CharacterB);
    Relation.Affinity = FMath::Clamp(NewAffinity, 0, 100);

    // Affinity tiers may change social labels, never structural roles.
    switch (Relation.TieType)
    {
    case EFamilyTie::None:
    case EFamilyTie::Enemy:
    case EFamilyTie::Rival:
    case EFamilyTie::Friend:
    case EFamilyTie::BestFriend:
        break;
    default:
        DB->RefreshPlayerAlias();
        return;
    }

    if (Relation.Affinity >= 90) Relation.TieType = EFamilyTie::BestFriend;
    else if (Relation.Affinity >= 60) Relation.TieType = EFamilyTie::Friend;
    else if (Relation.Affinity <= 10) Relation.TieType = EFamilyTie::Enemy;
    else if (Relation.Affinity <= 30) Relation.TieType = EFamilyTie::Rival;
    else Relation.TieType = EFamilyTie::None;
    DB->RefreshPlayerAlias();
}

FString UDynastySubsystem::ProposeMarriage(FString ProposerID, FString TargetID) {
    UNemesisManager* DB = GetGameInstance()->GetSubsystem<UNemesisManager>();
    if (!DB) return TEXT("Error: DB Missing");
    DB->SynchronizeSimulation();
    ProposerID = DB->ResolveCharacterID(ProposerID);
    TargetID = DB->ResolveCharacterID(TargetID);
    if (ProposerID.IsEmpty() || TargetID.IsEmpty() || ProposerID == TargetID)
        return TEXT("Rejected: Invalid marriage participants.");

    if (DB->ActiveNemeses.Contains(ProposerID) && DB->ActiveNemeses.Contains(TargetID)) {
        FHunterProfile& Proposer = DB->ActiveNemeses[ProposerID];
        FHunterProfile& Target = DB->ActiveNemeses[TargetID];
        if (!Proposer.bIsAlive || !Target.bIsAlive || Proposer.Age < 18 || Target.Age < 18)
            return TEXT("Rejected: Both participants must be living adults.");
        for (const auto& Relation : Proposer.RelationshipWeb)
        {
            if (Relation.Value.TieType == EFamilyTie::Spouse)
                return TEXT("Rejected: A participant is already married.");
        }
        for (const auto& Relation : Target.RelationshipWeb)
        {
            if (Relation.Value.TieType == EFamilyTie::Spouse)
                return TEXT("Rejected: A participant is already married.");
        }

        int32 Affinity = Proposer.RelationshipWeb.Contains(TargetID) ? Proposer.RelationshipWeb[TargetID].Affinity : 0;

        if (Affinity > 75 || static_cast<int64>(Proposer.PowerLevel) > static_cast<int64>(Target.PowerLevel) + 10) {
            Proposer.RelationshipWeb.FindOrAdd(TargetID).TieType = EFamilyTie::Spouse;
            Target.RelationshipWeb.FindOrAdd(ProposerID).TieType = EFamilyTie::Spouse;
            DB->RefreshPlayerAlias();
            return TEXT("Accepted: A royal union is formed.");
        }
    }
    return TEXT("Rejected: You are not worthy of this bloodline.");
}

TArray<FFamilyNode> UDynastySubsystem::GetFamilyTree(const FHunterProfile& TargetProfile)
{
    TArray<FFamilyNode> FamilyTree;
    UNemesisManager* DB = GetGameInstance()->GetSubsystem<UNemesisManager>();
    if (!DB) return FamilyTree;
    DB->SynchronizeSimulation();

    FString TargetHunterID = TargetProfile.UniqueID;
    if (TargetHunterID.IsEmpty()) return FamilyTree;

    auto AddFamilyNode = [&](const FString& RawRelativeID, EFamilyTie OverriddenTie) {
        const FString RelativeID = DB->ResolveCharacterID(RawRelativeID);
        if (DB->ActiveNemeses.Contains(RelativeID)) {
            bool bAlreadyExists = false;
            for (const FFamilyNode& ExistingNode : FamilyTree) {
                if (ExistingNode.Profile.UniqueID == RelativeID && ExistingNode.TieType == OverriddenTie) {
                    bAlreadyExists = true;
                    break;
                }
            }
            if (!bAlreadyExists) {
                FFamilyNode NewNode;
                NewNode.Profile = DB->ActiveNemeses[RelativeID];
                NewNode.TieType = OverriddenTie;
                NewNode.Affinity = 50;

                const FRelationshipData* Relation = TargetProfile.RelationshipWeb.Find(RelativeID);
                if (!Relation)
                    for (const auto& Pair : TargetProfile.RelationshipWeb)
                        if (DB->ResolveCharacterID(Pair.Key) == RelativeID) { Relation = &Pair.Value; break; }
                if (Relation) NewNode.Affinity = Relation->Affinity;
                FamilyTree.Add(NewNode);
            }
        }
        };

    TArray<FString> ParentsIDs;

    for (const auto& Relation : TargetProfile.RelationshipWeb) {
        const FString RelID = DB->ResolveCharacterID(Relation.Key);
        EFamilyTie DirectTie = Relation.Value.TieType;

        if (DirectTie == EFamilyTie::Parent) { ParentsIDs.Add(RelID); AddFamilyNode(RelID, EFamilyTie::Parent); }
        else if (DirectTie == EFamilyTie::Spouse) { AddFamilyNode(RelID, EFamilyTie::Spouse); }
        else if (DirectTie == EFamilyTie::Child) {
            AddFamilyNode(RelID, EFamilyTie::Child);
            if (DB->ActiveNemeses.Contains(RelID)) {
                for (const auto& ChildRelation : DB->ActiveNemeses[RelID].RelationshipWeb) {
                    if (ChildRelation.Value.TieType == EFamilyTie::Student) {
                        AddFamilyNode(ChildRelation.Key, EFamilyTie::Student);
                    }
                }
            }
        }
        else if (DirectTie == EFamilyTie::Sibling) { AddFamilyNode(RelID, EFamilyTie::Sibling); }
        else if (DirectTie == EFamilyTie::Student) { AddFamilyNode(RelID, EFamilyTie::Student); }
        else if (DirectTie == EFamilyTie::Master) { AddFamilyNode(RelID, EFamilyTie::Master); }
    }

    for (const FString& ParentID : ParentsIDs) {
        if (!DB->ActiveNemeses.Contains(ParentID)) continue;
        FHunterProfile& ParentProfile = DB->ActiveNemeses[ParentID];

        for (const auto& ParentRelation : ParentProfile.RelationshipWeb) {
            const FString AncestorID = DB->ResolveCharacterID(ParentRelation.Key);
            EFamilyTie ParentTie = ParentRelation.Value.TieType;

            if (ParentTie == EFamilyTie::Parent) { AddFamilyNode(AncestorID, EFamilyTie::Grandparent); }
            else if (ParentTie == EFamilyTie::Sibling) {
                EFamilyTie RoleOverride = (DB->ActiveNemeses.Contains(AncestorID) && DB->ActiveNemeses[AncestorID].Gender == ERoninGender::Female)
                    ? EFamilyTie::Aunt : EFamilyTie::Uncle;
                AddFamilyNode(AncestorID, RoleOverride);
            }
        }
    }

    return FamilyTree;
}

void UDynastySubsystem::RunBackgroundLifeSimulation(float DaysPassed)
{
    if (!FMath::IsFinite(DaysPassed) || DaysPassed <= 0.f) return;
    UNemesisManager* DB = GetGameInstance()->GetSubsystem<UNemesisManager>();
    if (!DB) return;
    DB->SynchronizeSimulation();

    TArray<FString> PregnantMothers;
    TArray<FString> ActiveLivingKeys;

    DB->ActiveNemeses.GetKeys(ActiveLivingKeys);

    for (const FString& KeyA : ActiveLivingKeys)
    {
        if (!DB->ActiveNemeses.Contains(KeyA)) continue;
        FHunterProfile& ProfileA = DB->ActiveNemeses[KeyA];
        if (!ProfileA.bIsAlive || DB->IsRedundantPlayerAlias(KeyA) || ProfileA.UniqueID.Equals("PLAYER") || ProfileA.Age < 18) continue;

        TMap<FString, FRelationshipData> TempWeb = ProfileA.RelationshipWeb;

        for (auto& RelationPair : TempWeb)
        {
            const FString TargetID = DB->ResolveCharacterID(RelationPair.Key);
            if (TargetID.IsEmpty() || TargetID == KeyA || !DB->ActiveNemeses.Contains(TargetID) ||
                (TargetID != RelationPair.Key && TempWeb.Contains(TargetID))) continue;
            FRelationshipData& Relation = RelationPair.Value;

            Relation.Affinity = FMath::Clamp(FMath::Clamp(Relation.Affinity, 0, 100) + FMath::RandRange(-5, 5), 0, 100);
            ProfileA.RelationshipWeb.FindOrAdd(TargetID).Affinity = Relation.Affinity;

            if (Relation.TieType == EFamilyTie::None && Relation.Affinity > 80 && ProfileA.Gender == ERoninGender::Male)
            {
                if (TargetID != KeyA && DB->ActiveNemeses.Contains(TargetID) &&
                    DB->ActiveNemeses[TargetID].Gender == ERoninGender::Female &&
                    DB->ActiveNemeses[TargetID].bIsAlive && DB->ActiveNemeses[TargetID].Age >= 18)
                {
                    ProposeMarriage(KeyA, TargetID);
                    if (ProfileA.RelationshipWeb[TargetID].TieType != EFamilyTie::Spouse) continue;

                    if (FMath::RandRange(0, 100) > 40)
                    {
                        PregnantMothers.AddUnique(TargetID);
                    }
                }
            }
        }
    }

    for (const FString& MotherID : PregnantMothers)
    {
        ConceiveHeir(MotherID);
        DB->EnforcePopulationEquilibrium();
    }
    DB->RefreshPlayerAlias();
}