#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "../RoninEconomyAccess.h"
#include "../RoninLocalIdentitySaveGame.h"
#include "../EconomySystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoninEconomyPermissionTest,
    "LastRonin.Economy.SettlementRights", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoninEconomyPermissionTest::RunTest(const FString& Parameters)
{
    FHunterProfile Player;
    FRegionData Region;
    TestFalse(TEXT("Neutral is never shared ownership"), FRoninEconomyAccess::HasSettlementRights(Player, Region));
    Player.ClanID = TEXT("Crane");
    Region.ControllingClanID = TEXT("Crane");
    TestTrue(TEXT("Living controlling clan member"), FRoninEconomyAccess::HasSettlementRights(Player, Region));
    Player.bIsAlive = false;
    TestFalse(TEXT("Dead member"), FRoninEconomyAccess::HasSettlementRights(Player, Region));
    Player.bIsAlive = true;
    Player.ClanID = TEXT("Wolf");
    TestFalse(TEXT("Foreign clan"), FRoninEconomyAccess::HasSettlementRights(Player, Region));
    Player.ClanID.Empty();
    Region.ControllingClanID.Empty();
    TestFalse(TEXT("Empty ownership"), FRoninEconomyAccess::HasSettlementRights(Player, Region));
    Player.ClanID = TEXT("neutral");
    Region.ControllingClanID = TEXT("neutral");
    TestFalse(TEXT("Neutral case alias"), FRoninEconomyAccess::HasSettlementRights(Player, Region));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoninEconomyRangeTest,
    "LastRonin.Economy.InteractionRange", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoninEconomyRangeTest::RunTest(const FString& Parameters)
{
    const FVector Origin = FVector::ZeroVector;
    TestTrue(TEXT("Inclusive 500cm boundary"), FRoninEconomyAccess::IsWithinRange(Origin, FVector(500., 0., 0.), 500.));
    TestFalse(TEXT("Outside boundary"), FRoninEconomyAccess::IsWithinRange(Origin, FVector(500.01, 0., 0.), 500.));
    TestFalse(TEXT("Vertical separation counts"), FRoninEconomyAccess::IsWithinRange(Origin, FVector(0., 0., 501.), 500.));
    TestFalse(TEXT("Invalid range"), FRoninEconomyAccess::IsWithinRange(Origin, Origin, -1.));
    ARoninCharacter* Pawn = nullptr;
    FHunterProfile* Profile = nullptr;
    TestFalse(TEXT("Missing caller cannot authorize"), FRoninEconomyAccess::ResolveCaller(nullptr, nullptr, Pawn, Profile));
    TestTrue(TEXT("Rejected proof clears outputs"), Pawn == nullptr && Profile == nullptr);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoninLocalIdentityValidationTest,
    "LastRonin.Identity.LocalSlotValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoninLocalIdentityValidationTest::RunTest(const FString& Parameters)
{
    URoninLocalIdentitySaveGame* Identity = NewObject<URoninLocalIdentitySaveGame>();
    TestFalse(TEXT("Missing fields do not mint identity"), Identity->IsIdentityValid(0));
    Identity->IdentityVersion = 1;
    Identity->LocalPlayerSlot = 0;
    Identity->AccountGuid = FGuid::NewGuid();
    TestTrue(TEXT("Valid slot identity"), Identity->IsIdentityValid(0));
    TestFalse(TEXT("Another local slot"), Identity->IsIdentityValid(1));
    TestFalse(TEXT("Invalid expected slot"), Identity->IsIdentityValid(INDEX_NONE));
    Identity->IdentityVersion = 2;
    TestFalse(TEXT("Unknown version rejected"), Identity->IsIdentityValid(0));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoninPersonalResourcesPreflightTest,
    "LastRonin.Economy.PersonalResourcePreflight", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRoninPersonalResourcesPreflightTest::RunTest(const FString& Parameters)
{
    FItemData Iron;
    Iron.ItemName = TEXT("Iron");
    Iron.Potency = 1.f;
    Iron.Quantity = 4;
    FItemData Lumber = Iron;
    Lumber.ItemName = TEXT("Lumber");
    Lumber.Quantity = 2;
    FItemData Katana;
    Katana.ItemName = TEXT("Folded Steel Katana");
    Katana.Type = EItemType::Weapon;
    Katana.Potency = 45.f;
    Katana.Quantity = 1;
    Katana.Rarity = ERoninRarity::Rare;
    TMap<FString, FItemData> Original;
    Original.Add(Iron.ItemName, Iron);
    Original.Add(Lumber.ItemName, Lumber);
    TMap<FString, FItemData> Working = Original;
    TestTrue(TEXT("Personal iron can be consumed"), UEconomySystem::TryAdjustInventory(Working, Iron.ItemName, -4, Iron));
    TestTrue(TEXT("Personal lumber can be consumed"), UEconomySystem::TryAdjustInventory(Working, Lumber.ItemName, -2, Lumber));
    TestTrue(TEXT("Katana output accepted"), UEconomySystem::TryAdjustInventory(Working, Katana.ItemName, 1, Katana));
    TestFalse(TEXT("Iron depleted"), Working.Contains(Iron.ItemName));
    TestFalse(TEXT("Lumber depleted"), Working.Contains(Lumber.ItemName));
    TestEqual(TEXT("Output quantity"), Working.FindChecked(Katana.ItemName).Quantity, 1);
    TestEqual(TEXT("Preflight leaves source untouched"), Original.FindChecked(Iron.ItemName).Quantity, 4);
    Working = Original;
    TestFalse(TEXT("Insufficient personal iron"), UEconomySystem::TryAdjustInventory(Working, Iron.ItemName, -5, Iron));
    TestEqual(TEXT("Failed adjustment unchanged"), Working.FindChecked(Iron.ItemName).Quantity, 4);
    return true;
}
#endif
