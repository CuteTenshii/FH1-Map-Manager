#pragma once

#include <QWidget>

class EditSession;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

/// Lists every edit of an EditSession, grouped by the file it changes:
/// which are done and which undone, and where each file was last saved.
/// Double-clicking an edit undoes or redoes that file's edits back to the
/// state right after it.
class EditHistoryPanel : public QWidget {
    Q_OBJECT

public:
    explicit EditHistoryPanel(EditSession* session, QWidget* parent = nullptr);

    /// Lists the edits as they are now.
    void refresh();

private:
    void onItemActivated(QTreeWidgetItem* item);

    EditSession* m_session;
    QTreeWidget* m_tree;
    QLabel* m_summary;
};
