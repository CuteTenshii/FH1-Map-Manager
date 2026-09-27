#include "ModelPreview.h"

#include <QFutureWatcher>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace {

/// The camera starts south-east of the model, looking down at it; models
/// have no front the files record, so one corner is as good as another.
constexpr float kStartYaw = 3.0F * std::numbers::pi_v<float> / 4.0F;
constexpr float kStartPitch = -0.45F;
constexpr float kMaxPitch = 1.5F;
constexpr float kRadiansPerPixel = 0.01F;
constexpr float kKeyTurn = 0.15F;
constexpr float kKeyZoom = 1.25F;
constexpr float kWheelZoomPerStep = 1.15F;
/// Margin around the model when the camera fits it.
constexpr float kFitMargin = 1.1F;
/// How far the camera may come in and go out, in model radii.
constexpr float kMinDistanceRadii = 0.3F;
constexpr float kMaxDistanceRadii = 30.0F;
/// Draw distance in multiples of the farthest point of the model, so fog
/// never tints it.
constexpr float kViewDistanceFactor = 10.0F;
constexpr float kMinNearPlane = 0.02F;

} // namespace

ModelPreview::ModelPreview(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(120, 120);
    setToolTip(tr("Drag to turn the model and scroll to zoom. The arrow keys, + and − do the same; "
                  "Home or a double-click resets the view."));
}

ModelPreview::~ModelPreview()
{
    ++m_epoch;
    // QOpenGLWidget destroys the context after this destructor; its
    // aboutToBeDestroyed must not reach releaseGL() then.
    if (context() != nullptr) {
        disconnect(context(), &QOpenGLContext::aboutToBeDestroyed, this, &ModelPreview::releaseGL);
    }
    releaseGL();
}

void ModelPreview::releaseGL()
{
    if (!m_renderer.isReady() || context() == nullptr) {
        return;
    }
    makeCurrent();
    m_renderer.release();
    doneCurrent();
    m_meshUploaded = false;
}

void ModelPreview::clear(const QString& message)
{
    if (!m_chunk && message == m_message) {
        return;
    }
    ++m_epoch;
    m_gridChanged = true;
    m_meshUploaded = false;
    m_archive.reset();
    m_sourceIndex.reset();
    m_textures.reset();
    m_chunk.reset();
    m_grid.reset();
    m_index.reset();
    m_mesh.reset();
    m_readyTextures.clear();
    m_textureJobs = 0;
    m_message = message;
    m_wasSettled = false;
    update();
}

void ModelPreview::setModel(std::shared_ptr<const fh1::ForzaZip> archive, std::shared_ptr<const fh1::WorldIndex> index,
    std::shared_ptr<const fh1::TrackTextures> textures, std::uint32_t chunk)
{
    if (!archive || !index || chunk >= index->chunks().size()) {
        clear();
        return;
    }
    if (archive == m_archive && index == m_sourceIndex && textures == m_textures && m_chunk == chunk) {
        return;
    }
    clear();
    m_archive = std::move(archive);
    m_sourceIndex = std::move(index);
    m_textures = std::move(textures);
    m_chunk = chunk;

    // The model alone, drawn at any distance and whatever the camera's zone.
    fh1::WorldChunk only = m_sourceIndex->chunks()[chunk];
    only.bandStart = 0.0F;
    only.bandEnd = std::numeric_limits<float>::infinity();
    only.zones.clear();
    m_index = std::make_shared<const fh1::WorldIndex>(fh1::WorldIndex::fromChunks({only}));
    // One tile holds the whole model, whatever its size.
    const float extent = std::max(only.boundsMax.x() - only.boundsMin.x(), only.boundsMax.z() - only.boundsMin.z());
    m_grid = std::make_unique<fh1::WorldTileGrid>(*m_index, std::max(extent, 1.0F) * 2.0F, fh1::EventPropFilter::all());
    m_gridChanged = true;

    m_boundsMin = only.boundsMin;
    m_boundsMax = only.boundsMax;
    m_centre = (only.boundsMin + only.boundsMax) / 2.0F;
    m_userMoved = false;
    resetView();

    const quint64 epoch = m_epoch;
    const QString file = only.entry < m_archive->entries().size() ? m_archive->entries()[only.entry].name : QString();
    m_message = tr("Loading %1…").arg(file);
    auto* watcher = new QFutureWatcher<std::shared_ptr<const fh1::TileMesh>>(this);
    connect(watcher, &QFutureWatcher<std::shared_ptr<const fh1::TileMesh>>::finished, this, [this, watcher, epoch] {
        watcher->deleteLater();
        if (epoch != m_epoch) {
            return;
        }
        m_mesh = watcher->result();
        m_meshUploaded = false;
        if (m_mesh->failedChunks > 0) {
            m_message = m_mesh->errors.value(0);
        } else if (m_mesh->indices.empty()) {
            m_message = tr("This model has no visible surfaces, so the 3D view leaves it out.");
        } else {
            m_message.clear();
        }
        update();
    });
    watcher->setFuture(QtConcurrent::run([archive = m_archive, index = m_index, textures = m_textures] {
        return std::make_shared<const fh1::TileMesh>(
            fh1::buildTileMesh(*archive, *index, {0}, textures ? textures.get() : nullptr));
    }));
    update();
}

bool ModelPreview::isSettled() const
{
    return m_mesh != nullptr && (m_meshUploaded || !m_renderer.isReady()) && m_textureJobs == 0
        && m_readyTextures.empty() && !m_renderer.texturesPending();
}

float ModelPreview::aspect() const
{
    return height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0F;
}

float ModelPreview::fitDistance(const QVector3D& lo, const QVector3D& hi, float aspect)
{
    const float radius = std::max((hi - lo).length() / 2.0F, 0.01F);
    const float halfVertical = WorldRenderer::kFieldOfViewDegrees * std::numbers::pi_v<float> / 360.0F;
    // In a narrow viewport the horizontal angle is the smaller one.
    const float halfHorizontal = std::atan(std::tan(halfVertical) * std::max(aspect, 0.01F));
    return radius / std::sin(std::min(halfVertical, halfHorizontal)) * kFitMargin;
}

WorldCamera ModelPreview::orbitCamera(const QVector3D& centre, float distance, float yaw, float pitch)
{
    WorldCamera camera;
    camera.yaw = yaw;
    camera.pitch = pitch;
    camera.position = centre - camera.forward() * distance;
    return camera;
}

void ModelPreview::resetView()
{
    m_yaw = kStartYaw;
    m_pitch = kStartPitch;
    m_distance = fitDistance(m_boundsMin, m_boundsMax, aspect());
    update();
}

void ModelPreview::zoomBy(float factor)
{
    const float radius = std::max((m_boundsMax - m_boundsMin).length() / 2.0F, 0.01F);
    m_distance = std::clamp(m_distance * factor, radius * kMinDistanceRadii, radius * kMaxDistanceRadii);
    m_userMoved = true;
    update();
}

void ModelPreview::turnBy(float yaw, float pitch)
{
    m_yaw += yaw;
    m_pitch = std::clamp(m_pitch + pitch, -kMaxPitch, kMaxPitch);
    m_userMoved = true;
    update();
}

void ModelPreview::initializeGL()
{
    // Qt can replace the widget's context; the renderer's objects belong to
    // the old one and must be freed while it exists.
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, &ModelPreview::releaseGL, Qt::UniqueConnection);
    if (m_renderer.initialize()) {
        m_renderer.setGrid(m_grid.get());
    }
    m_gridChanged = false;
    m_meshUploaded = false;
    m_readyTextures.clear();
}

void ModelPreview::resizeGL(int /*width*/, int /*height*/)
{
    if (!m_userMoved && m_chunk) {
        m_distance = fitDistance(m_boundsMin, m_boundsMax, aspect());
    }
}

void ModelPreview::requestTextures()
{
    if (!m_archive || !m_textures) {
        return;
    }
    for (const std::uint32_t id : m_renderer.takeTextureRequests()) {
        ++m_textureJobs;
        const quint64 epoch = m_epoch;
        auto* watcher = new QFutureWatcher<DecodedTexture>(this);
        connect(watcher, &QFutureWatcher<DecodedTexture>::finished, this, [this, watcher, epoch] {
            watcher->deleteLater();
            if (epoch != m_epoch) {
                return;
            }
            --m_textureJobs;
            m_readyTextures.push_back(watcher->result());
            update();
        });
        watcher->setFuture(QtConcurrent::run([archive = m_archive, textures = m_textures, id] {
            DecodedTexture decoded;
            decoded.id = id;
            if (std::optional<fh1::TextureMipChain> chain = textures->loadTexture(*archive, id, &decoded.error)) {
                decoded.chain = std::make_shared<fh1::TextureMipChain>(std::move(*chain));
            }
            return decoded;
        }));
    }
}

void ModelPreview::paintGL()
{
    if (m_renderer.isReady()) {
        drawModel();
    } else {
        drawMessage(tr("The model preview needs OpenGL 3.3: %1").arg(m_renderer.errorString()));
    }
    const bool settledNow = isSettled();
    if (settledNow && !m_wasSettled) {
        QMetaObject::invokeMethod(this, &ModelPreview::settled, Qt::QueuedConnection);
    }
    m_wasSettled = settledNow;
}

void ModelPreview::drawModel()
{
    if (m_gridChanged) {
        m_renderer.setGrid(m_grid.get());
        m_gridChanged = false;
        m_meshUploaded = false;
    }
    if (m_mesh && !m_meshUploaded) {
        m_renderer.upload(0, 0, *m_mesh);
        m_meshUploaded = true;
    }
    for (const DecodedTexture& decoded : m_readyTextures) {
        if (decoded.chain) {
            m_renderer.uploadTexture(decoded.id, *decoded.chain);
        } else {
            m_renderer.failTexture(decoded.id, decoded.error);
        }
    }
    m_readyTextures.clear();
    requestTextures();

    if (!m_mesh || m_mesh->indices.empty()) {
        drawMessage(m_message);
    } else {
        const float radius = (m_boundsMax - m_boundsMin).length() / 2.0F;
        m_renderer.setNearPlane(std::max(kMinNearPlane, (m_distance - radius) * 0.5F));
        m_renderer.setViewDistance((m_distance + radius) * kViewDistanceFactor);
        m_renderer.draw(orbitCamera(m_centre, m_distance, m_yaw, m_pitch), size() * devicePixelRatioF());
        if (!m_message.isEmpty()) {
            drawMessage(m_message);
        }
    }
}

void ModelPreview::drawMessage(const QString& text)
{
    QPainter painter(this);
    if (!m_mesh || m_mesh->indices.empty() || !m_renderer.isReady()) {
        painter.fillRect(rect(), palette().color(QPalette::Base));
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignCenter | Qt::TextWordWrap, text);
        return;
    }
    // Over the model: a caption along the bottom, on a strip that keeps it
    // readable whatever is behind it.
    const QRect strip(0, height() - fontMetrics().height() - 8, width(), fontMetrics().height() + 8);
    QColor shade = palette().color(QPalette::Window);
    shade.setAlpha(200);
    painter.fillRect(strip, shade);
    painter.setPen(palette().color(QPalette::WindowText));
    painter.drawText(strip.adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
}

void ModelPreview::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_lastMouse = event->position().toPoint();
    }
    QOpenGLWidget::mousePressEvent(event);
}

void ModelPreview::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging) {
        const QPoint position = event->position().toPoint();
        const QPoint delta = position - m_lastMouse;
        m_lastMouse = position;
        // The model's near side follows the cursor: dragging right moves
        // the camera to its left, and dragging down raises it.
        turnBy(-static_cast<float>(delta.x()) * kRadiansPerPixel, -static_cast<float>(delta.y()) * kRadiansPerPixel);
    }
    QOpenGLWidget::mouseMoveEvent(event);
}

void ModelPreview::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}

void ModelPreview::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_userMoved = false;
        resetView();
    }
}

void ModelPreview::wheelEvent(QWheelEvent* event)
{
    const float steps = static_cast<float>(event->angleDelta().y()) / 120.0F;
    if (steps != 0.0F) {
        zoomBy(std::pow(kWheelZoomPerStep, -steps));
    }
    event->accept();
}

void ModelPreview::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    // The arrows turn the model as dragging in their direction would.
    case Qt::Key_Left:
        turnBy(kKeyTurn, 0.0F);
        break;
    case Qt::Key_Right:
        turnBy(-kKeyTurn, 0.0F);
        break;
    case Qt::Key_Up:
        turnBy(0.0F, kKeyTurn);
        break;
    case Qt::Key_Down:
        turnBy(0.0F, -kKeyTurn);
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomBy(1.0F / kKeyZoom);
        break;
    case Qt::Key_Minus:
        zoomBy(kKeyZoom);
        break;
    case Qt::Key_Home:
        m_userMoved = false;
        resetView();
        break;
    default:
        QOpenGLWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}
