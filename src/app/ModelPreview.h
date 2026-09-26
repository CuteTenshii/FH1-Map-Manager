#pragma once

#include "ForzaZip.h"
#include "TrackTextures.h"
#include "WorldIndex.h"
#include "WorldRenderer.h"
#include "WorldTiles.h"

#include <QOpenGLWidget>
#include <QPoint>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

/// Shows one model of the 3D world on its own, textured as in the world,
/// with a camera that orbits it.
///
/// The model is drawn by a WorldRenderer of its own, over an index that
/// holds only that model, so it looks exactly as in the 3D view. Geometry
/// and textures are decoded on worker threads. All OpenGL work happens in
/// paintGL(): making the context current from elsewhere, while the window
/// also composes the 3D view, crashed Qt's OpenGL widget painting.
class ModelPreview : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit ModelPreview(QWidget* parent = nullptr);
    ~ModelPreview() override;

    /// Shows chunk `chunk` of `index`. Does nothing if it is already shown.
    void setModel(std::shared_ptr<const fh1::ForzaZip> archive, std::shared_ptr<const fh1::WorldIndex> index,
        std::shared_ptr<const fh1::TrackTextures> textures, std::uint32_t chunk);
    /// Shows no model, only `message`.
    void clear(const QString& message = {});

    /// The chunk shown, if any.
    std::optional<std::uint32_t> model() const { return m_chunk; }
    /// True when the model and its textures are loaded, or failed to load.
    bool isSettled() const;
    /// Puts the camera back where it started for this model.
    void resetView();

    /// Distance from which a camera with the draw()'s field of view sees all
    /// of the box `lo`..`hi` in a viewport of width / height `aspect`.
    static float fitDistance(const QVector3D& lo, const QVector3D& hi, float aspect);
    /// A camera `distance` metres from `centre`, looking at it along `yaw`
    /// and `pitch` (see WorldCamera).
    static WorldCamera orbitCamera(const QVector3D& centre, float distance, float yaw, float pitch);

signals:
    /// Emitted when the preview becomes settled (see isSettled()).
    void settled();

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct DecodedTexture {
        std::uint32_t id = 0;
        /// Null when the texture could not be loaded; `error` says why.
        std::shared_ptr<fh1::TextureMipChain> chain;
        QString error;
    };

    void releaseGL();
    void requestTextures();
    /// Uploads what the workers have finished and draws the model.
    void drawModel();
    void zoomBy(float factor);
    void turnBy(float yaw, float pitch);
    void drawMessage(const QString& text);
    float aspect() const;

    std::shared_ptr<const fh1::ForzaZip> m_archive;
    std::shared_ptr<const fh1::WorldIndex> m_sourceIndex;
    std::shared_ptr<const fh1::TrackTextures> m_textures;
    std::optional<std::uint32_t> m_chunk;
    /// The shown chunk alone, and the grid the renderer draws from it.
    std::shared_ptr<const fh1::WorldIndex> m_index;
    std::unique_ptr<fh1::WorldTileGrid> m_grid;

    WorldRenderer m_renderer;
    /// The built model, kept so a new OpenGL context can upload it again.
    std::shared_ptr<const fh1::TileMesh> m_mesh;
    bool m_meshUploaded = false;
    /// Set when m_grid changed since the renderer last took it; the renderer
    /// may point at a grid that no longer exists until then, and only
    /// paintGL() reads it.
    bool m_gridChanged = false;
    std::vector<DecodedTexture> m_readyTextures;
    int m_textureJobs = 0;
    /// Incremented per model; results of jobs for an older one are dropped.
    quint64 m_epoch = 0;
    QString m_message;
    bool m_wasSettled = false;

    QVector3D m_centre;
    QVector3D m_boundsMin;
    QVector3D m_boundsMax;
    float m_distance = 10.0F;
    float m_yaw = 0.0F;
    float m_pitch = 0.0F;
    /// Set once the user moves the camera; until then resizing refits it.
    bool m_userMoved = false;
    bool m_dragging = false;
    QPoint m_lastMouse;
};
