#pragma once

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <utility>
#include <vector>

namespace fh1 {

/// A track XML file read as the list of elements under its root, such as
/// the objects of CollObjs.xml, the emitters of ParticleEmitters.xml or the
/// zones of PostProcessingZones_Safe.xml, so that elements can be removed
/// and the rest written back byte for byte.
struct XmlElementsFile {
    /// The file's path under the media folder, as the disc spells it.
    QString mediaPath;
    /// The file's contents, byte-order mark included.
    QByteArray text;
    /// Where each element's text lies in `text`, from the end of the one
    /// before it to the end of its closing line, in file order.
    std::vector<std::pair<qsizetype, qsizetype>> elements;
    /// Their names ("Obj17", "SimpleEmitter"), in the same order.
    std::vector<QString> names;
    /// Where the first element's text starts and the last one's ends.
    qsizetype elementsStart = -1;
    qsizetype elementsEnd = -1;
    /// True if the elements were named Obj0, Obj1… in order, which writing
    /// keeps unbroken after removals.
    bool numbered = false;
};

/// Reads the elements under the root of `data`, a plain ASCII file with or
/// without a UTF-8 byte-order mark. Throws LoadError when the data is
/// malformed or not ASCII, as element positions are counted in bytes.
XmlElementsFile readXmlElements(const QByteArray& data, const QString& source);

/// Removes element `index`.
void removeXmlElement(XmlElementsFile& file, std::size_t index);

/// The file's contents: everything but the removed elements byte for byte,
/// with numbered elements renamed to stay Obj0, Obj1… in order.
QByteArray writeXmlElements(const XmlElementsFile& file);

} // namespace fh1
