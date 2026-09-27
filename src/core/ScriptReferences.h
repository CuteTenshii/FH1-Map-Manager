#pragma once

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

namespace fh1 {

/// The names the game's scripts use (the attribute values of the XML
/// configs in gamemodes.zip), with the files that use each: the objects
/// the sat nav points at, the cutscenes and animations of a race, the
/// hubs that start races. Deleting something they name can stop the game
/// working; a deleted opening race froze it at the start.
class ScriptReferences {
public:
    /// Adds the attribute values of `xml`, the config `file`. Data that is
    /// not well-formed XML adds what was read before the error.
    void addFile(const QString& file, const QByteArray& xml);

    /// The files that use `name`, ignoring case: as a whole value, or as the
    /// end of one after an underscore ("prerace_intro_FESTIVAL_SECOND" uses
    /// "FESTIVAL_SECOND"). Sorted; empty when none does.
    QStringList filesUsing(const QString& name) const;

    bool isEmpty() const { return m_values.isEmpty(); }

private:
    /// Upper-cased value -> the files using it.
    QHash<QString, QSet<QString>> m_values;
};

} // namespace fh1
