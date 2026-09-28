#include "Activities.h"
#include "Loaders.h"
#include "StringTable.h"
#include "XboxTexture.h"

#include <QPainter>
#include <QTest>
#include <QtEndian>

#include <xds/xds.h>

namespace {

void appendBe16(QByteArray& out, quint16 value)
{
    char bytes[2];
    qToBigEndian(value, bytes);
    out.append(bytes, 2);
}

void appendBe32(QByteArray& out, quint32 value)
{
    char bytes[4];
    qToBigEndian(value, bytes);
    out.append(bytes, 4);
}

/// Builds an LSB2 table with the given (key, text) entries, sorted by key.
QByteArray stringTable(const QList<QPair<quint16, QString>>& entries)
{
    QByteArray data("LSB2");
    appendBe32(data, 0x01000000);
    appendBe32(data, 0x02000000);
    // Header words up to the entry count at 0x28.
    for (int i = 0; i < 7; ++i) {
        appendBe32(data, 0);
    }
    appendBe32(data, static_cast<quint32>(entries.size()));
    QByteArray strings;
    for (const auto& [key, text] : entries) {
        appendBe16(data, key);
        appendBe32(data, static_cast<quint32>(strings.size() / 2));
        for (QChar c : text) {
            appendBe16(strings, c.unicode());
        }
        appendBe16(strings, 0);
    }
    appendBe16(data, 0xFFFF);
    appendBe32(data, static_cast<quint32>(strings.size() / 2));
    return data + strings;
}

/// An .xds header for a tiled texture of `format`, `width` x `height`, with
/// the 8-in-16 byte order.
QByteArray textureHeader(quint32 format, int width, int height)
{
    QByteArray data;
    appendBe32(data, 3);
    appendBe32(data, 1);
    for (int i = 0; i < 3; ++i) {
        appendBe32(data, 0);
    }
    appendBe32(data, 0xFFFF0000);
    appendBe32(data, 0xFFFF0000);
    // Tiled, one 32-block tile wide; 8-in-16 byte order; 2D; swizzle XYZW.
    const quint32 pitch = 32 * (xds::isFormatSupported(format) ? xds::blockEdge(format) : 1);
    appendBe32(data, 0x80000002u | ((pitch >> 5) << 22));
    appendBe32(data, (1u << 6) | format);
    appendBe32(data, static_cast<quint32>(width - 1) | (static_cast<quint32>(height - 1) << 13));
    appendBe32(data, 0x688u << 1);
    appendBe32(data, 0);
    appendBe32(data, 1u << 9);
    return data;
}

/// A DXT1 block of one solid RGB565 colour, stored with 16-bit words swapped
/// as the 8-in-16 byte order expects.
QByteArray solidDxt1Block(quint16 rgb565)
{
    QByteArray block(8, '\0');
    block[0] = static_cast<char>(rgb565 >> 8);
    block[1] = static_cast<char>(rgb565 & 0xFF);
    block[2] = static_cast<char>(rgb565 >> 8);
    block[3] = static_cast<char>(rgb565 & 0xFF);
    return block;
}

} // namespace

class TestLabels : public QObject {
    Q_OBJECT

private slots:
    void parsesStringTable()
    {
        const QByteArray data = stringTable({{0x0497, QStringLiteral("1964 Aston Martin DB5 Vantage")},
            {0x05F9, QStringLiteral("BARN FIND READY - {0}")}, {0xDAD5, QStringLiteral("Colorado")}});
        QString error;
        const std::optional<fh1::StringTable> table = fh1::StringTable::parse(data, &error);
        QVERIFY2(table.has_value(), qPrintable(error));
        QCOMPARE(table->size(), 3);
        QCOMPARE(table->value(0x0497), QStringLiteral("1964 Aston Martin DB5 Vantage"));
        QCOMPARE(table->value(0xDAD5), QStringLiteral("Colorado"));
        QVERIFY(table->value(0x1234).isNull());
    }

    void rejectsBadStringTables()
    {
        QVERIFY(!fh1::StringTable::parse("NOPE").has_value());
        QByteArray truncated = stringTable({{1, QStringLiteral("a")}});
        truncated.truncate(0x30);
        QVERIFY(!fh1::StringTable::parse(truncated).has_value());
    }

    void databaseReferences()
    {
        // 167434965 = 0x09FADAD5: table 0x9FA, key 0xDAD5.
        QCOMPARE(fh1::StringTables::referenceKey(QStringLiteral("_&167434965")), std::optional<quint16>(0xDAD5));
        QVERIFY(!fh1::StringTables::referenceKey(QStringLiteral("Colorado")).has_value());
        QVERIFY(!fh1::StringTables::referenceKey(QStringLiteral("_&abc")).has_value());
    }

    void decodesTiledDxt1()
    {
        // 8x8 texels = 2x2 blocks in a 32-block-wide tiled surface. Each block
        // gets its own colour, written at the block's tiled offset.
        const quint16 colours[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
        QByteArray texels(qsizetype{32} * 32 * 8, '\0');
        for (quint32 by = 0; by < 2; ++by) {
            for (quint32 bx = 0; bx < 2; ++bx) {
                const quint32 offset = xds::tiledOffset2D(bx, by, 32, 3);
                texels.replace(static_cast<qsizetype>(offset), 8, solidDxt1Block(colours[by * 2 + bx]));
            }
        }
        QString error;
        const QImage image = fh1::decodeXboxTexture(textureHeader(18, 8, 8) + texels, &error);
        QVERIFY2(!image.isNull(), qPrintable(error));
        QCOMPARE(image.size(), QSize(8, 8));
        QCOMPARE(image.pixel(1, 1), qRgb(255, 0, 0));
        QCOMPARE(image.pixel(6, 1), qRgb(0, 255, 0));
        QCOMPARE(image.pixel(1, 6), qRgb(0, 0, 255));
        QCOMPARE(image.pixel(6, 6), qRgb(255, 255, 255));
    }

    void rejectsUnsupportedTextures()
    {
        QString error;
        QVERIFY(fh1::decodeXboxTexture(QByteArray(10, '\0'), &error).isNull());
        QVERIFY(fh1::decodeXboxTexture(textureHeader(33, 8, 8) + QByteArray(8192, '\0'), &error).isNull());
        QVERIFY(error.contains(QLatin1String("format")));
        QVERIFY(fh1::decodeXboxTexture(textureHeader(18, 8, 8) + QByteArray(16, '\0'), &error).isNull());
    }

    void parsesActivities()
    {
        const QByteArray xml = R"(<ActivityManager>
            <Activity type="ActivityBarnFind" name="barnfind_01">
                <DoorInfo open_id="BF_A_OPEN" closed_id="BF_A_CLOSED" />
                <TriggerZone object="BARNFIND_A" mapTag="barnfind" radius="10"/>
                <UnlockCar id="1277" stringTableNameId="IDS_Barnfind_Car_Cuda426"/>
            </Activity>
            <Activity type="ActivityFlyers" name="flyer_001" />
            <Activity type="ActivityCareerEventActivation" name="FR08">
                <State><Behaviour id="CPlaceCarAtObject" object_name="FR08_NODE" /></State>
                <TriggerZone object="FR08" name="FR08" radius="25"/>
            </Activity>
        </ActivityManager>)";
        const std::vector<fh1::Activity> activities = fh1::activities::parse(xml, QStringLiteral("Colorado/x.xml"));
        QCOMPARE(activities.size(), std::size_t{3});
        QCOMPARE(activities[0].mapTag, QStringLiteral("barnfind"));
        QCOMPARE(activities[0].triggerObjects, QStringList{QStringLiteral("BARNFIND_A")});
        QCOMPARE(activities[0].unlockCarId, QStringLiteral("1277"));
        QVERIFY(activities[0].referencedValues.contains(QStringLiteral("BF_A_OPEN")));
        QVERIFY(activities[1].triggerObjects.isEmpty());
        QVERIFY(activities[2].referencedValues.contains(QStringLiteral("FR08_NODE")));
        QCOMPARE(activities[2].carPlacementObjects, QStringList{QStringLiteral("FR08_NODE")});
        QCOMPARE(activities[2].triggerRadius.value(QStringLiteral("FR08")), QStringLiteral("25"));

        QCOMPARE(fh1::activities::iconCategory(activities[0], {}), QStringLiteral("barnfind"));
        QCOMPARE(fh1::activities::iconCategory(activities[1], {}), QStringLiteral("flyer"));
        QCOMPARE(fh1::activities::iconCategory(activities[2], QStringLiteral("FR08")), QStringLiteral("race"));
        QCOMPARE(
            fh1::activities::iconCategory(activities[2], QStringLiteral("NEM_FINAL")), QStringLiteral("nemesisrace"));
        QCOMPARE(fh1::activities::categoryTitle(QStringLiteral("race"), {}), QStringLiteral("Race events"));
        QCOMPARE(fh1::activities::categoryTitle({}, QStringLiteral("ActivityFestivalEntrance")),
            QStringLiteral("Festival entrances"));

        QVERIFY_THROWS_EXCEPTION(fh1::LoadError, fh1::activities::parse("<A><Activity>", QStringLiteral("x")));
    }

    void buildsIconsFromBaseSymbolizers()
    {
        // A 2x2 sheet: red, green / blue, white cells.
        QImage sheet(8, 8, QImage::Format_ARGB32);
        QPainter painter(&sheet);
        painter.fillRect(0, 0, 4, 4, Qt::red);
        painter.fillRect(4, 0, 4, 4, Qt::green);
        painter.fillRect(0, 4, 4, 4, Qt::blue);
        painter.fillRect(4, 4, 4, 4, Qt::white);
        painter.end();
        const QByteArray profile = R"(<MapRenderProfiles><Profile>
            <Group name="race">
                <Symbolizer type="icon_up" sort_order="1">
                    <Atlas x="0" y="0" x_slots="2" y_slots="2" />
                    <Texture value="horizon\map\icons\MapIcons\EN\MapIconSheet.tga" />
                </Symbolizer>
                <Symbolizer type="icon_up_career_finish" sort_order="9">
                    <Atlas x="1" y="1" x_slots="2" y_slots="2" />
                    <Texture value="horizon\map\icons\MapIcons\EN\MapIconSheet.tga" />
                </Symbolizer>
                <Symbolizer type="icon_up_career" sort_order="5">
                    <Atlas x="1" y="0" x_slots="2" y_slots="2" />
                    <Texture value="horizon\map\icons\MapIcons\EN\MapIconSheet.tga" />
                </Symbolizer>
                <Symbolizer type="icon_up" sort_order="0">
                    <Texture value="horizon\map\icons\smoke.tga" />
                </Symbolizer>
                <Filter tag="activity_type" value="race" />
            </Group>
            <Group name="roads"><Filter tag="road_type" value="dirt" /></Group>
        </Profile></MapRenderProfiles>)";
        const QHash<QString, QImage> icons = fh1::activities::buildIcons(profile, sheet);
        QCOMPARE(icons.keys(), QStringList{QStringLiteral("race")});
        const QImage icon = icons.value(QStringLiteral("race"));
        QCOMPARE(icon.size(), QSize(4, 4));
        // The career layer (green, order 5) covers the base (red, order 1);
        // the finish trophy overlay (white) is not part of the icon.
        QCOMPARE(QColor(icon.pixel(2, 2)), QColor(Qt::green));
    }
};

QTEST_MAIN(TestLabels)
#include "tst_labels.moc"
