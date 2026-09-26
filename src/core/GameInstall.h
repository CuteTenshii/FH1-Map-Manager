#pragma once

#include <QDir>
#include <QString>
#include <QStringList>

namespace fh1 {

/// Locates files in an extracted Forza Horizon disc. Paths in the game are
/// case-insensitive while extracted copies keep whatever case the extraction
/// tool produced, so every lookup here matches names case-insensitively.
class GameInstall {
public:
    /// Accepts the disc root (the folder holding default.xex and media) or the
    /// media folder itself. On failure returns false and sets errorString().
    bool open(const QString& path);

    QString mediaPath() const { return m_media.absolutePath(); }
    QString errorString() const { return m_error; }

    /// Resolves a path relative to the media folder, such as
    /// "tracks/colorado/Ribbon_00/CollObjs.xml". Returns an empty string if any
    /// component is missing.
    QString resolve(const QString& relativePath) const;

    /// Track folders under media/tracks that have ribbon data.
    QStringList trackFolders() const;

    /// Case-insensitive lookup of one child name inside `dir`.
    static QString findChild(const QDir& dir, const QString& name);

private:
    QDir m_media;
    QString m_error;
};

} // namespace fh1
