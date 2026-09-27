#include "WorldView3D.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr float kTileSize = 500.0F;
constexpr float kMouseRadiansPerPixel = 0.0035F;
constexpr float kFastMultiplier = 5.0F;
constexpr int kUploadsPerFrame = 4;
constexpr int kTextureUploadsPerFrame = 24;
/// Below every tile job (whose priority is minus its distance), so geometry
/// comes first and textures fill in after it.
constexpr int kTexturePriority = -1'000'000;
constexpr int kTickMs = 16;
/// A press and release closer than this many pixels is a click, not a drag.
constexpr int kClickSlop = 4;
constexpr double kPickTolerance = 8.0;
/// Labels further away than this are too small to read and crowd the view.
constexpr float kLabelDistance = 2500.0F;
constexpr float kFocusMinDistance = 40.0F;
constexpr float kFocusPitch = -0.45F;

} // namespace

WorldView3D::WorldView3D(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    m_pool.setMaxThreadCount(std::max(1, QThread::idealThreadCount() - 1));
    m_ticker.setInterval(kTickMs);
    connect(&m_ticker, &QTimer::timeout, this, &WorldView3D::tick);
}

WorldView3D::~WorldView3D()
{
    ++m_epoch;
    m_pool.clear();
    m_pool.waitForDone();
    releaseGL();
}

void WorldView3D::releaseGL()
{
    if (!m_renderer.isReady() || context() == nullptr) {
        return;
    }
    makeCurrent();
    m_renderer.release();
    m_entities.release();
    doneCurrent();
}

void WorldView3D::setWorld(std::shared_ptr<const fh1::ForzaZip> archive, std::shared_ptr<const fh1::WorldIndex> index,
    std::shared_ptr<const fh1::TrackTextures> textures)
{
    clearWorld();
    m_archive = std::move(archive);
    m_index = std::move(index);
    m_textures = std::move(textures);
    m_grid = std::make_unique<fh1::WorldTileGrid>(*m_index, kTileSize, m_eventProps);
    m_inFlight.assign(m_grid->tiles().size(), -1);
    if (m_renderer.isReady()) {
        makeCurrent();
    }
    m_renderer.setGrid(m_grid.get());
    if (m_renderer.isReady()) {
        doneCurrent();
    }
    m_wasSettled = false;
    requestTiles();
    m_ticker.start();
    update();
}

void WorldView3D::setEventPropsVisible(bool visible)
{
    if (visible == m_eventProps) {
        return;
    }
    m_eventProps = visible;
    // The tile grid decides which chunks exist, so the world is reloaded
    // with a new one; the camera stays where it is.
    if (hasWorld()) {
        setWorld(m_archive, m_index, m_textures);
    }
}

void WorldView3D::clearWorld()
{
    ++m_epoch;
    m_pool.clear();
    m_ready.clear();
    m_jobsInFlight = 0;
    m_readyTextures.clear();
    m_textureJobsInFlight = 0;
    m_failedChunks = 0;
    if (m_renderer.isReady()) {
        makeCurrent();
        m_renderer.setGrid(nullptr);
        doneCurrent();
    } else {
        m_renderer.setGrid(nullptr);
    }
    m_inFlight.clear();
    m_grid.reset();
    m_index.reset();
    m_textures.reset();
    m_archive.reset();
    emit loadedFilesChanged();
    update();
}

void WorldView3D::setCamera(const Camera& camera)
{
    m_camera = camera;
    m_camera.pitch = std::clamp(m_camera.pitch, -1.55F, 1.55F);
    requestTiles();
    update();
}

void WorldView3D::lookFromAbove(float x, float z, float height)
{
    float ground = 0.0F;
    bool found = false;
    if (m_grid) {
        for (const auto& tile : m_grid->tiles()) {
            if (x >= tile.boundsMin.x() && x <= tile.boundsMax.x() && z >= tile.boundsMin.z()
                && z <= tile.boundsMax.z()) {
                // Tile bounds include far-off backdrop meshes; the lowest top
                // of the tiles under the point is the best cheap guess.
                ground = found ? std::min(ground, tile.boundsMax.y()) : tile.boundsMax.y();
                found = true;
            }
        }
    }
    Camera camera = m_camera;
    camera.position = QVector3D(x, ground + height, z);
    setCamera(camera);
}

void WorldView3D::setViewDistance(float metres)
{
    m_renderer.setViewDistance(std::clamp(metres, 500.0F, 30000.0F));
    requestTiles();
    update();
}

void WorldView3D::setEntities(std::shared_ptr<const fh1::MapData> map, const std::vector<std::vector<bool>>& visible)
{
    m_entities.setMap(std::move(map), visible);
    update();
}

void WorldView3D::setEntityGroupVisible(int layer, int group, bool visible)
{
    m_entities.setGroupVisible(layer, group, visible);
    update();
}

void WorldView3D::setHighlightedEntity(int layer, int feature)
{
    m_entities.setHighlighted({layer, feature});
    update();
}

void WorldView3D::focusOnEntity(int layer, int feature)
{
    const auto [lo, hi] = m_entities.featureBounds({layer, feature});
    if (lo.isNull() && hi.isNull()) {
        return;
    }
    QVector3D centre = (lo + hi) / 2.0F;
    const float extent = std::max(hi.x() - lo.x(), hi.z() - lo.z());
    if (lo == hi) {
        // A point: aim at its marker, which is drawn raised off the ground.
        centre.setY(centre.y() + EntityRenderer::kPointLift);
    }
    Camera camera = m_camera;
    camera.pitch = kFocusPitch;
    const float distance = std::max(kFocusMinDistance, extent * 1.2F);
    camera.position = centre - camera.forward() * distance;
    setCamera(camera);
}

void WorldView3D::setHighlightedModel(std::optional<std::uint32_t> chunk)
{
    m_highlightedModel = chunk;
    update();
}

std::optional<fh1::PickHit> WorldView3D::pickModelAt(const QPointF& position) const
{
    if (!m_index || !m_archive || width() <= 0 || height() <= 0) {
        return std::nullopt;
    }
    // The point under the cursor on the far plane, back in world space; the
    // ray runs from the camera through it.
    bool invertible = false;
    const QMatrix4x4 toWorld = m_renderer.worldViewProjection(m_camera, size()).inverted(&invertible);
    if (!invertible) {
        return std::nullopt;
    }
    const float x = static_cast<float>(2.0 * position.x() / width() - 1.0);
    const float y = static_cast<float>(1.0 - 2.0 * position.y() / height());
    const QVector3D farPoint = toWorld.map(QVector3D(x, y, 1.0F));
    const QVector3D direction = (farPoint - m_camera.position).normalized();
    std::vector<std::uint32_t> candidates;
    for (const LoadedModel& model : m_renderer.loadedModels()) {
        if (model.error.isEmpty()) {
            candidates.push_back(model.chunk);
        }
    }
    return fh1::pickModel(*m_archive, *m_index, candidates, m_camera.position, direction, m_renderer.viewDistance());
}

void WorldView3D::requestTiles()
{
    if (!m_grid) {
        return;
    }
    const quint64 epoch = m_epoch;
    const QPointer<WorldView3D> self(this);
    for (TileRequest& request : m_renderer.requests(m_camera, m_inFlight)) {
        m_inFlight[static_cast<std::size_t>(request.tile)] = request.state;
        ++m_jobsInFlight;
        std::shared_ptr<const fh1::ForzaZip> archive = m_archive;
        std::shared_ptr<const fh1::WorldIndex> index = m_index;
        std::shared_ptr<const fh1::TrackTextures> textures = m_textures;
        const int tile = request.tile;
        const int state = request.state;
        m_pool.start(
            [self, archive, index, textures, chunks = std::move(request.chunks), tile, state, epoch] {
                auto mesh
                    = std::make_shared<fh1::TileMesh>(fh1::buildTileMesh(*archive, *index, chunks, textures.get()));
                QMetaObject::invokeMethod(
                    qApp,
                    [self, mesh, tile, state, epoch] {
                        if (!self || self->m_epoch != epoch) {
                            return;
                        }
                        self->m_ready.push_back({tile, state, mesh});
                        --self->m_jobsInFlight;
                        self->update();
                    },
                    Qt::QueuedConnection);
            },
            -static_cast<int>(request.distance));
    }
}

void WorldView3D::requestTextures()
{
    if (!m_archive || !m_textures) {
        return;
    }
    const quint64 epoch = m_epoch;
    const QPointer<WorldView3D> self(this);
    for (const std::uint32_t id : m_renderer.takeTextureRequests()) {
        ++m_textureJobsInFlight;
        std::shared_ptr<const fh1::ForzaZip> archive = m_archive;
        std::shared_ptr<const fh1::TrackTextures> textures = m_textures;
        m_pool.start(
            [self, archive, textures, id, epoch] {
                QString error;
                std::optional<fh1::TextureMipChain> chain = textures->loadTexture(*archive, id, &error);
                auto result = chain ? std::make_shared<fh1::TextureMipChain>(std::move(*chain)) : nullptr;
                QMetaObject::invokeMethod(
                    qApp,
                    [self, id, result, error, epoch] {
                        if (!self || self->m_epoch != epoch) {
                            return;
                        }
                        self->m_readyTextures.push_back({id, result, error});
                        --self->m_textureJobsInFlight;
                        self->update();
                    },
                    Qt::QueuedConnection);
            },
            kTexturePriority);
    }
}

void WorldView3D::initializeGL()
{
    // Qt replaces the widget's context in some situations, for example when
    // the window first switches to OpenGL composition. Buffers and textures
    // belong to the old context and must be released while it still exists.
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, &WorldView3D::releaseGL, Qt::UniqueConnection);
    if (m_renderer.initialize()) {
        m_entities.initialize();
    }
    // Tiles of a previous context are gone; build them again for this one.
    std::fill(m_inFlight.begin(), m_inFlight.end(), -1);
    m_ready.clear();
    m_readyTextures.clear();
    ++m_epoch;
    m_jobsInFlight = 0;
    m_textureJobsInFlight = 0;
    requestTiles();
}

void WorldView3D::paintGL()
{
    WorldRenderer::Stats stats;
    if (m_renderer.isReady()) {
        int uploads = 0;
        while (!m_ready.empty() && uploads < kUploadsPerFrame) {
            const BuiltTile built = std::move(m_ready.front());
            m_ready.erase(m_ready.begin());
            m_inFlight[static_cast<std::size_t>(built.tile)] = -1;
            m_failedChunks += built.mesh->failedChunks;
            m_renderer.upload(built.tile, built.state, *built.mesh);
            ++uploads;
        }
        if (uploads > 0) {
            // A tile whose job finished for a state the camera has since left
            // is requested again at its current state.
            requestTiles();
        }
        int textureUploads = 0;
        while (!m_readyTextures.empty() && textureUploads < kTextureUploadsPerFrame) {
            const DecodedTexture decoded = std::move(m_readyTextures.front());
            m_readyTextures.erase(m_readyTextures.begin());
            if (decoded.chain) {
                m_renderer.uploadTexture(decoded.id, *decoded.chain);
            } else {
                m_renderer.failTexture(decoded.id, decoded.error);
            }
            ++textureUploads;
        }
        const bool evicted = m_renderer.evictDistant(m_camera);
        requestTextures();
        if (uploads > 0 || textureUploads > 0 || evicted) {
            // Queued, so a slot that reads the renderer runs after painting.
            QMetaObject::invokeMethod(this, &WorldView3D::loadedFilesChanged, Qt::QueuedConnection);
        }
        const QSize viewport = size() * devicePixelRatioF();
        stats = m_renderer.draw(m_camera, viewport);
        if (m_entities.isReady()) {
            m_entities.draw(m_renderer.worldViewProjection(m_camera, viewport), viewport, m_camera.position,
                m_renderer.fogDistance(), WorldRenderer::fogColour(), static_cast<float>(devicePixelRatioF()));
        }
    }
    drawOverlay(stats);

    if (m_highlightedModel) {
        QPainter painter(this);
        drawModelOutline(painter);
    }

    const bool settledNow = isSettled();
    if (settledNow && !m_wasSettled) {
        m_wasSettled = true;
        emit settled();
    } else if (!settledNow) {
        m_wasSettled = false;
    }
}

bool WorldView3D::isSettled() const
{
    return m_grid && m_jobsInFlight == 0 && m_ready.empty() && m_textureJobsInFlight == 0 && m_readyTextures.empty()
        && m_renderer.isReady() && m_renderer.isComplete(m_camera) && !m_renderer.texturesPending();
}

void WorldView3D::drawOverlay(const WorldRenderer::Stats& stats)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (m_renderer.isReady() && m_entities.map() != nullptr) {
        // The painter works in logical pixels, so the labels are projected
        // onto the logical viewport.
        EntityRenderer::paintLabels(painter,
            m_entities.labels(m_renderer.worldViewProjection(m_camera, size()), size(), m_camera.position,
                std::min(kLabelDistance, m_renderer.viewDistance()), 1.0F),
            font());
    }
    QStringList lines;
    if (!m_renderer.isReady()) {
        lines << tr("The 3D view is unavailable: %1").arg(m_renderer.errorString());
    } else if (!m_grid) {
        lines << tr("No world loaded");
    } else {
        const QVector3D& p = m_camera.position;
        lines << tr("X %1   Y %2   Z %3").arg(p.x(), 0, 'f', 0).arg(p.y(), 0, 'f', 0).arg(p.z(), 0, 'f', 0);
        lines << tr("%1 tiles, %2 M triangles, %3 draw calls")
                     .arg(stats.drawnTiles)
                     .arg(static_cast<double>(stats.drawnTriangles) / 1e6, 0, 'f', 1)
                     .arg(stats.drawCalls);
        const int pending = m_jobsInFlight + static_cast<int>(m_ready.size());
        if (pending > 0) {
            lines << tr("Loading %n tile(s)…", nullptr, pending);
        }
        if (m_textures) {
            const WorldRenderer::TextureStats textures = m_renderer.textureStats();
            QString line = tr("%n texture(s), %1 MB", nullptr, textures.loaded)
                               .arg(static_cast<double>(textures.bytes) / (1024.0 * 1024.0), 0, 'f', 0);
            if (textures.pending > 0) {
                line += tr(", %n loading", nullptr, textures.pending);
            }
            if (textures.failed > 0) {
                line += tr(", %n missing", nullptr, textures.failed);
            }
            lines << line;
        } else {
            lines << tr("No texture tables for this track, so nothing is textured");
        }
        if (m_failedChunks > 0) {
            lines << tr("%n mesh file(s) failed to load", nullptr, m_failedChunks);
        }
        lines << tr("Click: select   Drag: look   W A S D: move   Q E: down / up   Shift: faster   Wheel: speed %1 m/s")
                     .arg(m_speed, 0, 'f', 0);
    }
    const QFontMetrics metrics(font());
    int y = 12 + metrics.ascent();
    for (const QString& line : std::as_const(lines)) {
        QPainterPath path;
        path.addText(QPointF(12, y), font(), line);
        painter.setPen(QPen(QColor(0, 0, 0, 170), 3.0));
        painter.drawPath(path);
        painter.fillPath(path, Qt::white);
        y += metrics.height() + 2;
    }
}

void WorldView3D::drawModelOutline(QPainter& painter)
{
    if (!m_index || !m_highlightedModel || *m_highlightedModel >= m_index->chunks().size()) {
        return;
    }
    const fh1::WorldChunk& chunk = m_index->chunks()[*m_highlightedModel];
    const QMatrix4x4 toClip = m_renderer.worldViewProjection(m_camera, size());
    std::array<QVector4D, 8> corners;
    for (int k = 0; k < 8; ++k) {
        const QVector3D corner((k & 1) != 0 ? chunk.boundsMax.x() : chunk.boundsMin.x(),
            (k & 2) != 0 ? chunk.boundsMax.y() : chunk.boundsMin.y(),
            (k & 4) != 0 ? chunk.boundsMax.z() : chunk.boundsMin.z());
        corners[static_cast<std::size_t>(k)] = toClip * QVector4D(corner, 1.0F);
    }
    const auto toScreen = [this](const QVector4D& clip) {
        return QPointF((clip.x() / clip.w() + 1.0F) * 0.5F * static_cast<float>(width()),
            (1.0F - clip.y() / clip.w()) * 0.5F * static_cast<float>(height()));
    };
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(255, 210, 60), 2.0));
    // Corners differing in one bit share an edge. Edges reaching behind the
    // camera are left out rather than clipped; the rest still frame the
    // model.
    for (int a = 0; a < 8; ++a) {
        for (const int bit : {1, 2, 4}) {
            const int b = a | bit;
            if (b == a) {
                continue;
            }
            const QVector4D& p = corners[static_cast<std::size_t>(a)];
            const QVector4D& q = corners[static_cast<std::size_t>(b)];
            if (p.w() <= 0.0F || q.w() <= 0.0F) {
                continue;
            }
            painter.drawLine(toScreen(p), toScreen(q));
        }
    }
}

void WorldView3D::tick()
{
    float dt = 0.0F;
    if (m_frameClock.isValid()) {
        dt = std::min(0.1F, static_cast<float>(m_frameClock.restart()) / 1000.0F);
    } else {
        m_frameClock.start();
    }
    QVector3D move;
    const QVector3D f = m_camera.forward();
    const QVector3D flat = QVector3D(f.x(), 0.0F, f.z()).normalized();
    // In the world frame (X east, Z north) "right" of a heading is a
    // clockwise turn: facing north (0, 0, 1) it is east (1, 0, 0).
    const QVector3D right(flat.z(), 0.0F, -flat.x());
    if (m_keys.contains(Qt::Key_W) || m_keys.contains(Qt::Key_Up)) {
        move += f;
    }
    if (m_keys.contains(Qt::Key_S) || m_keys.contains(Qt::Key_Down)) {
        move -= f;
    }
    if (m_keys.contains(Qt::Key_D) || m_keys.contains(Qt::Key_Right)) {
        move += right;
    }
    if (m_keys.contains(Qt::Key_A) || m_keys.contains(Qt::Key_Left)) {
        move -= right;
    }
    if (m_keys.contains(Qt::Key_E) || m_keys.contains(Qt::Key_Space)) {
        move += QVector3D(0.0F, 1.0F, 0.0F);
    }
    if (m_keys.contains(Qt::Key_Q)) {
        move -= QVector3D(0.0F, 1.0F, 0.0F);
    }
    if (!move.isNull() && dt > 0.0F) {
        const float speed = m_speed * (m_keys.contains(Qt::Key_Shift) ? kFastMultiplier : 1.0F);
        m_camera.position += move.normalized() * speed * dt;
        requestTiles();
    }
    const bool busy = !m_keys.isEmpty() || m_jobsInFlight > 0 || !m_ready.empty() || m_textureJobsInFlight > 0
        || !m_readyTextures.empty();
    if (busy) {
        update();
    } else {
        m_frameClock.invalidate();
    }
}

void WorldView3D::keyPressEvent(QKeyEvent* event)
{
    if (!event->isAutoRepeat()) {
        m_keys.insert(event->key());
        m_ticker.start();
    }
    event->accept();
}

void WorldView3D::keyReleaseEvent(QKeyEvent* event)
{
    if (!event->isAutoRepeat()) {
        m_keys.remove(event->key());
    }
    event->accept();
}

void WorldView3D::focusOutEvent(QFocusEvent* event)
{
    m_keys.clear();
    QOpenGLWidget::focusOutEvent(event);
}

void WorldView3D::mousePressEvent(QMouseEvent* event)
{
    m_looking = true;
    m_lastMouse = event->position().toPoint();
    m_pressPosition = m_lastMouse;
    setFocus();
    event->accept();
}

void WorldView3D::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_looking) {
        return;
    }
    const QPoint position = event->position().toPoint();
    const QPoint delta = position - m_lastMouse;
    m_lastMouse = position;
    // Dragging right turns right: clockwise seen from above, which in the
    // world frame (Z north) lowers the yaw.
    m_camera.yaw -= static_cast<float>(delta.x()) * kMouseRadiansPerPixel;
    m_camera.pitch = std::clamp(m_camera.pitch - static_cast<float>(delta.y()) * kMouseRadiansPerPixel, -1.55F, 1.55F);
    update();
}

void WorldView3D::mouseReleaseEvent(QMouseEvent* event)
{
    m_looking = false;
    event->accept();
    const QPoint position = event->position().toPoint();
    if (event->button() != Qt::LeftButton || (position - m_pressPosition).manhattanLength() > kClickSlop
        || !m_renderer.isReady()) {
        return;
    }
    // Map features are drawn over the world and are picked first.
    if (m_entities.map() != nullptr) {
        const auto ratio = static_cast<float>(devicePixelRatioF());
        const QSize viewport = size() * devicePixelRatioF();
        const std::optional<EntityRenderer::FeatureRef> hit
            = m_entities.pick(m_renderer.worldViewProjection(m_camera, viewport), viewport, m_camera.position,
                event->position() * ratio, kPickTolerance * ratio, m_renderer.viewDistance(), ratio);
        if (hit) {
            emit entityClicked(hit->layer, hit->feature);
            return;
        }
    }
    if (const std::optional<fh1::PickHit> model = pickModelAt(event->position())) {
        emit modelClicked(*model);
    } else {
        emit emptyClicked();
    }
}

void WorldView3D::wheelEvent(QWheelEvent* event)
{
    const int steps = event->angleDelta().y() / 120;
    if (steps != 0) {
        m_speed = std::clamp(m_speed * std::pow(1.25F, static_cast<float>(steps)), 5.0F, 2000.0F);
        update();
    }
    event->accept();
}
