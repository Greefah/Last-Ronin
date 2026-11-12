#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "../RoninCombatHistory.h"

namespace RoninCombatHistoryTests
{
// Exercise identical retention rules for both streams without creating engine objects.
struct FStream
{
    FRoninCombatHistory& History;
    bool bSnapshot;

    const TCHAR* Name() const { return bSnapshot ? TEXT("Snapshot") : TEXT("Input"); }

    bool Record(int64 Frame, int32 Seed = 17)
    {
        if (bSnapshot)
        {
            FRoninCombatSnapshot Value;
            Value.Frame = Frame;
            Value.DeterministicSeed = Seed;
            return History.RecordSnapshot(Value);
        }
        FRoninCombatInput Value;
        Value.Frame = Frame;
        Value.DeterministicSeed = Seed;
        return History.RecordInput(Value);
    }

    bool Has(int64 Frame) const
    {
        return bSnapshot ? History.FindSnapshot(Frame) != nullptr : History.FindInput(Frame) != nullptr;
    }

    bool HasSeed(int64 Frame, int32 Seed) const
    {
        if (bSnapshot)
        {
            FRoninCombatSnapshot Out;
            return History.FindSnapshot(Frame, Out) && Out.Frame == Frame && Out.DeterministicSeed == Seed;
        }
        FRoninCombatInput Out;
        return History.FindInput(Frame, Out) && Out.Frame == Frame && Out.DeterministicSeed == Seed;
    }
};

FRoninCombatInput MakeInput(int64 Frame, int32 Seed)
{
    FRoninCombatInput Value;
    Value.Frame = Frame;
    Value.Move = FVector2D(2.0, -3.0);
    Value.Look = FVector2D(-4.0, 5.0);
    Value.Buttons = 0x5a;
    Value.DeterministicSeed = Seed;
    return Value;
}

FRoninCombatSnapshot MakeSnapshot(int64 Frame, int32 Seed)
{
    FRoninCombatSnapshot Value;
    Value.Frame = Frame;
    Value.Velocity = FVector(3.0, -5.0, 7.0);
    Value.Blood = 11.f;
    Value.Stamina = 22.f;
    Value.Potential = 33.f;
    Value.Posture = 44.f;
    Value.FreezeFrames = 5;
    Value.AttackID = 61;
    Value.WindowID = 62;
    Value.AttackStartFrame = 63;
    Value.WindowStartFrame = 64;
    Value.WindowEndFrame = 65;
    Value.bIsAttacking = true;
    Value.bIsGuarding = true;
    Value.DeterministicSeed = Seed;
    return Value;
}

bool CheckInput(FAutomationTestBase& Test, const FRoninCombatInput& Actual, const FRoninCombatInput& Expected)
{
    return Test.TestTrue(TEXT("All input fields match"),
        Actual.Frame == Expected.Frame &&
        Actual.Move.X == Expected.Move.X && Actual.Move.Y == Expected.Move.Y &&
        Actual.Look.X == Expected.Look.X && Actual.Look.Y == Expected.Look.Y &&
        Actual.Buttons == Expected.Buttons && Actual.RequestedStance == Expected.RequestedStance &&
        Actual.DeterministicSeed == Expected.DeterministicSeed);
}

bool CheckSnapshot(FAutomationTestBase& Test, const FRoninCombatSnapshot& Actual, const FRoninCombatSnapshot& Expected)
{
    bool bOk = Test.TestEqual(TEXT("Snapshot transform matches"), Actual.Transform, Expected.Transform);
    bOk &= Test.TestEqual(TEXT("Snapshot velocity matches"), Actual.Velocity, Expected.Velocity);
    bOk &= Test.TestTrue(TEXT("All remaining snapshot fields match"),
        Actual.Frame == Expected.Frame && Actual.Stance == Expected.Stance &&
        Actual.Blood == Expected.Blood && Actual.Stamina == Expected.Stamina &&
        Actual.Potential == Expected.Potential && Actual.Posture == Expected.Posture &&
        Actual.FreezeFrames == Expected.FreezeFrames && Actual.AttackID == Expected.AttackID &&
        Actual.WindowID == Expected.WindowID && Actual.AttackStartFrame == Expected.AttackStartFrame &&
        Actual.WindowStartFrame == Expected.WindowStartFrame && Actual.WindowEndFrame == Expected.WindowEndFrame &&
        Actual.bIsAttacking == Expected.bIsAttacking && Actual.bIsGuarding == Expected.bIsGuarding &&
        Actual.DeterministicSeed == Expected.DeterministicSeed);
    return bOk;
}
}

using namespace RoninCombatHistoryTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoninHistoryInvalidConfigureTest,
    "LastRonin.CombatHistory.InvalidConfigureAndUnconfigured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoninHistoryInvalidConfigureTest::RunTest(const FString& Parameters)
{
    FRoninCombatHistory History;
    bool bOk = TestEqual(TEXT("Initially unconfigured"), History.GetCapacity(), 0);
    bOk &= TestTrue(TEXT("Zero capacity rejected"), !History.Configure(0));
    bOk &= TestTrue(TEXT("Negative capacity rejected"), !History.Configure(-7));
    bOk &= TestTrue(TEXT("Minimum capacity rejected"), !History.Configure(MIN_int32));
    bOk &= TestEqual(TEXT("Invalid configuration leaves zero capacity"), History.GetCapacity(), 0);
    for (int32 Index = 0; Index < 2; ++Index)
    {
        FStream Stream{History, Index != 0};
        bOk &= TestTrue(Stream.Name(), !Stream.Record(0));
        bOk &= TestTrue(TEXT("Unconfigured negative write rejected"), !Stream.Record(-1));
        bOk &= TestTrue(TEXT("Unconfigured maximum write rejected"), !Stream.Record(MAX_int64));
        bOk &= TestTrue(TEXT("Unconfigured reads miss"), !Stream.Has(0) && !Stream.Has(-1) && !Stream.Has(MAX_int64));
    }
    History.Reset();
    bOk &= TestEqual(TEXT("Reset does not configure storage"), History.GetCapacity(), 0);
    bOk &= TestTrue(TEXT("Configure valid history"), History.Configure(3));
    for (int32 Index = 0; Index < 2; ++Index)
    {
        FStream Stream{History, Index != 0};
        bOk &= TestTrue(TEXT("Populate configured history"), Stream.Record(10, 101));
    }
    bOk &= TestTrue(TEXT("Zero reconfiguration rejected"), !History.Configure(0));
    bOk &= TestTrue(TEXT("Negative reconfiguration rejected"), !History.Configure(-1));
    bOk &= TestEqual(TEXT("Invalid reconfiguration preserves capacity"), History.GetCapacity(), 3);
    for (int32 Index = 0; Index < 2; ++Index)
    {
        FStream Stream{History, Index != 0};
        bOk &= TestTrue(TEXT("Invalid reconfiguration preserves payload"), Stream.HasSeed(10, 101));
        bOk &= TestTrue(TEXT("Invalid reconfiguration preserves watermark"), !Stream.Record(7));
        bOk &= TestTrue(TEXT("Configured negative write rejected"), !Stream.Record(-1));
    }
    return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoninHistoryRingTest,
    "LastRonin.CombatHistory.RingWrapEvictionAndGaps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoninHistoryRingTest::RunTest(const FString& Parameters)
{
    bool bOk = true;
    for (int32 Index = 0; Index < 2; ++Index)
    {
        FRoninCombatHistory History;
        bOk &= TestTrue(TEXT("Configure three slots"), History.Configure(3));
        FStream Stream{History, Index != 0};
        for (int64 Frame = 0; Frame < 8; ++Frame)
        {
            bOk &= TestTrue(Stream.Name(), Stream.Record(Frame, static_cast<int32>(Frame)));
            const int64 First = Frame >= 2 ? Frame - 2 : 0;
            for (int64 Retained = First; Retained <= Frame; ++Retained)
            {
                bOk &= TestTrue(TEXT("Retained payload survives wrap"), Stream.HasSeed(Retained, static_cast<int32>(Retained)));
            }
            if (Frame >= 3)
            {
                bOk &= TestTrue(TEXT("Evicted frame misses"), !Stream.Has(Frame - 3));
            }
            bOk &= TestTrue(TEXT("Future frame misses"), !Stream.Has(Frame + 1));
        }
        bOk &= TestTrue(TEXT("Jump forward by more than capacity"), Stream.Record(20, 200));
        bOk &= TestTrue(TEXT("Unwritten retained gaps miss"), !Stream.Has(18) && !Stream.Has(19));
        bOk &= TestTrue(TEXT("Old modulo alias cannot be read"), !Stream.Has(5) && !Stream.Has(6) && !Stream.Has(7));
        bOk &= TestTrue(TEXT("Newest payload survives jump"), Stream.HasSeed(20, 200));
        bOk &= TestTrue(TEXT("Fill oldest retained gap"), Stream.Record(18, 180));
        bOk &= TestTrue(TEXT("Filled gap readable"), Stream.HasSeed(18, 180));
        bOk &= TestTrue(TEXT("Other gap still misses"), !Stream.Has(19));
    }
    return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoninHistoryCorrectionsTest,
    "LastRonin.CombatHistory.CorrectionsAndStaleWrites",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoninHistoryCorrectionsTest::RunTest(const FString& Parameters)
{
    bool bOk = true;
    for (int32 Index = 0; Index < 2; ++Index)
    {
        FRoninCombatHistory History;
        bOk &= TestTrue(TEXT("Configure correction window"), History.Configure(4));
        FStream Stream{History, Index != 0};
        bOk &= TestTrue(Stream.Name(), Stream.Record(12, 120));
        bOk &= TestTrue(TEXT("Backfill oldest retained frame"), Stream.Record(9, 90));
        bOk &= TestTrue(TEXT("Correct oldest retained frame"), Stream.Record(9, 91));
        bOk &= TestTrue(TEXT("Correct latest frame"), Stream.Record(12, 121));
        bOk &= TestTrue(TEXT("Corrected payloads readable"), Stream.HasSeed(9, 91) && Stream.HasSeed(12, 121));
        bOk &= TestTrue(TEXT("Reject exact stale boundary"), !Stream.Record(8, 800));
        bOk &= TestTrue(TEXT("Reject older modulo alias of frame 9"), !Stream.Record(5, 500));
        bOk &= TestTrue(TEXT("Reject negative frame"), !Stream.Record(-1));
        bOk &= TestTrue(TEXT("Rejected writes preserve retained payloads"), Stream.HasSeed(9, 91) && Stream.HasSeed(12, 121));
        bOk &= TestTrue(TEXT("Rejected frames remain misses"), !Stream.Has(8) && !Stream.Has(5) && !Stream.Has(-1));
        bOk &= TestTrue(TEXT("Advance after correction"), Stream.Record(13, 130));
        bOk &= TestTrue(TEXT("Correction did not rewind watermark"), !Stream.Has(9) && !Stream.Record(9, 999));
        bOk &= TestTrue(TEXT("Latest correction retained"), Stream.HasSeed(12, 121));
    }
    return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoninHistoryIndependentStreamsTest,
    "LastRonin.CombatHistory.IndependentStreams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoninHistoryIndependentStreamsTest::RunTest(const FString& Parameters)
{
    FRoninCombatHistory History;
    bool bOk = TestTrue(TEXT("Configure independent streams"), History.Configure(3));
    FStream Input{History, false};
    FStream Snapshot{History, true};
    bOk &= TestTrue(TEXT("Write input alone"), Input.Record(4, 41));
    bOk &= TestTrue(TEXT("Input does not populate snapshot"), !Snapshot.Has(4));
    bOk &= TestTrue(TEXT("Write snapshot at same frame"), Snapshot.Record(4, 42));
    bOk &= TestTrue(TEXT("Same frame has independent payloads"), Input.HasSeed(4, 41) && Snapshot.HasSeed(4, 42));
    bOk &= TestTrue(TEXT("Advance input far ahead"), Input.Record(100, 1001));
    bOk &= TestTrue(TEXT("Input advancement leaves old snapshot"), Snapshot.HasSeed(4, 42));
    bOk &= TestTrue(TEXT("Snapshot uses its own oldest boundary"), Snapshot.Record(2, 22));
    bOk &= TestTrue(TEXT("Advance snapshot farther ahead"), Snapshot.Record(200, 2002));
    bOk &= TestTrue(TEXT("Snapshot advancement leaves input"), Input.HasSeed(100, 1001));
    bOk &= TestTrue(TEXT("Input uses its own oldest boundary"), Input.Record(98, 981));
    bOk &= TestTrue(TEXT("Independent evictions"), !Input.Has(4) && !Snapshot.Has(4) && !Snapshot.Has(2));
    bOk &= TestTrue(TEXT("No cross-stream records"), !Input.Has(200) && !Snapshot.Has(100));
    return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoninHistoryCopyOutTest,
    "LastRonin.CombatHistory.CopyOutAndMissesUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoninHistoryCopyOutTest::RunTest(const FString& Parameters)
{
    FRoninCombatHistory History;
    const FRoninCombatInput InputSentinel = MakeInput(777, -123);
    const FRoninCombatSnapshot SnapshotSentinel = MakeSnapshot(888, -456);
    FRoninCombatInput OutInput = InputSentinel;
    FRoninCombatSnapshot OutSnapshot = SnapshotSentinel;
    bool bOk = TestTrue(TEXT("Unconfigured input copy miss"), !History.FindInput(0, OutInput));
    bOk &= TestTrue(TEXT("Unconfigured snapshot copy miss"), !History.FindSnapshot(0, OutSnapshot));
    bOk &= CheckInput(*this, OutInput, InputSentinel);
    bOk &= CheckSnapshot(*this, OutSnapshot, SnapshotSentinel);
    bOk &= TestTrue(TEXT("Configure copy history"), History.Configure(3));
    FRoninCombatInput Input = MakeInput(10, 1010);
    FRoninCombatSnapshot Snapshot = MakeSnapshot(10, 2020);
    const FRoninCombatInput ExpectedInput = Input;
    const FRoninCombatSnapshot ExpectedSnapshot = Snapshot;
    bOk &= TestTrue(TEXT("Record full input"), History.RecordInput(Input));
    bOk &= TestTrue(TEXT("Record full snapshot"), History.RecordSnapshot(Snapshot));
    // Stored records own their values, not references to the producer variables.
    Input = FRoninCombatInput();
    Snapshot = FRoninCombatSnapshot();
    bOk &= TestTrue(TEXT("Input copy hit"), History.FindInput(10, OutInput));
    bOk &= TestTrue(TEXT("Snapshot copy hit"), History.FindSnapshot(10, OutSnapshot));
    bOk &= CheckInput(*this, OutInput, ExpectedInput);
    bOk &= CheckSnapshot(*this, OutSnapshot, ExpectedSnapshot);
    const int64 MissFrames[] = {-1, 7, 8, 9, 11, MAX_int64};
    for (const int64 Frame : MissFrames)
    {
        OutInput = InputSentinel;
        OutSnapshot = SnapshotSentinel;
        bOk &= TestTrue(TEXT("Input pointer miss"), History.FindInput(Frame) == nullptr);
        bOk &= TestTrue(TEXT("Snapshot pointer miss"), History.FindSnapshot(Frame) == nullptr);
        bOk &= TestTrue(TEXT("Input copy miss"), !History.FindInput(Frame, OutInput));
        bOk &= TestTrue(TEXT("Snapshot copy miss"), !History.FindSnapshot(Frame, OutSnapshot));
        bOk &= CheckInput(*this, OutInput, InputSentinel);
        bOk &= CheckSnapshot(*this, OutSnapshot, SnapshotSentinel);
    }
    return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoninHistoryClearTest,
    "LastRonin.CombatHistory.ConfigureAndResetClear",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoninHistoryClearTest::RunTest(const FString& Parameters)
{
    FRoninCombatHistory History;
    bool bOk = TestTrue(TEXT("Configure clear test"), History.Configure(4));
    FStream Input{History, false};
    FStream Snapshot{History, true};
    bOk &= TestTrue(TEXT("Populate both streams"), Input.Record(100) && Snapshot.Record(200));
    History.Reset();
    bOk &= TestEqual(TEXT("Reset retains capacity"), History.GetCapacity(), 4);
    bOk &= TestTrue(TEXT("Reset clears both streams"), !Input.Has(100) && !Snapshot.Has(200));
    bOk &= TestTrue(TEXT("Reset clears both watermarks"), Input.Record(0, 1) && Snapshot.Record(0, 2));
    bOk &= TestTrue(TEXT("Same-capacity Configure succeeds"), History.Configure(4));
    bOk &= TestTrue(TEXT("Same-capacity Configure clears entries"), !Input.Has(0) && !Snapshot.Has(0));
    bOk &= TestTrue(TEXT("Repopulate high frames"), Input.Record(100) && Snapshot.Record(200));
    bOk &= TestTrue(TEXT("Resize Configure succeeds"), History.Configure(1));
    bOk &= TestEqual(TEXT("Capacity changed"), History.GetCapacity(), 1);
    bOk &= TestTrue(TEXT("Resize clears both streams"), !Input.Has(100) && !Snapshot.Has(200));
    bOk &= TestTrue(TEXT("Configure clears both watermarks"), Input.Record(0, 3) && Snapshot.Record(0, 4));
    bOk &= TestTrue(TEXT("Capacity-one overwrite"), Input.Record(1, 5) && Snapshot.Record(1, 6));
    bOk &= TestTrue(TEXT("Capacity-one eviction"), !Input.Has(0) && !Snapshot.Has(0));
    bOk &= TestTrue(TEXT("Capacity-one payloads"), Input.HasSeed(1, 5) && Snapshot.HasSeed(1, 6));
    History.Reset();
    History.Reset();
    bOk &= TestTrue(TEXT("Repeated Reset remains empty"), !Input.Has(1) && !Snapshot.Has(1));
    return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoninHistoryMaxFrameTest,
    "LastRonin.CombatHistory.MaxInt64Boundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRoninHistoryMaxFrameTest::RunTest(const FString& Parameters)
{
    bool bOk = true;
    for (int32 Index = 0; Index < 2; ++Index)
    {
        FRoninCombatHistory History;
        bOk &= TestTrue(TEXT("Configure maximum-frame test"), History.Configure(3));
        FStream Stream{History, Index != 0};
        // Never form MAX_int64 + 1, even in a test expectation or loop increment.
        bOk &= TestTrue(Stream.Name(), Stream.Record(MAX_int64 - 2, 1));
        bOk &= TestTrue(TEXT("Maximum is initially a future miss"), !Stream.Has(MAX_int64));
        bOk &= TestTrue(TEXT("Advance to maximum frame"), Stream.Record(MAX_int64, 2));
        bOk &= TestTrue(TEXT("Oldest retained maximum-edge payload"), Stream.HasSeed(MAX_int64 - 2, 1));
        bOk &= TestTrue(TEXT("Maximum-edge gap misses"), !Stream.Has(MAX_int64 - 1));
        bOk &= TestTrue(TEXT("Backfill maximum-edge gap"), Stream.Record(MAX_int64 - 1, 3));
        bOk &= TestTrue(TEXT("Correct maximum frame"), Stream.Record(MAX_int64, 4));
        bOk &= TestTrue(TEXT("Reject exact stale maximum-edge boundary"), !Stream.Record(MAX_int64 - 3, 5));
        bOk &= TestTrue(TEXT("Reject low and negative frames at maximum"), !Stream.Record(0) && !Stream.Record(-1));
        bOk &= TestTrue(TEXT("Maximum-edge retained payloads intact"),
            Stream.HasSeed(MAX_int64 - 2, 1) && Stream.HasSeed(MAX_int64 - 1, 3) && Stream.HasSeed(MAX_int64, 4));
        bOk &= TestTrue(TEXT("Maximum-edge stale reads miss"), !Stream.Has(MAX_int64 - 3) && !Stream.Has(0) && !Stream.Has(-1));
        bOk &= TestTrue(TEXT("Reconfigure capacity one"), History.Configure(1));
        bOk &= TestTrue(TEXT("Maximum frame accepted with capacity one"), Stream.Record(MAX_int64, 6));
        bOk &= TestTrue(TEXT("Capacity-one maximum correction"), Stream.Record(MAX_int64, 7));
        bOk &= TestTrue(TEXT("Capacity-one stale predecessor rejected"), !Stream.Record(MAX_int64 - 1, 8));
        bOk &= TestTrue(TEXT("Capacity-one maximum payload intact"), Stream.HasSeed(MAX_int64, 7));
        bOk &= TestTrue(TEXT("Capacity-one predecessor misses"), !Stream.Has(MAX_int64 - 1));
        History.Reset();
        bOk &= TestTrue(TEXT("Reset clears maximum watermark"), !Stream.Has(MAX_int64) && Stream.Record(0, 9));
    }
    return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
