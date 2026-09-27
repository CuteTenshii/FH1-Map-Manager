#include "EditSession.h"

#include "RouteEditing.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QUndoGroup>
#include <QUndoStack>

#include <algorithm>
#include <utility>

namespace {

constexpr QLatin1StringView kOutputFolderKey("edit/outputFolder");

} // namespace

EditSession::EditSession(QWidget* window)
    : QObject(window)
    , m_window(window)
    , m_group(new QUndoGroup(this))
{
    // Backups were kept in "route-backups" while only routes could be
    // edited. They hold the game's originals, which a save would not copy
    // again, so they move rather than being left behind.
    const QString oldBackups
        = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/route-backups");
    if (QFileInfo::exists(oldBackups) && !QFileInfo::exists(backupFolder())) {
        QDir().rename(oldBackups, backupFolder());
    }
}

void EditSession::reset(const QString& gameMediaPath)
{
    m_group->setActiveStack(nullptr);
    for (const File& file : std::as_const(m_files)) {
        delete file.stack;
    }
    m_files.clear();
    m_gameMediaPath = gameMediaPath;
    emit modifiedChanged();
    emit historyChanged();
}

void EditSession::addFile(const QString& key, const QString& label, const QString& mediaPath, Contents contents)
{
    File& file = m_files[key];
    file.label = label;
    file.mediaPath = mediaPath;
    file.contents = std::move(contents);
    if (file.stack == nullptr) {
        file.stack = new QUndoStack(this);
        m_group->addStack(file.stack);
        connect(file.stack, &QUndoStack::cleanChanged, this, &EditSession::modifiedChanged);
        connect(file.stack, &QUndoStack::indexChanged, this, &EditSession::historyChanged);
        connect(file.stack, &QUndoStack::cleanChanged, this, &EditSession::historyChanged);
    }
}

void EditSession::addLargeFile(const QString& key, const QString& label, const QString& mediaPath, Writer writer)
{
    addFile(key, label, mediaPath, {});
    m_files[key].writer = std::move(writer);
}

void EditSession::push(const QString& key, QUndoCommand* command)
{
    const auto it = m_files.find(key);
    if (it == m_files.end()) {
        delete command;
        return;
    }
    m_group->setActiveStack(it->stack);
    it->stack->push(command);
}

void EditSession::setActiveFile(const QString& key)
{
    if (key.isEmpty()) {
        m_group->setActiveStack(nullptr);
    } else if (const auto it = m_files.constFind(key); it != m_files.cend()) {
        m_group->setActiveStack(it->stack);
    }
}

bool EditSession::isModified(const QString& key) const
{
    const auto it = m_files.constFind(key);
    return it != m_files.cend() && !it->stack->isClean();
}

QStringList EditSession::files() const
{
    QStringList keys = m_files.keys();
    keys.sort();
    return keys;
}

QUndoStack* EditSession::history(const QString& key) const
{
    const auto it = m_files.constFind(key);
    return it == m_files.cend() ? nullptr : it->stack;
}

QString EditSession::label(const QString& key) const
{
    return m_files.value(key).label;
}

QStringList EditSession::modifiedFiles() const
{
    QStringList keys;
    for (auto [key, file] : m_files.asKeyValueRange()) {
        if (!file.stack->isClean()) {
            keys.append(key);
        }
    }
    keys.sort();
    return keys;
}

QString EditSession::backupFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/backups");
}

QString EditSession::outputFolder() const
{
    return QSettings().value(kOutputFolderKey).toString();
}

bool EditSession::chooseOutputFolder()
{
    QMessageBox box(QMessageBox::Question, tr("Save Edits"), tr("Where should your edits be saved?"),
        QMessageBox::Cancel, m_window);
    box.setInformativeText(tr("Game Folder replaces the edited files in the game you opened. The originals are "
                              "kept in %1, not in the game folder.\n\n"
                              "Another Folder saves them in a folder you pick, such as a copy of the game, and "
                              "leaves the game unchanged.")
            .arg(QDir::toNativeSeparators(backupFolder())));
    QPushButton* gameButton = box.addButton(tr("Game Folder"), QMessageBox::AcceptRole);
    QPushButton* otherButton = box.addButton(tr("Another Folder…"), QMessageBox::AcceptRole);
    gameButton->setEnabled(!m_gameMediaPath.isEmpty());
    box.setDefaultButton(otherButton);
    box.exec();

    QString folder;
    if (box.clickedButton() == gameButton) {
        // A folder without a media folder of its own takes the disc's paths
        // directly (see fh1::routeOutputPath), so this lands on the game's
        // own files.
        folder = m_gameMediaPath;
    } else if (box.clickedButton() == otherButton) {
        QString start = outputFolder();
        if (start.isEmpty() && !m_gameMediaPath.isEmpty()) {
            start = QFileInfo(m_gameMediaPath).absolutePath();
        }
        folder = QFileDialog::getExistingDirectory(m_window, tr("Choose Where to Save Your Edits"), start);
    }
    if (folder.isEmpty()) {
        return false;
    }
    QSettings().setValue(kOutputFolderKey, folder);
    return true;
}

bool EditSession::save(const QString& key)
{
    const auto it = m_files.constFind(key);
    if (it == m_files.cend()) {
        return false;
    }
    if (outputFolder().isEmpty() && !chooseOutputFolder()) {
        return false;
    }
    QString error;
    const QString path = fh1::routeOutputPath(outputFolder(), it->mediaPath);
    const QString original = QDir(m_gameMediaPath).filePath(it->mediaPath);
    const QString backup = QDir(backupFolder()).filePath(it->mediaPath);
    bool written = false;
    if (it->writer) {
        // Large files take seconds: the backup of the original and the copy
        // of everything that did not change.
        const Job job = it->writer();
        written = runWithProgress<bool>(
            m_window, tr("Saving %1…").arg(QFileInfo(it->mediaPath).fileName()), [&](const ProgressReport& progress) {
                progress(0, 0);
                return fh1::prepareEditedFile(path, original, backup, &error) && job(path, &error, progress);
            });
    } else {
        const std::optional<QByteArray> contents = it->contents(&error);
        written = contents && fh1::saveEditedFile(*contents, path, original, backup, &error);
    }
    if (!written) {
        QMessageBox::warning(m_window, tr("Could not save %1").arg(QFileInfo(it->mediaPath).fileName()), error);
        return false;
    }
    it->stack->setClean();
    emit saved(path);
    return true;
}

bool EditSession::saveAll()
{
    const QStringList keys = modifiedFiles();
    return std::all_of(keys.begin(), keys.end(), [this](const QString& key) { return save(key); });
}

bool EditSession::maybeSave()
{
    const auto count = static_cast<int>(modifiedFiles().size());
    if (count == 0) {
        return true;
    }
    const QMessageBox::StandardButton answer = QMessageBox::question(m_window, tr("Unsaved Edits"),
        tr("%n file(s) have edits that are not saved. Save them before they are lost?", nullptr, count),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save) {
        return saveAll();
    }
    return answer == QMessageBox::Discard;
}
