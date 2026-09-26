#include "Lzx.h"
#include "LzxTestEncoder.h"

#include <QTest>

#include <random>

namespace {

std::vector<std::uint8_t> randomBytes(std::size_t size, std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> byte(0, 255);
    std::vector<std::uint8_t> data(size);
    for (auto& b : data) {
        b = static_cast<std::uint8_t>(byte(rng));
    }
    return data;
}

/// Text-like data with long runs, so verbatim blocks use repeat matches.
std::vector<std::uint8_t> runnyBytes(std::size_t size, std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> letter('a', 'h');
    std::uniform_int_distribution<int> runLength(1, 12);
    std::vector<std::uint8_t> data;
    data.reserve(size);
    while (data.size() < size) {
        const auto value = static_cast<std::uint8_t>(letter(rng));
        for (int i = runLength(rng); i > 0 && data.size() < size; --i) {
            data.push_back(value);
        }
    }
    return data;
}

std::vector<std::uint8_t> decode(const std::vector<std::uint8_t>& stream, std::size_t size)
{
    return fh1::LzxDecoder::decompress(stream, size);
}

} // namespace

class TestLzx : public QObject {
    Q_OBJECT

private slots:
    void uncompressedSingleChunk()
    {
        const auto data = randomBytes(1000, 1);
        const auto stream = lzxtest::encode(data, {{lzxtest::BlockKind::Uncompressed, data.size()}});
        QCOMPARE(stream.front(), std::uint8_t{0xFF});
        QVERIFY(decode(stream, data.size()) == data);
    }

    void uncompressedBlockSpanningChunks()
    {
        const auto data = randomBytes(100000, 2);
        const auto stream = lzxtest::encode(data, {{lzxtest::BlockKind::Uncompressed, data.size()}});
        QVERIFY(decode(stream, data.size()) == data);
    }

    // The XMemCompress quirk behind the reference JPEGs in UI.zip: an
    // odd-length uncompressed block is followed directly by the next block
    // header, with word alignment restarting at that odd byte.
    void oddUncompressedBlocksHaveNoPadByte()
    {
        const auto data = randomBytes(3 * 32768 + 517, 3);
        const std::vector<lzxtest::Block> blocks{
            {lzxtest::BlockKind::Uncompressed, 40001},
            {lzxtest::BlockKind::Uncompressed, 33333},
            {lzxtest::BlockKind::Uncompressed, data.size() - 40001 - 33333},
        };
        const auto stream = lzxtest::encode(data, blocks);
        QVERIFY(decode(stream, data.size()) == data);
    }

    void verbatimWithRepeatMatches()
    {
        const auto data = runnyBytes(5000, 4);
        const auto stream = lzxtest::encode(data, {{lzxtest::BlockKind::Verbatim, data.size()}});
        QVERIFY(stream.size() < data.size() + 1000);
        QVERIFY(decode(stream, data.size()) == data);
    }

    void verbatimBlockSpanningChunks()
    {
        const auto data = runnyBytes(90000, 5);
        const auto stream = lzxtest::encode(data, {{lzxtest::BlockKind::Verbatim, data.size()}});
        QVERIFY(decode(stream, data.size()) == data);
    }

    void mixedBlocksAcrossChunks()
    {
        const auto text = runnyBytes(50001, 6);
        const auto noise = randomBytes(30001, 7);
        std::vector<std::uint8_t> data = text;
        data.insert(data.end(), noise.begin(), noise.end());
        data.insert(data.end(), text.begin(), text.begin() + 20000);
        const std::vector<lzxtest::Block> blocks{
            {lzxtest::BlockKind::Verbatim, 50001},
            {lzxtest::BlockKind::Uncompressed, 30001},
            {lzxtest::BlockKind::Verbatim, 20000},
        };
        const auto stream = lzxtest::encode(data, blocks);
        QVERIFY(decode(stream, data.size()) == data);
    }

    void intelE8Translation()
    {
        // The decoder turns absolute call targets back into relative ones:
        // an E8 at offset 5 carrying 105 becomes 105 - 5 = 100.
        std::vector<std::uint8_t> encoded(64, 0x90);
        encoded[5] = 0xE8;
        encoded[6] = 105;
        encoded[7] = 0;
        encoded[8] = 0;
        encoded[9] = 0;
        const auto stream = lzxtest::encode(encoded, {{lzxtest::BlockKind::Verbatim, encoded.size()}}, 1000000);
        const auto decoded = decode(stream, encoded.size());
        std::vector<std::uint8_t> expected = encoded;
        expected[6] = 100;
        QVERIFY(decoded == expected);
    }

    void rejectsSizeMismatch()
    {
        const auto data = randomBytes(100, 8);
        const auto stream = lzxtest::encode(data, {{lzxtest::BlockKind::Uncompressed, data.size()}});
        QVERIFY_THROWS_EXCEPTION(fh1::LzxError, decode(stream, data.size() + 1));
    }

    void rejectsTruncatedInput()
    {
        const auto data = randomBytes(40000, 9);
        auto stream = lzxtest::encode(data, {{lzxtest::BlockKind::Uncompressed, data.size()}});
        stream.resize(stream.size() - 100);
        QVERIFY_THROWS_EXCEPTION(fh1::LzxError, decode(stream, data.size()));
    }

    void rejectsBadBlockType()
    {
        // One 0xFF chunk: 16 bytes output, payload starting with header bit 0
        // then block type 7 (bits 0111 -> first word 0x7000).
        const std::vector<std::uint8_t> stream{0xFF, 0x00, 0x10, 0x00, 0x04, 0x00, 0x70, 0x00, 0x00};
        QVERIFY_THROWS_EXCEPTION(fh1::LzxError, decode(stream, 16));
    }
};

QTEST_APPLESS_MAIN(TestLzx)
#include "tst_lzx.moc"
