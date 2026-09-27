#include "ModelRemoval.h"

#include "ScatterSet.h"
#include "TrackPlacements.h"

#include <QSet>

#include <algorithm>

namespace fh1 {

namespace {

void setError(QString* error, const QString& message)
{
    if (error != nullptr) {
        *error = message;
    }
}

/// The entries that are copies of entry `entry`: the same name ignoring
/// case and slashes, and the same contents. Entry `entry` included.
std::vector<std::uint32_t> copiesOf(const ForzaZip& archive, std::uint32_t entry)
{
    const std::vector<ZipEntry>& entries = archive.entries();
    const ZipEntry& original = entries[entry];
    const QString name = ForzaZip::normalizeName(original.name);
    std::vector<std::uint32_t> copies;
    for (std::uint32_t i = 0; i < entries.size(); ++i) {
        if (entries[i].crc32 == original.crc32 && entries[i].uncompressedSize == original.uncompressedSize
            && ForzaZip::normalizeName(entries[i].name) == name) {
            copies.push_back(i);
        }
    }
    return copies;
}

/// The current contents of entry `entry`: edited, or as the archive has it.
QByteArray* contentsOf(
    const ForzaZip& archive, std::uint32_t entry, QHash<std::uint32_t, QByteArray>& edited, QString* error)
{
    auto it = edited.find(entry);
    if (it == edited.end()) {
        QString readError;
        QByteArray data = archive.read(archive.entries()[entry], &readError);
        if (data.isNull()) {
            setError(error, readError);
            return nullptr;
        }
        it = edited.insert(entry, std::move(data));
    }
    return &it.value();
}

} // namespace

std::optional<ZoneRecordIndex> ZoneRecordIndex::build(const ForzaZip& archive, QString* error)
{
    ZoneRecordIndex index;
    const std::vector<ZipEntry>& entries = archive.entries();
    for (std::uint32_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].name.endsWith(QLatin1String(".pvsz"), Qt::CaseInsensitive)) {
            continue;
        }
        QString readError;
        const QByteArray data = archive.read(entries[i], &readError);
        std::vector<qsizetype> offsets;
        const auto records = data.isNull() ? std::nullopt : TrackPlacements::readZone(data, &readError, &offsets);
        if (!records) {
            setError(error, QStringLiteral("%1: %2").arg(entries[i].name, readError));
            return std::nullopt;
        }
        for (std::size_t r = 0; r < records->size(); ++r) {
            index.m_records[(*records)[r].first].emplace_back(i, offsets[r]);
        }
    }
    return index;
}

const std::vector<std::pair<std::uint32_t, qsizetype>>& ZoneRecordIndex::recordsOf(std::uint32_t draw) const
{
    static const std::vector<std::pair<std::uint32_t, qsizetype>> none;
    const auto it = m_records.constFind(draw);
    return it == m_records.cend() ? none : it.value();
}

std::vector<std::uint32_t> placedModelChunks(const WorldIndex& index, std::uint32_t chunk)
{
    const std::vector<WorldChunk>& chunks = index.chunks();
    if (chunk >= chunks.size() || !chunks[chunk].placed) {
        return {};
    }
    const WorldChunk& target = chunks[chunk];
    std::vector<std::uint32_t> result{chunk};
    for (std::uint32_t i = 0; i < chunks.size(); ++i) {
        const WorldChunk& other = chunks[i];
        if (i == chunk || !other.placed || other.scattered != target.scattered) {
            continue;
        }
        const bool same = target.scattered
            ? other.sourceEntry == target.sourceEntry && other.sourceRecord == target.sourceRecord
            : other.group == target.group && other.placement.position == target.placement.position
                && other.placement.rows == target.placement.rows;
        if (same) {
            result.push_back(i);
        }
    }
    return result;
}

bool removePlacedModel(const ForzaZip& archive, const WorldIndex& index, const ZoneRecordIndex& zones,
    std::uint32_t chunk, QHash<std::uint32_t, QByteArray>& edited, QString* error)
{
    const std::vector<std::uint32_t> model = placedModelChunks(index, chunk);
    if (model.empty()) {
        setError(error, QStringLiteral("the model is part of the world's geometry, not a placed one"));
        return false;
    }
    const WorldChunk& target = index.chunks()[chunk];
    if (target.scattered) {
        if (target.sourceEntry < 0 || target.sourceRecord < 0
            || static_cast<std::size_t>(target.sourceEntry) >= archive.entries().size()) {
            setError(error, QStringLiteral("the copy's procedural set is not known; rebuild the world index"));
            return false;
        }
        for (const std::uint32_t copy : copiesOf(archive, static_cast<std::uint32_t>(target.sourceEntry))) {
            QByteArray* data = contentsOf(archive, copy, edited, error);
            if (data == nullptr) {
                return false;
            }
            hideScatterInstance(*data, target.sourceRecord);
        }
        return true;
    }
    QSet<std::int32_t> draws;
    for (const std::uint32_t c : model) {
        if (index.chunks()[c].sourceDraw >= 0) {
            draws.insert(index.chunks()[c].sourceDraw);
        }
    }
    bool found = false;
    for (const std::int32_t draw : draws) {
        for (const auto& [entry, offset] : zones.recordsOf(static_cast<std::uint32_t>(draw))) {
            QByteArray* data = contentsOf(archive, entry, edited, error);
            if (data == nullptr) {
                return false;
            }
            TrackPlacements::hideZoneRecord(*data, offset);
            found = true;
        }
    }
    if (!found) {
        setError(error, QStringLiteral("no zone file places the model; rebuild the world index"));
    }
    return found;
}

} // namespace fh1
