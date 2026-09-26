#pragma once

#include "LayerStyle.h"
#include "MapData.h"

#include <QFont>
#include <QMatrix4x4>
#include <QOpenGLFunctions_3_3_Core>
#include <QPointF>
#include <QSize>
#include <QVector3D>

#include <memory>
#include <optional>
#include <vector>

class QOpenGLShaderProgram;
class QPainter;

/// Draws the map's layers (the same objects, routes and zones as the 2D map)
/// in the 3D world, and finds which one is under the cursor.
///
/// Points are screen-sized squares at their world position, built by a
/// geometry shader: the game's map icon where the 2D map draws one,
/// otherwise a disc in the group's colour.
/// Polylines are lines of a fixed screen width, widened by a geometry shader
/// because core-profile OpenGL only guarantees one-pixel lines. Zones are
/// translucent fills with their outline. Everything is depth-tested against
/// the world, so hills hide what is behind them; the map-wide backdrop
/// terrain never does (see WorldRenderer::kForegroundDepthFar).
///
/// Every method that touches OpenGL needs the context initialize() ran in to
/// be current.
class EntityRenderer : protected QOpenGLFunctions_3_3_Core {
public:
    /// A feature, by index into MapData::layers and that layer's features.
    struct FeatureRef {
        int layer = -1;
        int feature = -1;

        bool isValid() const { return layer >= 0 && feature >= 0; }
        bool operator==(const FeatureRef&) const = default;
    };

    /// A label to draw at a screen position.
    struct ScreenLabel {
        QPointF position;
        QString text;
        /// Pixels between the anchor and the text, so it clears the marker.
        double clearance = 0.0;
        /// Zone names are centred on their anchor.
        bool centred = false;
        float distance = 0.0F;
    };

    EntityRenderer();
    /// Call release() first, with the context current.
    ~EntityRenderer() override;
    EntityRenderer(const EntityRenderer&) = delete;
    EntityRenderer& operator=(const EntityRenderer&) = delete;

    /// Prepares OpenGL objects in the current context. Returns false, with
    /// errorString() set, when the shaders do not build.
    bool initialize();
    bool isReady() const { return m_initialized; }
    QString errorString() const { return m_error; }
    /// Frees every OpenGL object; the map is kept and uploaded again after
    /// the next initialize().
    void release();

    /// Shows `map`'s layers, or nothing when it is null. `visible[layer][group]`
    /// gives the starting visibility; missing entries count as visible.
    void setMap(std::shared_ptr<const fh1::MapData> map, const std::vector<std::vector<bool>>& visible = {});
    const fh1::MapData* map() const { return m_map.get(); }
    const LayerStyle* style(int layer) const;

    void setGroupVisible(int layer, int group, bool visible);
    bool isGroupVisible(int layer, int group) const;
    bool isFeatureShown(const FeatureRef& feature) const;

    /// Draws `feature` emphasised, on top of the world; an invalid reference
    /// clears the highlight.
    void setHighlighted(const FeatureRef& feature);
    FeatureRef highlighted() const { return m_highlighted; }

    /// Draws the shown layers. `worldViewProjection` maps world coordinates
    /// to clip space; `devicePixelRatio` scales sprite and line sizes.
    void draw(const QMatrix4x4& worldViewProjection, QSize viewport, const QVector3D& camera, float fogDistance,
        const QVector3D& fogColour, float devicePixelRatio);

    /// The shown feature nearest `screenPos` (in device pixels) within
    /// `tolerance` pixels, ignoring anything further than `maxDistance`
    /// metres from the camera. Markers win over lines, lines over zones.
    std::optional<FeatureRef> pick(const QMatrix4x4& worldViewProjection, QSize viewport, const QVector3D& camera,
        const QPointF& screenPos, double tolerance, float maxDistance, float devicePixelRatio) const;

    /// Labels of shown features within `maxDistance` metres, nearest first.
    std::vector<ScreenLabel> labels(const QMatrix4x4& worldViewProjection, QSize viewport, const QVector3D& camera,
        float maxDistance, float devicePixelRatio) const;

    /// World-space bounds of a feature (its position, or its shapes' extent).
    std::pair<QVector3D, QVector3D> featureBounds(const FeatureRef& feature) const;

    /// Draws `labels` with dark outlines, skipping any that would overlap one
    /// already drawn (so nearer labels win). Returns how many were drawn.
    static int paintLabels(QPainter& painter, const std::vector<ScreenLabel>& labels, const QFont& font);

    /// Metres points are raised above their stored position, so markers on
    /// the road surface are not buried in it.
    static constexpr float kPointLift = 1.5F;

private:
    struct Range {
        GLint first = 0;
        GLsizei count = 0;
    };
    /// Where one group's geometry sits in each buffer.
    struct GroupRanges {
        Range points;
        Range lines;
        Range ticks;
        Range fills;
    };
    struct GpuBuffer {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLsizei vertices = 0;
    };

    void build();
    void upload();
    void uploadIcons();
    void buildHighlight();
    void releaseBuffer(GpuBuffer& buffer);
    /// Replaces `buffer`'s contents: point vertices (position, colour, size,
    /// icon layer) or line and fill vertices (position, colour).
    void fillBuffer(GpuBuffer& buffer, const std::vector<float>& data, bool points);
    void drawLines(const GpuBuffer& buffer, const std::vector<Range>& ranges, float width, bool halo);
    float pointSize(int layer, int feature) const;

    std::shared_ptr<const fh1::MapData> m_map;
    std::vector<LayerStyle> m_styles;
    std::vector<std::vector<bool>> m_visible;
    /// [layer][group]
    std::vector<std::vector<GroupRanges>> m_ranges;
    /// Index into the icon texture array for each feature, or -1.
    std::vector<std::vector<int>> m_featureIcon;
    std::vector<QImage> m_icons;
    std::vector<float> m_pointData;
    std::vector<float> m_lineData;
    std::vector<float> m_tickData;
    std::vector<float> m_fillData;
    std::vector<float> m_highlightPoint;
    std::vector<float> m_highlightLines;
    FeatureRef m_highlighted;
    bool m_dirty = false;
    bool m_highlightDirty = false;

    bool m_initialized = false;
    QString m_error;
    std::unique_ptr<QOpenGLShaderProgram> m_pointProgram;
    std::unique_ptr<QOpenGLShaderProgram> m_lineProgram;
    std::unique_ptr<QOpenGLShaderProgram> m_fillProgram;
    GpuBuffer m_points;
    GpuBuffer m_lines;
    GpuBuffer m_ticks;
    GpuBuffer m_fills;
    GpuBuffer m_highlightPointBuffer;
    GpuBuffer m_highlightLineBuffer;
    GLuint m_iconTexture = 0;
};
