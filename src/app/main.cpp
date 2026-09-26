#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QSettings>
#include <QSurfaceFormat>

#include <cstdio>

namespace {

std::optional<QPointF> parsePoint(const QString& text)
{
    const QStringList parts = text.split(QLatin1Char(','));
    if (parts.size() != 2) {
        return std::nullopt;
    }
    bool okX = false;
    bool okZ = false;
    const double x = parts.at(0).trimmed().toDouble(&okX);
    const double z = parts.at(1).trimmed().toDouble(&okZ);
    if (!okX || !okZ) {
        return std::nullopt;
    }
    return QPointF(x, z);
}

std::optional<QSize> parseSize(const QString& text)
{
    const QStringList parts = text.toLower().split(QLatin1Char('x'));
    if (parts.size() != 2) {
        return std::nullopt;
    }
    bool okW = false;
    bool okH = false;
    const int w = parts.at(0).toInt(&okW);
    const int h = parts.at(1).toInt(&okH);
    if (!okW || !okH || w < 320 || h < 240) {
        return std::nullopt;
    }
    return QSize(w, h);
}

} // namespace

std::optional<std::array<float, 5>> parseCamera(const QString& text)
{
    const QStringList parts = text.split(QLatin1Char(','));
    if (parts.size() != 5) {
        return std::nullopt;
    }
    std::array<float, 5> values{};
    for (int i = 0; i < 5; ++i) {
        bool ok = false;
        values[static_cast<std::size_t>(i)] = parts.at(i).trimmed().toFloat(&ok);
        if (!ok) {
            return std::nullopt;
        }
    }
    return values;
}

int main(int argc, char* argv[])
{
    // The 3D view needs desktop OpenGL 3.3 core with a depth buffer; this has
    // to be set before the application creates any window. The renderable
    // type must be explicit: on Wayland Qt otherwise picks OpenGL ES, where a
    // 3.3 core request fails (EGL_BAD_MATCH) and takes window compositing
    // down with it.
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("fh1-map-viewer"));
    QApplication::setApplicationDisplayName(QStringLiteral("FH1 Map Viewer"));
    QApplication::setOrganizationName(QStringLiteral("fh1-tools"));
    QApplication::setApplicationVersion(QStringLiteral(FH1_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Map viewer for Forza Horizon (Xbox 360) track data."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(
        QStringLiteral("game-folder"), QStringLiteral("Extracted disc folder (holding media) or the media folder."));
    const QCommandLineOption trackOption(
        QStringLiteral("track"), QStringLiteral("Track folder to open."), QStringLiteral("name"));
    const QCommandLineOption screenshotOption(QStringLiteral("screenshot"),
        QStringLiteral("Save a screenshot of the window to <file> and exit."), QStringLiteral("file"));
    const QCommandLineOption sizeOption(QStringLiteral("size"),
        QStringLiteral("Window size for --screenshot, e.g. 1600x1000."), QStringLiteral("WxH"),
        QStringLiteral("1600x1000"));
    const QCommandLineOption layersOption(QStringLiteral("layers"),
        QStringLiteral("Comma-separated layer ids to show (gameobjs, airoutes, trackroutes, collobjs, particles, "
                       "nav, ppzones); all others start hidden."),
        QStringLiteral("ids"));
    const QCommandLineOption centreOption(
        QStringLiteral("centre"), QStringLiteral("World X,Z to centre the view on."), QStringLiteral("x,z"));
    const QCommandLineOption zoomOption(QStringLiteral("zoom"),
        QStringLiteral("Zoom for --centre, in screen pixels per map pixel."), QStringLiteral("factor"),
        QStringLiteral("2"));
    const QCommandLineOption selectOption(QStringLiteral("select"),
        QStringLiteral("Select the first object with this exact ID or name."), QStringLiteral("name"));
    const QCommandLineOption viewOption(QStringLiteral("view"),
        QStringLiteral("Start in the 2d map or the 3d world view."), QStringLiteral("2d|3d"), QStringLiteral("2d"));
    const QCommandLineOption cameraOption(QStringLiteral("camera"),
        QStringLiteral("3D camera: world X,Y,Z, heading and pitch in degrees (heading 0 = east, 90 = north)."),
        QStringLiteral("x,y,z,heading,pitch"));
    parser.addOptions({trackOption, screenshotOption, sizeOption, layersOption, centreOption, zoomOption, selectOption,
        viewOption, cameraOption});
    parser.process(app);

    const bool scripted = parser.isSet(screenshotOption) || parser.isSet(selectOption) || parser.isSet(centreOption)
        || parser.isSet(viewOption) || parser.isSet(cameraOption);
    MainWindow::ScriptOptions script;
    script.screenshotPath = parser.value(screenshotOption);
    script.select = parser.value(selectOption);
    const QString view = parser.value(viewOption).toLower();
    if (view != QLatin1String("2d") && view != QLatin1String("3d")) {
        std::fprintf(stderr, "--view expects 2d or 3d\n");
        return 2;
    }
    script.world3D = view == QLatin1String("3d");
    if (parser.isSet(cameraOption)) {
        script.camera = parseCamera(parser.value(cameraOption));
        if (!script.camera) {
            std::fprintf(stderr, "--camera expects x,y,z,heading,pitch\n");
            return 2;
        }
    }
    if (parser.isSet(layersOption)) {
        script.visibleLayers = parser.value(layersOption).split(QLatin1Char(','), Qt::SkipEmptyParts);
    }
    if (parser.isSet(centreOption)) {
        script.centre = parsePoint(parser.value(centreOption));
        if (!script.centre) {
            std::fprintf(stderr, "--centre expects x,z\n");
            return 2;
        }
        bool ok = false;
        script.zoom = parser.value(zoomOption).toDouble(&ok);
        if (!ok || script.zoom <= 0.0) {
            std::fprintf(stderr, "--zoom expects a positive number\n");
            return 2;
        }
    }

    const QStringList positional = parser.positionalArguments();
    QString folder = positional.isEmpty() ? QString() : positional.first();
    if (scripted && folder.isEmpty()) {
        std::fprintf(
            stderr, "--screenshot, --select and --centre need a game folder argument (with --view or --camera too)\n");
        return 2;
    }

    MainWindow window;
    if (scripted) {
        const std::optional<QSize> size = parseSize(parser.value(sizeOption));
        if (!size) {
            std::fprintf(stderr, "--size expects WxH, at least 320x240\n");
            return 2;
        }
        window.setScriptOptions(script);
        window.resize(*size);
        if (!script.screenshotPath.isEmpty()) {
            QObject::connect(&window, &MainWindow::scriptFinished, &app, [](int code) { QApplication::exit(code); });
        }
    }
    window.show();

    if (folder.isEmpty()) {
        folder = QSettings().value(QStringLiteral("gameFolder")).toString();
    }
    if (!folder.isEmpty()) {
        const bool opened = window.openGameFolder(folder, parser.value(trackOption));
        if (!opened && scripted) {
            return 1;
        }
    }
    return QApplication::exec();
}
