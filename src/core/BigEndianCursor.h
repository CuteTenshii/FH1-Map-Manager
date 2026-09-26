#pragma once

#include <QByteArray>
#include <QtEndian>

#include <cstdint>
#include <cstring>

namespace fh1 {

/// Bounds-checked reader of big-endian values from a byte array. A read past
/// the end returns zero and marks the cursor failed, so a parser can read a
/// whole structure and check ok() once.
class BigEndianCursor {
public:
    explicit BigEndianCursor(const QByteArray& data, qsizetype pos = 0)
        : m_data(data)
        , m_pos(pos)
    {
    }

    bool ok() const { return !m_failed; }
    qsizetype pos() const { return m_pos; }

    std::uint32_t u32() { return read<std::uint32_t>(); }
    std::uint16_t u16() { return read<std::uint16_t>(); }
    std::uint8_t u8() { return read<std::uint8_t>(); }

    float f32()
    {
        const std::uint32_t bits = u32();
        float value = 0.0F;
        static_assert(sizeof value == sizeof bits);
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }

    void skip(qsizetype bytes)
    {
        if (has(bytes)) {
            m_pos += bytes;
        }
    }

    /// True if `bytes` more bytes can be read; marks the cursor failed if not.
    bool has(qsizetype bytes)
    {
        if (bytes < 0 || m_pos + bytes > m_data.size()) {
            m_failed = true;
            return false;
        }
        return true;
    }

private:
    template <typename T> T read()
    {
        if (!has(sizeof(T))) {
            return 0;
        }
        const T value = qFromBigEndian<T>(m_data.constData() + m_pos);
        m_pos += static_cast<qsizetype>(sizeof(T));
        return value;
    }

    const QByteArray& m_data;
    qsizetype m_pos = 0;
    bool m_failed = false;
};

} // namespace fh1
