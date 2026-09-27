#include "LayerStyle.h"

#include <QHash>

#include <algorithm>
#include <array>

namespace {

/// A fixed categorical palette. The colours are light and saturated enough to
/// read on dark forest and bright desert alike, and are ordered so that
/// neighbouring groups (sorted by size) contrast strongly. Groups past the
/// twelfth reuse colours; the layer tree swatches tell them apart.
QColor groupColorFor(int layerIndex, int group)
{
    static const std::array<QColor, 12> palette{
        QColor(0x4E, 0x9B, 0xFF),
        QColor(0xFF, 0xB0, 0x00),
        QColor(0xFF, 0x5A, 0x5F),
        QColor(0x3D, 0xDC, 0x97),
        QColor(0xC7, 0x7D, 0xFF),
        QColor(0xF2, 0xE9, 0x4E),
        QColor(0x00, 0xD1, 0xD1),
        QColor(0xFF, 0x7F, 0x11),
        QColor(0xFF, 0x8F, 0xAB),
        QColor(0x9B, 0xE5, 0x64),
        QColor(0xB5, 0xB5, 0xFF),
        QColor(0xE0, 0xE0, 0xE0),
    };
    // Offsetting by layer keeps the largest group of each layer on a
    // different colour, so layers stay distinguishable when shown together.
    const std::size_t index = static_cast<std::size_t>(layerIndex * 5 + group) % palette.size();
    return palette[index];
}

} // namespace

LayerStyle LayerStyle::of(const fh1::Layer& layer, int layerIndex)
{
    LayerStyle style;
    QHash<QString, int> counts;
    for (const fh1::Feature& feature : layer.features) {
        ++counts[feature.group];
    }
    style.groups = counts.keys();
    std::sort(style.groups.begin(), style.groups.end(), [&counts](const QString& a, const QString& b) {
        const int ca = counts.value(a);
        const int cb = counts.value(b);
        return ca != cb ? ca > cb : a.compare(b, Qt::CaseInsensitive) < 0;
    });

    QHash<QString, int> index;
    for (int i = 0; i < style.groups.size(); ++i) {
        index.insert(style.groups.at(i), i);
        style.groupSizes.push_back(counts.value(style.groups.at(i)));
        style.groupColors.push_back(layer.groupColours.value(style.groups.at(i), groupColorFor(layerIndex, i)));
    }
    style.featureGroup.reserve(layer.features.size());
    for (const fh1::Feature& feature : layer.features) {
        style.featureGroup.push_back(index.value(feature.group));
    }
    return style;
}

double markerRadiusFor(const QString& layerId)
{
    if (layerId == QLatin1String("gameobjs")) {
        return 5.0;
    }
    if (layerId == QLatin1String("trackroutes")) {
        return 4.0;
    }
    if (layerId == QLatin1String("nav")) {
        return 2.0;
    }
    return 3.0;
}
