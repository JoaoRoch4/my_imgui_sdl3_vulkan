#include "thumbnailprovider.h"

#include <QDebug>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    // Set before any QSettings use (layout persistence lives in QML).
    QCoreApplication::setOrganizationName(QStringLiteral("VulkanMedia"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("vulkanmedia.local"));
    QCoreApplication::setApplicationName(QStringLiteral("VulkanMedia"));

    // Basic is the only fully themeable built-in style; Fusion would override
    // the palette defined in Theme.qml.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // Qt Multimedia's FFmpeg backend prints a version banner at info level on
    // every launch. Only the banner is dropped — warnings and errors from the
    // category still reach stderr, which the headless smoke test depends on.
    QLoggingCategory::setFilterRules(QStringLiteral("qt.multimedia.ffmpeg.info=false"));

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("thumbnails"), new ThumbnailProvider);

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [](const QUrl &url) {
            qWarning() << "VulkanMedia: failed to create" << url;
            QCoreApplication::exit(-1);
        },
        Qt::QueuedConnection);

    engine.loadFromModule("VulkanMedia", "Main");

    return QGuiApplication::exec();
}
