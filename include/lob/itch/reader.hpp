#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "lob/itch/decode.hpp"

namespace lob::itch {

struct RawMessage {
    char type;                        // body[0], for convenience
    std::span<const std::byte> body;  // the message payload, length-prefix excluded
    std::size_t file_offset;          // byte offset of the length prefix -- for diagnostics
};

// Walks a buffer of sequential [2-byte big-endian length][body] messages --
// the framing NASDAQ's historical sample files use. Confirmed against a
// real PSX sample rather than assumed from the spec alone: the first bytes
// of 20190730.PSX_ITCH_50 decode as length=12, body starting with 'S'
// (System Event), whose fields decode to EventCode='O' (Start of
// Messages) -- exactly the message every ITCH file is expected to open
// with.
//
// No allocation, no copying: every RawMessage is a view into the
// caller-owned buffer (typically an mmap'd file -- see mapped_file.hpp).
class MessageReader {
  public:
    explicit MessageReader(std::span<const std::byte> data) : data_(data) {}

    std::optional<RawMessage> Next() {
        if (offset_ + 2 > data_.size()) {
            return std::nullopt;  // end of stream (or a truncated trailing prefix, e.g. a
                                  // range-sampled file)
        }
        const std::uint16_t length = ReadU16(data_.data() + offset_);
        if (offset_ + 2 + length > data_.size()) {
            return std::nullopt;  // a message body cut off mid-stream -- same handling, not an
                                  // error
        }

        RawMessage msg;
        msg.file_offset = offset_;
        msg.body = data_.subspan(offset_ + 2, length);
        msg.type = static_cast<char>(std::to_integer<std::uint8_t>(msg.body[0]));
        offset_ += 2 + length;
        return msg;
    }

  private:
    std::span<const std::byte> data_;
    std::size_t offset_ = 0;
};

}  // namespace lob::itch
