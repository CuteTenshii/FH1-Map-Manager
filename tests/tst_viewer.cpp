#include "FeatureTableModel.h"
#include "LayerItem.h"
#include "MainWindow.h"
#include "MapView.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QGraphicsScene>
#include <QMessageBox>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>

#include <functional>

namespace {

fh1::Feature point(const QString& name, const QString& group, float x, float z)
{
    fh1::Feature feature;
    feature.name = name;
    feature.group = group;
    feature.position = QVector3D(x, 0.0F, z);
    return feature;
}

/// Answers the next modal message box by clicking the button labelled
/// `button`, as soon as the box is open.
void answerNextMessageBox(const QString& button)
{
    auto* poll = new QTimer(qApp);
    QObject::connect(poll, &QTimer::timeout, poll, [poll, button] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box == nullptr) {
            return;
        }
        for (QAbstractButton* candidate : box->buttons()) {
            if (candidate->text().remove(QLatin1Char('&')) == button) {
                poll->deleteLater();
                candidate->click();
                return;
            }
        }
    });
    poll->start(10);
}

/// 1 image pixel per metre, Z not flipped: scene coordinates equal world X/Z.
const fh1::MapCalibration kIdentity{1.0, 0.0, 1.0, 0.0};

} // namespace

class TestViewer : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Keeps the cache test away from the user's own cache.
        QStandardPaths::setTestModeEnabled(true);
    }

    void clearCacheDeletesWorldIndexes()
    {
        const QString directory
            = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/world");
        QVERIFY(QDir().mkpath(directory));
        const QStringList indexes{QStringLiteral("colorado.index"), QStringLiteral("coloradodirt.index")};
        for (const QString& name : indexes + QStringList{QStringLiteral("notes.txt")}) {
            QFile file(QDir(directory).filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("cached");
        }

        MainWindow window;
        QAction* clearCache = nullptr;
        for (QAction* action : window.findChildren<QAction*>()) {
            if (action->text().startsWith(QLatin1String("Clear &Cache"))) {
                clearCache = action;
            }
        }
        QVERIFY(clearCache != nullptr);

        // Cancelling keeps everything.
        answerNextMessageBox(QStringLiteral("Cancel"));
        clearCache->trigger();
        for (const QString& name : indexes) {
            QVERIFY(QFile::exists(QDir(directory).filePath(name)));
        }

        answerNextMessageBox(QStringLiteral("Delete"));
        clearCache->trigger();
        for (const QString& name : indexes) {
            QVERIFY(!QFile::exists(QDir(directory).filePath(name)));
        }
        // Only index files are the viewer's to delete.
        QVERIFY(QFile::exists(QDir(directory).filePath(QStringLiteral("notes.txt"))));

        // With nothing cached the action says so and deletes nothing.
        answerNextMessageBox(QStringLiteral("OK"));
        clearCache->trigger();
        QVERIFY(QFile::remove(QDir(directory).filePath(QStringLiteral("notes.txt"))));
    }

    void pointBoundsCoverEveryPoint()
    {
        // Regression: bounds built with QRectF::united() collapsed to the last
        // point, which hid every other marker once the view zoomed in.
        fh1::Layer layer;
        layer.features = {point("a", "g", -500, -300), point("b", "g", 800, 20), point("c", "g", 10, 900)};
        PointLayerItem item(layer, kIdentity, 0, 3.0);
        const QRectF bounds = item.boundingRect();
        QVERIFY(bounds.contains(QPointF(-500, -300)));
        QVERIFY(bounds.contains(QPointF(800, 20)));
        QVERIFY(bounds.contains(QPointF(10, 900)));
        for (int i = 0; i < 3; ++i) {
            const auto hit = item.hitTest(item.featureBounds(i).center(), 1.0);
            QVERIFY(hit.has_value());
            QCOMPARE(hit->feature, i);
        }
    }

    void pointHitTestPicksNearestShown()
    {
        fh1::Layer layer;
        layer.features
            = {point("near", "shown", 10, 10), point("nearer", "hidden", 10, 11), point("far", "shown", 50, 50)};
        PointLayerItem item(layer, kIdentity, 0, 3.0);

        auto hit = item.hitTest(QPointF(10, 11.2), 5.0);
        QVERIFY(hit.has_value());
        QCOMPARE(hit->feature, 1);

        item.setGroupVisible(item.groupIndex(QStringLiteral("hidden")), false);
        hit = item.hitTest(QPointF(10, 11.2), 5.0);
        QVERIFY(hit.has_value());
        QCOMPARE(hit->feature, 0);

        QVERIFY(!item.hitTest(QPointF(30, 30), 5.0).has_value());
    }

    void groupsAreOrderedBySize()
    {
        fh1::Layer layer;
        layer.features = {point("1", "small", 0, 0), point("2", "big", 0, 0), point("3", "big", 0, 0)};
        PointLayerItem item(layer, kIdentity, 0, 3.0);
        QCOMPARE(item.groups(), (QStringList{QStringLiteral("big"), QStringLiteral("small")}));
        QCOMPARE(item.groupSize(0), 2);
        QVERIFY(item.groupColor(0) != item.groupColor(1));
    }

    void polylineAndPolygonHitTests()
    {
        fh1::Layer lines;
        lines.kind = fh1::FeatureKind::Polyline;
        fh1::Feature line = point("line", "g", 0, 0);
        line.shapes = {{QVector3D(0, 0, 0), QVector3D(100, 0, 0)}};
        lines.features = {line};
        ShapeLayerItem lineItem(lines, kIdentity, 0);
        auto hit = lineItem.hitTest(QPointF(50, 3), 5.0);
        QVERIFY(hit.has_value());
        QVERIFY(std::abs(hit->distance - 3.0) < 1e-9);
        QVERIFY(!lineItem.hitTest(QPointF(50, 30), 5.0).has_value());

        fh1::Layer zones;
        zones.kind = fh1::FeatureKind::Polygon;
        fh1::Feature zone = point("zone", "g", 5, 5);
        zone.shapes = {{QVector3D(0, 0, 0), QVector3D(10, 0, 0), QVector3D(0, 0, 10)},
            {QVector3D(10, 0, 0), QVector3D(10, 0, 10), QVector3D(0, 0, 10)}};
        zones.features = {zone};
        ShapeLayerItem zoneItem(zones, kIdentity, 0);
        // A click deep inside a zone ranks at the tolerance, so nearby markers win.
        hit = zoneItem.hitTest(QPointF(5, 5), 2.0);
        QVERIFY(hit.has_value());
        QCOMPARE(hit->distance, 2.0);
        // The shared diagonal of the two triangles is not an edge of the zone.
        hit = zoneItem.hitTest(QPointF(5.2, 5.2), 2.0);
        QVERIFY(hit.has_value());
        QCOMPARE(hit->distance, 2.0);
        QVERIFY(!zoneItem.hitTest(QPointF(20, 20), 2.0).has_value());
    }

    void tableListsTopLayerFirst()
    {
        fh1::MapData map;
        fh1::Layer bottom;
        bottom.title = QStringLiteral("Bottom");
        bottom.features = {point("b0", "g", 0, 0), point("b1", "g", 0, 0)};
        fh1::Layer top;
        top.title = QStringLiteral("Top");
        top.features = {point("t0", "g", 1.5F, 2.25F)};
        top.features[0].label = QStringLiteral("Oakley Blitz");
        map.layers = {bottom, top};

        FeatureTableModel model;
        model.setMap(&map);
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.data(model.index(0, FeatureTableModel::Name), Qt::DisplayRole).toString(),
            QStringLiteral("Oakley Blitz"));
        QCOMPARE(model.data(model.index(0, FeatureTableModel::Id), Qt::DisplayRole).toString(), QStringLiteral("t0"));
        QCOMPARE(model.data(model.index(1, FeatureTableModel::Name), Qt::DisplayRole).toString(), QStringLiteral("b0"));
        QCOMPARE(model.data(model.index(0, FeatureTableModel::X), Qt::DisplayRole).toString(), QStringLiteral("1.5"));
        QCOMPARE(model.data(model.index(0, FeatureTableModel::Z), Qt::EditRole).toDouble(), 2.25);
        const FeatureTableModel::Location location = model.locationAt(2);
        QCOMPARE(location.layer, 0);
        QCOMPARE(location.feature, 1);
        QCOMPARE(model.rowOf(0, 1), 2);
        QCOMPARE(model.rowOf(1, 0), 0);
        QCOMPARE(model.locationAt(3).layer, -1);

        model.setMap(nullptr);
        QCOMPARE(model.rowCount(), 0);
    }

    void viewClickVersusDrag()
    {
        QGraphicsScene scene(0, 0, 1000, 1000);
        MapView view;
        view.setScene(&scene);
        view.resize(400, 400);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.fitScene();

        QSignalSpy clicks(&view, &MapView::clicked);
        const QPoint centre = view.viewport()->rect().center();
        QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, centre);
        QCOMPARE(clicks.count(), 1);
        const QPointF scenePos = clicks.first().first().toPointF();
        QVERIFY((scenePos - view.mapToScene(centre)).manhattanLength() < 1e-6);

        QTest::mousePress(view.viewport(), Qt::LeftButton, {}, centre);
        QTest::mouseMove(view.viewport(), centre + QPoint(40, 0));
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, {}, centre + QPoint(40, 0));
        QCOMPARE(clicks.count(), 1);
    }

    void viewZoomIsClamped()
    {
        QGraphicsScene scene(0, 0, 1000, 1000);
        MapView view;
        view.setScene(&scene);
        view.resize(400, 400);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.fitScene();
        const double fitted = view.zoom();
        for (int i = 0; i < 50; ++i) {
            view.zoomBy(2.0);
        }
        QCOMPARE(view.zoom(), 16.0);
        for (int i = 0; i < 50; ++i) {
            view.zoomBy(0.5);
        }
        QVERIFY(view.zoom() < fitted);
        QVERIFY(view.zoom() >= fitted * 0.49);

        view.focusOn(QRectF(100, 100, 0, 0), 4.0);
        QCOMPARE(view.zoom(), 4.0);
        QVERIFY((view.mapToScene(view.viewport()->rect().center()) - QPointF(100, 100)).manhattanLength() < 1.0);
    }
};

QTEST_MAIN(TestViewer)
#include "tst_viewer.moc"
