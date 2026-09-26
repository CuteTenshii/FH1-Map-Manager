#include "ForzaZip.h"

#include <QCoreApplication>
#include <QFile>
#include <QTextStream>

namespace {

int usage(QTextStream& err)
{
    err << "usage:\n"
           "  fh1zip list <archive> [prefix]\n"
           "  fh1zip extract <archive> <entry> <output-file>\n"
           "  fh1zip verify <archive> [prefix]\n";
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);
    const QStringList args = app.arguments();
    if (args.size() < 3) {
        return usage(err);
    }

    const QString& command = args.at(1);
    fh1::ForzaZip zip;
    if (!zip.open(args.at(2))) {
        err << zip.errorString() << '\n';
        return 1;
    }

    if (command == QLatin1String("list")) {
        const QString prefix = args.size() > 3 ? args.at(3) : QString();
        const QString normalizedPrefix = fh1::ForzaZip::normalizeName(prefix);
        for (const fh1::ZipEntry& entry : zip.entries()) {
            if (!normalizedPrefix.isEmpty() && !fh1::ForzaZip::normalizeName(entry.name).startsWith(normalizedPrefix)) {
                continue;
            }
            out << entry.uncompressedSize << '\t' << entry.compressedSize << '\t' << entry.method << '\t' << entry.name
                << '\n';
        }
        return 0;
    }

    if (command == QLatin1String("extract")) {
        if (args.size() < 5) {
            return usage(err);
        }
        QString error;
        const QByteArray data = zip.read(args.at(3), &error);
        if (data.isNull()) {
            err << error << '\n';
            return 1;
        }
        QFile file(args.at(4));
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
            err << "cannot write " << args.at(4) << ": " << file.errorString() << '\n';
            return 1;
        }
        return 0;
    }

    if (command == QLatin1String("verify")) {
        const QString normalizedPrefix = fh1::ForzaZip::normalizeName(args.size() > 3 ? args.at(3) : QString());
        int failures = 0;
        int checked = 0;
        for (const fh1::ZipEntry& entry : zip.entries()) {
            if (!normalizedPrefix.isEmpty() && !fh1::ForzaZip::normalizeName(entry.name).startsWith(normalizedPrefix)) {
                continue;
            }
            QString error;
            ++checked;
            if (zip.read(entry, &error).isNull()) {
                ++failures;
                err << "FAIL " << error << '\n';
                err.flush();
            }
        }
        out << checked << " checked, " << failures << " failed\n";
        return failures == 0 ? 0 : 1;
    }

    return usage(err);
}
