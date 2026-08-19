#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

// Generation-tagged arena handles. ON by default for ordinary builds
// (tests, CI, sanitizer jobs), compiled OUT for the benchmark build --
// see the CMake option LOB_ARENA_GENERATION_TAGS and the header comment
// on Arena below for why this specific thing is a compile-time choice
// rather than something always-on or always-off.
#ifndef LOB_ARENA_GENERATION_TAGS
#define LOB_ARENA_GENERATION_TAGS 1
#endif

namespace lob {

// A handle into an Arena<T>: a slot index plus the generation that slot
// was on when the handle was issued.
//
// Deliberately NOT a pointer, for two independent reasons:
//
// 1. The arena's slot storage is a std::vector that grows. Growth
//    reallocates, which would invalidate every outstanding raw pointer;
//    an index survives it untouched. This is what lets the arena start
//    small and grow to fit a symbol's real depth instead of requiring a
//    correct capacity guess up front.
// 2. It carries the generation, which is what makes a stale handle
//    DETECTABLE rather than silently reading recycled memory. See
//    docs/phase5_readiness.md: a use-after-free that happens to return
//    plausible-looking data is precisely the failure a behavioural
//    differential test cannot see.
struct ArenaHandle {
    static constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t index = kInvalidIndex;
    std::uint32_t generation = 0;

    [[nodiscard]] bool valid() const { return index != kInvalidIndex; }

    friend bool operator==(const ArenaHandle& a, const ArenaHandle& b) {
        return a.index == b.index && a.generation == b.generation;
    }
    friend bool operator!=(const ArenaHandle& a, const ArenaHandle& b) { return !(a == b); }
};

// A slab allocator with a free list, handing out generation-tagged
// handles instead of pointers.
//
// WHAT THE GENERATION TAG DOES, AND WHAT IT DOESN'T
//
// Every slot carries a counter, bumped on Free(). A handle remembers the
// generation it was issued at. Get() compares them, so dereferencing a
// handle to a slot that has since been freed (and possibly reused by an
// unrelated allocation) is a detected error rather than a silent read of
// whatever now lives there.
//
// That converts arena use-after-free from a memory-safety bug -- visible
// only under ASan, and only if the corrupted read happens to produce
// output wrong enough for a differential comparison to notice -- into an
// ordinary engine invariant violation that the normal unit tests,
// RunDifferential and CheckInvariants all catch on every push.
//
// It does NOT catch everything, and the difference matters:
//
//   - A reference obtained from Get() and held across an Allocate() that
//     triggers slot-vector growth is dangling, and the generation is
//     still perfectly valid. Handles survive growth; references do not.
//     Do not hold a T& across any call that can allocate. ASan catches
//     this one; the tags cannot.
//   - Generations are 32-bit and wrap. After 2^32 free/reuse cycles on
//     one slot a stale handle could alias a live one. At this engine's
//     measured churn (~1.5 level lifecycle events per op) that is on the
//     order of 10^9 ops on a single slot; noted for honesty, not because
//     it is reachable in a trading session.
//
// This is why LOB_ARENA_GENERATION_TAGS is a compile-time option and why
// the ASan nightly remains step 4's gate: the benchmark build has the
// tags compiled out, so it is a genuinely different program from the one
// CI exercises, and it is the one this project publishes cycle counts
// about. Tags are the fast catcher for the tagged build; ASan is the
// backstop for the untagged one. Neither covers the other's build.
template <typename T>
class Arena {
  public:
    Arena() = default;
    explicit Arena(std::size_t initial_capacity) { slots_.reserve(initial_capacity); }

    // Copyable, and it has to be: OptimizedOrderBook is copied wholesale
    // (bench_matching_engine starts every timed variant from a fresh
    // `pass_book = warmup_book`), so a book that holds an arena must
    // deep-copy it.
    //
    // The reason this works without any handle fixup is that a copy
    // preserves SLOT INDICES exactly -- same slots vector, same
    // positions, same generations. So every handle stored in the copied
    // book's price levels already refers to the corresponding chunk in
    // the copied arena. Copying pointers would have required rewriting
    // every one of them; copying indices requires rewriting none.
    Arena(const Arena& other)
        : free_head_(other.free_head_), live_count_(other.live_count_) {
        slots_.resize(other.slots_.size());
        for (std::size_t i = 0; i < other.slots_.size(); ++i) {
            const Slot& src = other.slots_[i];
            Slot& dst = slots_[i];
            dst.generation = src.generation;
            dst.next_free = src.next_free;
            dst.live = src.live;
            if (src.live) {
                ::new (static_cast<void*>(dst.storage)) T(*Ptr(src));
            }
        }
    }

    Arena& operator=(const Arena& other) {
        if (this == &other) return *this;
        Arena copy(other);
        swap(copy);
        return *this;
    }

    Arena(Arena&& other) noexcept { swap(other); }

    Arena& operator=(Arena&& other) noexcept {
        if (this == &other) return *this;
        Clear();
        swap(other);
        return *this;
    }

    void swap(Arena& other) noexcept {
        slots_.swap(other.slots_);
        std::swap(free_head_, other.free_head_);
        std::swap(live_count_, other.live_count_);
    }

    ~Arena() { Clear(); }

    // Construct a T in a free slot (reusing one if available, else
    // growing) and return a handle to it.
    template <typename... Args>
    ArenaHandle Allocate(Args&&... args) {
        std::uint32_t index;
        if (free_head_ != ArenaHandle::kInvalidIndex) {
            index = free_head_;
            free_head_ = slots_[index].next_free;
        } else {
            index = static_cast<std::uint32_t>(slots_.size());
            slots_.emplace_back();
        }

        Slot& slot = slots_[index];
        assert(!slot.live && "arena free list handed out a slot that is already live");
        ::new (static_cast<void*>(slot.storage)) T(std::forward<Args>(args)...);
        slot.live = true;
        ++live_count_;

        return ArenaHandle{index, slot.generation};
    }

    // Destroy the T in this handle's slot and return the slot to the
    // free list. Bumps the generation, which is what invalidates every
    // other outstanding handle to that slot.
    void Free(ArenaHandle handle) {
        Slot& slot = SlotFor(handle);
        Ptr(slot)->~T();
        slot.live = false;
        ++slot.generation;
        slot.next_free = free_head_;
        free_head_ = handle.index;
        --live_count_;
    }

    [[nodiscard]] T& Get(ArenaHandle handle) { return *Ptr(SlotFor(handle)); }
    [[nodiscard]] const T& Get(ArenaHandle handle) const { return *Ptr(SlotFor(handle)); }

    // Whether this handle currently refers to a live slot at the right
    // generation. Exists so callers (and tests) can ASK rather than
    // having to risk the assert in Get(); with tags compiled out this
    // can only answer the liveness half, and says so.
    [[nodiscard]] bool IsValid(ArenaHandle handle) const {
        if (!handle.valid() || handle.index >= slots_.size()) return false;
        const Slot& slot = slots_[handle.index];
        if (!slot.live) return false;
#if LOB_ARENA_GENERATION_TAGS
        if (slot.generation != handle.generation) return false;
#endif
        return true;
    }

    [[nodiscard]] std::size_t live_count() const { return live_count_; }
    [[nodiscard]] std::size_t slot_count() const { return slots_.size(); }

    // Destroy every live T and drop all slots. Handles issued before
    // this are all invalid afterwards.
    void Clear() {
        for (Slot& slot : slots_) {
            if (slot.live) {
                Ptr(slot)->~T();
                slot.live = false;
            }
        }
        slots_.clear();
        free_head_ = ArenaHandle::kInvalidIndex;
        live_count_ = 0;
    }

  private:
    struct Slot {
        alignas(T) std::byte storage[sizeof(T)];
        std::uint32_t generation = 0;
        std::uint32_t next_free = ArenaHandle::kInvalidIndex;
        bool live = false;
    };

    static T* Ptr(Slot& slot) { return std::launder(reinterpret_cast<T*>(slot.storage)); }
    static const T* Ptr(const Slot& slot) {
        return std::launder(reinterpret_cast<const T*>(slot.storage));
    }

    Slot& SlotFor(ArenaHandle handle) {
        return const_cast<Slot&>(static_cast<const Arena*>(this)->SlotFor(handle));
    }

    const Slot& SlotFor(ArenaHandle handle) const {
        assert(handle.valid() && "dereferenced a default-constructed (null) arena handle");
        assert(handle.index < slots_.size() && "arena handle index out of range");
        const Slot& slot = slots_[handle.index];
        assert(slot.live && "arena use-after-free: handle refers to a freed slot");
#if LOB_ARENA_GENERATION_TAGS
        assert(slot.generation == handle.generation &&
               "arena use-after-free: slot was freed and reused since this handle was issued");
#endif
        return slot;
    }

    std::vector<Slot> slots_;
    std::uint32_t free_head_ = ArenaHandle::kInvalidIndex;
    std::size_t live_count_ = 0;
};

}  // namespace lob
