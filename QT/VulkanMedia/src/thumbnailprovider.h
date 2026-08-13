#pragma once

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QQuickAsyncImageProvider>
#include <QQuickImageResponse>
#include <QRunnable>
#include <QThreadPool>
#include <QtQml/qqmlregistration.h>

/*!
 * Shared, thread-safe LRU cache plus the "thumbs n/total" progress counter the
 * status bar shows. Exposed to QML as a singleton so the status bar can bind to
 * it directly; the image provider writes into the same instance from worker
 * threads.
 */
class ThumbnailCache : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(int ready READ ready NOTIFY progressChanged)
    Q_PROPERTY(int total READ total NOTIFY progressChanged)

public:
    static ThumbnailCache *instance();
    static ThumbnailCache *create(QQmlEngine *, QJSEngine *);

    int ready() const;
    int total() const;

    //! Announce a new folder load: resets the counter to 0/count.
    void beginBatch(int count);

    bool lookup(const QString &key, QImage *out);
    void insert(const QString &key, const QImage &image);
    void noteDecoded();

    Q_INVOKABLE void clear();

signals:
    void progressChanged();

private:
    explicit ThumbnailCache(QObject *parent = nullptr);

    mutable QMutex m_mutex;
    QCache<QString, QImage> m_cache{192}; // LRU, capped by entry count
    int m_ready = 0;
    int m_total = 0;
};

/*!
 * Decodes one thumbnail off the GUI thread. Falls back to a procedurally drawn
 * placeholder when the source file does not exist (which is the case for the
 * stub folder listing used on first run).
 */
class ThumbnailResponse : public QQuickImageResponse, public QRunnable
{
    Q_OBJECT

public:
    ThumbnailResponse(const QString &id, const QSize &requestedSize);

    QQuickTextureFactory *textureFactory() const override;
    void run() override;

private:
    QString m_id;
    QSize m_requestedSize;
    QImage m_image;
};

class ThumbnailProvider : public QQuickAsyncImageProvider
{
public:
    ThumbnailProvider();

    QQuickImageResponse *requestImageResponse(const QString &id,
                                              const QSize &requestedSize) override;

private:
    QThreadPool m_pool;
};
