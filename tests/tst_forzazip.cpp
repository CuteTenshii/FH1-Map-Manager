#include "ForzaZip.h"
#include "LzxTestEncoder.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <zlib.h>

namespace {

struct TestEntry {
    QString name;
    QByteArray content;
    std::uint16_t method = 0;
    /// Forza layout: no local header, data offset in extra field 0x1123.
    bool forzaLayout = true;
    bool corruptCrc = false;
};

void appendLe16(QByteArray& out, std::uint16_t value)
{
    char bytes[2];
    qToLittleEndian(value, bytes);
    out.append(bytes, 2);
}

void appendLe32(QByteArray& out, std::uint32_t value)
{
    char bytes[4];
    qToLittleEndian(value, bytes);
    out.append(bytes, 4);
}

QByteArray compress(const QByteArray& content)
{
    const std::vector<std::uint8_t> data(content.begin(), content.end());
    const auto stream = lzxtest::encode(data, {{lzxtest::BlockKind::Verbatim, data.size()}});
    return QByteArray(reinterpret_cast<const char*>(stream.data()), static_cast<qsizetype>(stream.size()));
}

/// Writes a zip archive in the mix of layouts Forza uses.
QByteArray buildArchive(const QList<TestEntry>& entries)
{
    QByteArray file;
    QByteArray directory;
    for (const TestEntry& entry : entries) {
        const QByteArray stored = entry.method == 21 ? compress(entry.content) : entry.content;
        std::uint32_t crc = static_cast<std::uint32_t>(crc32(
            0L, reinterpret_cast<const Bytef*>(entry.content.constData()), static_cast<uInt>(entry.content.size())));
        if (entry.corruptCrc) {
            crc ^= 1u;
        }
        const QByteArray name = entry.name.toUtf8();
        const auto headerOffset = static_cast<std::uint32_t>(file.size());
        if (!entry.forzaLayout) {
            appendLe32(file, 0x04034b50);
            appendLe16(file, 20);
            appendLe16(file, 0);
            appendLe16(file, entry.method);
            appendLe32(file, 0);
            appendLe32(file, crc);
            appendLe32(file, static_cast<std::uint32_t>(stored.size()));
            appendLe32(file, static_cast<std::uint32_t>(entry.content.size()));
            appendLe16(file, static_cast<std::uint16_t>(name.size()));
            appendLe16(file, 0);
            file.append(name);
        }
        const auto dataOffset = static_cast<std::uint32_t>(file.size());
        file.append(stored);

        QByteArray extra;
        if (entry.forzaLayout) {
            appendLe16(extra, 0x1123);
            appendLe16(extra, 4);
            appendLe32(extra, dataOffset);
        }
        appendLe32(directory, 0x02014b50);
        appendLe16(directory, 20);
        appendLe16(directory, 20);
        appendLe16(directory, 0);
        appendLe16(directory, entry.method);
        appendLe32(directory, 0);
        appendLe32(directory, crc);
        appendLe32(directory, static_cast<std::uint32_t>(stored.size()));
        appendLe32(directory, static_cast<std::uint32_t>(entry.content.size()));
        appendLe16(directory, static_cast<std::uint16_t>(name.size()));
        appendLe16(directory, static_cast<std::uint16_t>(extra.size()));
        appendLe16(directory, 0);
        appendLe16(directory, 0);
        appendLe16(directory, 0);
        appendLe32(directory, 0);
        appendLe32(directory, headerOffset);
        directory.append(name);
        directory.append(extra);
    }
    const auto directoryOffset = static_cast<std::uint32_t>(file.size());
    file.append(directory);
    appendLe32(file, 0x06054b50);
    appendLe16(file, 0);
    appendLe16(file, 0);
    appendLe16(file, static_cast<std::uint16_t>(entries.size()));
    appendLe16(file, static_cast<std::uint16_t>(entries.size()));
    appendLe32(file, static_cast<std::uint32_t>(directory.size()));
    appendLe32(file, directoryOffset);
    appendLe16(file, 0);
    return file;
}

QString writeArchive(const QTemporaryDir& dir, const QList<TestEntry>& entries)
{
    QString path = dir.filePath(QStringLiteral("test.zip"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    file.write(buildArchive(entries));
    return path;
}

} // namespace

class TestForzaZip : public QObject {
    Q_OBJECT

private slots:
    void readsAllLayouts()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray text("route data, route data, route data!");
        QByteArray big(70000, 'x');
        for (int i = 0; i < big.size(); i += 7) {
            big[i] = static_cast<char>('a' + (i % 5));
        }
        const QString path = writeArchive(dir,
            {
                {QStringLiteral("Tracks/Colorado/Info.TXT"), text, 0, true},
                {QStringLiteral("big.bin"), big, 21, true},
                {QStringLiteral("classic.txt"), text, 0, false},
            });
        fh1::ForzaZip zip;
        QVERIFY2(zip.open(path), qPrintable(zip.errorString()));
        QCOMPARE(zip.entries().size(), std::size_t{3});

        QString error;
        QCOMPARE(zip.read(QStringLiteral("tracks\\colorado\\info.txt"), &error), text);
        QCOMPARE(zip.read(QStringLiteral("BIG.bin"), &error), big);
        QCOMPARE(zip.read(QStringLiteral("classic.txt"), &error), text);
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    void listsByPrefix()
    {
        QTemporaryDir dir;
        const QString path = writeArchive(dir,
            {
                {QStringLiteral("colorado/Ribbon_00/route_002.owt"), "a"},
                {QStringLiteral("colorado/Ribbon_00/route_003.owt"), "b"},
                {QStringLiteral("testbed/Ribbon_00/route_001.owt"), "c"},
            });
        fh1::ForzaZip zip;
        QVERIFY(zip.open(path));
        QCOMPARE(zip.list(QStringLiteral("COLORADO/")).size(), 2);
        QCOMPARE(zip.list().size(), 3);
        QVERIFY(zip.find(QStringLiteral("missing.txt")) == nullptr);
    }

    void detectsCrcMismatch()
    {
        QTemporaryDir dir;
        const QString path = writeArchive(dir, {{QStringLiteral("bad.txt"), "content", 0, true, true}});
        fh1::ForzaZip zip;
        QVERIFY(zip.open(path));
        QString error;
        QVERIFY(zip.read(QStringLiteral("bad.txt"), &error).isNull());
        QVERIFY2(error.contains(QLatin1String("CRC")), qPrintable(error));
    }

    void reportsMissingEntry()
    {
        QTemporaryDir dir;
        const QString path = writeArchive(dir, {{QStringLiteral("a.txt"), "a"}});
        fh1::ForzaZip zip;
        QVERIFY(zip.open(path));
        QString error;
        QVERIFY(zip.read(QStringLiteral("b.txt"), &error).isNull());
        QVERIFY(!error.isEmpty());
    }

    void rejectsNonZip()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("not.zip"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(100, 'z'));
        file.close();
        fh1::ForzaZip zip;
        QVERIFY(!zip.open(path));
        QVERIFY(!zip.errorString().isEmpty());
        QVERIFY(!zip.open(dir.filePath(QStringLiteral("does-not-exist.zip"))));
    }
};

QTEST_APPLESS_MAIN(TestForzaZip)
#include "tst_forzazip.moc"
