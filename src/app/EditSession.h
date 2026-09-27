#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class QUndoCommand;
class QUndoGroup;
class QUndoStack;
class QWidget;

/// The files of a track edited in the viewer (route files, GameObjs.xml,
/// gamedb.slt): an undo history for each, which have unsaved edits, and
/// saving them where the user chooses, the game folder or another (see
/// fh1::routeOutputPath). Saving over the game's own files first copies
/// each original to backupFolder().
class EditSession : public QObject {
    Q_OBJECT

public:
    /// Gives a file's contents for saving, or nothing with `error` set.
    using Contents = std::function<std::optional<QByteArray>(QString* error)>;

    /// Dialogs open over `window`.
    explicit EditSession(QWidget* window);

    /// Undo and Redo act on the history of the file edited last.
    QUndoGroup* undoGroup() const { return m_group; }

    /// Forgets every file and its history; the game's files lie under
    /// `gameMediaPath`.
    void reset(const QString& gameMediaPath);
    /// Registers the file `key`, at `mediaPath` under the media folder,
    /// named `label` in the edit history, whose contents `contents` gives.
    /// Registering it again keeps its history.
    void addFile(const QString& key, const QString& label, const QString& mediaPath, Contents contents);
    /// Adds `command` to the history of file `key` and makes it the one
    /// Undo and Redo act on.
    void push(const QString& key, QUndoCommand* command);
    /// Makes file `key` the one Undo and Redo act on; empty for none. A
    /// file not registered yet leaves it as it is.
    void setActiveFile(const QString& key);

    bool isModified(const QString& key) const;
    /// Keys of every registered file, sorted.
    QStringList files() const;
    /// The history of file `key`, or nullptr.
    QUndoStack* history(const QString& key) const;
    /// How the edit history names file `key`.
    QString label(const QString& key) const;
    /// Keys of the files with unsaved edits.
    QStringList modifiedFiles() const;

    /// Where the game's own files are copied before a save replaces them,
    /// under their media paths.
    static QString backupFolder();

    /// Where edited files are saved; empty until the user chooses.
    QString outputFolder() const;
    /// Asks the user where to save. Returns false if they cancel.
    bool chooseOutputFolder();

    /// Saves file `key`, asking where first if that is not set yet.
    /// Returns false if it was not saved.
    bool save(const QString& key);
    /// Saves every file with unsaved edits.
    bool saveAll();
    /// Offers to save unsaved edits before they are lost. Returns false if
    /// the user cancels.
    bool maybeSave();

signals:
    /// A file gained or lost unsaved edits.
    void modifiedChanged();
    /// A file was saved to `path`.
    void saved(const QString& path);
    /// An edit was made, undone or redone, a file was saved, or the files
    /// were forgotten.
    void historyChanged();

private:
    struct File {
        QString label;
        QString mediaPath;
        Contents contents;
        QUndoStack* stack = nullptr;
    };

    QWidget* m_window;
    QUndoGroup* m_group;
    QString m_gameMediaPath;
    QHash<QString, File> m_files;
};
