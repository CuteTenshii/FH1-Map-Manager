#include "RenderMesh.h"

#include "Loaders.h"

#include <QRegularExpression>
#include <QtEndian>

#include <cstring>

namespace fh1 {

namespace {

constexpr std::uint32_t kVersion = 6;
constexpr qsizetype kPartCountOffset = 0x74;
constexpr qsizetype kFirstPartOffset = 0xB0;
constexpr std::uint32_t kMaxNameLength = 256;
constexpr std::uint32_t kMinStride = 12;
constexpr std::uint32_t kMaxStride = 64;
constexpr std::uint32_t kStripRestart16 = 0xFFFF;
constexpr std::uint32_t kStripRestart32 = 0xFFFFFFFF;

/// Bounds-checked big-endian reader over one file.
class Reader {
public:
    Reader(const QByteArray& data, QString source)
        : m_data(data)
        , m_source(std::move(source))
    {
    }

    [[noreturn]] void fail(const QString& message) const
    {
        throw LoadError(
            QStringLiteral("%1: offset 0x%2: %3").arg(m_source).arg(m_pos, 0, 16).arg(message).toStdString());
    }

    void need(qsizetype bytes) const
    {
        if (bytes < 0 || m_pos + bytes > m_data.size()) {
            fail(QStringLiteral("unexpected end of file"));
        }
    }

    std::uint32_t u32()
    {
        need(4);
        const auto v = qFromBigEndian<std::uint32_t>(m_data.constData() + m_pos);
        m_pos += 4;
        return v;
    }

    float f32()
    {
        const std::uint32_t bits = u32();
        float v = 0.0F;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }

    void expect(std::uint32_t value, const char* what)
    {
        const std::uint32_t got = u32();
        if (got != value) {
            m_pos -= 4;
            fail(QStringLiteral("expected %1 = %2, found %3").arg(QLatin1String(what)).arg(value).arg(got));
        }
    }

    QString name()
    {
        const std::uint32_t length = u32();
        if (length == 0 || length > kMaxNameLength) {
            m_pos -= 4;
            fail(QStringLiteral("implausible name length %1").arg(length));
        }
        need(length);
        QByteArray bytes(m_data.constData() + m_pos, static_cast<qsizetype>(length));
        m_pos += length;
        while (bytes.endsWith('\0')) {
            bytes.chop(1);
        }
        return QString::fromLatin1(bytes);
    }

    void skip(qsizetype bytes)
    {
        need(bytes);
        m_pos += bytes;
    }

    const char* here() const { return m_data.constData() + m_pos; }
    qsizetype pos() const { return m_pos; }
    void seek(qsizetype pos) { m_pos = pos; }

private:
    const QByteArray& m_data;
    QString m_source;
    qsizetype m_pos = 0;
};

QVector3D worldPoint(float x, float y, float z)
{
    return {x, y, -z};
}

/// Converts one index buffer to triangle-list indices, dropping degenerates.
void appendTriangles(const std::vector<std::uint32_t>& indices, std::uint32_t restart, std::uint32_t vertexCount,
    Reader& reader, std::vector<std::uint32_t>& out)
{
    bool hasRestart = false;
    for (std::uint32_t index : indices) {
        if (index == restart) {
            hasRestart = true;
        } else if (index >= vertexCount) {
            reader.fail(QStringLiteral("index %1 is past the %2 vertices").arg(index).arg(vertexCount));
        }
    }
    auto addTriangle = [&out](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        if (a != b && b != c && a != c) {
            out.push_back(a);
            out.push_back(b);
            out.push_back(c);
        }
    };
    if (!hasRestart && indices.size() % 3 == 0) {
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            addTriangle(indices[i], indices[i + 1], indices[i + 2]);
        }
        return;
    }
    // Triangle strip: every other triangle flips its winding.
    std::size_t runStart = 0;
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] == restart) {
            runStart = i + 1;
            continue;
        }
        if (i >= runStart + 2) {
            const std::size_t k = i - runStart - 2;
            if (k % 2 == 0) {
                addTriangle(indices[i - 2], indices[i - 1], indices[i]);
            } else {
                addTriangle(indices[i - 1], indices[i - 2], indices[i]);
            }
        }
    }
}

/// Reads the material table and shader paths after the last part. The tail
/// only matters for texturing, so a layout this parser does not know leaves
/// the table empty instead of rejecting the model.
void readMaterialTable(Reader& r, RenderMesh& mesh)
{
    constexpr std::uint32_t kMaxEntries = 4096;
    constexpr std::uint32_t kMaxConstants = 256;
    constexpr std::uint32_t kMaxSlots = 64;
    constexpr std::uint32_t kEngineSlot = 0x80000000;
    try {
        r.expect(1, "material table tag");
        r.expect(1, "material table tag");
        r.expect(1, "material table tag");
        const std::uint32_t count = r.u32();
        if (count > kMaxEntries) {
            r.fail(QStringLiteral("implausible material table size %1").arg(count));
        }
        std::vector<RenderMesh::MaterialInfo> table;
        table.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            RenderMesh::MaterialInfo info;
            r.expect(3, "material entry tag");
            info.shader = r.u32();
            r.expect(0, "material entry header");
            // Two groups of shader constants (float4 each); which constants
            // they set is not decoded.
            for (int group = 0; group < 2; ++group) {
                r.expect(1, "shader constant tag");
                const std::uint32_t constants = r.u32();
                if (constants > kMaxConstants) {
                    r.fail(QStringLiteral("implausible shader constant count %1").arg(constants));
                }
                r.skip(static_cast<qsizetype>(constants) * 16);
            }
            r.expect(1, "texture slot tag");
            const std::uint32_t slotCount = r.u32();
            if (slotCount > kMaxSlots) {
                r.fail(QStringLiteral("implausible texture slot count %1").arg(slotCount));
            }
            for (std::uint32_t s = 0; s < slotCount; ++s) {
                const std::uint32_t slot = r.u32();
                info.textureSlots.push_back(slot >= kEngineSlot ? -1 : static_cast<int>(slot));
            }
            table.push_back(std::move(info));
        }
        r.expect(1, "shader list tag");
        const std::uint32_t shaderCount = r.u32();
        if (shaderCount > kMaxEntries) {
            r.fail(QStringLiteral("implausible shader count %1").arg(shaderCount));
        }
        QStringList shaders;
        for (std::uint32_t i = 0; i < shaderCount; ++i) {
            shaders.append(r.name());
        }
        mesh.materialTable = std::move(table);
        mesh.shaders = std::move(shaders);
    } catch (const LoadError&) {
        mesh.materialTable.clear();
        mesh.shaders.clear();
    }
}

} // namespace

QVector2D RenderMesh::texcoord(const Part& part, const Material& material, std::uint32_t vertex, int offset)
{
    const qsizetype at = static_cast<qsizetype>(vertex) * part.stride + offset;
    if (offset < 0 || static_cast<std::uint32_t>(offset) + 4 > part.stride || at + 4 > part.vertexData.size()) {
        return {};
    }
    constexpr float kUnit = 1.0F / 65535.0F;
    const float u = static_cast<float>(qFromBigEndian<std::uint16_t>(part.vertexData.constData() + at)) * kUnit;
    const float v = static_cast<float>(qFromBigEndian<std::uint16_t>(part.vertexData.constData() + at + 2)) * kUnit;
    const QVector4D& t = material.uvOffsetScale;
    return {t.x() + t.z() * u, t.y() + t.w() * v};
}

std::size_t RenderMesh::triangleCount() const
{
    std::size_t count = 0;
    for (const Part& part : parts) {
        for (const Material& material : part.materials) {
            count += material.triangles.size() / 3;
        }
    }
    return count;
}

namespace rendermesh {

RenderMesh parse(const QByteArray& data, const QString& source)
{
    Reader r(data, source);
    r.expect(kVersion, "version");
    RenderMesh mesh;
    const float minX = r.f32();
    const float minY = r.f32();
    const float minZ = r.f32();
    r.skip(4);
    const float maxX = r.f32();
    const float maxY = r.f32();
    const float maxZ = r.f32();
    // Negating Z swaps which corner holds the minimum.
    mesh.boundsMin = QVector3D(minX, minY, -maxZ);
    mesh.boundsMax = QVector3D(maxX, maxY, -minZ);

    r.seek(kPartCountOffset);
    const std::uint32_t partCount = r.u32();
    if (partCount == 0 || partCount > 4096) {
        r.fail(QStringLiteral("implausible part count %1").arg(partCount));
    }
    r.seek(kFirstPartOffset);
    mesh.parts.reserve(partCount);
    for (std::uint32_t p = 0; p < partCount; ++p) {
        if (p > 0) {
            r.skip(48);
            r.expect(1, "part separator");
        }
        RenderMesh::Part part;
        part.name = r.name();
        r.expect(3, "vertex block tag");
        const std::uint32_t vertexCount = r.u32();
        part.stride = r.u32();
        r.expect(0, "vertex block padding");
        if (part.stride < kMinStride || part.stride > kMaxStride) {
            r.fail(QStringLiteral("unsupported vertex stride %1").arg(part.stride));
        }
        r.need(static_cast<qsizetype>(vertexCount) * part.stride);
        part.vertexData = QByteArray(r.here(), static_cast<qsizetype>(vertexCount) * part.stride);
        part.positions.reserve(vertexCount);
        for (std::uint32_t v = 0; v < vertexCount; ++v) {
            const qsizetype start = r.pos();
            const float x = r.f32();
            const float y = r.f32();
            const float z = r.f32();
            part.positions.push_back(worldPoint(x, y, z));
            r.seek(start + part.stride);
        }

        r.expect(1, "material list tag");
        const std::uint32_t materialCount = r.u32();
        r.expect(1, "material list tag");
        if (materialCount > 4096) {
            r.fail(QStringLiteral("implausible material count %1").arg(materialCount));
        }
        for (std::uint32_t m = 0; m < materialCount; ++m) {
            r.expect(2, "material tag");
            RenderMesh::Material material;
            material.name = r.name();
            r.expect(0, "material header");
            r.u32();
            material.tableIndex = r.u32();
            r.expect(1, "material header");
            r.skip(32);
            const float uOffset = r.f32();
            const float vOffset = r.f32();
            const float uScale = r.f32();
            const float vScale = r.f32();
            material.uvOffsetScale = QVector4D(uOffset, vOffset, uScale, vScale);
            r.expect(4, "index buffer tag");
            r.expect(0, "index buffer header");
            const std::uint32_t indexCount = r.u32();
            const std::uint32_t width = r.u32();
            if (width != 2 && width != 4) {
                r.fail(QStringLiteral("unsupported index width %1").arg(width));
            }
            const qsizetype bytes = static_cast<qsizetype>(indexCount) * width;
            r.need(bytes);
            std::vector<std::uint32_t> indices(indexCount);
            for (std::uint32_t i = 0; i < indexCount; ++i) {
                const char* at = r.here() + static_cast<qsizetype>(i) * width;
                indices[i] = width == 2 ? qFromBigEndian<std::uint16_t>(at) : qFromBigEndian<std::uint32_t>(at);
            }
            appendTriangles(
                indices, width == 2 ? kStripRestart16 : kStripRestart32, vertexCount, r, material.triangles);
            r.skip(bytes);
            r.u32();
            r.expect(1, "material end");
            part.materials.push_back(std::move(material));
        }
        mesh.parts.push_back(std::move(part));
    }
    readMaterialTable(r, mesh);
    return mesh;
}

std::optional<RenderMeshHeader> readHeader(const QByteArray& prefix)
{
    try {
        Reader r(prefix, QString());
        r.expect(kVersion, "version");
        RenderMeshHeader header;
        const float minX = r.f32();
        const float minY = r.f32();
        const float minZ = r.f32();
        r.skip(4);
        const float maxX = r.f32();
        const float maxY = r.f32();
        const float maxZ = r.f32();
        header.boundsMin = QVector3D(minX, minY, -maxZ);
        header.boundsMax = QVector3D(maxX, maxY, -minZ);
        r.seek(kPartCountOffset);
        header.partCount = r.u32();
        r.seek(kFirstPartOffset);
        header.firstPartName = r.name();
        return header;
    } catch (const LoadError&) {
        return std::nullopt;
    }
}

int lodLevel(const QString& partName)
{
    static const QRegularExpression pattern(
        QStringLiteral("(?:^|_)LOD(\\d+)"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(partName);
    return match.hasMatch() ? match.captured(1).toInt() : -1;
}

QString lodGroupKey(const QString& partName)
{
    static const QRegularExpression lodToken(QStringLiteral("_?LOD\\d+"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression chunkNumbers(QStringLiteral("(_\\d*)+$"));
    QString key = partName;
    key.remove(lodToken);
    key.remove(chunkNumbers);
    return key.toLower();
}

} // namespace rendermesh
} // namespace fh1
