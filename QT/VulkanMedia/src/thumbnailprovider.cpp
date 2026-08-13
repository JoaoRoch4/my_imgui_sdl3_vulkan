#include "thumbnailprovider.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QImageReader>
#include <QLinearGradient>
#include <QMutexLocker>
#include <QPainter>
#include <QQuickTextureFactory>
#include <QThread>
#include <QtMath>

namespace {

constexpr int kThumbWidth = 320;
constexpr int kThumbHeight = 200; // 16:10

//! Stable pseudo-random value in [0, 1) derived from a string.
qreal hashUnit(const QString &s, int salt)
{
    const QByteArray digest = QCryptographicHash::hash(
        (s + QString::number(salt)).toUtf8(), QCryptographicHash::Md5);
    const quint16 v = (quint8(digest[0]) << 8) | quint8(digest[1]);
    return v / 65536.0;
}

/*!
 * Placeholder artwork: a tinted gradient with a soft accent glyph. Deterministic
 * per path, so the grid looks stable across runs while no real decoder is wired
 * up.
 */
QImage drawPlaceholder(const QString &path, const QString &kind, const QSize &size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal h = hashUnit(path, 1);
    const QColor top = QColor::fromHslF(0.62 + h * 0.12, 0.22, 0.20);
    const QColor bottom = QColor::fromHslF(0.66 + h * 0.10, 0.26, 0.13);

    QLinearGradient g(0, 0, size.width(), size.height());
    g.setColorAt(0.0, top);
    g.setColorAt(1.0, bottom);
    p.fillRect(img.rect(), g);

    // A couple of faint accent strokes so cells are visually distinguishable.
    QPen pen(QColor(145, 132, 217, 40));
    pen.setWidthF(size.height() * 0.012);
    p.setPen(pen);
    for (int i = 0; i < 3; ++i) {
        const qreal y = size.height() * (0.25 + 0.25 * i + hashUnit(path, i + 2) * 0.08);
        p.drawLine(QPointF(0, y), QPointF(size.width(), y - size.height() * 0.10));
    }

    QString glyph = QStringLiteral("▦"); // ▦
    if (kind == QLatin1String("folder"))
        glyph = QStringLiteral("▧");
    else if (kind == QLatin1String("video"))
        glyph = QStringLiteral("▶");
    else if (kind == QLatin1String("audio"))
        glyph = QStringLiteral("♪");

    QFont f;
    f.setPixelSize(int(size.height() * 0.30));
    p.setFont(f);
    p.setPen(QColor(181, 171, 252, 90));
    p.drawText(img.rect(), Qt::AlignCenter, glyph);
    p.end();

    return img;
}

} // namespace

// ---------------------------------------------------------------- cache ----

ThumbnailCache::ThumbnailCache(QObject *parent)
    : QObject(parent)
{
}

ThumbnailCache *ThumbnailCache::instance()
{
    static ThumbnailCache cache;
    return &cache;
}

ThumbnailCache *ThumbnailCache::create(QQmlEngine *, QJSEngine *)
{
    ThumbnailCache *c = instance();
    QJSEngine::setObjectOwnership(c, QJSEngine::CppOwnership);
    return c;
}

int ThumbnailCache::ready() const
{
    QMutexLocker lock(&m_mutex);
    return m_ready;
}

int ThumbnailCache::total() const
{
    QMutexLocker lock(&m_mutex);
    return m_total;
}

void ThumbnailCache::beginBatch(int count)
{
    {
        QMutexLocker lock(&m_mutex);
        m_ready = 0;
        m_total = count;
    }
    emit progressChanged();
}

bool ThumbnailCache::lookup(const QString &key, QImage *out)
{
    QMutexLocker lock(&m_mutex);
    if (QImage *hit = m_cache.object(key)) {
        *out = *hit;
        return true;
    }
    return false;
}

void ThumbnailCache::insert(const QString &key, const QImage &image)
{
    QMutexLocker lock(&m_mutex);
    m_cache.insert(key, new QImage(image));
}

void ThumbnailCache::noteDecoded()
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_ready >= m_total)
            return;
        ++m_ready;
    }
    emit progressChanged();
}

void ThumbnailCache::clear()
{
    {
        QMutexLocker lock(&m_mutex);
        m_cache.clear();
        m_ready = 0;
    }
    emit progressChanged();
}

// ------------------------------------------------------------- response ----

ThumbnailResponse::ThumbnailResponse(const QString &id, const QSize &requestedSize)
    : m_id(id)
    , m_requestedSize(requestedSize)
{
    setAutoDelete(false);
}

QQuickTextureFactory *ThumbnailResponse::textureFactory() const
{
    return QQuickTextureFactory::textureFactoryForImage(m_image);
}

void ThumbnailResponse::run()
{
    // id is "<kind>/<path>" — kind only steers the placeholder glyph.
    const int slash = m_id.indexOf(QLatin1Char('/'));
    const QString kind = slash > 0 ? m_id.left(slash) : QString();
    const QString path = slash > 0 ? m_id.mid(slash + 1) : m_id;

    QSize size = m_requestedSize;
    if (size.width() <= 0 || size.height() <= 0)
        size = QSize(kThumbWidth, kThumbHeight);

    const QString key = QStringLiteral("%1|%2x%3").arg(m_id).arg(size.width()).arg(size.height());

    if (ThumbnailCache::instance()->lookup(key, &m_image)) {
        emit finished();
        return;
    }

    // Simulated decode latency, so the loading spinner and the grid's add
    // transition are actually observable in the scaffold. A real decoder
    // replaces this whole block.
    QThread::msleep(ulong(40 + hashUnit(path, 7) * 220));

    QImage decoded;
    if (QFileInfo::exists(path)) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        QSize src = reader.size();
        if (src.isValid()) {
            src.scale(size, Qt::KeepAspectRatio);
            reader.setScaledSize(src);
        }
        decoded = reader.read();
    }

    if (decoded.isNull())
        decoded = drawPlaceholder(path, kind, size);
    else
        decoded = decoded.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

    m_image = decoded;
    ThumbnailCache::instance()->insert(key, m_image);
    ThumbnailCache::instance()->noteDecoded();

    emit finished();
}

// ------------------------------------------------------------- provider ----

ThumbnailProvider::ThumbnailProvider()
{
    m_pool.setMaxThreadCount(qMax(2, QThread::idealThreadCount() - 1));
}

QQuickImageResponse *ThumbnailProvider::requestImageResponse(const QString &id,
                                                             const QSize &requestedSize)
{
    auto *response = new ThumbnailResponse(id, requestedSize);
    m_pool.start(response);
    return response;
}
