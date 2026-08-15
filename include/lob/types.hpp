#pragma once

#include <compare>
#include <cstdint>
#include <functional>

namespace lob {

// Every price and quantity in this engine is a plain integer -- no floats,
// anywhere. Floating-point price comparisons are a classic source of
// matching-priority bugs (rounding can make (a > b) and (b > a) evaluate
// inconsistently depending on how each value was computed), and real venues
// do not price in floats for exactly that reason. NASDAQ TotalView-ITCH
// itself encodes prices as an integer with an implied number of decimal
// places (Price(4) = 4 implied decimals), so treating "ticks" as the unit
// here also means Phase 2's wire parser needs no conversion layer between
// wire format and engine format.
struct Price {
    std::int64_t ticks = 0;
    auto operator<=>(const Price&) const = default;
};

struct Quantity {
    std::int64_t units = 0;
    auto operator<=>(const Quantity&) const = default;

    Quantity& operator+=(Quantity rhs) {
        units += rhs.units;
        return *this;
    }
    Quantity& operator-=(Quantity rhs) {
        units -= rhs.units;
        return *this;
    }
    friend Quantity operator+(Quantity lhs, Quantity rhs) { return {lhs.units + rhs.units}; }
    friend Quantity operator-(Quantity lhs, Quantity rhs) { return {lhs.units - rhs.units}; }
};

// A day-unique order identifier. Wrapped rather than left as a bare
// std::uint64_t so the compiler rejects passing an OrderId where a Quantity
// or a raw tick count was meant, or vice versa -- a mixed-up argument order
// between two same-typed integers is exactly the kind of bug that's easy to
// write and easy to miss in review, and catching it at compile time is
// worth the small amount of boilerplate below.
struct OrderId {
    std::uint64_t value = 0;
    auto operator<=>(const OrderId&) const = default;
};

enum class Side : std::uint8_t { Buy, Sell };

}  // namespace lob

template <>
struct std::hash<lob::OrderId> {
    std::size_t operator()(const lob::OrderId& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};
