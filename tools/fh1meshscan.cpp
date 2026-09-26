// Parses every render model in a track archive and reports what it finds:
// the check that the mesh format is fully understood (including the material
// table that texturing depends on), and the numbers the 3D view's
// level-of-detail choices are based on.

#include "ForzaZip.h"
#include "Loaders.h"
#include "RenderMesh.h"

#include <QCoreApplication>
#include <QFile>
#include <QTextStream>

#include <map>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);
    if (argc < 2) {
        err << "usage: fh1meshscan <bin.zip> [max-files]\n";
        return 2;
    }
    fh1::ForzaZip zip;
    if (!zip.open(QString::fromLocal8Bit(argv[1]))) {
        err << zip.errorString() << '\n';
        return 1;
    }
    long limit = -1;
    if (argc > 2) {
        bool ok = false;
        limit = QString::fromLocal8Bit(argv[2]).toLong(&ok);
        if (!ok || limit < 0) {
            err << "max-files must be a non-negative number\n";
            return 2;
        }
    }
    // FH1_MESHSCAN_TSV=<file> also writes one line per mesh for analysis.
    QFile tsvFile(qEnvironmentVariable("FH1_MESHSCAN_TSV"));
    QTextStream tsv(&tsvFile);
    const bool writeTsv = !tsvFile.fileName().isEmpty() && tsvFile.open(QIODevice::WriteOnly);

    struct LodStats {
        long files = 0;
        long long triangles = 0;
    };
    std::map<int, LodStats> byLod;
    long parsed = 0;
    long failed = 0;
    long local = 0;
    long headerMismatch = 0;
    long withoutMaterialTable = 0;
    long badMaterialIndex = 0;
    long long triangles = 0;
    std::map<std::uint32_t, long> strides;
    for (const fh1::ZipEntry& entry : zip.entries()) {
        if (!entry.name.endsWith(QLatin1String(".rmb.bin"), Qt::CaseInsensitive)) {
            continue;
        }
        if (limit >= 0 && parsed + failed >= limit) {
            break;
        }
        QString error;
        const QByteArray data = zip.read(entry, &error);
        if (data.isNull()) {
            err << error << '\n';
            ++failed;
            continue;
        }
        try {
            const fh1::RenderMesh mesh = fh1::rendermesh::parse(data, entry.name);
            const auto header = fh1::rendermesh::readHeader(data.left(fh1::rendermesh::kHeaderBytes));
            if (!header || header->firstPartName != mesh.parts.front().name) {
                ++headerMismatch;
            }
            if (mesh.materialTable.empty()) {
                if (++withoutMaterialTable <= 20) {
                    err << entry.name << ": no material table\n";
                }
            }
            for (const auto& part : mesh.parts) {
                for (const auto& material : part.materials) {
                    if (!mesh.materialTable.empty() && material.tableIndex >= mesh.materialTable.size()) {
                        ++badMaterialIndex;
                    }
                }
            }
            ++parsed;
            const std::size_t count = mesh.triangleCount();
            triangles += static_cast<long long>(count);
            const QVector3D centre = (mesh.boundsMin + mesh.boundsMax) / 2.0F;
            if (std::hypot(centre.x(), centre.z()) < 200.0F) {
                ++local;
            }
            if (writeTsv) {
                tsv << entry.name << '\t' << mesh.parts.front().name << '\t' << mesh.parts.size() << '\t' << count
                    << '\t' << mesh.boundsMin.x() << '\t' << mesh.boundsMin.y() << '\t' << mesh.boundsMin.z() << '\t'
                    << mesh.boundsMax.x() << '\t' << mesh.boundsMax.y() << '\t' << mesh.boundsMax.z() << '\n';
            }
            LodStats& lod = byLod[fh1::rendermesh::lodLevel(mesh.parts.front().name)];
            ++lod.files;
            lod.triangles += static_cast<long long>(count);
            for (const auto& part : mesh.parts) {
                ++strides[part.stride];
            }
        } catch (const fh1::LoadError& e) {
            ++failed;
            if (failed <= 20) {
                err << e.what() << '\n';
            }
        }
    }
    out << "parsed " << parsed << ", failed " << failed << ", header mismatches " << headerMismatch
        << ", near origin (local space) " << local << ", triangles " << triangles << '\n';
    out << "  without a material table " << withoutMaterialTable << ", materials outside their table "
        << badMaterialIndex << '\n';
    for (const auto& [lod, stats] : byLod) {
        out << "  LOD " << lod << ": " << stats.files << " files, " << stats.triangles << " triangles\n";
    }
    out << "  strides:";
    for (const auto& [stride, count] : strides) {
        out << ' ' << stride << '=' << count;
    }
    out << '\n';
    return failed == 0 ? 0 : 1;
}
