#include "EditHistoryPanel.h"

#include "EditSession.h"

#include <QHeaderView>
#include <QLabel>
#include <QSet>
#include <QTreeWidget>
#include <QUndoStack>
#include <QVBoxLayout>

namespace {

constexpr int kFileRole = Qt::UserRole;
constexpr int kEditRole = Qt::UserRole + 1;

} // namespace

EditHistoryPanel::EditHistoryPanel(EditSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
    , m_tree(new QTreeWidget)
    , m_summary(new QLabel)
{
    m_tree->setHeaderLabels({tr("Edit"), tr("State")});
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->setUniformRowHeights(true);
    m_tree->setToolTip(tr("Double-click an edit to undo or redo its file's edits back to it"));
    connect(m_tree, &QTreeWidget::itemActivated, this, &EditHistoryPanel::onItemActivated);
    m_summary->setWordWrap(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(m_tree);
    layout->addWidget(m_summary);

    connect(m_session, &EditSession::historyChanged, this, &EditHistoryPanel::refresh);
    refresh();
}

void EditHistoryPanel::refresh()
{
    // Keep the files the user opened open.
    QSet<QString> collapsed;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        if (!m_tree->topLevelItem(i)->isExpanded()) {
            collapsed.insert(m_tree->topLevelItem(i)->data(0, kFileRole).toString());
        }
    }
    m_tree->clear();
    int edits = 0;
    int files = 0;
    int unsaved = 0;
    for (const QString& key : m_session->files()) {
        const QUndoStack* history = m_session->history(key);
        if (history == nullptr || history->count() == 0) {
            continue;
        }
        ++files;
        edits += history->count();
        const bool modified = !history->isClean();
        unsaved += modified ? 1 : 0;
        auto* file = new QTreeWidgetItem(m_tree, {m_session->label(key), modified ? tr("Not saved") : tr("Saved")});
        file->setData(0, kFileRole, key);
        QFont bold = file->font(0);
        bold.setBold(true);
        file->setFont(0, bold);
        for (int i = 0; i < history->count(); ++i) {
            const bool done = i < history->index();
            QString state = done ? tr("Done") : tr("Undone");
            // The clean index counts the edits in the saved file.
            if (history->cleanIndex() == i + 1) {
                state = tr("%1, saved").arg(state);
            }
            auto* edit = new QTreeWidgetItem(file, {history->text(i), state});
            edit->setData(0, kFileRole, key);
            edit->setData(0, kEditRole, i);
            if (!done) {
                for (int column = 0; column < 2; ++column) {
                    edit->setForeground(column, palette().color(QPalette::Disabled, QPalette::Text));
                }
            }
        }
        file->setExpanded(!collapsed.contains(key));
    }
    if (edits == 0) {
        m_summary->setText(tr("No edits yet. Edits to routes, gameplay objects and race settings are listed here."));
    } else {
        m_summary->setText(tr("%n edit(s) in %1; %2", nullptr, edits)
                .arg(tr("%n file(s)", nullptr, files),
                    unsaved == 0 ? tr("all saved") : tr("%n not saved", nullptr, unsaved)));
    }
}

void EditHistoryPanel::onItemActivated(QTreeWidgetItem* item)
{
    const QVariant edit = item->data(0, kEditRole);
    QUndoStack* history = m_session->history(item->data(0, kFileRole).toString());
    if (!edit.isValid() || history == nullptr) {
        return;
    }
    history->setIndex(edit.toInt() + 1);
}
