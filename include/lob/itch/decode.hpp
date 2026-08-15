#pragma once

#include <cstddef>
#include <cstdint>

namespace lob::itch {

// NASDAQ TotalView-ITCH 5.0 encodes every multi-byte integer field as
// big-endian ("network byte order", per the spec's Data Types section).
// These read straight out of the source buffer with no intermediate copy
// or allocation -- that's the "zero-copy" part of this parser: a field is
// decoded directly from wherever the message bytes already live (a mapped
// file, in this codebase), never staged into an owned scratch buffer
// first. Manual byte-shifting rather than a bswap intrinsic + reinterpret
// cast because ITCH field widths (16/32/48/64-bit) aren't guaranteed to be
// naturally aligned within a message -- reading through a misaligned
// integer pointer is undefined behaviour, whereas this is well-defined
// for any offset.
inline std::uint16_t ReadU16(const std::byte* p) {
    return (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0])) << 8) |
           static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[1]));
}

inline std::uint32_t ReadU32(const std::byte* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v = (v << 8) | std::to_integer<std::uint8_t>(p[i]);
    }
    return v;
}

// ITCH timestamps are 6-byte (48-bit) nanoseconds-since-midnight fields.
// There's no native 48-bit integer type, so this widens into a uint64_t.
inline std::uint64_t ReadU48(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 6; ++i) {
        v = (v << 8) | std::to_integer<std::uint8_t>(p[i]);
    }
    return v;
}

inline std::uint64_t ReadU64(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | std::to_integer<std::uint8_t>(p[i]);
    }
    return v;
}

}  // namespace lob::itch
