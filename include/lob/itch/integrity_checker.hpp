#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace lob::itch {

struct IntegrityViolation {
    std::size_t file_offset;
    char message_type;
    std::uint64_t order_ref;
    std::string reason;
};

// Validates the referential-integrity invariant every ITCH depth-of-book
// consumer depends on: an Order Executed / Executed-With-Price / Cancel /
// Delete / Replace message's order reference number must already be a
// live, sufficiently-large resting order. This needs no external ground
// truth and no price-level book at all -- just a running per-reference
// remaining-quantity count -- so it runs over every symbol in the file in
// one cheap pass, independent of whichever single symbol Phase 2's engine
// replay focuses on. This is the primary correctness check for the raw
// ITCH parsing path: millions of assertions against the file's own
// internal consistency, no LOBSTER or other external data required.
//
// This is deliberately a second, independent implementation of "is this
// order reference known, with enough remaining quantity" rather than
// reusing OrderBook's locations_ map -- if this checker and the
// single-symbol OrderBook engine (replay.hpp) ever disagree about an
// order's validity, that disagreement itself is a signal worth seeing,
// not a redundancy to eliminate.
class OrderReferenceIntegrityChecker {
  public:
    void OnAdd(std::uint64_t order_ref, std::uint32_t shares, std::size_t file_offset);
    void OnExecuted(std::uint64_t order_ref, std::uint32_t shares, char message_type,
                    std::size_t file_offset);
    void OnCanceled(std::uint64_t order_ref, std::uint32_t shares, std::size_t file_offset);
    void OnDeleted(std::uint64_t order_ref, std::size_t file_offset);
    void OnReplaced(std::uint64_t original_ref, std::uint64_t new_ref, std::uint32_t new_shares,
                    std::size_t file_offset);

    const std::vector<IntegrityViolation>& Violations() const { return violations_; }
    std::size_t LiveOrderCount() const { return live_.size(); }

  private:
    void Reduce(std::uint64_t order_ref, std::uint32_t shares, char message_type,
                std::size_t file_offset);

    std::unordered_map<std::uint64_t, std::uint32_t> live_;  // order_ref -> remaining shares
    std::vector<IntegrityViolation> violations_;
};

}  // namespace lob::itch
