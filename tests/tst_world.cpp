#include "EntityRenderer.h"
#include "ForzaZip.h"
#include "Loaders.h"
#include "ModelPreview.h"
#include "RenderMesh.h"
#include "ScatterSet.h"
#include "TrackPlacements.h"
#include "TrackTextures.h"
#include "WorldDebugPanel.h"
#include "WorldIndex.h"
#include "WorldRenderer.h"
#include "WorldTiles.h"
#include "XboxTexture.h"
#include "ZoneGrid.h"

#include <QDataStream>
#include <QFile>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QPainter>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <utility>
#include <zlib.h>

namespace {

void be32(QByteArray& out, quint32 v)
{
    char b[4];
    qToBigEndian(v, b);
    out.append(b, 4);
}

void be16(QByteArray& out, quint16 v)
{
    char b[2];
    qToBigEndian(v, b);
    out.append(b, 2);
}

void beFloat(QByteArray& out, float v)
{
    quint32 bits = 0;
    std::memcpy(&bits, &v, 4);
    be32(out, bits);
}

void le16(QByteArray& out, quint16 v)
{
    char b[2];
    qToLittleEndian(v, b);
    out.append(b, 2);
}

void le32(QByteArray& out, quint32 v)
{
    char b[4];
    qToLittleEndian(v, b);
    out.append(b, 4);
}

struct TestMaterial {
    QList<quint32> indices;
    quint32 width = 2;
    quint32 tableIndex = 0;
    /// Texture coordinate offset u, v and scale u, v.
    std::array<float, 4> uvOffsetScale{0.0F, 0.0F, 1.0F, 1.0F};
    QString name = QStringLiteral("Mat0");
};

struct TestPart {
    QString name;
    /// Positions in file coordinates (Z not yet flipped).
    QList<QVector3D> positions;
    /// Raw 16-bit texture coordinates written at byte 16 of each vertex,
    /// when the stride has room for them.
    QList<QPair<quint16, quint16>> texcoords;
    QList<TestMaterial> materials;
    quint32 stride = 16;
};

/// An entry of the material table after the parts.
struct TestMaterialInfo {
    quint32 shader = 0;
    /// -1 is written as an engine-supplied slot.
    QList<int> textureSlots;
};

/// The material table and shader list after the parts; without one the
/// model ends in bytes the parser cannot read as a table.
struct TestTail {
    QList<TestMaterialInfo> materials;
    QStringList shaders;
};

/// Builds a render model file in the layout RenderMesh documents.
QByteArray renderModel(const QList<TestPart>& parts, const std::optional<TestTail>& tail = std::nullopt)
{
    QVector3D lo(1e9F, 1e9F, 1e9F);
    QVector3D hi(-1e9F, -1e9F, -1e9F);
    for (const TestPart& part : parts) {
        for (const QVector3D& p : part.positions) {
            lo = QVector3D(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()), std::min(lo.z(), p.z()));
            hi = QVector3D(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()), std::max(hi.z(), p.z()));
        }
    }
    QByteArray d;
    be32(d, 6);
    beFloat(d, lo.x());
    beFloat(d, lo.y());
    beFloat(d, lo.z());
    be32(d, 0);
    beFloat(d, hi.x());
    beFloat(d, hi.y());
    beFloat(d, hi.z());
    be32(d, 0);
    for (int i = 0; i < 16; ++i) {
        beFloat(d, i % 5 == 0 ? 1.0F : 0.0F);
    }
    be32(d, 4);
    be32(d, 4);
    be32(d, 0);
    be32(d, 1);
    be32(d, static_cast<quint32>(parts.size()));
    while (d.size() < 0xB0) {
        d.append('\0');
    }
    for (int p = 0; p < parts.size(); ++p) {
        const TestPart& part = parts[p];
        if (p > 0) {
            d.append(QByteArray(48, '\0'));
            be32(d, 1);
        }
        const QByteArray name = part.name.toLatin1();
        be32(d, static_cast<quint32>(name.size()));
        d.append(name);
        be32(d, 3);
        be32(d, static_cast<quint32>(part.positions.size()));
        be32(d, part.stride);
        be32(d, 0);
        for (int v = 0; v < part.positions.size(); ++v) {
            const QVector3D& p = part.positions[v];
            beFloat(d, p.x());
            beFloat(d, p.y());
            beFloat(d, p.z());
            QByteArray rest(static_cast<qsizetype>(part.stride) - 12, '\x7f');
            if (v < part.texcoords.size() && part.stride >= 20) {
                qToBigEndian(part.texcoords[v].first, rest.data() + 4);
                qToBigEndian(part.texcoords[v].second, rest.data() + 6);
            }
            d.append(rest);
        }
        be32(d, 1);
        be32(d, static_cast<quint32>(part.materials.size()));
        be32(d, 1);
        for (const TestMaterial& material : part.materials) {
            const QByteArray name = material.name.toLatin1();
            be32(d, 2);
            be32(d, static_cast<quint32>(name.size()));
            d.append(name);
            be32(d, 0);
            be32(d, 6);
            be32(d, material.tableIndex);
            be32(d, 1);
            d.append(QByteArray(32, '\0'));
            for (float f : material.uvOffsetScale) {
                beFloat(d, f);
            }
            be32(d, 4);
            be32(d, 0);
            be32(d, static_cast<quint32>(material.indices.size()));
            be32(d, material.width);
            for (quint32 index : material.indices) {
                if (material.width == 2) {
                    be16(d, static_cast<quint16>(index));
                } else {
                    be32(d, index);
                }
            }
            be32(d, 5);
            be32(d, 1);
        }
    }
    if (!tail) {
        d.append("trailing shader data");
        return d;
    }
    be32(d, 1);
    be32(d, 1);
    be32(d, 1);
    be32(d, static_cast<quint32>(tail->materials.size()));
    for (const TestMaterialInfo& info : tail->materials) {
        be32(d, 3);
        be32(d, info.shader);
        be32(d, 0);
        be32(d, 1);
        be32(d, 0);
        be32(d, 1);
        be32(d, 1);
        d.append(QByteArray(16, '\0'));
        be32(d, 1);
        be32(d, static_cast<quint32>(info.textureSlots.size()));
        for (int slot : info.textureSlots) {
            be32(d, slot < 0 ? 0xFFFFFFFEu : static_cast<quint32>(slot));
        }
    }
    be32(d, 1);
    be32(d, static_cast<quint32>(tail->shaders.size()));
    for (const QString& shader : tail->shaders) {
        const QByteArray path = shader.toLatin1() + '\0';
        be32(d, static_cast<quint32>(path.size()));
        d.append(path);
    }
    d.append("compiled shader code");
    return d;
}

/// A compiled shader whose vertex inputs are `inputs` (see readShaderLayout).
QByteArray shaderObject(const QList<quint32>& inputs)
{
    QByteArray d("constant table", 14);
    d.append("vs_3_0", 7);
    d.append("2.0.21119.0", 12);
    while (d.size() % 4 != 0) {
        d.append('\0');
    }
    d.append(QByteArray(20, '\0'));
    be32(d, 0x290);
    be32(d, 0x00100000U | static_cast<quint32>(inputs.size() + 1));
    for (quint32 input : inputs) {
        be32(d, input);
    }
    be32(d, 0x7030); // the first output ends the inputs
    return d;
}

/// A PVS file whose texture records hold `textureIds` and whose objects list
/// record numbers.
QByteArray pvsFile(const QList<quint32>& textureIds, const QList<QList<quint32>>& objects,
    const QList<quint16>& drawObjects = {0x5555})
{
    QByteArray d("FPVS");
    be32(d, 0x32);
    while (d.size() < 0x20) {
        d.append('\0');
    }
    be32(d, 2);
    be32(d, 0x1234);
    be32(d, 0x5678);
    // The real file has seven unexplained bytes here.
    d.append(QByteArray::fromHex("00003200000000"));
    be32(d, static_cast<quint32>(textureIds.size()));
    for (int i = 0; i < textureIds.size(); ++i) {
        be32(d, textureIds[i]);
        be32(d, static_cast<quint32>(i));
        beFloat(d, 1.0F);
        beFloat(d, 1.0F);
        be32(d, 0);
        be32(d, 0);
        be32(d, 0x104);
    }
    be32(d, 1);
    be32(d, 8);
    d.append("h_diff_1");
    be32(d, static_cast<quint32>(drawObjects.size()));
    for (quint16 object : drawObjects) {
        be16(d, object);
        d.append(QByteArray(16, '\x55'));
    }
    be32(d, static_cast<quint32>(objects.size()));
    for (const QList<quint32>& records : objects) {
        be32(d, static_cast<quint32>(records.size()));
        for (quint32 record : records) {
            be32(d, record);
        }
        be32(d, 1);
        be32(d, 0);
        for (int i = 0; i < 15; ++i) {
            beFloat(d, 0.5F);
        }
    }
    d.append("more tables");
    return d;
}

/// IEEE 754 half-precision bits of `value`, for the values tests use (whole
/// numbers and simple fractions, which half floats hold exactly).
quint16 halfBits(float value)
{
    if (value == 0.0F) {
        return 0;
    }
    const quint16 sign = value < 0.0F ? 0x8000 : 0;
    int exponent = 0;
    const float fraction = std::frexp(std::abs(value), &exponent); // value = fraction * 2^exponent
    const auto mantissa = static_cast<quint16>(std::lround((fraction * 2.0F - 1.0F) * 1024.0F));
    return static_cast<quint16>(sign | ((exponent - 1 + 15) << 10) | mantissa);
}

struct TestPlacement {
    quint16 draw = 0;
    QVector3D position;
    std::array<float, 9> rows{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<float, 3> ranges{-1, -1, -1};
    /// The id of the record's extra block; 0xFFFFFFFF marks an event prop.
    quint32 blockId = 0xA8;
};

/// A zone file in the layout TrackPlacements documents, with a few entries
/// in the sections the reader skips.
QByteArray zoneFile(const QList<TestPlacement>& placements)
{
    QByteArray d;
    be32(d, static_cast<quint32>(placements.size()));
    for (const TestPlacement& p : placements) {
        be32(d, 0x80000000U | p.draw);
    }
    be32(d, 2);
    be16(d, 7);
    be16(d, 8);
    be32(d, 1);
    d.append(QByteArray(16, '\x11'));
    be32(d, 1);
    be32(d, 0x22222222);
    be32(d, 1);
    d.append('\x33');
    be32(d, 1);
    be16(d, 9);
    be32(d, 1);
    be32(d, 0x44444444);
    be32(d, 1);
    d.append('\x55');
    be32(d, 0);
    be32(d, static_cast<quint32>(placements.size()));
    d.append(QByteArray(placements.size(), '\x66'));
    be32(d, static_cast<quint32>(placements.size()));
    for (const TestPlacement& p : placements) {
        for (float range : p.ranges) {
            be16(d, halfBits(range));
        }
        beFloat(d, p.position.x());
        beFloat(d, p.position.y());
        beFloat(d, p.position.z());
        for (float value : p.rows) {
            be16(d, halfBits(value));
        }
        d.append(QByteArray(16, '\0'));
        d.append('\x01'); // one extra block
        be32(d, p.blockId);
        d.append('\x02');
        d.append("xy", 2);
        d.append(QByteArray(32, '\x77'));
    }
    return d;
}

struct TestScatterMesh {
    /// Draw record per level of detail; empty for a mesh named by `path`.
    QList<quint32> draws;
    QByteArray path;
};

struct TestScatterInstance {
    QVector3D position;
    /// X, Y and Z axes, scaled, as the set stores them.
    std::array<QVector3D, 3> axes{QVector3D(1, 0, 0), QVector3D(0, 1, 0), QVector3D(0, 0, 1)};
};

struct TestScatterGroup {
    quint32 mesh = 0;
    QList<TestScatterInstance> instances;
    float drawDistance = 220.0F;
    /// First and second switch distance of each block; {0, 0} for a block
    /// that switches nothing.
    QList<QPair<float, float>> blocks;
};

/// A procedural placement set in the layout ScatterSet documents.
QByteArray scatterFile(const QByteArray& name, const QList<TestScatterMesh>& meshes,
    const QList<TestScatterGroup>& groups, quint32 extraEntries = 2)
{
    QByteArray d("OEGP");
    be32(d, 42);
    d.append(QByteArray(0x30 - d.size(), '\0'));
    be32(d, 7);
    beFloat(d, -1.0F);
    beFloat(d, 2500.0F);
    be32(d, 22);
    be32(d, extraEntries);
    be32(d, 0);
    be32(d, 0); // file size, patched below
    be32(d, 0);
    be32(d, 0);
    be32(d, static_cast<quint32>(meshes.size()));
    d.append(QByteArray(8, '\0'));
    d.append(name.leftJustified(0x1C, '\0', true));
    be32(d, 0);
    for (const TestScatterMesh& mesh : meshes) {
        for (float v : {-1.0F, 0.0F, -1.0F, 0.0F, 1.0F, 5.0F, 1.0F, 0.0F}) {
            beFloat(d, v);
        }
        be32(d, static_cast<quint32>(mesh.path.size()));
        for (int slot = 0; slot < 3; ++slot) {
            be32(d, slot < mesh.draws.size() ? 1 : 0);
            be32(d, 0x376AD290);
        }
        be32(d, 0x14756A8);
    }
    for (const TestScatterMesh& mesh : meshes) {
        for (quint32 draw : mesh.draws) {
            be32(d, draw);
        }
    }
    for (const TestScatterMesh& mesh : meshes) {
        d.append(mesh.path);
    }
    while (d.size() % 4 != 0) {
        d.append('\0');
    }
    for (quint32 e = 0; e < extraEntries; ++e) {
        be32(d, 0x4C80 + e);
        be32(d, 0);
    }
    be32(d, 0x376AD608);
    beFloat(d, 220.0F);
    be32(d, 0);
    be32(d, static_cast<quint32>(groups.size()));
    be32(d, 0);
    for (const TestScatterGroup& group : groups) {
        be32(d, 0);
        be32(d, 0xFFFFFFFF);
        for (float v : {0.5F, 1.0F, 0.5F, 0.0F, 6.0F, 6.77F, 1.0F}) {
            beFloat(d, v);
        }
        be32(d, 0);
        be32(d, 1);
        be32(d, group.mesh);
        be32(d, static_cast<quint32>(group.instances.size()));
        be32(d, 3);
        be32(d, static_cast<quint32>(group.blocks.size()));
        for (float v : {group.drawDistance, 150.0F, 300.0F, group.drawDistance}) {
            beFloat(d, v);
        }
        d.append(QByteArray(16, '\0'));
    }
    while (d.size() % 16 != 0) {
        d.append('\0');
    }
    for (const TestScatterGroup& group : groups) {
        for (const TestScatterInstance& instance : group.instances) {
            for (const QVector3D& axis : instance.axes) {
                beFloat(d, axis.x());
                beFloat(d, axis.y());
                beFloat(d, axis.z());
                beFloat(d, 0.0F);
            }
            for (float v : {instance.position.x(), instance.position.y(), instance.position.z(), 1.0F, 0.5F, 0.5F, 0.5F,
                     0.0F, 0.0F, 1.0F, 0.0F, 0.0F}) {
                beFloat(d, v);
            }
        }
        for (const auto& [first, second] : group.blocks) {
            for (float v : {first, second, first > 0.0F ? group.drawDistance : 0.0F, 150.0F, 300.0F, 2500.0F,
                     first > 0.0F ? group.drawDistance : 0.0F, first > 0.0F ? 15.0F : 0.0F}) {
                beFloat(d, v);
            }
        }
    }
    d.append(QByteArray(32, '\x5A')); // data after the groups the reader skips
    qToBigEndian(static_cast<quint32>(d.size()), d.data() + 0x48);
    return d;
}

/// A zone grid file in the layout ZoneGrid documents, with the origin at
/// 0, 0 and cells of radius 100.
QByteArray hexFile(quint32 zones, quint32 columns, quint32 rows, const QList<quint32>& cells)
{
    QByteArray d("HEXY");
    be32(d, 101);
    beFloat(d, 100.0F);
    beFloat(d, 0.0F);
    beFloat(d, 0.0F);
    be32(d, zones);
    be32(d, columns);
    be32(d, rows);
    for (quint32 cell : cells) {
        be32(d, cell);
    }
    d.append(QByteArray(static_cast<qsizetype>(zones) * 9, '\x01'));
    return d;
}

/// A 4x4-texel block of solid `colour`, as DXT1.
QByteArray solidDxt1Block(QRgb colour)
{
    std::array<fh1::dxt::Rgba, 16> texels{};
    for (auto& t : texels) {
        t = {static_cast<std::uint8_t>(qRed(colour)), static_cast<std::uint8_t>(qGreen(colour)),
            static_cast<std::uint8_t>(qBlue(colour)), 255};
    }
    QByteArray block(8, '\0');
    fh1::dxt::encodeDxt1(texels.data(), reinterpret_cast<std::uint8_t*>(block.data()));
    return block;
}

/// DXT1 texture data in the Xbox 360 layout: tiled, 8-in-16 byte swapped, the
/// top level of a small texture placed in the packed mip tail. `colourAt`
/// gives each 4x4 block's colour.
QByteArray tiledDxt1(int width, int height, const std::function<QRgb(int bx, int by)>& colourAt)
{
    const int blocksWide = std::max(1, width / 4);
    const int blocksHigh = std::max(1, height / 4);
    const auto pitch = static_cast<quint32>((blocksWide + 31) & ~31);
    const QPoint start = fh1::xenos::packedBaseOffset(width, height, 4);
    QByteArray tiled;
    for (int by = 0; by < blocksHigh; ++by) {
        for (int bx = 0; bx < blocksWide; ++bx) {
            const auto offset = static_cast<qsizetype>(fh1::xenos::tiledOffset(
                static_cast<quint32>(bx + start.x()), static_cast<quint32>(by + start.y()), pitch, 3));
            if (tiled.size() < offset + 8) {
                tiled.resize(offset + 8, '\0');
            }
            const QByteArray block = solidDxt1Block(colourAt(bx, by));
            for (int i = 0; i < 8; i += 2) {
                tiled[offset + i] = block[i + 1];
                tiled[offset + i + 1] = block[i];
            }
        }
    }
    return tiled;
}

/// True when two colours differ by no more than DXT's 5:6:5 rounding.
bool closeColour(QRgb a, QRgb b)
{
    return std::abs(qRed(a) - qRed(b)) <= 8 && std::abs(qGreen(a) - qGreen(b)) <= 4
        && std::abs(qBlue(a) - qBlue(b)) <= 8;
}

/// DXT1 with the 8-in-16 byte order, as on the disc.
constexpr quint32 kDxt1FormatWord = 0x1A200052;

/// A BIX1 texture of solid `colour` as its `.bix` header file and its tiled
/// `_B.bix` top level.
QPair<QByteArray, QByteArray> bixTexture(int size, QRgb colour)
{
    const QByteArray top = tiledDxt1(size, size, [colour](int, int) { return colour; });
    QByteArray header("BIX1");
    be32(header, static_cast<quint32>(size));
    be32(header, static_cast<quint32>(size));
    be32(header, 1);
    be32(header, kDxt1FormatWord);
    be32(header, static_cast<quint32>(top.size()));
    be32(header, static_cast<quint32>(top.size()));
    return {header, top};
}

struct TestBundleTexture {
    quint32 record = 0;
    int size = 8;
    std::function<QRgb(int bx, int by)> colourAt;
};

/// A bundle pack in the layout TrackTextures documents.
QByteArray bundlePack(const QList<TestBundleTexture>& textures)
{
    QByteArray d;
    be32(d, static_cast<quint32>(textures.size()));
    for (const TestBundleTexture& texture : textures) {
        const QByteArray data = tiledDxt1(texture.size, texture.size, texture.colourAt);
        be32(d, texture.record);
        be32(d, static_cast<quint32>(texture.size));
        be32(d, static_cast<quint32>(texture.size));
        be32(d, 3);
        be32(d, kDxt1FormatWord);
        be32(d, 0xFFFFFFFF);
        be32(d, static_cast<quint32>(data.size()));
        d.append(data);
    }
    return d;
}

/// A flat 2x2 quad at height `y`, centred on (cx, cz) in world coordinates.
TestPart quad(const QString& name, float cx, float cz, float y, float half)
{
    TestPart part;
    part.name = name;
    // File Z is world Z negated.
    part.positions = {{cx - half, y, -(cz - half)}, {cx + half, y, -(cz - half)}, {cx - half, y, -(cz + half)},
        {cx + half, y, -(cz + half)}};
    part.materials = {{{0, 1, 2, 3}, 2}};
    return part;
}

/// Writes a zip with stored entries in Forza's layout.
QString writeZip(const QTemporaryDir& dir, const QList<QPair<QString, QByteArray>>& files)
{
    QByteArray file;
    QByteArray directory;
    for (const auto& [name, content] : files) {
        const auto offset = static_cast<quint32>(file.size());
        file.append(content);
        const auto crc = static_cast<quint32>(
            crc32(0L, reinterpret_cast<const Bytef*>(content.constData()), static_cast<uInt>(content.size())));
        const QByteArray n = name.toUtf8();
        le32(directory, 0x02014b50);
        le16(directory, 20);
        le16(directory, 20);
        le16(directory, 0);
        le16(directory, 0);
        le32(directory, 0);
        le32(directory, crc);
        le32(directory, static_cast<quint32>(content.size()));
        le32(directory, static_cast<quint32>(content.size()));
        le16(directory, static_cast<quint16>(n.size()));
        le16(directory, 8);
        le16(directory, 0);
        le16(directory, 0);
        le16(directory, 0);
        le32(directory, 0);
        le32(directory, offset);
        directory.append(n);
        le16(directory, 0x1123);
        le16(directory, 4);
        le32(directory, offset);
    }
    const auto directoryOffset = static_cast<quint32>(file.size());
    file.append(directory);
    le32(file, 0x06054b50);
    le16(file, 0);
    le16(file, 0);
    le16(file, static_cast<quint16>(files.size()));
    le16(file, static_cast<quint16>(files.size()));
    le32(file, static_cast<quint32>(directory.size()));
    le32(file, directoryOffset);
    le16(file, 0);
    const QString path = dir.filePath(QStringLiteral("bin.zip"));
    QFile out(path);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(file);
    }
    return path;
}

} // namespace

class TestWorld : public QObject {
    Q_OBJECT

private slots:
    void parsesStripsListsAndParts()
    {
        TestPart strip = quad(QStringLiteral("Road_LOD01_02"), 100.0F, 200.0F, 5.0F, 10.0F);
        strip.materials[0].indices = {0, 1, 2, 3, 0xFFFF, 0, 1, 2};
        TestPart list;
        list.name = QStringLiteral("Road_LOD01_02");
        list.stride = 36;
        list.positions = {{0, 0, 0}, {1, 0, 0}, {0, 0, -1}, {1, 0, -1}, {2, 0, 0}, {2, 0, -1}};
        list.materials = {{{0, 1, 2, 1, 3, 2}, 4}, {{1, 4, 3, 4, 5, 3}, 2}};
        const QByteArray data = renderModel({strip, list});

        const fh1::RenderMesh mesh = fh1::rendermesh::parse(data, QStringLiteral("test"));
        QCOMPARE(mesh.parts.size(), std::size_t{2});
        // Strip: 0-1-2, 1-2-3 (winding flipped), restart, 0-1-2 again.
        QCOMPARE(mesh.parts[0].materials[0].triangles.size(), std::size_t{9});
        QCOMPARE(mesh.parts[0].materials[0].triangles[3], 2u);
        QCOMPARE(mesh.parts[0].materials[0].triangles[4], 1u);
        QCOMPARE(mesh.parts[1].stride, 36u);
        QCOMPARE(mesh.parts[1].materials.size(), std::size_t{2});
        QCOMPARE(mesh.parts[1].materials[0].triangles.size(), std::size_t{6});
        QCOMPARE(mesh.triangleCount(), std::size_t{7});
        // Z is flipped into world coordinates.
        QCOMPARE(mesh.parts[0].positions[0], QVector3D(90.0F, 5.0F, 190.0F));
        QVERIFY(mesh.boundsMin.z() <= 190.0F && mesh.boundsMax.z() >= 210.0F);

        const auto header = fh1::rendermesh::readHeader(data.left(fh1::rendermesh::kHeaderBytes));
        QVERIFY(header.has_value());
        QCOMPARE(header->partCount, 2u);
        QCOMPARE(header->firstPartName, QStringLiteral("Road_LOD01_02"));
        QCOMPARE(header->boundsMin, mesh.boundsMin);
        // The tail is not a material table; the geometry is still read.
        QVERIFY(mesh.materialTable.empty());
        QVERIFY(mesh.shaders.isEmpty());
    }

    void parsesMaterialTableAndTexcoords()
    {
        TestPart part = quad(QStringLiteral("Sign_LOD00"), 0, 0, 0, 1);
        part.stride = 24;
        part.texcoords = {{0, 0}, {65535, 0}, {0, 65535}, {32768, 16384}};
        part.materials[0].tableIndex = 1;
        part.materials[0].uvOffsetScale = {0.5F, -0.25F, 2.0F, 4.0F};
        const TestTail tail{{{0, {-1}}, {1, {3, -1, -1, -1, -1, 0}}},
            {QStringLiteral("shaders\\track\\add_diff_opac_rgba.fx"), QStringLiteral("shaders\\track\\h_diff_1.fx")}};
        const fh1::RenderMesh mesh = fh1::rendermesh::parse(renderModel({part}, tail), QStringLiteral("test"));

        QCOMPARE(mesh.materialTable.size(), std::size_t{2});
        QCOMPARE(mesh.materialTable[1].shader, 1u);
        QCOMPARE(mesh.materialTable[1].textureSlots, (std::vector<int>{3, -1, -1, -1, -1, 0}));
        QCOMPARE(mesh.shaders.size(), 2);
        QCOMPARE(mesh.shaders[1], QStringLiteral("shaders\\track\\h_diff_1.fx"));

        const fh1::RenderMesh::Part& parsed = mesh.parts[0];
        const fh1::RenderMesh::Material& material = parsed.materials[0];
        QCOMPARE(material.tableIndex, 1u);
        QCOMPARE(parsed.vertexData.size(), qsizetype{96}); // 4 vertices of 24 bytes
        QCOMPARE(fh1::RenderMesh::texcoord(parsed, material, 0, 16), QVector2D(0.5F, -0.25F));
        QCOMPARE(fh1::RenderMesh::texcoord(parsed, material, 1, 16), QVector2D(2.5F, -0.25F));
        QCOMPARE(fh1::RenderMesh::texcoord(parsed, material, 2, 16), QVector2D(0.5F, 3.75F));
        const QVector2D mid = fh1::RenderMesh::texcoord(parsed, material, 3, 16);
        QVERIFY(std::abs(mid.x() - 1.5F) < 1e-3F && std::abs(mid.y() - 0.75F) < 1e-3F);
        // A pair that would reach past the vertex is not read.
        QCOMPARE(fh1::RenderMesh::texcoord(parsed, material, 0, 22), QVector2D());
    }

    void readsShaderLayoutsAndPvsTables()
    {
        // Normal, TEXCOORD0, TEXCOORD1 (flagged), as in h_diff_spec_ao_2.
        const std::optional<fh1::ShaderLayout> diffuse
            = fh1::readShaderLayout(shaderObject({0x3007, 0x5008, 0x215009}));
        QVERIFY(diffuse.has_value());
        QCOMPARE(diffuse->texcoord0Offset, 16);
        QCOMPARE(diffuse->vertexBytes, 24);
        // TEXCOORD0 first, as in the animated flag shader.
        const std::optional<fh1::ShaderLayout> flag = fh1::readShaderLayout(shaderObject({0x5012, 0xA013}));
        QVERIFY(flag.has_value());
        QCOMPARE(flag->texcoord0Offset, 12);
        const std::optional<fh1::ShaderLayout> none = fh1::readShaderLayout(shaderObject({0x3007, 0xA008}));
        QVERIFY(none.has_value());
        QCOMPARE(none->texcoord0Offset, -1);
        QVERIFY(!fh1::readShaderLayout(QByteArray("no shader here")).has_value());

        const QByteArray pvs = pvsFile({0x10, 0x20, 0x2B40}, {{2, 0}, {}, {1}});
        QString error;
        const auto objects = fh1::TrackTextures::readObjectTextures(pvs, &error);
        QVERIFY2(objects.has_value(), qPrintable(error));
        QCOMPARE(objects->size(), std::size_t{3});
        QCOMPARE((*objects)[0], (std::vector<std::uint32_t>{0x2B40, 0x10}));
        QVERIFY((*objects)[1].empty());
        QCOMPARE((*objects)[2], (std::vector<std::uint32_t>{0x20}));

        QVERIFY(!fh1::TrackTextures::readObjectTextures(pvsFile({0x10}, {{5}}), &error).has_value());
        QVERIFY(error.contains(QStringLiteral("record 5")));
        QVERIFY(!fh1::TrackTextures::readObjectTextures(QByteArray("FPVS but nothing else"), &error).has_value());

        QCOMPARE(fh1::TrackTextures::objectNumber(QStringLiteral("coloradoout.06454.rmb.bin")), 6454u);
        QVERIFY(!fh1::TrackTextures::objectNumber(QStringLiteral("_0x00002B40.bix")).has_value());
    }

    void placesSmallTexturesInThePackedMipTail()
    {
        QCOMPARE(fh1::xenos::packedBaseOffset(16, 16, 4), QPoint(4, 0));
        QCOMPARE(fh1::xenos::packedBaseOffset(4, 4, 4), QPoint(4, 0));
        QCOMPARE(fh1::xenos::packedBaseOffset(8, 16, 4), QPoint(4, 0));
        QCOMPARE(fh1::xenos::packedBaseOffset(16, 8, 4), QPoint(0, 4));
        QCOMPARE(fh1::xenos::packedBaseOffset(256, 16, 4), QPoint(0, 4));
        QCOMPARE(fh1::xenos::packedBaseOffset(8, 256, 4), QPoint(4, 0));
        QCOMPARE(fh1::xenos::packedBaseOffset(16, 16, 1), QPoint(16, 0));
        QCOMPARE(fh1::xenos::packedBaseOffset(32, 32, 4), QPoint(0, 0));
        QCOMPARE(fh1::xenos::packedBaseOffset(512, 32, 4), QPoint(0, 0));

        // A 16x8 texture sits below the tail's first 16 texel rows.
        const auto colourAt = [](int bx, int by) { return qRgb(bx * 60, by * 120, 30); };
        const QByteArray tiled = tiledDxt1(16, 8, colourAt);
        QString error;
        const std::optional<fh1::TextureSurface> packed
            = fh1::untileXboxSurface(tiled, kDxt1FormatWord, 16, 8, fh1::MipLayout::Packed, &error);
        QVERIFY2(packed.has_value(), qPrintable(error));
        const QImage image = fh1::surfaceToImage(*packed);
        QVERIFY(closeColour(image.pixel(13, 5), colourAt(3, 1)));
        QVERIFY(closeColour(image.pixel(2, 2), colourAt(0, 0)));
        const std::optional<fh1::TextureSurface> unpacked
            = fh1::untileXboxSurface(tiled, kDxt1FormatWord, 16, 8, fh1::MipLayout::Unpacked, &error);
        QVERIFY(unpacked.has_value());
        QVERIFY(!closeColour(fh1::surfaceToImage(*unpacked).pixel(13, 5), colourAt(3, 1)));

        QVERIFY(!fh1::TrackTextures::readBundle(QByteArray::fromHex("00000001"), &error).has_value());
        QByteArray badSeparator = bundlePack({{0, 4, [](int, int) { return qRgb(1, 2, 3); }}});
        badSeparator[4 + 20] = 0;
        QVERIFY(!fh1::TrackTextures::readBundle(badSeparator, &error).has_value());
        const auto entries = fh1::TrackTextures::readBundle(bundlePack({{7, 4, [](int, int) { return 0u; }}}), &error);
        QVERIFY2(entries.has_value(), qPrintable(error));
        QCOMPARE(entries->size(), std::size_t{1});
        QCOMPARE(entries->front().record, 7u);
        QCOMPARE(entries->front().offset, qsizetype{32});
    }

    void readsZonePlacements()
    {
        const TestPlacement first{
            3, QVector3D(-1670.5F, 12.5F, -1207.75F), {0.5F, 0, 1, 0, 1, 0, 1, 0, -0.5F}, {150, 300, 500}};
        const TestPlacement second{7, QVector3D(10, 20, 30), {1, 0, 0, 0, 1, 0, 0, 0, 1}, {-1, -1, -1}, 0xFFFFFFFF};
        QString error;
        const auto placements = fh1::TrackPlacements::readZone(zoneFile({first, second}), &error);
        QVERIFY2(placements.has_value(), qPrintable(error));
        QCOMPARE(placements->size(), std::size_t{2});
        QCOMPARE((*placements)[0].first, 3u);
        QCOMPARE((*placements)[1].first, 7u);
        const fh1::Placement& p = (*placements)[0].second;
        QCOMPARE(p.position, first.position);
        QCOMPARE(p.rows, first.rows);
        QCOMPARE(p.ranges, first.ranges);
        QCOMPARE((*placements)[1].second.ranges, (std::array<float, 3>{-1, -1, -1}));
        QVERIFY(!p.eventProp);
        QVERIFY((*placements)[1].second.eventProp);

        QByteArray truncated = zoneFile({first, second});
        truncated.chop(10);
        QVERIFY(!fh1::TrackPlacements::readZone(truncated, &error).has_value());
        QByteArray mismatch = zoneFile({first});
        mismatch[3] = 2; // two draws, one transform record
        QVERIFY(!fh1::TrackPlacements::readZone(mismatch, &error).has_value());
    }

    void appliesPlacements()
    {
        // World-space models are placed by an identity with Z mirrored,
        // which reproduces the parser's own coordinates.
        fh1::Placement asStored;
        asStored.rows = {1, 0, 0, 0, 1, 0, 0, 0, -1};
        QCOMPARE(asStored.apply(QVector3D(3, 4, 5)), QVector3D(3, 4, 5));

        // A marker pole from CollObjs.xml: its XAxis (0.164, 0, 0.986) is
        // row 0 and its ZAxis (-0.986, 0, 0.164), negated, row 2.
        fh1::Placement pole;
        pole.position = QVector3D(-1670.525F, 12.5432F, -1207.935F);
        pole.rows = {0.164F, 0, 0.986F, 0, 1, 0, 0.986F, 0, -0.164F};
        // Differences of large coordinates keep about three decimals.
        const QVector3D alongX = pole.apply(QVector3D(1, 0, 0)) - pole.position;
        QVERIFY((alongX - QVector3D(0.164F, 0, 0.986F)).length() < 1e-3F);
        // Row 2 is the negated Z axis, and the parser negated the file's Z,
        // so the parsed model's Z maps to the XML's ZAxis.
        const QVector3D alongZ = pole.apply(QVector3D(0, 0, 1)) - pole.position;
        QVERIFY((alongZ - QVector3D(-0.986F, 0, 0.164F)).length() < 1e-3F);
        QCOMPARE(pole.apply(QVector3D(0, 2, 0)), pole.position + QVector3D(0, 2, 0));
    }

    void readsScatterSets()
    {
        // A tree at two levels, a mesh named by a path of odd length, and a
        // bush at one level whose group gives no switch distances.
        TestScatterInstance turned;
        turned.position = QVector3D(-167.5F, 36.25F, 854.0F);
        turned.axes = {QVector3D(0, 0, -0.5F), QVector3D(0, 0.5F, 0), QVector3D(0.5F, 0, 0)};
        const TestScatterInstance upright{QVector3D(10, 20, 30)};
        const QByteArray data = scatterFile("Models_Ungrouped_1706",
            {{{61908, 61909}, {}}, {{}, "Tracks\\Colorado\\Scene\\Vegetation\\VEG_Tree.max"}, {{61915}, {}}},
            {{0, {turned, upright}, 220.0F, {{0.0F, 0.0F}, {50.0F, 100.0F}}}, {1, {upright}, 220.0F, {{50.0F, 100.0F}}},
                {2, {upright}, 100.0F, {}}});
        QString error;
        const std::optional<fh1::ScatterSet> set = fh1::readScatterSet(data, &error);
        QVERIFY2(set.has_value(), qPrintable(error));
        QCOMPARE(set->name, QStringLiteral("Models_Ungrouped_1706"));
        QCOMPARE(set->meshDraws.size(), std::size_t{3});
        QCOMPARE(set->meshDraws[0], (std::vector<std::uint32_t>{61908, 61909}));
        QVERIFY(set->meshDraws[1].empty());
        QCOMPARE(set->meshDraws[2], (std::vector<std::uint32_t>{61915}));
        // The path-named mesh's instance is left out.
        QCOMPARE(set->instances.size(), std::size_t{3});
        const fh1::ScatterSet::Instance& tree = set->instances[0];
        QCOMPARE(tree.mesh, 0u);
        QCOMPARE(tree.placement.position, turned.position);
        // The stored Z axis, negated, maps the model's stored Z.
        QCOMPARE(tree.placement.rows, (std::array<float, 9>{0, 0, -0.5F, 0, 0.5F, 0, -0.5F, 0, 0}));
        // The block that switches levels gives the distances.
        QCOMPARE(tree.placement.ranges, (std::array<float, 3>{50.0F, 220.0F, -1.0F}));
        QCOMPARE(set->instances[1].placement.position, upright.position);
        // One level, drawn to the group's distance.
        QCOMPARE(set->instances[2].mesh, 2u);
        QCOMPARE(set->instances[2].placement.ranges, (std::array<float, 3>{100.0F, -1.0F, -1.0F}));

        QByteArray wrongSize = data;
        wrongSize[0x4B] = static_cast<char>(wrongSize[0x4B] + 1);
        QVERIFY(!fh1::readScatterSet(wrongSize, &error).has_value());
        QVERIFY(!fh1::readScatterSet(QByteArray("OEGP") + QByteArray(0x100, '\0'), &error).has_value());
        QByteArray truncated = data.left(data.size() - 200);
        qToBigEndian(static_cast<quint32>(truncated.size()), truncated.data() + 0x48);
        QVERIFY(!fh1::readScatterSet(truncated, &error).has_value());
    }

    void placesScatteredModels()
    {
        // A tree at two levels whose template draws are placed at the
        // origin, as the game places them, and copied twice by a set.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray treeNear = renderModel({quad(QStringLiteral("Tree_WhiteFir01_LOD00"), 0, 0, 0, 1)});
        const QByteArray treeFar = renderModel({quad(QStringLiteral("Tree_WhiteFir01_LOD01"), 0, 0, 0, 1)});
        const QByteArray ground = renderModel({quad(QStringLiteral("Terrain_LOD00_01"), 600, 600, 0, 100)});
        const TestPlacement templateNear{1, QVector3D(0, 0, 0), {1, 0, 0, 0, 1, 0, 0, 0, -1}};
        const TestPlacement templateFar{2, QVector3D(0, 0, 0), {1, 0, 0, 0, 1, 0, 0, 0, -1}};
        const TestScatterInstance first{QVector3D(500, 10, 500)};
        const TestScatterInstance second{QVector3D(540, 12, 520)};
        const QString archivePath = writeZip(dir,
            {{QStringLiteral("coloradoout.00000.rmb.bin"), ground},
                {QStringLiteral("coloradoout.00001.rmb.bin"), treeNear},
                {QStringLiteral("coloradoout.00002.rmb.bin"), treeFar},
                {QStringLiteral("__R00Z00000.pvsz"), zoneFile({templateNear, templateFar})},
                {QStringLiteral("__R00G00000.pgeo"),
                    scatterFile(
                        "Models_Ungrouped_1", {{{1, 2}, {}}}, {{0, {first, second}, 220.0F, {{50.0F, 100.0F}}}})},
                // Sets of other kinds are not read as model sets.
                {QStringLiteral("__R00G00001.pgeo"), QByteArray("OEGP") + QByteArray(0x5C, '\0') + "Grass_Ungrouped"},
                {QStringLiteral("__r00g00000.pgeo"),
                    scatterFile(
                        "Models_Ungrouped_1", {{{1, 2}, {}}}, {{0, {first, second}, 220.0F, {{50.0F, 100.0F}}}})}});
        fh1::ForzaZip archive;
        QVERIFY2(archive.open(archivePath), qPrintable(archive.errorString()));
        QString error;
        const std::optional<fh1::TrackPlacements> placements
            = fh1::TrackPlacements::load(pvsFile({0x10}, {{}, {}, {}}, {0, 1, 2}), archive, nullptr, &error);
        QVERIFY2(placements.has_value(), qPrintable(error));
        QCOMPARE(placements->scatterSets().size(), std::size_t{1});
        QCOMPARE(placements->failedScatterSets(), 0);
        QVERIFY(!placements->isScatterTemplate(0));
        QVERIFY(placements->isScatterTemplate(1));
        QVERIFY(placements->isScatterTemplate(2));

        const std::optional<fh1::WorldIndex> index = fh1::WorldIndex::build(archive, {}, nullptr, &*placements);
        QVERIFY(index.has_value());
        // Two copies at two levels each; nothing at the origin.
        QCOMPARE(index->placedCount(), 4);
        QCOMPARE(index->localModelCount(), 0);
        for (const fh1::WorldChunk& chunk : index->chunks()) {
            if (!chunk.placed) {
                continue;
            }
            QVERIFY(chunk.placement.position == first.position || chunk.placement.position == second.position);
            QCOMPARE(chunk.bandStart, chunk.lod == 0 ? 0.0F : 50.0F);
            QCOMPARE(chunk.bandEnd, chunk.lod == 0 ? 50.0F : 220.0F);
        }
    }

    void placesPropsWithDistantPivots()
    {
        // A gondola cabin modelled 200 m from its pivot, which its draw
        // moves to 3000, 150, 3400, and ground whose draw leaves it where
        // its file puts it.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray cabin = renderModel({quad(QStringLiteral("Gondola_LOD00_001"), 200, 200, 30, 2)});
        const QByteArray ground = renderModel({quad(QStringLiteral("Terrain_LOD00_01"), 600, 600, 0, 100)});
        const TestPlacement moved{0, QVector3D(3000, 150, 3400), {1, 0, 0, 0, 1, 0, 0, 0, -1}, {500, 501, 502}};
        const TestPlacement asStored{1, QVector3D(0, 0, 0), {1, 0, 0, 0, 1, 0, 0, 0, -1}, {400, 650, -1}};
        const QString archivePath = writeZip(dir,
            {{QStringLiteral("coloradoout.00000.rmb.bin"), cabin},
                {QStringLiteral("coloradoout.00001.rmb.bin"), ground},
                {QStringLiteral("__R00Z00000.pvsz"), zoneFile({moved, asStored})}});
        fh1::ForzaZip archive;
        QVERIFY2(archive.open(archivePath), qPrintable(archive.errorString()));
        QString error;
        const std::optional<fh1::TrackPlacements> placements
            = fh1::TrackPlacements::load(pvsFile({0x10}, {{}, {}}, {0, 1}), archive, nullptr, &error);
        QVERIFY2(placements.has_value(), qPrintable(error));
        const std::optional<fh1::WorldIndex> index = fh1::WorldIndex::build(archive, {}, nullptr, &*placements);
        QVERIFY(index.has_value());
        QCOMPARE(index->chunks().size(), std::size_t{2});
        QCOMPARE(index->placedCount(), 1);
        for (const fh1::WorldChunk& chunk : index->chunks()) {
            const QVector3D centre = (chunk.boundsMin + chunk.boundsMax) / 2.0F;
            if (chunk.placed) {
                // The cabin, 200 m from the pivot its draw moved.
                QVERIFY((centre - QVector3D(3200, 180, 3600)).length() < 1.0F);
            } else {
                QVERIFY((centre - QVector3D(600, 0, 600)).length() < 1.0F);
            }
        }
    }

    void placesPropsFromZones()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // A sign modelled around its own origin, at two levels of detail, a
        // bench that is never placed, and helper geometry.
        const QByteArray signNear = renderModel({quad(QStringLiteral("Sign_LOD00"), 0, 0, 0, 1)});
        const QByteArray signFar = renderModel({quad(QStringLiteral("Sign_LOD01"), 0, 0, 0, 1)});
        const QByteArray bench = renderModel({quad(QStringLiteral("Bench_LOD00"), 0, 0, 0, 1)});
        const QByteArray ground = renderModel({quad(QStringLiteral("Terrain_LOD00_01"), 600, 600, 0, 100)});
        const QByteArray cube = renderModel({quad(QStringLiteral("TERR_CUBE_MainTown_Area3_12"), 600, 600, 0, 50)});
        const TestTail effectTail{{{0, {-1}}}, {QStringLiteral("shaders\\track\\light_pollution.fx")}};
        const QByteArray glow = renderModel({quad(QStringLiteral("Plane001"), 700, 700, 50, 20)}, effectTail);
        // Draws: 0 = sign LOD00 (in no zone), 1 = sign LOD01, 2 = sign LOD00
        // of a second sign, 3 = sign LOD01 of it. The second sign is only
        // put out for events.
        const QList<quint16> draws{1, 2, 1, 2};
        const std::array<float, 9> quarterTurn{0, 0, -1, 0, 1, 0, -1, 0, 0};
        const TestPlacement a{1, QVector3D(500, 10, 500), quarterTurn, {50, 200, -1}};
        const TestPlacement b{2, QVector3D(520, 12, 510), {1, 0, 0, 0, 1, 0, 0, 0, -1}, {50, 200, -1}, 0xFFFFFFFF};
        const TestPlacement c{3, b.position, b.rows, b.ranges, b.blockId};
        const QString archivePath = writeZip(dir,
            {{QStringLiteral("coloradoout.00000.rmb.bin"), ground},
                {QStringLiteral("coloradoout.00001.rmb.bin"), signNear},
                {QStringLiteral("coloradoout.00002.rmb.bin"), signFar},
                {QStringLiteral("coloradoout.00003.rmb.bin"), bench},
                {QStringLiteral("coloradoout.00004.rmb.bin"), cube},
                {QStringLiteral("coloradoout.00005.rmb.bin"), glow},
                {QStringLiteral("__R00Z00000.pvsz"), zoneFile({a, b})},
                // Zones overlap: the second repeats draw 2 and adds draw 3.
                {QStringLiteral("__R00Z00001.pvsz"), zoneFile({b, c})},
                {QStringLiteral("__r00z00001.pvsz"), zoneFile({b, c})}});
        fh1::ForzaZip archive;
        QVERIFY2(archive.open(archivePath), qPrintable(archive.errorString()));
        QString error;
        const std::optional<fh1::TrackPlacements> placements
            = fh1::TrackPlacements::load(pvsFile({0x10}, {{}, {}, {}, {}, {}, {}}, draws), archive, nullptr, &error);
        QVERIFY2(placements.has_value(), qPrintable(error));
        QCOMPARE(placements->zoneCount(), 2);
        QCOMPARE(placements->failedZones(), 0);
        QCOMPARE(placements->drawCount(), std::size_t{4});
        QVERIFY(placements->placement(0) == nullptr);
        QVERIFY(placements->placement(1) != nullptr);
        QCOMPARE(placements->placement(3)->position, c.position);

        const std::optional<fh1::WorldIndex> index = fh1::WorldIndex::build(archive, {}, nullptr, &*placements);
        QVERIFY(index.has_value());
        // The ground, the effect plane and four sign draws; the cube copy is
        // left out and the bench, never placed, is counted.
        QCOMPARE(index->placedCount(), 4);
        QCOMPARE(index->chunks().size(), std::size_t{6});
        QCOMPARE(index->localModelCount(), 1);
        std::vector<const fh1::WorldChunk*> signs;
        for (const fh1::WorldChunk& chunk : index->chunks()) {
            if (chunk.placed) {
                signs.push_back(&chunk);
            }
        }
        // Draw 0 borrowed the transform of draw 1, the same sign's LOD01.
        QCOMPARE(signs[0]->placement.position, a.position);
        QCOMPARE(signs[0]->lod, std::int8_t{0});
        QCOMPARE(signs[0]->bandStart, 0.0F);
        QCOMPARE(signs[0]->bandEnd, 50.0F);
        QCOMPARE(signs[1]->bandStart, 50.0F);
        QCOMPARE(signs[1]->bandEnd, 200.0F);
        QVERIFY(!signs[0]->placement.eventProp && !signs[1]->placement.eventProp);
        QVERIFY(signs[2]->placement.eventProp && signs[3]->placement.eventProp);
        // A 2 m sign, turned a quarter, around its position.
        QVERIFY(signs[0]->boundsMin.x() >= 498.9F && signs[0]->boundsMax.x() <= 501.1F);
        QVERIFY(signs[0]->boundsMin.z() >= 498.9F && signs[0]->boundsMax.z() <= 501.1F);

        const QString cache = dir.filePath(QStringLiteral("world.index"));
        QVERIFY(index->save(cache, QStringLiteral("sig")));
        const std::optional<fh1::WorldIndex> reloaded = fh1::WorldIndex::load(cache, QStringLiteral("sig"));
        QVERIFY(reloaded.has_value());
        QCOMPARE(reloaded->placedCount(), 4);
        for (std::size_t i = 0; i < index->chunks().size(); ++i) {
            QCOMPARE(reloaded->chunks()[i].placement.position, index->chunks()[i].placement.position);
            QCOMPARE(reloaded->chunks()[i].placement.rows, index->chunks()[i].placement.rows);
            QCOMPARE(reloaded->chunks()[i].bandEnd, index->chunks()[i].bandEnd);
            QCOMPARE(reloaded->chunks()[i].placement.eventProp, index->chunks()[i].placement.eventProp);
        }

        // The tile grid leaves the event sign out unless asked for it.
        const auto gridChunks = [&index](bool eventProps) {
            const fh1::WorldTileGrid grid(*index, 500.0F, eventProps);
            std::size_t count = 0;
            for (const auto& tile : grid.tiles()) {
                count += tile.chunks.size();
            }
            return count;
        };
        QCOMPARE(gridChunks(false), std::size_t{4});
        QCOMPARE(gridChunks(true), std::size_t{6});

        // Tile building places the sign's vertices.
        const auto signIndex = static_cast<std::uint32_t>(signs[0] - index->chunks().data());
        const fh1::TileMesh mesh = fh1::buildTileMesh(archive, *index, {signIndex});
        QCOMPARE(mesh.failedChunks, 0);
        QCOMPARE(mesh.vertexCount(), std::size_t{4});
        for (std::size_t v = 0; v < mesh.vertexCount(); ++v) {
            const float* vertex = mesh.vertices.data() + v * fh1::TileMesh::kFloatsPerVertex;
            QVERIFY((QVector3D(vertex[0], vertex[1], vertex[2]) - a.position).length() < 1.5F);
            // Still facing up after the turn.
            QVERIFY(vertex[4] > 0.99F);
        }
        // The light pollution plane is an effect, not a surface.
        const auto glowChunk = std::find_if(index->chunks().begin(), index->chunks().end(),
            [](const fh1::WorldChunk& chunk) { return !chunk.placed && chunk.boundsMin.y() > 40.0F; });
        QVERIFY(glowChunk != index->chunks().end());
        const fh1::TileMesh glowMesh
            = fh1::buildTileMesh(archive, *index, {static_cast<std::uint32_t>(glowChunk - index->chunks().begin())});
        QCOMPARE(glowMesh.failedChunks, 0);
        QVERIFY(glowMesh.indices.empty());

        // Without placements, props are only counted.
        const std::optional<fh1::WorldIndex> unplaced = fh1::WorldIndex::build(archive);
        QVERIFY(unplaced.has_value());
        QCOMPARE(unplaced->placedCount(), 0);
        QCOMPARE(unplaced->localModelCount(), 3);
    }

    void readsZoneGrid()
    {
        // Three columns and two rows; the middle cell of the second row has
        // no zone. Odd columns sit half a row further along Z.
        constexpr quint32 kNone = 0xFFFFFFFF;
        const std::optional<fh1::ZoneGrid> grid = fh1::ZoneGrid::read(hexFile(5, 3, 2, {0, 1, 2, 3, kNone, 4}));
        QVERIFY(grid.has_value());
        QCOMPARE(grid->zoneCount(), 5);
        const float rowPitch = std::sqrt(3.0F) * 100.0F;
        QCOMPARE(grid->zoneAt(100.0F, rowPitch / 2), 0);
        QCOMPARE(grid->zoneAt(250.0F, rowPitch), 1);
        QCOMPARE(grid->zoneAt(400.0F, rowPitch / 2), 2);
        QCOMPARE(grid->zoneAt(100.0F, rowPitch * 1.5F), 3);
        QCOMPARE(grid->zoneAt(250.0F, rowPitch * 2), -1);
        QCOMPARE(grid->zoneAt(400.0F, rowPitch * 1.5F), 4);
        // Near the edge between cells 0 and 1, on cell 0's side.
        QCOMPARE(grid->zoneAt(170.0F, 120.0F), 0);
        // Below the odd column's first cell is outside the grid.
        QCOMPARE(grid->zoneAt(250.0F, 60.0F), -1);
        QCOMPARE(grid->zoneAt(-500.0F, 0.0F), -1);

        QByteArray stored;
        {
            QDataStream out(&stored, QIODevice::WriteOnly);
            out << *grid;
        }
        QDataStream in(stored);
        fh1::ZoneGrid reloaded;
        in >> reloaded;
        QCOMPARE(in.status(), QDataStream::Ok);
        QCOMPARE(reloaded.zoneAt(400.0F, rowPitch * 1.5F), 4);
        QCOMPARE(reloaded.zoneAt(250.0F, rowPitch * 2), -1);

        QString error;
        QByteArray wrongMagic = hexFile(1, 1, 1, {0});
        wrongMagic[0] = 'X';
        QVERIFY(!fh1::ZoneGrid::read(wrongMagic, &error).has_value());
        QVERIFY(!fh1::ZoneGrid::read(hexFile(1, 1, 1, {3}), &error).has_value());
        QVERIFY(error.contains(QLatin1String("zone 3")));
        QVERIFY(!fh1::ZoneGrid::read(hexFile(1, 2, 2, {0}).left(40), &error).has_value());
    }

    void limitsBackdropToZones()
    {
        // Ground in zone 0's cell, and backdrop terrain at two levels over
        // both cells. Zone 0 lists only the ground, zone 1 the ground and
        // the backdrop's finest level; nothing lists its coarser level.
        const float rowPitch = std::sqrt(3.0F) * 100.0F;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const TestPlacement ground{0, QVector3D(), {1, 0, 0, 0, 1, 0, 0, 0, -1}};
        const TestPlacement backdropNear{1, QVector3D(), ground.rows};
        const QString archivePath = writeZip(dir,
            {{QStringLiteral("coloradoout.00000.rmb.bin"),
                 renderModel({quad(QStringLiteral("Terrain_LOD00"), 100, rowPitch / 2, 0, 60)})},
                {QStringLiteral("coloradoout.00001.rmb.bin"),
                    renderModel({quad(QStringLiteral("TERR_UberLOD_Patch01_LOD00"), 175, rowPitch / 2, 5, 150)})},
                {QStringLiteral("coloradoout.00002.rmb.bin"),
                    renderModel({quad(QStringLiteral("TERR_UberLOD_Patch01_LOD01"), 175, rowPitch / 2, 5, 150)})},
                {QStringLiteral("__R00Z00000.pvsz"), zoneFile({ground})},
                {QStringLiteral("__R00Z00001.pvsz"), zoneFile({ground, backdropNear})}});
        fh1::ForzaZip archive;
        QVERIFY2(archive.open(archivePath), qPrintable(archive.errorString()));
        const std::optional<fh1::TrackPlacements> placements
            = fh1::TrackPlacements::load(pvsFile({0x10}, {{}, {}, {}}, {0, 1, 2}), archive);
        QVERIFY(placements.has_value());
        QCOMPARE(placements->zonesListing(0), (std::vector<std::uint16_t>{0, 1}));
        QCOMPARE(placements->zonesListing(1), (std::vector<std::uint16_t>{1}));
        QVERIFY(placements->zonesListing(2).empty());
        QVERIFY(placements->zonesListing(99).empty());
        const std::optional<fh1::ZoneGrid> zones = fh1::ZoneGrid::read(hexFile(2, 2, 1, {0, 1}));
        QVERIFY(zones.has_value());

        const std::optional<fh1::WorldIndex> index
            = fh1::WorldIndex::build(archive, {}, nullptr, &*placements, &*zones);
        QVERIFY(index.has_value());
        QVERIFY(index->zoneGrid() != nullptr);
        // The coarser backdrop level is left out, so the finest one serves
        // every distance.
        QCOMPARE(index->chunks().size(), std::size_t{2});
        const auto backdrop = std::find_if(
            index->chunks().begin(), index->chunks().end(), [](const fh1::WorldChunk& c) { return c.backdrop; });
        QVERIFY(backdrop != index->chunks().end());
        const auto backdropIndex = static_cast<std::uint32_t>(backdrop - index->chunks().begin());
        QCOMPARE(backdrop->lod, std::int8_t{0});
        QCOMPARE(backdrop->bandEnd, std::numeric_limits<float>::infinity());
        QCOMPARE(backdrop->zones, (std::vector<std::uint16_t>{1}));
        QVERIFY(!backdrop->visibleFrom(0));
        QVERIFY(backdrop->visibleFrom(1));
        QVERIFY(backdrop->visibleFrom(-1));
        const auto groundChunk = std::find_if(
            index->chunks().begin(), index->chunks().end(), [](const fh1::WorldChunk& c) { return !c.backdrop; });
        QVERIFY(groundChunk->zones.empty());
        QVERIFY(groundChunk->visibleFrom(0));

        const QString cache = dir.filePath(QStringLiteral("world.index"));
        QVERIFY(index->save(cache, QStringLiteral("sig")));
        const std::optional<fh1::WorldIndex> reloaded = fh1::WorldIndex::load(cache, QStringLiteral("sig"));
        QVERIFY(reloaded.has_value());
        QVERIFY(reloaded->zoneGrid() != nullptr);
        QCOMPARE(reloaded->zoneGrid()->zoneAt(250.0F, rowPitch), 1);
        QCOMPARE(reloaded->chunks()[backdropIndex].zones, backdrop->zones);

        // Backdrop geometry gets a batch of its own, naming its chunk.
        const fh1::TileMesh mesh = fh1::buildTileMesh(archive, *index, {0, 1});
        QCOMPARE(mesh.batches.size(), std::size_t{2});
        for (const fh1::TileMesh::Batch& batch : mesh.batches) {
            QCOMPARE(batch.chunk, batch.backdrop ? backdropIndex : fh1::TileMesh::kMergedChunks);
        }

        // Without a zone grid, every backdrop level is kept and drawn from
        // everywhere.
        const std::optional<fh1::WorldIndex> everywhere = fh1::WorldIndex::build(archive, {}, nullptr, &*placements);
        QVERIFY(everywhere.has_value());
        QCOMPARE(everywhere->chunks().size(), std::size_t{3});
        QVERIFY(everywhere->zoneGrid() == nullptr);
        for (const fh1::WorldChunk& chunk : everywhere->chunks()) {
            QVERIFY(chunk.zones.empty());
        }

        // From the zone that lists it, the renderer draws the backdrop; from
        // another, only where it lies below the camera, so it fills gaps in
        // the ground without hanging over the view.
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        QOpenGLContext context;
        context.setFormat(format);
        QOffscreenSurface surface;
        surface.setFormat(format);
        surface.create();
        if (!context.create() || !context.makeCurrent(&surface)) {
            QSKIP("no OpenGL 3.3 context available to check drawing");
        }
        const fh1::WorldTileGrid grid(*index, 500.0F);
        WorldRenderer renderer;
        QVERIFY2(renderer.initialize(), qPrintable(renderer.errorString()));
        renderer.setGrid(&grid);
        const QSize size(32, 32);
        QOpenGLFramebufferObject fbo(size, QOpenGLFramebufferObject::Depth);
        fbo.bind();
        const auto centreIsSky = [&](const QVector3D& position, float pitch) {
            WorldCamera camera;
            camera.position = position;
            camera.pitch = pitch;
            for (const TileRequest& request : renderer.requests(camera, {})) {
                renderer.upload(request.tile, request.state, fh1::buildTileMesh(archive, *index, request.chunks));
            }
            renderer.draw(camera, size);
            const QColor centre = fbo.toImage().pixelColor(size.width() / 2, size.height() / 2);
            const QVector3D fog = WorldRenderer::fogColour();
            return std::abs(centre.redF() - fog.x()) < 0.02F && std::abs(centre.greenF() - fog.y()) < 0.02F
                && std::abs(centre.blueF() - fog.z()) < 0.02F;
        };
        // Below the backdrop (5 m up), looking up: hidden from zone 0,
        // drawn from zone 1.
        QVERIFY(centreIsSky(QVector3D(100.0F, 2.0F, rowPitch / 2), 1.5F));
        QVERIFY(!centreIsSky(QVector3D(250.0F, 2.0F, rowPitch / 2), 1.5F));
        // From zone 0, high up, looking down beside the ground: the backdrop
        // shows where the ground has a gap.
        QVERIFY(!centreIsSky(QVector3D(30.0F, 400.0F, rowPitch / 2), -1.5F));
        fbo.release();
        renderer.release();
    }

    void encodesDxtAndBuildsMipChains()
    {
        std::array<fh1::dxt::Rgba, 16> gradient{};
        for (int i = 0; i < 16; ++i) {
            const auto v = static_cast<std::uint8_t>(i * 16);
            gradient[static_cast<std::size_t>(i)] = {v, static_cast<std::uint8_t>(255 - v), 64, v};
        }
        std::array<std::uint8_t, 16> block{};
        std::array<fh1::dxt::Rgba, 16> decoded{};
        fh1::dxt::encodeDxt5(gradient.data(), block.data());
        fh1::dxt::decodeBlock(fh1::TextureSurface::Format::Dxt5, block.data(), decoded.data());
        // Four palette colours across a 240-step gradient leave errors of up
        // to a sixth of the range; an encoder that collapsed the block onto
        // its mean colour would be off by 64 on average.
        int totalError = 0;
        for (std::size_t i = 0; i < 16; ++i) {
            QVERIFY(std::abs(decoded[i].r - gradient[i].r) <= 42);
            QVERIFY(std::abs(decoded[i].g - gradient[i].g) <= 42);
            QVERIFY(std::abs(decoded[i].a - gradient[i].a) <= 18);
            totalError += std::abs(decoded[i].r - gradient[i].r);
        }
        QVERIFY2(totalError / 16 < 24, qPrintable(QString::number(totalError / 16)));

        // DXT1 keeps cut-out texels transparent.
        std::array<fh1::dxt::Rgba, 16> cutout{};
        for (std::size_t i = 0; i < 16; ++i) {
            cutout[i] = {200, 40, 40, static_cast<std::uint8_t>(i % 2 == 0 ? 255 : 0)};
        }
        fh1::dxt::encodeDxt1(cutout.data(), block.data());
        fh1::dxt::decodeBlock(fh1::TextureSurface::Format::Dxt1, block.data(), decoded.data());
        for (std::size_t i = 0; i < 16; ++i) {
            QCOMPARE(decoded[i].a, static_cast<std::uint8_t>(i % 2 == 0 ? 255 : 0));
            if (i % 2 == 0) {
                QVERIFY(std::abs(decoded[i].r - 200) <= 8 && std::abs(decoded[i].g - 40) <= 8);
            }
        }

        fh1::TextureSurface dxt1;
        dxt1.format = fh1::TextureSurface::Format::Dxt1;
        dxt1.width = 8;
        dxt1.height = 8;
        dxt1.data = solidDxt1Block(qRgb(10, 200, 30)).repeated(4);
        const fh1::TextureMipChain chain = fh1::buildMipChain(dxt1);
        QCOMPARE(chain.format, fh1::TextureSurface::Format::Dxt1);
        QCOMPARE(chain.levels.size(), std::size_t{4}); // 8, 4, 2, 1
        QCOMPARE(chain.levels[0], dxt1.data);          // the game's own blocks
        QCOMPARE(chain.levels[1].size(), qsizetype{8});
        QCOMPARE(chain.levels[3].size(), qsizetype{8});
        const QImage smallest = fh1::surfaceToImage(chain.level(3));
        QCOMPARE(smallest.size(), QSize(1, 1));
        QVERIFY(std::abs(qGreen(smallest.pixel(0, 0)) - 200) <= 8);

        fh1::TextureSurface dxt3 = dxt1;
        dxt3.format = fh1::TextureSurface::Format::Dxt3;
        dxt3.data = (QByteArray(8, '\xff') + solidDxt1Block(qRgb(10, 200, 30))).repeated(4);
        QCOMPARE(fh1::buildMipChain(dxt3).format, fh1::TextureSurface::Format::Dxt5);

        fh1::TextureSurface rgba;
        rgba.format = fh1::TextureSurface::Format::Rgba8;
        rgba.width = 16;
        rgba.height = 4;
        rgba.data = QByteArray::fromHex("ff000080").repeated(64);
        const fh1::TextureMipChain rgbaChain = fh1::buildMipChain(rgba);
        QCOMPARE(rgbaChain.levels.size(), std::size_t{5}); // 16x4, 8x2, 4x1, 2x1, 1x1
        QCOMPARE(rgbaChain.levelWidth(2), 4);
        QCOMPARE(rgbaChain.levelHeight(2), 1);
        QCOMPARE(rgbaChain.levels[4], QByteArray::fromHex("ff000080"));
    }

    void texturesTiles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        TestPart part = quad(QStringLiteral("Sign_LOD00"), 200, 200, 3, 2);
        part.stride = 24;
        part.texcoords = {{0, 0}, {65535, 0}, {0, 65535}, {65535, 65535}};
        part.materials = {{{0, 1, 2, 3}, 2, 0, {0.0F, 0.0F, 2.0F, 1.0F}}, {{1, 3, 2}, 2, 1, {0.0F, 0.0F, 1.0F, 1.0F}}};
        const TestTail tail{{{0, {0, -1, -1, -1, -1, 1}}, {1, {-1}}},
            {QStringLiteral("shaders\\track\\h_diff_spec_ao_2.fx"), QStringLiteral("shaders\\track\\glow.fx")}};
        const auto [header, top] = bixTexture(8, qRgb(220, 30, 30));
        // Texture 0x30 (record 2) exists only in the bundle packs, as four
        // coloured quadrants; 0x2B40 (record 1) also has a blue small copy
        // there, which its full-size files take precedence over.
        const auto quadrants = [](int bx, int by) {
            static const std::array<QRgb, 4> colours{
                qRgb(255, 0, 0), qRgb(0, 255, 0), qRgb(0, 0, 255), qRgb(255, 255, 255)};
            return colours.at((static_cast<std::size_t>(by) * 2) + static_cast<std::size_t>(bx));
        };
        const QByteArray pack = bundlePack({{1, 16, [](int, int) { return qRgb(0, 0, 255); }}, {2, 8, quadrants}});
        const QString archivePath = writeZip(dir,
            {{QStringLiteral("coloradoout.00002.rmb.bin"), renderModel({part}, tail)},
                {QStringLiteral("shaders/track/H_DIFF_SPEC_AO_2.fxobj"), shaderObject({0x3007, 0x5008, 0x215009})},
                {QStringLiteral("_0x00002B40.bix"), header}, {QStringLiteral("_0x00002B40_B.bix"), top},
                {QStringLiteral("_0x10000001.bundle"), pack}, {QStringLiteral("_0x10000001.bundle"), pack},
                {QStringLiteral("_0x10000003.bundle"), bundlePack({{2, 4, [](int, int) { return qRgb(9, 9, 9); }}})}});
        fh1::ForzaZip archive;
        QVERIFY2(archive.open(archivePath), qPrintable(archive.errorString()));
        QString error;
        const std::optional<fh1::TrackTextures> textures
            = fh1::TrackTextures::load(pvsFile({0x10, 0x2B40, 0x30}, {{}, {}, {1, 2}}), archive, &error);
        QVERIFY2(textures.has_value(), qPrintable(error));
        QCOMPARE(textures->shaderCount(), 1);
        QVERIFY(textures->shader(QStringLiteral("shaders\\track\\H_DIFF_SPEC_AO_2.fx")) != nullptr);

        const std::optional<fh1::WorldIndex> index = fh1::WorldIndex::build(archive);
        QVERIFY(index.has_value());
        QCOMPARE(index->chunks().size(), std::size_t{1});
        const fh1::TileMesh mesh = fh1::buildTileMesh(archive, *index, {0}, &*textures);
        QCOMPARE(mesh.failedChunks, 0);
        // The first material uses texture 0x2B40; the second's shader is not
        // in the archive, so it is untextured.
        QCOMPARE(mesh.batches.size(), std::size_t{2});
        const auto textured = std::find_if(mesh.batches.begin(), mesh.batches.end(),
            [](const fh1::TileMesh::Batch& b) { return b.texture == 0x2B40; });
        QVERIFY(textured != mesh.batches.end());
        QCOMPARE(textured->indexCount, 6u);
        // Every material gets its own vertices: 4 for the quad, 3 for the
        // triangle.
        QCOMPARE(mesh.vertexCount(), std::size_t{7});
        float maxU = 0.0F;
        for (std::uint32_t i = textured->firstIndex; i < textured->firstIndex + textured->indexCount; ++i) {
            maxU = std::max(maxU, mesh.vertices[mesh.indices[i] * fh1::TileMesh::kFloatsPerVertex + 6]);
        }
        QCOMPARE(maxU, 2.0F);

        const std::optional<fh1::TextureMipChain> chain = textures->loadTexture(archive, 0x2B40, &error);
        QVERIFY2(chain.has_value(), qPrintable(error));
        QCOMPARE(chain->width, 8);
        QCOMPARE(chain->levels.size(), std::size_t{4});
        const QImage image = fh1::surfaceToImage(chain->level(0));
        QVERIFY(std::abs(qRed(image.pixel(5, 6)) - 220) <= 8 && qGreen(image.pixel(5, 6)) < 50);
        QCOMPARE(chain->format, fh1::TextureSurface::Format::Dxt1);

        // Records 1 and 2; the second pack's copy of record 2 is a duplicate.
        QCOMPARE(textures->bundledTextureCount(), 2);
        const std::optional<fh1::TextureMipChain> bundled = textures->loadTexture(archive, 0x30, &error);
        QVERIFY2(bundled.has_value(), qPrintable(error));
        QCOMPARE(bundled->width, 8);
        const QImage small = fh1::surfaceToImage(bundled->level(0));
        QCOMPARE(small.pixel(1, 1), qRgb(255, 0, 0));
        QCOMPARE(small.pixel(6, 1), qRgb(0, 255, 0));
        QCOMPARE(small.pixel(1, 6), qRgb(0, 0, 255));
        QCOMPARE(small.pixel(6, 6), qRgb(255, 255, 255));
        // Texture 0x10 is in neither the files nor the packs.
        QVERIFY(!textures->loadTexture(archive, 0x10, &error).has_value());
        QVERIFY(error.contains(QStringLiteral("_0x00000010")));
    }

    void rejectsMalformedModels()
    {
        TestPart part = quad(QStringLiteral("A"), 0, 0, 0, 1);
        part.materials[0].indices = {0, 1, 7};
        QVERIFY_THROWS_EXCEPTION(fh1::LoadError, fh1::rendermesh::parse(renderModel({part}), {}));

        QByteArray truncated = renderModel({quad(QStringLiteral("A"), 0, 0, 0, 1)});
        truncated.truncate(0xE0);
        QVERIFY_THROWS_EXCEPTION(fh1::LoadError, fh1::rendermesh::parse(truncated, {}));

        QByteArray wrongVersion = renderModel({quad(QStringLiteral("A"), 0, 0, 0, 1)});
        wrongVersion[3] = 7;
        QVERIFY_THROWS_EXCEPTION(fh1::LoadError, fh1::rendermesh::parse(wrongVersion, {}));
        QVERIFY(!fh1::rendermesh::readHeader(wrongVersion.left(fh1::rendermesh::kHeaderBytes)).has_value());
    }

    void skipsPlaceholderMaterials()
    {
        // A terrain piece whose second material is the placeholder of
        // unfinished geometry and whose third marks a crowd area.
        TestPart part = quad(QStringLiteral("TERR_Zone1_Area1_00"), 600, 600, 0, 100);
        part.materials.push_back({{1, 2, 3}, 2});
        part.materials.back().name = QStringLiteral("Placeholder001");
        part.materials.push_back({{0, 2, 3}, 2});
        part.materials.back().name = QStringLiteral("CrowdTERR");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        fh1::ForzaZip archive;
        QVERIFY(archive.open(writeZip(dir, {{QStringLiteral("coloradoout.00000.rmb.bin"), renderModel({part})}})));
        const std::optional<fh1::WorldIndex> index = fh1::WorldIndex::build(archive);
        QVERIFY(index.has_value());
        QCOMPARE(index->chunks().size(), std::size_t{1});
        const fh1::TileMesh mesh = fh1::buildTileMesh(archive, *index, {0});
        QCOMPARE(mesh.failedChunks, 0);
        // The two triangles of the first material; none of the others.
        QCOMPARE(mesh.indices.size(), std::size_t{6});
    }

    void lodNames()
    {
        QCOMPARE(fh1::rendermesh::lodLevel(QStringLiteral("Redstone_Area02_Road_LOD02_01")), 2);
        QCOMPARE(fh1::rendermesh::lodLevel(QStringLiteral("LOD00_Thing")), 0);
        QCOMPARE(fh1::rendermesh::lodLevel(QStringLiteral("cables_NOLOD_3")), -1);
        QCOMPARE(fh1::rendermesh::lodGroupKey(QStringLiteral("Redstone_Area02_Road_LOD01_02")),
            QStringLiteral("redstone_area02_road"));
        QCOMPARE(fh1::rendermesh::lodGroupKey(QStringLiteral("Redstone_Area02_Road_LOD02_11")),
            QStringLiteral("redstone_area02_road"));
        QCOMPARE(fh1::rendermesh::lodGroupKey(QStringLiteral("GrandstandStraight_LOD02_")),
            QStringLiteral("grandstandstraight"));
    }

    void indexesAndTilesAWorld()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray lod0 = renderModel({quad(QStringLiteral("Terrain_LOD00_01"), 1000, 1000, 10, 50)});
        const QByteArray lod1 = renderModel({quad(QStringLiteral("Terrain_LOD01_01"), 1000, 1000, 10, 50)});
        const QByteArray lod2 = renderModel({quad(QStringLiteral("Terrain_LOD02_01"), 1000, 1000, 10, 50)});
        const QByteArray onlyLod0 = renderModel({quad(QStringLiteral("Barrier_LOD00_4"), 3000, 3000, 10, 5)});
        const QByteArray noLod = renderModel({quad(QStringLiteral("Cables_NOLOD_1"), 1000, 1200, 12, 20)});
        const QByteArray prop = renderModel({quad(QStringLiteral("S_MetalSign"), 0, 0, 0, 1)});
        const QByteArray cage = renderModel({quad(QStringLiteral("UberLOD23_CAGE_LOD00"), 5000, 5000, 0, 100)});
        const QByteArray shadow = renderModel({quad(QStringLiteral("FT02_ShadowCaster_LOD01"), 2000, 2000, 0, 100)});
        const QString archivePath = writeZip(dir,
            {{QStringLiteral("a.rmb.bin"), lod0}, {QStringLiteral("b.rmb.bin"), lod1},
                {QStringLiteral("c.rmb.bin"), lod2}, {QStringLiteral("a.rmb.bin"), lod0},
                {QStringLiteral("d.rmb.bin"), onlyLod0}, {QStringLiteral("e.rmb.bin"), noLod},
                {QStringLiteral("f.rmb.bin"), prop}, {QStringLiteral("g.rmb.bin"), cage},
                {QStringLiteral("h.rmb.bin"), shadow}, {QStringLiteral("readme.txt"), "not a model"}});

        fh1::ForzaZip archive;
        QVERIFY2(archive.open(archivePath), qPrintable(archive.errorString()));
        const std::optional<fh1::WorldIndex> index = fh1::WorldIndex::build(archive);
        QVERIFY(index.has_value());
        // The duplicate a.rmb.bin, the local-space prop, the cage and the
        // shadow caster are left out.
        QCOMPARE(index->chunks().size(), std::size_t{5});
        QCOMPARE(index->localModelCount(), 1);

        auto chunkFor = [&](const QString& file) -> const fh1::WorldChunk& {
            for (const fh1::WorldChunk& c : index->chunks()) {
                if (archive.entries()[c.entry].name == file) {
                    return c;
                }
            }
            return index->chunks().front();
        };
        QCOMPARE(chunkFor(QStringLiteral("a.rmb.bin")).bandEnd, 400.0F);
        QCOMPARE(chunkFor(QStringLiteral("b.rmb.bin")).bandStart, 400.0F);
        QCOMPARE(chunkFor(QStringLiteral("b.rmb.bin")).bandEnd, 1500.0F);
        QVERIFY(std::isinf(chunkFor(QStringLiteral("c.rmb.bin")).bandEnd));
        QVERIFY(std::isinf(chunkFor(QStringLiteral("d.rmb.bin")).bandEnd));
        QCOMPARE(chunkFor(QStringLiteral("e.rmb.bin")).bandEnd, 1000.0F);

        const QString cache = dir.filePath(QStringLiteral("world.index"));
        QVERIFY(index->save(cache, QStringLiteral("sig")));
        const auto loaded = fh1::WorldIndex::load(cache, QStringLiteral("sig"));
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->chunks().size(), index->chunks().size());
        QCOMPARE(loaded->chunks()[1].bandEnd, index->chunks()[1].bandEnd);
        QString error;
        QVERIFY(!fh1::WorldIndex::load(cache, QStringLiteral("other"), &error).has_value());
        QVERIFY(!error.isEmpty());

        const fh1::WorldTileGrid grid(*index, 500.0F);
        const fh1::WorldTileGrid::Tile* terrainTile = nullptr;
        for (const auto& tile : grid.tiles()) {
            if (tile.boundsMin.x() <= 1000 && tile.boundsMax.x() >= 1000 && tile.chunks.size() == 4) {
                terrainTile = &tile;
            }
        }
        QVERIFY(terrainTile != nullptr);
        QCOMPARE(grid.chunksAt(*terrainTile, 0.0F).size(), std::size_t{2});    // LOD0 and the cables
        QCOMPARE(grid.chunksAt(*terrainTile, 800.0F).size(), std::size_t{2});  // LOD1 and the cables
        QCOMPARE(grid.chunksAt(*terrainTile, 5000.0F).size(), std::size_t{1}); // LOD2 only
        QVERIFY(grid.stateAt(*terrainTile, 0.0F) != grid.stateAt(*terrainTile, 800.0F));
        QCOMPARE(grid.stateAt(*terrainTile, 5000.0F), grid.stateAt(*terrainTile, 6000.0F));
        QCOMPARE(grid.distanceTo(*terrainTile, 1000.0F, 1000.0F), 0.0F);

        const fh1::TileMesh mesh = fh1::buildTileMesh(archive, *index, grid.chunksAt(*terrainTile, 0.0F));
        QCOMPARE(mesh.failedChunks, 0);
        QCOMPARE(mesh.indices.size(), std::size_t{12});
        QCOMPARE(mesh.vertexCount(), std::size_t{8});
        for (std::size_t v = 0; v < mesh.vertexCount(); ++v) {
            // Flat ground: every normal points straight up.
            QVERIFY(mesh.vertices[v * fh1::TileMesh::kFloatsPerVertex + 4] > 0.99F);
        }
        // Without texture tables everything is one untextured batch.
        QCOMPARE(mesh.batches.size(), std::size_t{1});
        QCOMPARE(mesh.batches[0].texture, fh1::TileMesh::kNoTexture);
        QCOMPARE(mesh.batches[0].indexCount, 12u);
    }

    void rendersOffscreen()
    {
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        QOpenGLContext context;
        context.setFormat(format);
        QOffscreenSurface surface;
        surface.setFormat(format);
        surface.create();
        if (!context.create() || !context.makeCurrent(&surface)) {
            QSKIP("no OpenGL 3.3 context available");
        }

        // A 800 m ground quad whose centre is away from the origin (a model
        // centred on the origin would count as a local-space prop).
        QTemporaryDir dir;
        const QString archivePath = writeZip(dir,
            {{QStringLiteral("ground.rmb.bin"),
                renderModel({quad(QStringLiteral("Ground_NOLOD"), 300, 300, 0, 400)})}});
        fh1::ForzaZip archive;
        QVERIFY(archive.open(archivePath));
        const std::optional<fh1::WorldIndex> index = fh1::WorldIndex::build(archive);
        QVERIFY(index.has_value());
        QCOMPARE(index->chunks().size(), std::size_t{1});
        const fh1::WorldTileGrid grid(*index, 500.0F);

        WorldRenderer renderer;
        QVERIFY2(renderer.initialize(), qPrintable(renderer.errorString()));
        renderer.setGrid(&grid);
        WorldCamera camera;
        camera.position = QVector3D(300.0F, 50.0F, 300.0F);
        camera.yaw = 1.5708F;
        camera.pitch = -1.2F;
        const auto requests = renderer.requests(camera, {});
        QCOMPARE(requests.size(), std::size_t{1});
        renderer.upload(requests[0].tile, requests[0].state, fh1::buildTileMesh(archive, *index, requests[0].chunks));
        QVERIFY(renderer.isComplete(camera));

        const QSize size(64, 64);
        QOpenGLFramebufferObject fbo(size, QOpenGLFramebufferObject::Depth);
        fbo.bind();
        const WorldRenderer::Stats stats = renderer.draw(camera, size);
        fbo.release();
        const QImage image = fbo.toImage();
        renderer.release();
        QCOMPARE(stats.drawnTiles, 1);
        QCOMPARE(stats.drawnTriangles, 2);
        // Looking down at the ground: the centre is lit ground, not sky.
        const QColor centre = image.pixelColor(32, 32);
        const QColor sky(179, 199, 219);
        QVERIFY2(
            std::abs(centre.red() - sky.red()) + std::abs(centre.blue() - sky.blue()) > 30, qPrintable(centre.name()));
    }

    void drawsAndPicksMapEntities()
    {
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        QOpenGLContext context;
        context.setFormat(format);
        QOffscreenSurface surface;
        surface.setFormat(format);
        surface.create();
        if (!context.create() || !context.makeCurrent(&surface)) {
            QSKIP("no OpenGL 3.3 context available");
        }

        // Two groups of markers, a route and a zone, around (0, 0, 60).
        auto map = std::make_shared<fh1::MapData>();
        fh1::Layer points;
        points.id = QStringLiteral("gameobjs");
        points.kind = fh1::FeatureKind::Point;
        const auto point = [](const QString& name, const QString& group, float x, float z) {
            fh1::Feature f;
            f.name = name;
            f.label = name;
            f.group = group;
            f.position = QVector3D(x, 0.0F, z);
            return f;
        };
        points.features = {point(QStringLiteral("Big A"), QStringLiteral("A"), -10, 60),
            point(QStringLiteral("Big B"), QStringLiteral("A"), 10, 60),
            point(QStringLiteral("Small"), QStringLiteral("B"), 0, 70)};
        fh1::Layer routes;
        routes.id = QStringLiteral("airoutes");
        routes.kind = fh1::FeatureKind::Polyline;
        fh1::Feature route;
        route.name = QStringLiteral("route");
        route.shapes = {{QVector3D(-40, 0, 45), QVector3D(40, 0, 45)}};
        route.position = route.shapes[0][0];
        routes.features = {route};
        fh1::Layer zones;
        zones.id = QStringLiteral("ppzones");
        zones.kind = fh1::FeatureKind::Polygon;
        fh1::Feature zone;
        zone.name = QStringLiteral("zone");
        zone.shapes = {{QVector3D(20, 0, 80), QVector3D(40, 0, 80), QVector3D(30, 0, 95)}};
        zone.position = QVector3D(30, 0, 85);
        zones.features = {zone};
        map->layers = {points, routes, zones};

        WorldRenderer world;
        QVERIFY2(world.initialize(), qPrintable(world.errorString()));
        world.setViewDistance(2000.0F);
        EntityRenderer entities;
        QVERIFY2(entities.initialize(), qPrintable(entities.errorString()));
        // Group B of the markers starts hidden.
        entities.setMap(map, {{true, false}, {true}, {true}});
        QVERIFY(entities.isGroupVisible(0, 0));
        QVERIFY(!entities.isGroupVisible(0, 1));
        QVERIFY(!entities.isFeatureShown({0, 2}));

        WorldCamera camera;
        camera.position = QVector3D(0.0F, 60.0F, 0.0F);
        camera.yaw = 1.5708F;
        camera.pitch = -0.75F;
        const QSize size(320, 240);
        const QMatrix4x4 matrix = world.worldViewProjection(camera, size);
        const auto screenOf = [&](const QVector3D& p) {
            const QVector4D clip = matrix * QVector4D(p, 1.0F);
            return QPointF(
                (clip.x() / clip.w() * 0.5 + 0.5) * size.width(), (0.5 - clip.y() / clip.w() * 0.5) * size.height());
        };
        const QVector3D lift(0.0F, EntityRenderer::kPointLift, 0.0F);
        const QPointF bigA = screenOf(QVector3D(-10, 0, 60) + lift);
        const QPointF small = screenOf(QVector3D(0, 0, 70) + lift);

        QOpenGLFramebufferObject fbo(size, QOpenGLFramebufferObject::Depth);
        fbo.bind();
        world.draw(camera, size);
        entities.draw(matrix, size, camera.position, world.fogDistance(), WorldRenderer::fogColour(), 1.0F);
        fbo.release();
        const QImage image = fbo.toImage();
        const QColor sky = image.pixelColor(2, 2);
        const QColor markerA = image.pixelColor(bigA.toPoint());
        const QColor groupA = map->layers.empty() ? QColor() : LayerStyle::of(map->layers[0], 0).groupColors[0];
        QVERIFY2(std::abs(markerA.red() - groupA.red()) + std::abs(markerA.green() - groupA.green())
                    + std::abs(markerA.blue() - groupA.blue())
                < 60,
            qPrintable(markerA.name()));
        // The hidden group leaves the sky showing.
        QCOMPARE(image.pixelColor(small.toPoint()), sky);

        // Picking: the marker, nothing at the hidden one, the route, the zone.
        const auto pick
            = [&](const QPointF& at) { return entities.pick(matrix, size, camera.position, at, 8.0, 2000.0F, 1.0F); };
        const auto hitA = pick(bigA + QPointF(3, 2));
        QVERIFY(hitA.has_value());
        QCOMPARE(hitA->layer, 0);
        QCOMPARE(hitA->feature, 0);
        QVERIFY(!pick(small).has_value());
        entities.setGroupVisible(0, 1, true);
        QVERIFY(pick(small).has_value());
        const auto hitRoute = pick(screenOf(QVector3D(25, 0.8F, 45)));
        QVERIFY(hitRoute.has_value());
        QCOMPARE(hitRoute->layer, 1);
        const auto hitZone = pick(screenOf(QVector3D(30, 0.3F, 85)));
        QVERIFY(hitZone.has_value());
        QCOMPARE(hitZone->layer, 2);
        QVERIFY(!pick(QPointF(2, 2)).has_value());

        // Labels come nearest first; far ones are left out.
        const auto labels = entities.labels(matrix, size, camera.position, 2000.0F, 1.0F);
        QCOMPARE(labels.size(), std::size_t{3});
        QVERIFY(labels.front().distance <= labels.back().distance);
        QVERIFY(entities.labels(matrix, size, camera.position, 10.0F, 1.0F).empty());
        QImage canvas(size, QImage::Format_ARGB32);
        canvas.fill(Qt::transparent);
        QPainter painter(&canvas);
        QVERIFY(EntityRenderer::paintLabels(painter, labels, QFont()) >= 1);
        painter.end();

        const auto [lo, hi] = entities.featureBounds({2, 0});
        QCOMPARE(lo, QVector3D(20, 0, 80));
        QCOMPARE(hi, QVector3D(40, 0, 95));

        // A highlight is drawn over everything, even a hidden marker's spot.
        entities.setHighlighted({0, 1});
        QCOMPARE(entities.highlighted(), (EntityRenderer::FeatureRef{0, 1}));
        entities.setMap(nullptr);
        QVERIFY(!entities.pick(matrix, size, camera.position, bigA, 8.0, 2000.0F, 1.0F).has_value());
        entities.release();
        world.release();
    }

    void debugPanelListsAndPreviewsLoadedFiles()
    {
        QTemporaryDir dir;
        TestPart part = quad(QStringLiteral("Sign_LOD00"), 200, 200, 3, 2);
        part.stride = 24;
        part.texcoords = {{0, 0}, {65535, 0}, {0, 65535}, {65535, 65535}};
        const TestTail tail{{{0, {0}}}, {QStringLiteral("shaders\\track\\h_diff_1.fx")}};
        const auto [header, top] = bixTexture(8, qRgb(220, 30, 30));
        const QString archivePath = writeZip(dir,
            {{QStringLiteral("coloradoout.00000.rmb.bin"), renderModel({part}, tail)},
                {QStringLiteral("shaders/track/h_diff_1.fxobj"), shaderObject({0x3007, 0x205008})},
                {QStringLiteral("_0x00002B40.bix"), header}, {QStringLiteral("_0x00002B40_B.bix"), top}});
        auto archive = std::make_shared<fh1::ForzaZip>();
        QVERIFY(archive->open(archivePath));
        std::optional<fh1::TrackTextures> loaded
            = fh1::TrackTextures::load(pvsFile({0x2B40, 0x30}, {{0, 1}}), *archive);
        QVERIFY(loaded.has_value());
        auto textures = std::make_shared<const fh1::TrackTextures>(std::move(*loaded));
        std::optional<fh1::WorldIndex> built = fh1::WorldIndex::build(*archive);
        QVERIFY(built.has_value());
        auto index = std::make_shared<const fh1::WorldIndex>(std::move(*built));

        LoadedTexture sign;
        sign.id = 0x2B40;
        sign.state = LoadedTexture::State::Loaded;
        sign.tiles = 1;
        sign.bytes = 48;
        sign.width = 8;
        sign.height = 8;
        sign.levels = 4;
        sign.format = fh1::TextureSurface::Format::Dxt1;
        sign.files = QStringLiteral("_0x00002B40.bix, _0x00002B40_B.bix");
        LoadedTexture missing;
        missing.id = 0x30;
        missing.state = LoadedTexture::State::Failed;
        missing.error = QStringLiteral("texture _0x00000030 is not in the archive");
        const std::vector<LoadedModel> models{{0, 3, 2, {0x2B40, 0x30}, {}}};

        WorldDebugPanel panel;
        panel.setWorld(archive, index, textures);
        panel.setLoadedFiles(models, {sign, missing});
        auto* tabs = panel.findChild<QTabWidget*>();
        QVERIFY(tabs != nullptr);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Models (1)"));
        QCOMPARE(tabs->tabText(1), QStringLiteral("Textures (2)"));
        QVERIFY(panel.showModel(0));
        auto* modelPreview = panel.findChild<ModelPreview*>();
        QVERIFY(modelPreview != nullptr);
        QCOMPARE(modelPreview->model(), std::optional<std::uint32_t>(0));

        QSignalSpy ready(&panel, &WorldDebugPanel::previewReady);
        QVERIFY(panel.showTexture(0x2B40));
        QVERIFY(ready.wait(5000));
        QCOMPARE(panel.previewedTexture(), std::optional<std::uint32_t>(0x2B40));
        QImage image = panel.previewImage();
        QCOMPARE(image.size(), QSize(8, 8));
        QVERIFY(std::abs(qRed(image.pixel(3, 3)) - 220) <= 8 && qGreen(image.pixel(3, 3)) < 50);
        panel.setPreviewLevel(2);
        QCOMPARE(panel.previewImage().size(), QSize(2, 2));
        panel.setPreviewChannels(WorldDebugPanel::Channels::Alpha);
        QCOMPARE(panel.previewImage().pixel(0, 0), qRgb(255, 255, 255));

        // A refresh with the same files keeps the selection and the preview.
        panel.setLoadedFiles(models, {sign, missing});
        QCOMPARE(panel.previewedTexture(), std::optional<std::uint32_t>(0x2B40));
        QCOMPARE(panel.previewImage().size(), QSize(2, 2));

        // A texture that cannot be decoded shows no image.
        QVERIFY(panel.showTexture(0x30));
        QVERIFY(ready.wait(5000));
        QVERIFY(panel.previewImage().isNull());

        panel.setWorld(nullptr, nullptr, nullptr);
        QVERIFY(!panel.previewedTexture().has_value());
        QVERIFY(!modelPreview->model().has_value());
        QCOMPARE(tabs->tabText(0), QStringLiteral("Models (0)"));
    }

    void previewsOneModel()
    {
        // The orbit camera looks at the centre from the fitted distance.
        const QVector3D lo(10, 0, 10);
        const QVector3D hi(14, 2, 18);
        const float distance = ModelPreview::fitDistance(lo, hi, 1.0F);
        const float radius = (hi - lo).length() / 2.0F;
        QVERIFY(distance > radius * 2.0F && distance < radius * 2.5F);
        // A viewport twice as tall as wide needs more distance.
        QVERIFY(ModelPreview::fitDistance(lo, hi, 0.5F) > distance);
        const QVector3D centre = (lo + hi) / 2.0F;
        const WorldCamera camera = ModelPreview::orbitCamera(centre, distance, 2.0F, -0.4F);
        QVERIFY((camera.position + camera.forward() * distance - centre).length() < 1e-3F);
        QVERIFY(camera.position.y() > centre.y());

        // A red sign, drawn textured on its own.
        QTemporaryDir dir;
        TestPart part = quad(QStringLiteral("Sign_LOD00"), 200, 200, 3, 2);
        part.stride = 24;
        part.texcoords = {{0, 0}, {65535, 0}, {0, 65535}, {65535, 65535}};
        const TestTail tail{{{0, {0}}}, {QStringLiteral("shaders\\track\\h_diff_1.fx")}};
        const auto [header, top] = bixTexture(8, qRgb(220, 30, 30));
        const QString archivePath = writeZip(dir,
            {{QStringLiteral("coloradoout.00000.rmb.bin"), renderModel({part}, tail)},
                {QStringLiteral("coloradoout.00001.rmb.bin"),
                    renderModel({quad(QStringLiteral("Ground_NOLOD"), 900, 900, 0, 50)})},
                {QStringLiteral("shaders/track/h_diff_1.fxobj"), shaderObject({0x3007, 0x205008})},
                {QStringLiteral("_0x00002B40.bix"), header}, {QStringLiteral("_0x00002B40_B.bix"), top}});
        auto archive = std::make_shared<fh1::ForzaZip>();
        QVERIFY(archive->open(archivePath));
        std::optional<fh1::TrackTextures> loaded = fh1::TrackTextures::load(pvsFile({0x2B40}, {{0}}), *archive);
        QVERIFY(loaded.has_value());
        auto textures = std::make_shared<const fh1::TrackTextures>(std::move(*loaded));
        std::optional<fh1::WorldIndex> built = fh1::WorldIndex::build(*archive);
        QVERIFY(built.has_value());
        auto index = std::make_shared<const fh1::WorldIndex>(std::move(*built));
        const auto sign = std::find_if(index->chunks().begin(), index->chunks().end(),
            [](const fh1::WorldChunk& c) { return c.boundsMin.x() < 500.0F; });
        QVERIFY(sign != index->chunks().end());
        const auto signChunk = static_cast<std::uint32_t>(sign - index->chunks().begin());

        ModelPreview preview;
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        preview.setFormat(format);
        preview.resize(96, 96);
        preview.setModel(archive, index, textures, signChunk);
        QCOMPARE(preview.model(), std::optional<std::uint32_t>(signChunk));
        preview.show();
        // The offscreen platform gives OpenGL widgets no framebuffer to draw
        // into, although it does give plain offscreen surfaces one.
        if (QGuiApplication::platformName() == QLatin1String("offscreen")) {
            QSKIP("OpenGL widgets cannot draw on the offscreen platform; run with a display to check drawing");
        }
        if (!QTest::qWaitForWindowExposed(&preview) || !preview.isValid()) {
            QSKIP("the preview widget has no OpenGL context on this platform");
        }
        QSignalSpy settled(&preview, &ModelPreview::settled);
        // Painting uploads the mesh, then its texture once decoded.
        QVERIFY(QTest::qWaitFor(
            [&] {
                preview.update();
                return preview.isSettled() && settled.count() > 0;
            },
            10000));
        const QImage image = preview.grabFramebuffer();
        if (image.isNull()) {
            QSKIP("no OpenGL 3.3 context available to the preview");
        }
        const QColor centrePixel = image.pixelColor(image.width() / 2, image.height() / 2);
        QVERIFY2(centrePixel.red() > 120 && centrePixel.green() < 60,
            qPrintable(QStringLiteral("centre pixel %1").arg(centrePixel.name())));

        // Choosing the same model again keeps the view; clearing drops it.
        preview.setModel(archive, index, textures, signChunk);
        QVERIFY(preview.isSettled());
        preview.clear(QStringLiteral("nothing"));
        QVERIFY(!preview.model().has_value());
        QVERIFY(!preview.isSettled());
    }

    void rendersTexturedOffscreen()
    {
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        QOpenGLContext context;
        context.setFormat(format);
        QOffscreenSurface surface;
        surface.setFormat(format);
        surface.create();
        if (!context.create() || !context.makeCurrent(&surface)) {
            QSKIP("no OpenGL 3.3 context available");
        }

        QTemporaryDir dir;
        TestPart ground = quad(QStringLiteral("Ground_NOLOD"), 300, 300, 0, 400);
        ground.stride = 24;
        ground.texcoords = {{0, 0}, {65535, 0}, {0, 65535}, {65535, 65535}};
        const TestTail tail{{{0, {0}}}, {QStringLiteral("shaders\\track\\h_diff_1.fx")}};
        const auto [header, top] = bixTexture(8, qRgb(230, 20, 20));
        // The map-wide far terrain, a few metres above the textured ground,
        // as it often is in the game's data.
        const TestPart backdrop = quad(QStringLiteral("TERR_UberLOD_Patch18"), 300, 300, 4, 400);
        const QString archivePath = writeZip(dir,
            {{QStringLiteral("track.00000.rmb.bin"), renderModel({ground}, tail)},
                {QStringLiteral("track.00001.rmb.bin"), renderModel({backdrop})},
                {QStringLiteral("shaders/track/h_diff_1.fxobj"), shaderObject({0x3007, 0x205008})},
                {QStringLiteral("_0x00000077.bix"), header}, {QStringLiteral("_0x00000077_B.bix"), top}});
        fh1::ForzaZip archive;
        QVERIFY(archive.open(archivePath));
        const std::optional<fh1::TrackTextures> textures = fh1::TrackTextures::load(pvsFile({0x77}, {{0}}), archive);
        QVERIFY(textures.has_value());
        const std::optional<fh1::WorldIndex> index = fh1::WorldIndex::build(archive);
        QVERIFY(index.has_value());
        QCOMPARE(index->chunks().size(), std::size_t{2});
        const auto backdropChunk = std::find_if(
            index->chunks().begin(), index->chunks().end(), [](const fh1::WorldChunk& c) { return c.backdrop; });
        QVERIFY(backdropChunk != index->chunks().end());
        QCOMPARE(std::count_if(index->chunks().begin(), index->chunks().end(),
                     [](const fh1::WorldChunk& c) { return c.backdrop; }),
            1);
        // The flag survives the index cache.
        const QString cache = dir.filePath(QStringLiteral("world.index"));
        QVERIFY(index->save(cache, QStringLiteral("sig")));
        const std::optional<fh1::WorldIndex> reloaded = fh1::WorldIndex::load(cache, QStringLiteral("sig"));
        QVERIFY(reloaded.has_value());
        for (std::size_t i = 0; i < index->chunks().size(); ++i) {
            QCOMPARE(reloaded->chunks()[i].backdrop, index->chunks()[i].backdrop);
        }
        const fh1::WorldTileGrid grid(*index, 500.0F);

        WorldRenderer renderer;
        QVERIFY2(renderer.initialize(), qPrintable(renderer.errorString()));
        renderer.setGrid(&grid);
        WorldCamera camera;
        camera.position = QVector3D(300.0F, 50.0F, 300.0F);
        camera.yaw = 1.5708F;
        camera.pitch = -1.2F;
        const auto requests = renderer.requests(camera, {});
        QCOMPARE(requests.size(), std::size_t{1});
        const fh1::TileMesh mesh = fh1::buildTileMesh(archive, *index, requests[0].chunks, &*textures);
        QCOMPARE(mesh.batches.size(), std::size_t{2});
        QCOMPARE(
            std::count_if(mesh.batches.begin(), mesh.batches.end(),
                [](const fh1::TileMesh::Batch& b) { return b.backdrop && b.texture == fh1::TileMesh::kNoTexture; }),
            1);
        renderer.upload(requests[0].tile, requests[0].state, mesh);
        QVERIFY(renderer.texturesPending());
        const std::vector<std::uint32_t> wanted = renderer.takeTextureRequests();
        QCOMPARE(wanted, (std::vector<std::uint32_t>{0x77}));
        // Each request is handed out once.
        QVERIFY(renderer.takeTextureRequests().empty());
        const std::optional<fh1::TextureMipChain> chain = textures->loadTexture(archive, 0x77);
        QVERIFY(chain.has_value());
        renderer.uploadTexture(0x77, *chain);
        QVERIFY(!renderer.texturesPending());
        QCOMPARE(renderer.textureStats().loaded, 1);

        const QSize size(64, 64);
        QOpenGLFramebufferObject fbo(size, QOpenGLFramebufferObject::Depth);
        fbo.bind();
        const WorldRenderer::Stats stats = renderer.draw(camera, size);
        fbo.release();
        const QImage image = fbo.toImage();
        QCOMPARE(stats.drawCalls, 2);
        // The ground shows the red texture, lit, although the untextured
        // backdrop terrain lies above it.
        const QColor centre = image.pixelColor(32, 32);
        QVERIFY2(centre.red() > 2 * centre.green() && centre.red() > 2 * centre.blue(), qPrintable(centre.name()));

        // Releasing the tile frees its texture.
        renderer.setGrid(nullptr);
        QCOMPARE(renderer.textureStats().loaded, 0);
        renderer.release();
    }
};

QTEST_MAIN(TestWorld)
#include "tst_world.moc"
