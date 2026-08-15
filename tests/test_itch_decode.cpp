#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <vector>

#include "lob/itch/messages.hpp"
#include "lob/itch/reader.hpp"

using namespace lob::itch;

namespace {

// Every fixture below is a real message body captured from
// 20190730.PSX_ITCH_50 (a public NASDAQ PSX ITCH 5.0 sample), not a
// hand-built synthetic one -- hand-crafted byte arrays can be
// self-consistently wrong in a way that only matches the code being
// tested. These are the actual first occurrence of each message type
// found scanning the file, with expected values cross-checked for
// plausibility (valid side characters, round share counts, tickers that
// are real symbols, prices in a sane range for mid-2019).
std::vector<std::byte> Bytes(std::initializer_list<unsigned char> raw) {
    std::vector<std::byte> out;
    out.reserve(raw.size());
    for (unsigned char b : raw) {
        out.push_back(static_cast<std::byte>(b));
    }
    return out;
}

}  // namespace

TEST(ItchDecode, StockDirectory) {
    // R, file offset 14: stock_locate=1, ticker "A" (Agilent Technologies)
    auto body =
        Bytes({0x52, 0x00, 0x01, 0x00, 0x00, 0x0a, 0x65, 0x26, 0x2c, 0x0f, 0xbc, 0x41, 0x20,
               0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x4e, 0x20, 0x00, 0x00, 0x00, 0x64, 0x4e,
               0x43, 0x5a, 0x20, 0x50, 0x4e, 0x20, 0x31, 0x4e, 0x00, 0x00, 0x00, 0x00, 0x4e});
    const auto m = DecodeStockDirectory(body);
    EXPECT_EQ(m.stock_locate, 1);
    EXPECT_EQ(m.stock.substr(0, m.stock.find(' ')), "A");
}

TEST(ItchDecode, AddOrderNoMpid) {
    // A, file offset 961759: sell 1000 CHK @ 2.3600
    auto body = Bytes({0x41, 0x05, 0x7a, 0x00, 0x00, 0x1a, 0x31, 0x86, 0x3b, 0xb4, 0x3b, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x34, 0x53, 0x00, 0x00, 0x03, 0xe8,
                       0x43, 0x48, 0x4b, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x5c, 0x30});
    const auto m = DecodeAddOrder(body);
    EXPECT_EQ(m.stock_locate, 1402);
    EXPECT_EQ(m.order_ref, 12340u);
    EXPECT_EQ(m.side, 'S');
    EXPECT_EQ(m.shares, 1000u);
    EXPECT_EQ(m.stock.substr(0, 3), "CHK");
    EXPECT_EQ(m.price_ticks, 23600u);  // 2.3600 at 4 implied decimals
}

TEST(ItchDecode, AddOrderWithMpid) {
    // F, file offset 4652815: buy 100 AMD @ 31.3600
    auto body =
        Bytes({0x46, 0x01, 0x51, 0x00, 0x00, 0x1d, 0x17, 0xf2, 0xa2, 0x18, 0x47, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x02, 0x56, 0x2b, 0x42, 0x00, 0x00, 0x00, 0x64, 0x41, 0x4d, 0x44, 0x20,
               0x20, 0x20, 0x20, 0x20, 0x00, 0x04, 0xc9, 0x00, 0x56, 0x41, 0x4c, 0x58});
    // DecodeAddOrder ignores the trailing 4-byte MPID attribution field,
    // since 'A' and 'F' share identical layout through Price -- this is
    // exactly that shared-decoder assumption under test.
    const auto m = DecodeAddOrder(body);
    EXPECT_EQ(m.stock_locate, 337);
    EXPECT_EQ(m.order_ref, 153131u);
    EXPECT_EQ(m.side, 'B');
    EXPECT_EQ(m.shares, 100u);
    EXPECT_EQ(m.stock.substr(0, 3), "AMD");
    EXPECT_EQ(m.price_ticks, 313600u);  // 31.3600
}

TEST(ItchDecode, OrderExecuted) {
    // E, file offset 965743
    auto body = Bytes({0x45, 0x14, 0x5a, 0x00, 0x02, 0x1a, 0x32, 0x88, 0xd4, 0x5e, 0x38,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0xc2, 0x00, 0x00, 0x00,
                       0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x45, 0x23});
    const auto m = DecodeOrderExecuted(body);
    EXPECT_EQ(m.stock_locate, 5210);
    EXPECT_EQ(m.order_ref, 12482u);
    EXPECT_EQ(m.executed_shares, 50u);
}

TEST(ItchDecode, OrderExecutedWithPrice) {
    // C, file offset 15304796
    auto body = Bytes({0x43, 0x00, 0x5b, 0x00, 0x02, 0x1f, 0x1b, 0x6b, 0x58, 0x5a, 0x5e, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x07, 0x08, 0x2c, 0x00, 0x00, 0x00, 0x06, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x7b, 0x59, 0x00, 0x2e, 0x7e, 0x58});
    const auto m = DecodeOrderExecutedWithPrice(body);
    EXPECT_EQ(m.stock_locate, 91);
    EXPECT_EQ(m.order_ref, 460844u);
    EXPECT_EQ(m.executed_shares, 6u);
}

TEST(ItchDecode, OrderCancel) {
    // X, file offset 1057624: 1800 shares partially cancelled
    auto body = Bytes({0x58, 0x1e, 0x5a, 0x00, 0x00, 0x1a, 0x5f, 0x4e, 0xf5, 0x3c, 0x85, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x3f, 0x00, 0x00, 0x07, 0x08});
    const auto m = DecodeOrderCancel(body);
    EXPECT_EQ(m.stock_locate, 7770);
    EXPECT_EQ(m.order_ref, 15423u);
    EXPECT_EQ(m.canceled_shares, 1800u);
}

TEST(ItchDecode, OrderDelete) {
    // D, file offset 963089
    auto body = Bytes({0x44, 0x00, 0x0e, 0x00, 0x00, 0x1a, 0x31, 0xc2, 0x8d, 0xdf, 0x6c, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x30, 0x5c});
    const auto m = DecodeOrderDelete(body);
    EXPECT_EQ(m.stock_locate, 14);
    EXPECT_EQ(m.order_ref, 12380u);
}

TEST(ItchDecode, OrderReplace) {
    // U, file offset 1302634: ref 30270 replaced by ref 30283, 2000 shares @ 300.3000
    auto body = Bytes({0x55, 0x1c, 0xe5, 0x00, 0x00, 0x1a, 0xf1, 0x35, 0x8e, 0x3b, 0x2c, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x76, 0x3e, 0x00, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x76, 0x4b, 0x00, 0x00, 0x07, 0xd0, 0x00, 0x2d, 0xd2, 0x78});
    const auto m = DecodeOrderReplace(body);
    EXPECT_EQ(m.stock_locate, 7397);
    EXPECT_EQ(m.original_order_ref, 30270u);
    EXPECT_EQ(m.new_order_ref, 30283u);
    EXPECT_EQ(m.shares, 2000u);
    EXPECT_EQ(m.price_ticks, 3003000u);  // 300.3000
}

TEST(ItchReader, WalksSequentialLengthPrefixedMessages) {
    // The real opening of 20190730.PSX_ITCH_50: a 12-byte System Event
    // message (EventCode 'O' = Start of Messages) followed by a 39-byte
    // Stock Directory message for ticker "A".
    auto data = Bytes({
        // System Event, length 12
        0x00,
        0x0c,
        0x53,
        0x00,
        0x00,
        0x00,
        0x00,
        0x0a,
        0x2a,
        0x92,
        0x22,
        0x57,
        0x86,
        0x4f,
        // Stock Directory, length 39
        0x00,
        0x27,
        0x52,
        0x00,
        0x01,
        0x00,
        0x00,
        0x0a,
        0x65,
        0x26,
        0x2c,
        0x0f,
        0xbc,
        0x41,
        0x20,
        0x20,
        0x20,
        0x20,
        0x20,
        0x20,
        0x20,
        0x4e,
        0x20,
        0x00,
        0x00,
        0x00,
        0x64,
        0x4e,
        0x43,
        0x5a,
        0x20,
        0x50,
        0x4e,
        0x20,
        0x31,
        0x4e,
        0x00,
        0x00,
        0x00,
        0x00,
        0x4e,
    });
    MessageReader reader(data);

    const auto first = reader.Next();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->type, 'S');
    EXPECT_EQ(first->file_offset, 0u);
    EXPECT_EQ(first->body.size(), 12u);
    // EventCode is the last byte of the System Event body.
    EXPECT_EQ(static_cast<char>(std::to_integer<unsigned char>(first->body.back())), 'O');

    const auto second = reader.Next();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->type, 'R');
    EXPECT_EQ(second->file_offset, 14u);
    EXPECT_EQ(second->body.size(), 39u);
    EXPECT_EQ(DecodeStockDirectory(second->body).stock_locate, 1);

    EXPECT_FALSE(reader.Next().has_value());
}

TEST(ItchReader, TruncatedTrailingMessageEndsTheStreamCleanly) {
    // A length prefix claiming more bytes than actually follow -- exactly
    // what a byte-range HTTP sample cut mid-message looks like. Must end
    // the stream, not read out of bounds.
    auto data =
        Bytes({0x00, 0x0c, 0x53, 0x00, 0x00, 0x00, 0x00, 0x0a});  // claims 12, only 6 available
    MessageReader reader(data);
    EXPECT_FALSE(reader.Next().has_value());
}
