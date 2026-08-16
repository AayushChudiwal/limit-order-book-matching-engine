#include "lob/fuzz/shrinker.hpp"

#include <algorithm>

namespace lob::fuzz {

std::vector<FuzzOp> Shrink(std::vector<FuzzOp> ops, const StillFailsPredicate& still_fails) {
    std::size_t n = 2;

    while (ops.size() >= 2) {
        const std::size_t chunk_size = ops.size() / n;
        bool some_complement_failed = false;

        for (std::size_t start = 0; start < ops.size(); start += chunk_size) {
            const std::size_t end = std::min(start + chunk_size, ops.size());

            std::vector<FuzzOp> complement;
            complement.reserve(ops.size() - (end - start));
            complement.insert(complement.end(), ops.begin(),
                              ops.begin() + static_cast<std::ptrdiff_t>(start));
            complement.insert(complement.end(), ops.begin() + static_cast<std::ptrdiff_t>(end),
                              ops.end());

            // Skip testing the empty complement: removing every op can't
            // reproduce a failure that needs at least one problematic op,
            // and RunDifferential trivially returns "ok" on empty input,
            // so this would never fire anyway -- skipping it just avoids
            // a wasted predicate call (which, for a differential run,
            // isn't free).
            if (!complement.empty() && still_fails(complement)) {
                ops = std::move(complement);
                n = std::max<std::size_t>(n - 1, 2);
                some_complement_failed = true;
                break;
            }
        }

        if (!some_complement_failed) {
            if (n >= ops.size()) {
                break;
            }
            n = std::min(n * 2, ops.size());
        }
    }

    return ops;
}

}  // namespace lob::fuzz
