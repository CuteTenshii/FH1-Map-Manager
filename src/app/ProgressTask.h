#pragma once

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QProgressDialog>
#include <QString>
#include <QtConcurrent/QtConcurrentRun>

#include <atomic>
#include <functional>

/// Tells a progress dialog how far work on a worker thread has got: `done`
/// of `total`, or a busy indicator while `total` is 0. May be called from
/// any thread.
using ProgressReport = std::function<void(qint64 done, qint64 total)>;

/// Runs `work` on a worker thread and returns its result, while a modal
/// progress dialog over `parent` shows `label` and what `work` reports. The
/// window keeps repainting but takes no input until the work is done, so
/// the work may read what the window holds without it changing.
template <typename Result>
Result runWithProgress(QWidget* parent, const QString& label, const std::function<Result(const ProgressReport&)>& work)
{
    QProgressDialog dialog(label, QString(), 0, 0, parent);
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setCancelButton(nullptr);
    dialog.setMinimumDuration(0);
    dialog.setAutoClose(false);
    dialog.setAutoReset(false);
    dialog.show();

    // Reports arrive far more often than the dialog can show them; only a
    // change of percentage is passed on, queued to the dialog's thread.
    std::atomic<int> shown{-1};
    const ProgressReport report = [&dialog, &shown](qint64 done, qint64 total) {
        const int percent = total > 0 ? static_cast<int>(done * 100 / total) : -1;
        if (shown.exchange(percent) == percent) {
            return;
        }
        QMetaObject::invokeMethod(
            &dialog,
            [&dialog, percent] {
                dialog.setRange(0, percent < 0 ? 0 : 100);
                dialog.setValue(percent < 0 ? 0 : percent);
            },
            Qt::QueuedConnection);
    };
    QFutureWatcher<Result> watcher;
    watcher.setFuture(QtConcurrent::run([&work, &report] { return work(report); }));
    // The watcher's finished signal is an event, which wakes this loop.
    while (!watcher.isFinished()) {
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents);
    }
    return watcher.result();
}
