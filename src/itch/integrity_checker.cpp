#include "lob/itch/integrity_checker.hpp"

namespace lob::itch {

void OrderReferenceIntegrityChecker::OnAdd(std::uint64_t order_ref, std::uint32_t shares,
                                           std::size_t file_offset) {
    // A duplicate Add (same reference already live) is itself a violation
    // -- the spec guarantees day-unique reference numbers.
    if (live_.contains(order_ref)) {
        violations_.push_back(
            {file_offset, 'A', order_ref, "order reference already live (duplicate Add)"});
        return;
    }
    live_[order_ref] = shares;
}

void OrderReferenceIntegrityChecker::Reduce(std::uint64_t order_ref, std::uint32_t shares,
                                            char message_type, std::size_t file_offset) {
    auto it = live_.find(order_ref);
    if (it == live_.end()) {
        violations_.push_back({file_offset, message_type, order_ref,
                               "references an order reference that is not live"});
        return;
    }
    if (it->second < shares) {
        violations_.push_back(
            {file_offset, message_type, order_ref, "reduces more shares than remain resting"});
        return;
    }
    it->second -= shares;
    if (it->second == 0) {
        live_.erase(it);
    }
}

void OrderReferenceIntegrityChecker::OnExecuted(std::uint64_t order_ref, std::uint32_t shares,
                                                char message_type, std::size_t file_offset) {
    Reduce(order_ref, shares, message_type, file_offset);
}

void OrderReferenceIntegrityChecker::OnCanceled(std::uint64_t order_ref, std::uint32_t shares,
                                                std::size_t file_offset) {
    Reduce(order_ref, shares, 'X', file_offset);
}

void OrderReferenceIntegrityChecker::OnDeleted(std::uint64_t order_ref, std::size_t file_offset) {
    auto it = live_.find(order_ref);
    if (it == live_.end()) {
        violations_.push_back(
            {file_offset, 'D', order_ref, "references an order reference that is not live"});
        return;
    }
    live_.erase(it);
}

void OrderReferenceIntegrityChecker::OnReplaced(std::uint64_t original_ref, std::uint64_t new_ref,
                                                std::uint32_t new_shares, std::size_t file_offset) {
    auto it = live_.find(original_ref);
    if (it == live_.end()) {
        violations_.push_back({file_offset, 'U', original_ref,
                               "replace references an order reference that is not live"});
        // Still register the new reference below -- losing track of the
        // original shouldn't cascade into a second, unrelated violation
        // the next time something references the new id.
    } else {
        live_.erase(it);
    }

    if (live_.contains(new_ref)) {
        violations_.push_back(
            {file_offset, 'U', new_ref, "replace's new order reference is already live"});
        return;
    }
    live_[new_ref] = new_shares;
}

}  // namespace lob::itch
