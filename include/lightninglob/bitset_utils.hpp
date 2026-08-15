// bitset_utils.hpp - a small dynamic bitset with hardware-accelerated
// "find nearest set bit" scans.
//
// This is the key primitive behind OrderBookArray's O(1)-amortized best-price
// tracking: each price tick maps to one bit ("is this price level occupied?").
// Moving the best bid/ask pointer after the current best level empties means
// finding the next occupied bit, which we do word-at-a-time using
// std::countr_zero/std::countl_zero (single CPU instructions: TZCNT/LZCNT on
// x86, CLZ/RBIT+CLZ on ARM) rather than testing one price at a time.
#pragma once

#include <cassert>
#include <cstdint>
#include <bit>
#include <optional>
#include <vector>

namespace lightninglob {

class Bitset {
public:
    using Word = std::uint64_t;
    static constexpr std::size_t kBitsPerWord = 64;

    Bitset() = default;

    explicit Bitset(std::size_t num_bits)
        : num_bits_(num_bits), words_((num_bits + kBitsPerWord - 1) / kBitsPerWord, Word{0}) {}

    [[nodiscard]] std::size_t size() const noexcept { return num_bits_; }

    void set(std::size_t index) noexcept {
        assert(index < num_bits_);
        words_[index / kBitsPerWord] |= (Word{1} << (index % kBitsPerWord));
    }

    void clear(std::size_t index) noexcept {
        assert(index < num_bits_);
        words_[index / kBitsPerWord] &= ~(Word{1} << (index % kBitsPerWord));
    }

    [[nodiscard]] bool test(std::size_t index) const noexcept {
        assert(index < num_bits_);
        return (words_[index / kBitsPerWord] & (Word{1} << (index % kBitsPerWord))) != 0;
    }

    // Total number of set bits, via hardware popcount per word.
    [[nodiscard]] std::size_t count() const noexcept {
        std::size_t total = 0;
        for (const Word w : words_) total += static_cast<std::size_t>(std::popcount(w));
        return total;
    }

    // Smallest set bit at position >= from. O(1) amortized: only touches
    // extra words when crossing a run of fully-empty 64-bit blocks.
    [[nodiscard]] std::optional<std::size_t> find_next_set(std::size_t from) const noexcept {
        if (from >= num_bits_) return std::nullopt;
        std::size_t word_idx = from / kBitsPerWord;
        const std::size_t bit_off = from % kBitsPerWord;

        Word word = words_[word_idx] & (~Word{0} << bit_off);
        for (;;) {
            if (word != 0) {
                const std::size_t pos =
                    word_idx * kBitsPerWord + static_cast<std::size_t>(std::countr_zero(word));
                return pos < num_bits_ ? std::optional<std::size_t>(pos) : std::nullopt;
            }
            ++word_idx;
            if (word_idx >= words_.size()) return std::nullopt;
            word = words_[word_idx];
        }
    }

    // Largest set bit at position <= from.
    [[nodiscard]] std::optional<std::size_t> find_prev_set(std::size_t from) const noexcept {
        if (words_.empty() || from >= num_bits_) return std::nullopt;
        std::size_t word_idx = from / kBitsPerWord;
        const std::size_t bit_off = from % kBitsPerWord;

        // Mask to keep only bits [0, bit_off] of this word. Guard the
        // bit_off == 63 case explicitly: shifting a 64-bit value by 64 is UB.
        const Word mask = (bit_off == kBitsPerWord - 1) ? ~Word{0} : ((Word{1} << (bit_off + 1)) - 1);
        Word word = words_[word_idx] & mask;
        for (;;) {
            if (word != 0) {
                const std::size_t highest_bit = kBitsPerWord - 1 - static_cast<std::size_t>(std::countl_zero(word));
                return word_idx * kBitsPerWord + highest_bit;
            }
            if (word_idx == 0) return std::nullopt;
            --word_idx;
            word = words_[word_idx];
        }
    }

private:
    std::size_t num_bits_ = 0;
    std::vector<Word> words_;
};

}  // namespace lightninglob
