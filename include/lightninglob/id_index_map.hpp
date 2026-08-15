// id_index_map.hpp - a small, purpose-built open-addressing hash map for
// the one lookup every OrderBook needs on every add/cancel/amend: OrderId
// -> pool index.
//
// Why this exists: the benchmarks in benchmarks/order_book_bench.cpp showed
// a ~230-320ns fixed latency floor shared identically by all three
// OrderBook approaches - present even where the level-lookup itself should
// be near-instant (OrderBookArray's O(1) direct indexing). That floor is
// std::unordered_map: a node-based container that heap-allocates one node
// per entry and chases a pointer to reach it, on the one hash lookup every
// approach still had in common. This class replaces it with:
//   - Open addressing (linear probing) into one contiguous std::vector -
//     no per-entry allocation, no pointer chasing.
//   - Fibonacci hashing (multiply by the golden-ratio constant, keep the
//     high bits) for good key distribution regardless of whether OrderIds
//     happen to be sequential, sparse, or adversarial.
//   - Knuth's backward-shift deletion (TAOCP Vol. 3, Algorithm R2) instead
//     of tombstones: erasing an entry repairs the probe sequence in place
//     by shifting a later entry backward when it's safe to do so, so
//     find()/insert() never have to skip over dead slots and the table
//     never needs a "clean up the tombstones" pass no matter how many
//     cancels it sees over its lifetime.
//
// Deliberately not a general-purpose container: it's keyed on OrderId with
// std::uint32_t values and 0 (kInvalidOrderId) reserved as the empty-slot
// sentinel, because that's the one thing this project actually needs. See
// docs/DESIGN.md for why backward-shift deletion was chosen over the
// simpler tombstone-plus-periodic-rehash alternative, and how it was
// validated.
#pragma once

#include "lightninglob/types.hpp"

#include <cassert>
#include <bit>
#include <vector>

namespace lightninglob {

class IdIndexMap {
public:
    explicit IdIndexMap(std::size_t capacity_hint = 16) {
        std::size_t cap = kMinCapacity;
        while (cap < capacity_hint * 2) {
            cap *= 2;
        }
        allocate(cap);
    }

    // Inserts key->value. Returns false without modifying the map if key is
    // already present (callers use this for the duplicate-order-id check).
    bool insert(OrderId key, std::uint32_t value) {
        assert(key != kInvalidOrderId);
        if ((size_ + 1) * 2 > slots_.size()) [[unlikely]] {
            grow();
        }
        std::size_t idx = slot_for(key);
        while (slots_[idx].key != kInvalidOrderId) {
            if (slots_[idx].key == key) return false;  // already present
            idx = (idx + 1) & mask();
        }
        slots_[idx] = Slot{key, value};
        ++size_;
        return true;
    }

    [[nodiscard]] std::uint32_t* find(OrderId key) noexcept {
        std::size_t idx = slot_for(key);
        while (slots_[idx].key != kInvalidOrderId) {
            if (slots_[idx].key == key) return &slots_[idx].value;
            idx = (idx + 1) & mask();
        }
        return nullptr;
    }
    [[nodiscard]] const std::uint32_t* find(OrderId key) const noexcept {
        return const_cast<IdIndexMap*>(this)->find(key);
    }

    [[nodiscard]] bool contains(OrderId key) const noexcept { return find(key) != nullptr; }

    // Knuth backward-shift deletion - see the file header. Never
    // reallocates, never leaves a tombstone.
    bool erase(OrderId key) noexcept {
        std::size_t i = slot_for(key);
        while (slots_[i].key != kInvalidOrderId && slots_[i].key != key) {
            i = (i + 1) & mask();
        }
        if (slots_[i].key == kInvalidOrderId) return false;  // not found

        slots_[i] = Slot{};
        --size_;

        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask();
            if (slots_[j].key == kInvalidOrderId) break;

            const std::size_t r = slot_for(slots_[j].key);  // home slot of the entry at j
            // Skip (leave slots_[j] where it is) iff r lies strictly within
            // the cyclic interval (i, j] - i.e. moving it back to the hole
            // at i would place it "behind" its own home slot in probe
            // order, making it unreachable by a future find(). Otherwise,
            // i is on the reachable path from r, so it's safe to move.
            const bool r_between_i_and_j = (i <= j) ? (i < r && r <= j) : (i < r || r <= j);
            if (r_between_i_and_j) continue;

            slots_[i] = slots_[j];
            slots_[j] = Slot{};
            i = j;
        }
        return true;
    }

    void clear() noexcept {
        std::fill(slots_.begin(), slots_.end(), Slot{});
        size_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

private:
    struct Slot {
        OrderId key = kInvalidOrderId;  // kInvalidOrderId (0) marks an empty slot
        std::uint32_t value = 0;
    };

    static constexpr std::size_t kMinCapacity = 16;
    static constexpr std::uint64_t kFibonacciMul = 0x9E3779B97F4A7C15ULL;  // golden ratio, 64-bit

    [[nodiscard]] std::size_t mask() const noexcept { return slots_.size() - 1; }

    // Fibonacci hashing: multiply by an odd golden-ratio-derived constant
    // and keep the *high* bits (shift right), which mix far better than
    // the low bits of a plain multiplicative hash - standard technique,
    // one multiply and one shift per lookup.
    [[nodiscard]] std::size_t slot_for(OrderId key) const noexcept {
        const std::uint64_t mixed = key * kFibonacciMul;
        return static_cast<std::size_t>(mixed >> shift_);
    }

    void allocate(std::size_t cap) {
        slots_.assign(cap, Slot{});
        shift_ = 64 - static_cast<unsigned>(std::countr_zero(cap));  // cap is always a power of 2
    }

    void grow() {
        std::vector<Slot> old = std::move(slots_);
        allocate(old.size() * 2);
        size_ = 0;
        for (const Slot& s : old) {
            if (s.key != kInvalidOrderId) {
                insert(s.key, s.value);  // old table was at <= 0.5 load factor; the new (2x) table can't need to grow again mid-rehash
            }
        }
    }

    std::vector<Slot> slots_;
    std::size_t size_ = 0;
    unsigned shift_ = 64 - 4;
};

}  // namespace lightninglob
