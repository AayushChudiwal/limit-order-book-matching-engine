#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "lob/arena.hpp"

namespace {

using lob::Arena;
using lob::ArenaHandle;

// Tracks construction/destruction so the tests can assert the arena
// actually runs T's lifetime, not just hands out storage.
struct Tracked {
    static int live;
    static int constructed;
    static int destroyed;

    int value;

    explicit Tracked(int v = 0) : value(v) {
        ++live;
        ++constructed;
    }
    Tracked(const Tracked& other) : value(other.value) {
        ++live;
        ++constructed;
    }
    ~Tracked() {
        --live;
        ++destroyed;
    }

    static void Reset() {
        live = 0;
        constructed = 0;
        destroyed = 0;
    }
};
int Tracked::live = 0;
int Tracked::constructed = 0;
int Tracked::destroyed = 0;

class ArenaTest : public ::testing::Test {
  protected:
    void SetUp() override { Tracked::Reset(); }
};

TEST_F(ArenaTest, DefaultHandleIsInvalidAndKnowsIt) {
    Arena<int> arena;
    ArenaHandle handle;
    EXPECT_FALSE(handle.valid());
    EXPECT_FALSE(arena.IsValid(handle));
}

TEST_F(ArenaTest, AllocateConstructsAndGetReturnsTheValue) {
    Arena<Tracked> arena;
    const auto handle = arena.Allocate(42);

    EXPECT_TRUE(arena.IsValid(handle));
    EXPECT_EQ(arena.Get(handle).value, 42);
    EXPECT_EQ(Tracked::live, 1);
    EXPECT_EQ(arena.live_count(), 1u);
}

TEST_F(ArenaTest, FreeRunsTheDestructor) {
    Arena<Tracked> arena;
    const auto handle = arena.Allocate(7);
    ASSERT_EQ(Tracked::live, 1);

    arena.Free(handle);

    EXPECT_EQ(Tracked::live, 0);
    EXPECT_EQ(Tracked::destroyed, 1);
    EXPECT_EQ(arena.live_count(), 0u);
}

TEST_F(ArenaTest, FreedSlotIsReusedRatherThanGrowingTheArena) {
    Arena<int> arena;
    const auto first = arena.Allocate(1);
    const std::size_t slots_after_first = arena.slot_count();

    arena.Free(first);
    const auto second = arena.Allocate(2);

    EXPECT_EQ(arena.slot_count(), slots_after_first) << "free list should have handed the slot back";
    EXPECT_EQ(second.index, first.index);
    EXPECT_EQ(arena.Get(second), 2);
}

// The whole point of the generation tag: a handle to a slot that has
// been freed and REUSED must not silently read the new occupant.
TEST_F(ArenaTest, StaleHandleToAReusedSlotIsRejected) {
    Arena<int> arena;
    const auto stale = arena.Allocate(111);
    arena.Free(stale);
    const auto fresh = arena.Allocate(222);

    ASSERT_EQ(stale.index, fresh.index) << "test needs the slot to actually be reused";
    EXPECT_TRUE(arena.IsValid(fresh));

#if LOB_ARENA_GENERATION_TAGS
    EXPECT_NE(stale.generation, fresh.generation);
    EXPECT_FALSE(arena.IsValid(stale))
        << "a stale handle to a recycled slot must be detectable, not silently readable";
#else
    // Tags compiled out (the benchmark build): the arena cannot tell
    // these apart, which is exactly why the ASan nightly remains the
    // gate for that build. Documented as a test so the difference is
    // explicit rather than assumed.
    EXPECT_TRUE(arena.IsValid(stale));
#endif
}

TEST_F(ArenaTest, HandleToAFreedButNotReusedSlotIsRejected) {
    Arena<int> arena;
    const auto handle = arena.Allocate(5);
    arena.Free(handle);

    EXPECT_FALSE(arena.IsValid(handle))
        << "liveness alone is enough here -- this case does not need the generation";
}

TEST_F(ArenaTest, HandlesSurviveSlotVectorGrowth) {
    // Handles are indices, not pointers, specifically so that growing
    // the backing vector does not invalidate them. Allocate well past
    // any plausible initial capacity and check the earliest handle still
    // reads correctly.
    Arena<int> arena(2);
    const auto first = arena.Allocate(1234);

    std::vector<ArenaHandle> rest;
    for (int i = 0; i < 1000; ++i) {
        rest.push_back(arena.Allocate(i));
    }

    EXPECT_TRUE(arena.IsValid(first));
    EXPECT_EQ(arena.Get(first), 1234) << "growth must not invalidate an outstanding handle";
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(arena.Get(rest[static_cast<std::size_t>(i)]), i);
    }
}

TEST_F(ArenaTest, GenerationsAreIndependentPerSlot) {
    Arena<int> arena;
    const auto a = arena.Allocate(1);
    const auto b = arena.Allocate(2);
    ASSERT_NE(a.index, b.index);

    arena.Free(a);
    const auto a2 = arena.Allocate(3);

    EXPECT_EQ(a2.index, a.index);
    EXPECT_TRUE(arena.IsValid(b)) << "recycling one slot must not disturb another's generation";
    EXPECT_EQ(arena.Get(b), 2);
}

TEST_F(ArenaTest, ClearDestroysEverythingLive) {
    Arena<Tracked> arena;
    arena.Allocate(1);
    arena.Allocate(2);
    arena.Allocate(3);
    ASSERT_EQ(Tracked::live, 3);

    arena.Clear();

    EXPECT_EQ(Tracked::live, 0);
    EXPECT_EQ(arena.live_count(), 0u);
    EXPECT_EQ(arena.slot_count(), 0u);
}

TEST_F(ArenaTest, DestructorDestroysEverythingLive) {
    {
        Arena<Tracked> arena;
        arena.Allocate(1);
        arena.Allocate(2);
        ASSERT_EQ(Tracked::live, 2);
    }
    EXPECT_EQ(Tracked::live, 0) << "arena destruction must run each live element's destructor";
}

TEST_F(ArenaTest, MixedAllocateFreeChurnKeepsLiveCountAndValuesCorrect) {
    Arena<int> arena;
    std::vector<ArenaHandle> handles;

    for (int round = 0; round < 50; ++round) {
        for (int i = 0; i < 10; ++i) {
            handles.push_back(arena.Allocate(round * 100 + i));
        }
        // Free every other one, oldest first.
        std::vector<ArenaHandle> kept;
        for (std::size_t i = 0; i < handles.size(); ++i) {
            if (i % 2 == 0) {
                arena.Free(handles[i]);
            } else {
                kept.push_back(handles[i]);
            }
        }
        handles = kept;

        EXPECT_EQ(arena.live_count(), handles.size());
        for (const auto& h : handles) {
            EXPECT_TRUE(arena.IsValid(h));
        }
    }
}

// The property OptimizedOrderBook's whole-book copy depends on: a copied
// arena preserves slot indices, so handles held elsewhere (in the copied
// book's price levels) stay valid against the copy with no fixup.
TEST_F(ArenaTest, CopyPreservesHandlesAndDeepCopiesContents) {
    Arena<Tracked> source;
    const auto a = source.Allocate(10);
    const auto b = source.Allocate(20);
    source.Free(a);
    const auto c = source.Allocate(30);  // reuses a's slot, bumped generation
    ASSERT_EQ(Tracked::live, 2);

    Arena<Tracked> copy(source);

    EXPECT_EQ(copy.live_count(), source.live_count());
    EXPECT_EQ(Tracked::live, 4) << "copy must construct its own elements";

    // Handles issued against the source resolve correctly against the copy.
    EXPECT_TRUE(copy.IsValid(b));
    EXPECT_TRUE(copy.IsValid(c));
    EXPECT_EQ(copy.Get(b).value, 20);
    EXPECT_EQ(copy.Get(c).value, 30);

#if LOB_ARENA_GENERATION_TAGS
    EXPECT_FALSE(copy.IsValid(a)) << "a stale handle must stay stale across a copy";
#endif

    // And the two are independent.
    copy.Get(b).value = 999;
    EXPECT_EQ(source.Get(b).value, 20) << "copy must not alias the source's storage";
}

TEST_F(ArenaTest, CopiedArenaReusesTheSameFreeSlotsAsTheOriginalWould) {
    Arena<int> source;
    const auto a = source.Allocate(1);
    source.Allocate(2);
    source.Free(a);

    Arena<int> copy(source);
    const auto from_copy = copy.Allocate(3);

    EXPECT_EQ(from_copy.index, a.index) << "free list must survive the copy";
    EXPECT_EQ(copy.live_count(), 2u);
}

TEST_F(ArenaTest, MoveLeavesTheSourceEmptyAndUsable) {
    Arena<Tracked> source;
    const auto h = source.Allocate(5);

    Arena<Tracked> moved(std::move(source));
    EXPECT_EQ(moved.Get(h).value, 5);
    EXPECT_EQ(Tracked::live, 1) << "move must not duplicate elements";

    // NOLINTNEXTLINE(bugprone-use-after-move) -- deliberately checking the moved-from state
    EXPECT_EQ(source.live_count(), 0u);
    const auto reused = source.Allocate(6);
    EXPECT_EQ(source.Get(reused).value, 6);
}

TEST_F(ArenaTest, WorksWithANonTriviallyDestructibleType) {
    // Long enough to defeat any small-string optimization, so the arena
    // is genuinely managing a type that owns heap memory of its own --
    // if Free() failed to run ~string(), ASan's leak check would catch
    // it in the sanitizer job.
    const std::string long_value(200, 'x');

    Arena<std::string> arena;
    const auto h = arena.Allocate(long_value);
    EXPECT_EQ(arena.Get(h), long_value);
    arena.Free(h);
    EXPECT_EQ(arena.live_count(), 0u);
}

}  // namespace
