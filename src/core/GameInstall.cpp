#include "GameInstall.h"

#include <QFileInfo>

namespace fh1 {

QString GameInstall::findChild(const QDir& dir, const QString& name)
{
    QString direct = dir.filePath(name);
    if (QFileInfo::exists(direct)) {
        return direct;
    }
    const QStringList entries = dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QString& entry : entries) {
        if (entry.compare(name, Qt::CaseInsensitive) == 0) {
            return dir.filePath(entry);
        }
    }
    return {};
}

bool GameInstall::open(const QString& path)
{
    m_error.clear();
    const QFileInfo info(path);
    if (!info.isDir()) {
        m_error = QStringLiteral("%1 is not a folder").arg(path);
        return false;
    }

    const QDir candidate(info.absoluteFilePath());
    const auto looksLikeMedia = [](const QDir& dir) {
        return !findChild(dir, QStringLiteral("tracks")).isEmpty() && !findChild(dir, QStringLiteral("db")).isEmpty();
    };

    if (looksLikeMedia(candidate)) {
        m_media = candidate;
        return true;
    }
    const QString media = findChild(candidate, QStringLiteral("media"));
    if (!media.isEmpty() && looksLikeMedia(QDir(media))) {
        m_media = QDir(media);
        return true;
    }
    m_error = QStringLiteral("%1 does not look like a Forza Horizon disc: expected a media folder "
                             "containing tracks and db")
                  .arg(path);
    return false;
}

QString GameInstall::resolve(const QString& relativePath) const
{
    QString current = m_media.absolutePath();
    const QStringList parts = QString(relativePath)
                                  .replace(QLatin1Char('\\'), QLatin1Char('/'))
                                  .split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        current = findChild(QDir(current), part);
        if (current.isEmpty()) {
            return {};
        }
    }
    return current;
}

QStringList GameInstall::trackFolders() const
{
    QStringList result;
    const QString tracks = findChild(m_media, QStringLiteral("tracks"));
    if (tracks.isEmpty()) {
        return result;
    }
    const QDir tracksDir(tracks);
    const QStringList folders = tracksDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    for (const QString& folder : folders) {
        if (!findChild(QDir(tracksDir.filePath(folder)), QStringLiteral("Ribbon_00")).isEmpty()) {
            result.append(folder);
        }
    }
    return result;
}

QString GameInstall::trackPvsPath(const QString& track) const
{
    return trackRibbonFile(track, QStringLiteral("pvs"));
}

QString GameInstall::trackZoneGridPath(const QString& track) const
{
    return trackRibbonFile(track, QStringLiteral("hex"));
}

QString GameInstall::trackRibbonFile(const QString& track, const QString& extension) const
{
    const QString ribbon = resolve(QStringLiteral("tracks/%1/Ribbon_00").arg(track));
    if (ribbon.isEmpty()) {
        return {};
    }
    QString named = resolve(QStringLiteral("tracks/%1/Ribbon_00/%1_00.%2").arg(track, extension));
    if (!named.isEmpty()) {
        return named;
    }
    const QStringList files = QDir(ribbon).entryList({QStringLiteral("*.%1").arg(extension)}, QDir::Files, QDir::Name);
    return files.isEmpty() ? QString() : QDir(ribbon).filePath(files.first());
}

} // namespace fh1
