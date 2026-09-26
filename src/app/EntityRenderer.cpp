#include "EntityRenderer.h"

#include "WorldRenderer.h"

#include <QFontMetricsF>
#include <QHash>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QPainterPath>
#include <QVector4D>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {

constexpr int kPointFloats = 9;
constexpr int kLineFloats = 7;
constexpr int kIconPixels = 64;
/// On-screen size of a map icon, in logical pixels.
constexpr float kIconSize = 30.0F;
/// Pixels added around a highlighted marker for its ring.
constexpr float kHighlightGrowth = 14.0F;
constexpr float kLineLift = 0.8F;
constexpr float kFillLift = 0.3F;
/// Length of the tick showing a point's heading, and how far away ticks are
/// still drawn: further out they only add noise.
constexpr float kTickLength = 5.0F;
constexpr float kTickDistance = 250.0F;
constexpr float kLineWidth = 3.5F;
constexpr float kHighlightLineWidth = 5.5F;
constexpr float kHaloWidth = 3.0F;
constexpr float kFillAlpha = 0.22F;
constexpr int kMaxLabels = 80;

const char* const kPointVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColour;
layout(location = 2) in float aSize;
layout(location = 3) in float aIcon;
uniform mat4 uWorldViewProjection;
uniform vec3 uCamera;
uniform float uFogDistance;
uniform float uPixelRatio;
uniform float uSizeBoost;
out vec4 gColour;
out float gIcon;
out float gFog;
out float gSize;
void main()
{
    gl_Position = uWorldViewProjection * vec4(aPosition, 1.0);
    float distance = length(aPosition - uCamera);
    // Markers shrink a little with distance so far-off clusters stay apart.
    float shrink = clamp(300.0 / max(distance, 1.0), 0.6, 1.0);
    gSize = (aSize * shrink + uSizeBoost) * uPixelRatio;
    gColour = aColour;
    gIcon = aIcon;
    gFog = clamp(1.0 - exp(-pow(distance / uFogDistance, 2.0)), 0.0, 1.0);
}
)";

// Turns each marker into a screen-aligned square of gSize pixels. Point
// sprites would do the same, but some drivers and window systems do not
// rasterise shader-sized points, while this works wherever geometry
// shaders do.
const char* const kPointGeometryShader = R"(#version 330 core
layout(points) in;
layout(triangle_strip, max_vertices = 4) out;
in vec4 gColour[];
in float gIcon[];
in float gFog[];
in float gSize[];
uniform vec2 uViewport;
out vec4 vColour;
flat out float vIcon;
out float vFog;
flat out float vSize;
out vec2 vCoord;
void main()
{
    vec4 centre = gl_in[0].gl_Position;
    if (centre.w < 1e-3) {
        return;
    }
    vec2 halfSize = vec2(gSize[0]) / uViewport * centre.w;
    // (0, 0) is the top left corner, as for gl_PointCoord and image rows.
    const vec2 corners[4] = vec2[4](vec2(-1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(1.0, -1.0));
    for (int i = 0; i < 4; ++i) {
        gl_Position = vec4(centre.xy + corners[i] * halfSize, centre.zw);
        vColour = gColour[0];
        vIcon = gIcon[0];
        vFog = gFog[0];
        vSize = gSize[0];
        vCoord = vec2(corners[i].x * 0.5 + 0.5, 0.5 - corners[i].y * 0.5);
        EmitVertex();
    }
    EndPrimitive();
}
)";

const char* const kPointFragmentShader = R"(#version 330 core
in vec4 vColour;
flat in float vIcon;
in float vFog;
flat in float vSize;
in vec2 vCoord;
uniform sampler2DArray uIcons;
uniform vec3 uFogColour;
uniform bool uRing;
out vec4 fragColour;
void main()
{
    vec2 c = vCoord * 2.0 - 1.0;
    float r = length(c);
    // One pixel is 2 / vSize in these radius units.
    float pixel = 2.0 / vSize;
    vec4 colour;
    if (uRing) {
        if (r > 1.0 || r < 1.0 - 4.0 * pixel) {
            discard;
        }
        colour = r > 1.0 - 1.5 * pixel ? vec4(0.0, 0.0, 0.0, 1.0) : vec4(1.0);
    } else if (vIcon >= 0.0) {
        colour = texture(uIcons, vec3(vCoord, vIcon));
        if (colour.a < 0.1) {
            discard;
        }
    } else {
        if (r > 1.0) {
            discard;
        }
        // A dark halo keeps light colours readable on snow and sand.
        colour = r > 1.0 - 1.5 * pixel ? vec4(0.0, 0.0, 0.0, 0.8) : vColour;
    }
    fragColour = vec4(mix(colour.rgb, uFogColour, vFog * 0.7), colour.a * (1.0 - vFog * 0.5));
}
)";

const char* const kLineVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColour;
uniform mat4 uWorldViewProjection;
uniform vec3 uCamera;
uniform float uFogDistance;
uniform float uMaxDistance;
out vec4 gColour;
out float gFog;
void main()
{
    gl_Position = uWorldViewProjection * vec4(aPosition, 1.0);
    float distance = length(aPosition - uCamera);
    gColour = aColour;
    gColour.a *= 1.0 - smoothstep(uMaxDistance * 0.7, uMaxDistance, distance);
    gFog = clamp(1.0 - exp(-pow(distance / uFogDistance, 2.0)), 0.0, 1.0);
}
)";

// Widens each segment into a quad of uWidth pixels. A segment that crosses
// the camera plane is cut there first, since dividing by a negative w would
// flip it across the screen.
const char* const kLineGeometryShader = R"(#version 330 core
layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;
in vec4 gColour[];
in float gFog[];
uniform vec2 uViewport;
uniform float uWidth;
out vec4 fColour;
out float fFog;
void main()
{
    const float near = 1e-3;
    vec4 a = gl_in[0].gl_Position;
    vec4 b = gl_in[1].gl_Position;
    if (a.w < near && b.w < near) {
        return;
    }
    vec4 ca = gColour[0];
    vec4 cb = gColour[1];
    float fa = gFog[0];
    float fb = gFog[1];
    if (a.w < near) {
        float t = (near - a.w) / (b.w - a.w);
        a = mix(a, b, t);
        ca = mix(ca, cb, t);
        fa = mix(fa, fb, t);
    } else if (b.w < near) {
        float t = (near - b.w) / (a.w - b.w);
        b = mix(b, a, t);
        cb = mix(cb, ca, t);
        fb = mix(fb, fa, t);
    }
    vec2 halfViewport = uViewport * 0.5;
    vec2 sa = a.xy / a.w * halfViewport;
    vec2 sb = b.xy / b.w * halfViewport;
    vec2 direction = sb - sa;
    float span = length(direction);
    direction = span > 1e-4 ? direction / span : vec2(1.0, 0.0);
    vec2 normal = vec2(-direction.y, direction.x) * uWidth * 0.5 / halfViewport;
    fColour = ca;
    fFog = fa;
    gl_Position = vec4(a.xy + normal * a.w, a.zw);
    EmitVertex();
    gl_Position = vec4(a.xy - normal * a.w, a.zw);
    EmitVertex();
    fColour = cb;
    fFog = fb;
    gl_Position = vec4(b.xy + normal * b.w, b.zw);
    EmitVertex();
    gl_Position = vec4(b.xy - normal * b.w, b.zw);
    EmitVertex();
    EndPrimitive();
}
)";

const char* const kLineFragmentShader = R"(#version 330 core
in vec4 fColour;
in float fFog;
uniform vec3 uFogColour;
uniform bool uHalo;
out vec4 fragColour;
void main()
{
    vec4 colour = uHalo ? vec4(0.0, 0.0, 0.0, 0.7 * fColour.a) : fColour;
    fragColour = vec4(mix(colour.rgb, uFogColour, fFog * 0.7), colour.a * (1.0 - fFog * 0.5));
}
)";

const char* const kFillVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColour;
uniform mat4 uWorldViewProjection;
uniform vec3 uCamera;
uniform float uFogDistance;
out vec4 vColour;
out float vFog;
void main()
{
    gl_Position = uWorldViewProjection * vec4(aPosition, 1.0);
    vColour = aColour;
    vFog = clamp(1.0 - exp(-pow(length(aPosition - uCamera) / uFogDistance, 2.0)), 0.0, 1.0);
}
)";

const char* const kFillFragmentShader = R"(#version 330 core
in vec4 vColour;
in float vFog;
uniform vec3 uFogColour;
out vec4 fragColour;
void main()
{
    fragColour = vec4(mix(vColour.rgb, uFogColour, vFog * 0.7), vColour.a * (1.0 - vFog));
}
)";

void appendVertex(std::vector<float>& out, const QVector3D& p, const QColor& colour, float alpha)
{
    out.insert(out.end(), {p.x(), p.y(), p.z(), colour.redF(), colour.greenF(), colour.blueF(), alpha});
}

/// Projects a world point to device pixels (origin top left), or nothing
/// when it is behind the camera.
std::optional<QPointF> project(const QMatrix4x4& matrix, QSize viewport, const QVector3D& point)
{
    const QVector4D clip = matrix * QVector4D(point, 1.0F);
    if (clip.w() <= 1e-3F) {
        return std::nullopt;
    }
    const float x = clip.x() / clip.w();
    const float y = clip.y() / clip.w();
    return QPointF((x * 0.5 + 0.5) * viewport.width(), (0.5 - y * 0.5) * viewport.height());
}

double distanceToSegment(const QPointF& p, const QPointF& a, const QPointF& b)
{
    const QPointF ab = b - a;
    const double lengthSquared = QPointF::dotProduct(ab, ab);
    double t = lengthSquared > 0.0 ? QPointF::dotProduct(p - a, ab) / lengthSquared : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF closest = a + ab * t;
    return std::hypot(p.x() - closest.x(), p.y() - closest.y());
}

bool insideTriangle(const QPointF& p, const QPointF& a, const QPointF& b, const QPointF& c)
{
    const auto side = [](const QPointF& p1, const QPointF& p2, const QPointF& p3) {
        return (p1.x() - p3.x()) * (p2.y() - p3.y()) - (p2.x() - p3.x()) * (p1.y() - p3.y());
    };
    const double d1 = side(p, a, b);
    const double d2 = side(p, b, c);
    const double d3 = side(p, c, a);
    const bool negative = d1 < 0 || d2 < 0 || d3 < 0;
    const bool positive = d1 > 0 || d2 > 0 || d3 > 0;
    return !(negative && positive);
}

/// Triangles of a zone shape: the shapes are stored as triangles, but a
/// longer ring is fanned out from its first vertex.
template <typename Visit> void forEachTriangle(const std::vector<QVector3D>& shape, Visit&& visit)
{
    for (std::size_t i = 1; i + 1 < shape.size(); ++i) {
        visit(shape[0], shape[i], shape[i + 1]);
    }
}

/// Edges of a zone that belong to one triangle only: its outline. Edges
/// shared by two triangles are inside the zone.
std::vector<std::pair<QVector3D, QVector3D>> outlineEdges(const fh1::Feature& feature)
{
    const auto key = [](const QVector3D& p) {
        return QStringLiteral("%1,%2").arg(std::lround(p.x() * 10.0F)).arg(std::lround(p.z() * 10.0F));
    };
    QHash<QString, int> counts;
    std::vector<std::pair<QVector3D, QVector3D>> edges;
    for (const auto& shape : feature.shapes) {
        forEachTriangle(shape, [&](const QVector3D& a, const QVector3D& b, const QVector3D& c) {
            for (const auto& [p, q] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
                const QString k1 = key(p);
                const QString k2 = key(q);
                ++counts[k1 < k2 ? k1 + QLatin1Char('|') + k2 : k2 + QLatin1Char('|') + k1];
                edges.emplace_back(p, q);
            }
        });
    }
    std::vector<std::pair<QVector3D, QVector3D>> outline;
    for (const auto& [p, q] : edges) {
        const QString k1 = key(p);
        const QString k2 = key(q);
        if (counts.value(k1 < k2 ? k1 + QLatin1Char('|') + k2 : k2 + QLatin1Char('|') + k1) == 1) {
            outline.emplace_back(p, q);
        }
    }
    return outline;
}

QVector3D lifted(const QVector3D& p, float metres)
{
    return {p.x(), p.y() + metres, p.z()};
}

} // namespace

EntityRenderer::EntityRenderer() = default;
EntityRenderer::~EntityRenderer() = default;

bool EntityRenderer::initialize()
{
    m_initialized = false;
    m_error.clear();
    if (!initializeOpenGLFunctions()) {
        m_error = QStringLiteral("could not resolve OpenGL 3.3 core functions");
        return false;
    }
    auto build = [this](std::unique_ptr<QOpenGLShaderProgram>& program, const char* vertex, const char* geometry,
                     const char* fragment) {
        program = std::make_unique<QOpenGLShaderProgram>();
        bool ok = program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex);
        if (geometry != nullptr) {
            ok = ok && program->addShaderFromSourceCode(QOpenGLShader::Geometry, geometry);
        }
        ok = ok && program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) && program->link();
        if (!ok) {
            m_error = QStringLiteral("entity shaders failed to build: %1").arg(program->log());
            program.reset();
        }
        return ok;
    };
    if (!build(m_pointProgram, kPointVertexShader, kPointGeometryShader, kPointFragmentShader)
        || !build(m_lineProgram, kLineVertexShader, kLineGeometryShader, kLineFragmentShader)
        || !build(m_fillProgram, kFillVertexShader, nullptr, kFillFragmentShader)) {
        m_pointProgram.reset();
        m_lineProgram.reset();
        m_fillProgram.reset();
        return false;
    }
    m_initialized = true;
    m_dirty = true;
    m_highlightDirty = true;
    return true;
}

void EntityRenderer::releaseBuffer(GpuBuffer& buffer)
{
    if (buffer.vao != 0) {
        glDeleteVertexArrays(1, &buffer.vao);
        glDeleteBuffers(1, &buffer.vbo);
    }
    buffer = GpuBuffer{};
}

void EntityRenderer::release()
{
    if (!m_initialized) {
        return;
    }
    for (GpuBuffer* buffer :
        {&m_points, &m_lines, &m_ticks, &m_fills, &m_highlightPointBuffer, &m_highlightLineBuffer}) {
        releaseBuffer(*buffer);
    }
    if (m_iconTexture != 0) {
        glDeleteTextures(1, &m_iconTexture);
        m_iconTexture = 0;
    }
    m_pointProgram.reset();
    m_lineProgram.reset();
    m_fillProgram.reset();
    m_initialized = false;
}

void EntityRenderer::setMap(std::shared_ptr<const fh1::MapData> map, const std::vector<std::vector<bool>>& visible)
{
    m_map = std::move(map);
    m_highlighted = {};
    build();
    for (std::size_t layer = 0; layer < m_visible.size(); ++layer) {
        for (std::size_t group = 0; group < m_visible[layer].size(); ++group) {
            m_visible[layer][group]
                = layer >= visible.size() || group >= visible[layer].size() || visible[layer][group];
        }
    }
    m_dirty = true;
    m_highlightDirty = true;
}

const LayerStyle* EntityRenderer::style(int layer) const
{
    return layer >= 0 && static_cast<std::size_t>(layer) < m_styles.size() ? &m_styles[static_cast<std::size_t>(layer)]
                                                                           : nullptr;
}

void EntityRenderer::setGroupVisible(int layer, int group, bool visible)
{
    if (layer >= 0 && static_cast<std::size_t>(layer) < m_visible.size() && group >= 0
        && static_cast<std::size_t>(group) < m_visible[static_cast<std::size_t>(layer)].size()) {
        m_visible[static_cast<std::size_t>(layer)][static_cast<std::size_t>(group)] = visible;
    }
}

bool EntityRenderer::isGroupVisible(int layer, int group) const
{
    return layer >= 0 && static_cast<std::size_t>(layer) < m_visible.size() && group >= 0
        && static_cast<std::size_t>(group) < m_visible[static_cast<std::size_t>(layer)].size()
        && m_visible[static_cast<std::size_t>(layer)][static_cast<std::size_t>(group)];
}

bool EntityRenderer::isFeatureShown(const FeatureRef& feature) const
{
    const LayerStyle* layerStyle = style(feature.layer);
    if (layerStyle == nullptr || feature.feature < 0
        || static_cast<std::size_t>(feature.feature) >= layerStyle->featureGroup.size()) {
        return false;
    }
    return isGroupVisible(feature.layer, layerStyle->featureGroup[static_cast<std::size_t>(feature.feature)]);
}

void EntityRenderer::setHighlighted(const FeatureRef& feature)
{
    if (!(feature == m_highlighted)) {
        m_highlighted = feature;
        m_highlightDirty = true;
    }
}

float EntityRenderer::pointSize(int layer, int feature) const
{
    const int icon = m_featureIcon[static_cast<std::size_t>(layer)][static_cast<std::size_t>(feature)];
    if (icon >= 0) {
        return kIconSize;
    }
    // Diameter plus the halo on both sides.
    return static_cast<float>(markerRadiusFor(m_map->layers[static_cast<std::size_t>(layer)].id) * 2.0 + 3.0);
}

void EntityRenderer::build()
{
    m_styles.clear();
    m_visible.clear();
    m_ranges.clear();
    m_featureIcon.clear();
    m_icons.clear();
    m_pointData.clear();
    m_lineData.clear();
    m_tickData.clear();
    m_fillData.clear();
    if (!m_map) {
        return;
    }

    QHash<QString, int> iconIndex;
    for (auto [key, image] : m_map->icons.asKeyValueRange()) {
        if (image.isNull()) {
            continue;
        }
        // Centred on a square, transparent canvas so every array layer has
        // the same size and the icon keeps its aspect ratio.
        QImage canvas(kIconPixels, kIconPixels, QImage::Format_RGBA8888);
        canvas.fill(Qt::transparent);
        const QImage scaled = image.scaled(kIconPixels, kIconPixels, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPainter painter(&canvas);
        painter.drawImage((kIconPixels - scaled.width()) / 2, (kIconPixels - scaled.height()) / 2, scaled);
        painter.end();
        iconIndex.insert(key, static_cast<int>(m_icons.size()));
        m_icons.push_back(canvas);
    }

    const auto& layers = m_map->layers;
    for (std::size_t l = 0; l < layers.size(); ++l) {
        const fh1::Layer& layer = layers[l];
        m_styles.push_back(LayerStyle::of(layer, static_cast<int>(l)));
        const LayerStyle& layerStyle = m_styles.back();
        const auto groupCount = static_cast<std::size_t>(layerStyle.groups.size());
        m_visible.emplace_back(groupCount, true);
        m_ranges.emplace_back(groupCount);
        std::vector<int>& icons = m_featureIcon.emplace_back(layer.features.size(), -1);
        for (std::size_t f = 0; f < layer.features.size(); ++f) {
            const QString& key = layer.features[f].icon;
            if (!key.isEmpty()) {
                icons[f] = iconIndex.value(key, -1);
            }
        }

        std::vector<std::vector<std::size_t>> members(groupCount);
        for (std::size_t f = 0; f < layer.features.size(); ++f) {
            members[static_cast<std::size_t>(layerStyle.featureGroup[f])].push_back(f);
        }
        for (std::size_t g = 0; g < groupCount; ++g) {
            GroupRanges& ranges = m_ranges[l][g];
            const QColor colour = layerStyle.groupColors[g];
            ranges.points.first = static_cast<GLint>(m_pointData.size() / kPointFloats);
            ranges.lines.first = static_cast<GLint>(m_lineData.size() / kLineFloats);
            ranges.ticks.first = static_cast<GLint>(m_tickData.size() / kLineFloats);
            ranges.fills.first = static_cast<GLint>(m_fillData.size() / kLineFloats);
            for (std::size_t f : members[g]) {
                const fh1::Feature& feature = layer.features[f];
                switch (layer.kind) {
                case fh1::FeatureKind::Point: {
                    const QVector3D p = lifted(feature.position, kPointLift);
                    appendVertex(m_pointData, p, colour, 1.0F);
                    m_pointData.push_back(pointSize(static_cast<int>(l), static_cast<int>(f)));
                    m_pointData.push_back(static_cast<float>(icons[f]));
                    if (!feature.forward.isNull()) {
                        const QVector3D ahead(feature.forward.x(), 0.0F, feature.forward.z());
                        if (!ahead.isNull()) {
                            appendVertex(m_tickData, p, Qt::white, 0.9F);
                            appendVertex(m_tickData, p + ahead.normalized() * kTickLength, Qt::white, 0.9F);
                        }
                    }
                    break;
                }
                case fh1::FeatureKind::Polyline:
                    for (const auto& shape : feature.shapes) {
                        for (std::size_t i = 1; i < shape.size(); ++i) {
                            appendVertex(m_lineData, lifted(shape[i - 1], kLineLift), colour, 1.0F);
                            appendVertex(m_lineData, lifted(shape[i], kLineLift), colour, 1.0F);
                        }
                    }
                    break;
                case fh1::FeatureKind::Polygon:
                    for (const auto& shape : feature.shapes) {
                        forEachTriangle(shape, [&](const QVector3D& a, const QVector3D& b, const QVector3D& c) {
                            for (const QVector3D& v : {a, b, c}) {
                                appendVertex(m_fillData, lifted(v, kFillLift), colour, kFillAlpha);
                            }
                        });
                    }
                    for (const auto& [p, q] : outlineEdges(feature)) {
                        appendVertex(m_lineData, lifted(p, kLineLift), colour, 1.0F);
                        appendVertex(m_lineData, lifted(q, kLineLift), colour, 1.0F);
                    }
                    break;
                }
            }
            ranges.points.count = static_cast<GLsizei>(m_pointData.size() / kPointFloats) - ranges.points.first;
            ranges.lines.count = static_cast<GLsizei>(m_lineData.size() / kLineFloats) - ranges.lines.first;
            ranges.ticks.count = static_cast<GLsizei>(m_tickData.size() / kLineFloats) - ranges.ticks.first;
            ranges.fills.count = static_cast<GLsizei>(m_fillData.size() / kLineFloats) - ranges.fills.first;
        }
    }
}

void EntityRenderer::upload()
{
    m_dirty = false;
    fillBuffer(m_points, m_pointData, true);
    fillBuffer(m_lines, m_lineData, false);
    fillBuffer(m_ticks, m_tickData, false);
    fillBuffer(m_fills, m_fillData, false);
    uploadIcons();
}

void EntityRenderer::fillBuffer(GpuBuffer& buffer, const std::vector<float>& data, bool points)
{
    releaseBuffer(buffer);
    if (data.empty()) {
        return;
    }
    glGenVertexArrays(1, &buffer.vao);
    glGenBuffers(1, &buffer.vbo);
    glBindVertexArray(buffer.vao);
    glBindBuffer(GL_ARRAY_BUFFER, buffer.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data(), GL_STATIC_DRAW);
    const int floats = points ? kPointFloats : kLineFloats;
    const auto stride = static_cast<GLsizei>(floats * sizeof(float));
    const auto offset = [](int count) {
        // OpenGL takes offsets into the bound buffer as pointer values.
        return reinterpret_cast<const void*>( // NOLINT(performance-no-int-to-ptr)
            static_cast<std::uintptr_t>(count) * sizeof(float));
    };
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, offset(3));
    if (points) {
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, offset(7));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, offset(8));
    }
    glBindVertexArray(0);
    buffer.vertices = static_cast<GLsizei>(data.size() / static_cast<std::size_t>(floats));
}

void EntityRenderer::uploadIcons()
{
    if (m_iconTexture != 0) {
        glDeleteTextures(1, &m_iconTexture);
        m_iconTexture = 0;
    }
    glGenTextures(1, &m_iconTexture);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_iconTexture);
    // A sampler must have a complete texture bound even when no sprite reads
    // it, so an empty map still gets one transparent layer.
    const auto layers = static_cast<GLsizei>(std::max<std::size_t>(1, m_icons.size()));
    glTexImage3D(
        GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, kIconPixels, kIconPixels, layers, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    if (m_icons.empty()) {
        const std::vector<std::uint8_t> clear(static_cast<std::size_t>(kIconPixels * kIconPixels * 4), 0);
        glTexSubImage3D(
            GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, kIconPixels, kIconPixels, 1, GL_RGBA, GL_UNSIGNED_BYTE, clear.data());
    }
    for (std::size_t i = 0; i < m_icons.size(); ++i) {
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(i), kIconPixels, kIconPixels, 1, GL_RGBA,
            GL_UNSIGNED_BYTE, m_icons[i].constBits());
    }
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

void EntityRenderer::buildHighlight()
{
    m_highlightDirty = false;
    m_highlightPoint.clear();
    m_highlightLines.clear();
    if (m_map && isFeatureShown(m_highlighted)) {
        const fh1::Layer& layer = m_map->layers[static_cast<std::size_t>(m_highlighted.layer)];
        const fh1::Feature& feature = layer.features[static_cast<std::size_t>(m_highlighted.feature)];
        switch (layer.kind) {
        case fh1::FeatureKind::Point:
            appendVertex(m_highlightPoint, lifted(feature.position, kPointLift), Qt::white, 1.0F);
            m_highlightPoint.push_back(pointSize(m_highlighted.layer, m_highlighted.feature));
            m_highlightPoint.push_back(-1.0F);
            break;
        case fh1::FeatureKind::Polyline:
            for (const auto& shape : feature.shapes) {
                for (std::size_t i = 1; i < shape.size(); ++i) {
                    appendVertex(m_highlightLines, lifted(shape[i - 1], kLineLift), Qt::white, 1.0F);
                    appendVertex(m_highlightLines, lifted(shape[i], kLineLift), Qt::white, 1.0F);
                }
            }
            break;
        case fh1::FeatureKind::Polygon:
            for (const auto& [p, q] : outlineEdges(feature)) {
                appendVertex(m_highlightLines, lifted(p, kLineLift), Qt::white, 1.0F);
                appendVertex(m_highlightLines, lifted(q, kLineLift), Qt::white, 1.0F);
            }
            break;
        }
    }
    if (!m_initialized) {
        return;
    }
    fillBuffer(m_highlightPointBuffer, m_highlightPoint, true);
    fillBuffer(m_highlightLineBuffer, m_highlightLines, false);
}

void EntityRenderer::drawLines(const GpuBuffer& buffer, const std::vector<Range>& ranges, float width, bool halo)
{
    if (buffer.vao == 0 || ranges.empty()) {
        return;
    }
    glBindVertexArray(buffer.vao);
    if (halo) {
        m_lineProgram->setUniformValue("uHalo", true);
        m_lineProgram->setUniformValue("uWidth", width + kHaloWidth * 2.0F);
        for (const Range& range : ranges) {
            glDrawArrays(GL_LINES, range.first, range.count);
        }
    }
    m_lineProgram->setUniformValue("uHalo", false);
    m_lineProgram->setUniformValue("uWidth", width);
    for (const Range& range : ranges) {
        glDrawArrays(GL_LINES, range.first, range.count);
    }
}

void EntityRenderer::draw(const QMatrix4x4& worldViewProjection, QSize viewport, const QVector3D& camera,
    float fogDistance, const QVector3D& fogColour, float devicePixelRatio)
{
    if (!m_initialized || !m_map) {
        return;
    }
    if (m_dirty) {
        upload();
    }
    if (m_highlightDirty) {
        buildHighlight();
    }

    std::vector<Range> points;
    std::vector<Range> lines;
    std::vector<Range> ticks;
    std::vector<Range> fills;
    for (std::size_t l = 0; l < m_ranges.size(); ++l) {
        for (std::size_t g = 0; g < m_ranges[l].size(); ++g) {
            if (!m_visible[l][g]) {
                continue;
            }
            const GroupRanges& r = m_ranges[l][g];
            for (auto [range, list] : {std::pair{r.points, &points}, std::pair{r.lines, &lines},
                     std::pair{r.ticks, &ticks}, std::pair{r.fills, &fills}}) {
                if (range.count > 0) {
                    list->push_back(range);
                }
            }
        }
    }

    glViewport(0, 0, viewport.width(), viewport.height());
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    // The same depth values as the world's own geometry, so the world hides
    // what is behind it but the backdrop terrain never does.
    glDepthRange(0.0, WorldRenderer::kForegroundDepthFar);
    // Markers and routes overlap each other freely; only the world hides them.
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    auto common = [&](QOpenGLShaderProgram& program) {
        program.bind();
        program.setUniformValue("uWorldViewProjection", worldViewProjection);
        program.setUniformValue("uCamera", camera);
        program.setUniformValue("uFogDistance", fogDistance);
        program.setUniformValue("uFogColour", fogColour);
    };

    if (m_fills.vao != 0 && !fills.empty()) {
        common(*m_fillProgram);
        glBindVertexArray(m_fills.vao);
        for (const Range& range : fills) {
            glDrawArrays(GL_TRIANGLES, range.first, range.count);
        }
        m_fillProgram->release();
    }

    common(*m_lineProgram);
    m_lineProgram->setUniformValue(
        "uViewport", QVector2D(static_cast<float>(viewport.width()), static_cast<float>(viewport.height())));
    m_lineProgram->setUniformValue("uMaxDistance", std::numeric_limits<float>::max());
    drawLines(m_lines, lines, kLineWidth * devicePixelRatio, true);
    m_lineProgram->setUniformValue("uMaxDistance", kTickDistance);
    drawLines(m_ticks, ticks, 1.5F * devicePixelRatio, true);
    m_lineProgram->release();

    common(*m_pointProgram);
    m_pointProgram->setUniformValue(
        "uViewport", QVector2D(static_cast<float>(viewport.width()), static_cast<float>(viewport.height())));
    m_pointProgram->setUniformValue("uPixelRatio", devicePixelRatio);
    m_pointProgram->setUniformValue("uSizeBoost", 0.0F);
    m_pointProgram->setUniformValue("uRing", false);
    m_pointProgram->setUniformValue("uIcons", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_iconTexture);
    if (m_points.vao != 0) {
        glBindVertexArray(m_points.vao);
        for (const Range& range : points) {
            glDrawArrays(GL_POINTS, range.first, range.count);
        }
    }

    // The selection stays visible through hills, so it can always be found.
    glDisable(GL_DEPTH_TEST);
    if (m_highlightPointBuffer.vao != 0) {
        m_pointProgram->setUniformValue("uRing", true);
        m_pointProgram->setUniformValue("uSizeBoost", kHighlightGrowth);
        glBindVertexArray(m_highlightPointBuffer.vao);
        glDrawArrays(GL_POINTS, 0, m_highlightPointBuffer.vertices);
    }
    m_pointProgram->release();
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    if (m_highlightLineBuffer.vao != 0) {
        common(*m_lineProgram);
        m_lineProgram->setUniformValue(
            "uViewport", QVector2D(static_cast<float>(viewport.width()), static_cast<float>(viewport.height())));
        m_lineProgram->setUniformValue("uMaxDistance", std::numeric_limits<float>::max());
        drawLines(
            m_highlightLineBuffer, {{0, m_highlightLineBuffer.vertices}}, kHighlightLineWidth * devicePixelRatio, true);
        m_lineProgram->release();
    }

    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glDepthRange(0.0, 1.0);
    // QPainter draws the overlay into the same framebuffer next and expects
    // depth testing off.
    glDisable(GL_DEPTH_TEST);
}

std::optional<EntityRenderer::FeatureRef> EntityRenderer::pick(const QMatrix4x4& worldViewProjection, QSize viewport,
    const QVector3D& camera, const QPointF& screenPos, double tolerance, float maxDistance,
    float devicePixelRatio) const
{
    if (!m_map) {
        return std::nullopt;
    }
    // Lower rank wins; within a rank, the smaller screen distance.
    struct Candidate {
        int rank = 0;
        double distance = 0.0;
        FeatureRef feature;
    };
    std::optional<Candidate> best;
    const auto offer = [&best](int rank, double distance, FeatureRef feature) {
        if (!best || rank < best->rank || (rank == best->rank && distance < best->distance)) {
            best = Candidate{rank, distance, feature};
        }
    };
    const auto near = [&](const QVector3D& p) { return (p - camera).length() <= maxDistance; };

    const auto& layers = m_map->layers;
    for (std::size_t l = 0; l < layers.size(); ++l) {
        const fh1::Layer& layer = layers[l];
        for (std::size_t f = 0; f < layer.features.size(); ++f) {
            const FeatureRef ref{static_cast<int>(l), static_cast<int>(f)};
            if (!isFeatureShown(ref)) {
                continue;
            }
            const fh1::Feature& feature = layer.features[f];
            switch (layer.kind) {
            case fh1::FeatureKind::Point: {
                const QVector3D p = lifted(feature.position, kPointLift);
                if (!near(p)) {
                    break;
                }
                const std::optional<QPointF> s = project(worldViewProjection, viewport, p);
                if (!s) {
                    break;
                }
                const double distance = std::hypot(s->x() - screenPos.x(), s->y() - screenPos.y());
                const double reach = std::max(
                    tolerance, static_cast<double>(pointSize(ref.layer, ref.feature) * devicePixelRatio) / 2.0);
                if (distance <= reach) {
                    offer(0, distance, ref);
                }
                break;
            }
            case fh1::FeatureKind::Polyline:
                for (const auto& shape : feature.shapes) {
                    for (std::size_t i = 1; i < shape.size(); ++i) {
                        const QVector3D a = lifted(shape[i - 1], kLineLift);
                        const QVector3D b = lifted(shape[i], kLineLift);
                        if (!near(a) && !near(b)) {
                            continue;
                        }
                        const std::optional<QPointF> sa = project(worldViewProjection, viewport, a);
                        const std::optional<QPointF> sb = project(worldViewProjection, viewport, b);
                        if (sa && sb) {
                            const double distance = distanceToSegment(screenPos, *sa, *sb);
                            if (distance <= tolerance) {
                                offer(1, distance, ref);
                            }
                        }
                    }
                }
                break;
            case fh1::FeatureKind::Polygon:
                for (const auto& shape : feature.shapes) {
                    forEachTriangle(shape, [&](const QVector3D& a, const QVector3D& b, const QVector3D& c) {
                        if (!near(a) && !near(b) && !near(c)) {
                            return;
                        }
                        const auto sa = project(worldViewProjection, viewport, lifted(a, kFillLift));
                        const auto sb = project(worldViewProjection, viewport, lifted(b, kFillLift));
                        const auto sc = project(worldViewProjection, viewport, lifted(c, kFillLift));
                        if (sa && sb && sc && insideTriangle(screenPos, *sa, *sb, *sc)) {
                            offer(2, 0.0, ref);
                        }
                    });
                }
                break;
            }
        }
    }
    return best ? std::optional<FeatureRef>(best->feature) : std::nullopt;
}

std::vector<EntityRenderer::ScreenLabel> EntityRenderer::labels(const QMatrix4x4& worldViewProjection, QSize viewport,
    const QVector3D& camera, float maxDistance, float devicePixelRatio) const
{
    std::vector<ScreenLabel> out;
    if (!m_map) {
        return out;
    }
    const QRectF screen(QPointF(0, 0), QSizeF(viewport));
    const auto& layers = m_map->layers;
    for (std::size_t l = 0; l < layers.size(); ++l) {
        const fh1::Layer& layer = layers[l];
        for (std::size_t f = 0; f < layer.features.size(); ++f) {
            const fh1::Feature& feature = layer.features[f];
            const FeatureRef ref{static_cast<int>(l), static_cast<int>(f)};
            if (feature.label.isEmpty() || !isFeatureShown(ref)) {
                continue;
            }
            QVector3D anchor;
            double clearance = 6.0 * devicePixelRatio;
            bool centred = false;
            switch (layer.kind) {
            case fh1::FeatureKind::Point:
                anchor = lifted(feature.position, kPointLift);
                clearance = (pointSize(ref.layer, ref.feature) / 2.0 + 4.0) * devicePixelRatio;
                break;
            case fh1::FeatureKind::Polyline:
                // Routes are labelled where they start, as on the 2D map.
                if (feature.shapes.empty() || feature.shapes.front().empty()) {
                    continue;
                }
                anchor = lifted(feature.shapes.front().front(), kLineLift);
                break;
            case fh1::FeatureKind::Polygon:
                anchor = lifted(feature.position, kFillLift);
                clearance = 0.0;
                centred = true;
                break;
            }
            const float distance = (anchor - camera).length();
            if (distance > maxDistance) {
                continue;
            }
            const std::optional<QPointF> s = project(worldViewProjection, viewport, anchor);
            if (s && screen.contains(*s)) {
                out.push_back({*s, feature.label, clearance, centred, distance});
            }
        }
    }
    std::sort(
        out.begin(), out.end(), [](const ScreenLabel& a, const ScreenLabel& b) { return a.distance < b.distance; });
    return out;
}

std::pair<QVector3D, QVector3D> EntityRenderer::featureBounds(const FeatureRef& feature) const
{
    if (!m_map || feature.layer < 0 || static_cast<std::size_t>(feature.layer) >= m_map->layers.size()) {
        return {};
    }
    const fh1::Layer& layer = m_map->layers[static_cast<std::size_t>(feature.layer)];
    if (feature.feature < 0 || static_cast<std::size_t>(feature.feature) >= layer.features.size()) {
        return {};
    }
    const fh1::Feature& f = layer.features[static_cast<std::size_t>(feature.feature)];
    QVector3D lo = f.position;
    QVector3D hi = f.position;
    for (const auto& shape : f.shapes) {
        for (const QVector3D& p : shape) {
            lo = QVector3D(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()), std::min(lo.z(), p.z()));
            hi = QVector3D(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()), std::max(hi.z(), p.z()));
        }
    }
    return {lo, hi};
}

int EntityRenderer::paintLabels(QPainter& painter, const std::vector<ScreenLabel>& labels, const QFont& font)
{
    const QFontMetricsF metrics(font);
    std::vector<QRectF> taken;
    int drawn = 0;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    for (const ScreenLabel& label : labels) {
        if (drawn >= kMaxLabels) {
            break;
        }
        const double width = metrics.horizontalAdvance(label.text);
        const double height = metrics.height();
        const QPointF topLeft = label.centred
            ? QPointF(label.position.x() - width / 2.0, label.position.y() - height / 2.0)
            : QPointF(label.position.x() + label.clearance, label.position.y() - height / 2.0);
        const QRectF box(topLeft, QSizeF(width, height));
        const QRectF padded = box.adjusted(-3.0, -2.0, 3.0, 2.0);
        if (std::any_of(taken.begin(), taken.end(), [&padded](const QRectF& r) { return r.intersects(padded); })) {
            continue;
        }
        taken.push_back(padded);
        QPainterPath path;
        path.addText(QPointF(box.left(), box.top() + metrics.ascent()), font, label.text);
        painter.setPen(QPen(QColor(0, 0, 0, 190), 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path);
        painter.fillPath(path, Qt::white);
        ++drawn;
    }
    painter.restore();
    return drawn;
}
