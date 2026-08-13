#include "filesystemmodel.h"
#include "thumbnailprovider.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QUrl>

namespace {

const QStringList kImageSuffixes = {"png", "jpg", "jpeg", "webp", "bmp", "tif", "tiff",
                                    "exr", "dpx", "tga", "gif", "avif", "heic"};
const QStringList kVideoSuffixes = {"mp4", "mkv", "mov", "webm", "avi", "m4v", "mxf", "wmv"};
const QStringList kAudioSuffixes = {"mp3", "flac", "wav", "ogg", "opus", "m4a", "aac"};

} // namespace

FileSystemModel::FileSystemModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_coalesce.setSingleShot(true);
    m_coalesce.setInterval(150);
    connect(&m_coalesce, &QTimer::timeout, this, &FileSystemModel::reload);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged,
            this, [this] { m_coalesce.start(); });

    reload(); // stub listing until a folder is assigned
}

QString FileSystemModel::kindForSuffix(const QString &suffix)
{
    const QString s = suffix.toLower();
    if (kImageSuffixes.contains(s))
        return QStringLiteral("image");
    if (kVideoSuffixes.contains(s))
        return QStringLiteral("video");
    if (kAudioSuffixes.contains(s))
        return QStringLiteral("audio");
    return QStringLiteral("other");
}

QList<FileSystemModel::Entry> FileSystemModel::stubEntries()
{
    struct Seed {
        const char *name;
        const char *kind;
        qint64 size;
        const char *duration;
        const char *dimensions;
    };

    // ~20 fake rows so the grid, details panel and status bar all render
    // populated on first run.
    static const Seed seeds[] = {
        {"plates",                    "folder", 0,          "",      ""},
        {"renders",                   "folder", 0,          "",      ""},
        {"audio_stems",               "folder", 0,          "",      ""},
        {"beach_sunset_0001.exr",     "image",  48234496,   "",      "4096 × 2160"},
        {"beach_sunset_0002.exr",     "image",  48591360,   "",      "4096 × 2160"},
        {"beach_sunset_0003.exr",     "image",  47992832,   "",      "4096 × 2160"},
        {"city_night_grade.png",      "image",  12648448,   "",      "3840 × 2160"},
        {"portrait_ref_a.jpg",        "image",  3244032,    "",      "2400 × 3600"},
        {"portrait_ref_b.jpg",        "image",  3391488,    "",      "2400 × 3600"},
        {"texture_atlas_01.png",      "image",  8912896,    "",      "2048 × 2048"},
        {"texture_atlas_02.png",      "image",  9175040,    "",      "2048 × 2048"},
        {"hdri_studio_4k.exr",        "image",  67108864,   "",      "4096 × 2048"},
        {"drone_pass_a.mp4",          "video",  486539264,  "2:14",  "3840 × 2160"},
        {"drone_pass_b.mp4",          "video",  392167424,  "1:48",  "3840 × 2160"},
        {"interview_master.mkv",      "video",  1288490188, "18:32", "1920 × 1080"},
        {"timelapse_rooftop.mov",     "video",  734003200,  "0:42",  "4096 × 2160"},
        {"vfx_comp_v07.mov",          "video",  912261120,  "0:09",  "2048 × 1152"},
        {"room_tone.wav",             "audio",  52428800,   "4:56",  ""},
        {"voiceover_take3.flac",      "audio",  31457280,   "2:03",  ""},
        {"shotlist.md",               "other",  4096,       "",      ""},
        {"grade_notes.txt",           "other",  1820,       "",      ""},
    };

    QList<Entry> out;
    const QDateTime base = QDateTime::currentDateTime().addDays(-3);
    int i = 0;
    for (const Seed &s : seeds) {
        Entry e;
        // fromUtf8, not fromLatin1: the seeds carry non-ASCII (the × in
        // dimensions).
        e.name = QString::fromUtf8(s.name);
        e.kind = QString::fromUtf8(s.kind);
        e.path = QStringLiteral("/media/reference/") + e.name;
        e.size = s.size;
        e.modified = base.addSecs(-i * 4273);
        e.duration = QString::fromUtf8(s.duration);
        e.dimensions = QString::fromUtf8(s.dimensions);
        const QString suffix = QFileInfo(e.name).suffix();
        e.format = e.kind == QLatin1String("folder") ? QStringLiteral("DIR") : suffix.toUpper();
        out.append(e);
        ++i;
    }
    return out;
}

void FileSystemModel::setFolder(const QString &path)
{
    QString clean = path;
    if (clean.startsWith(QLatin1String("file:")))
        clean = QUrl(clean).toLocalFile();
    if (clean == m_folder)
        return;
    m_folder = clean;
    reload();
    emit folderChanged();
}

QString FileSystemModel::folderName() const
{
    if (m_folder.isEmpty() || m_stubbed)
        return QStringLiteral("reference");
    const QString name = QDir(m_folder).dirName();
    return name.isEmpty() ? m_folder : name;
}

void FileSystemModel::rewatch()
{
    const QStringList watched = m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    if (!m_folder.isEmpty() && QFileInfo(m_folder).isDir())
        m_watcher.addPath(m_folder);
}

void FileSystemModel::reload()
{
    QList<Entry> entries;
    bool stubbed = false;

    const QFileInfo info(m_folder);
    if (m_folder.isEmpty() || !info.isDir()) {
        entries = stubEntries();
        stubbed = true;
    } else {
        QDir dir(m_folder);
        const QFileInfoList list =
            dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &fi : list) {
            Entry e;
            e.name = fi.fileName();
            e.path = fi.absoluteFilePath();
            e.kind = fi.isDir() ? QStringLiteral("folder") : kindForSuffix(fi.suffix());
            e.size = fi.isDir() ? 0 : fi.size();
            e.modified = fi.lastModified();
            e.format = fi.isDir() ? QStringLiteral("DIR") : fi.suffix().toUpper();
            entries.append(e);
        }
        // Folders first, then case-insensitive natural order.
        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::sort(entries.begin(), entries.end(), [&collator](const Entry &a, const Entry &b) {
            const bool aDir = a.kind == QLatin1String("folder");
            const bool bDir = b.kind == QLatin1String("folder");
            if (aDir != bDir)
                return aDir;
            return collator.compare(a.name, b.name) < 0;
        });
        if (entries.isEmpty()) {
            entries = stubEntries();
            stubbed = true;
        }
    }

    beginResetModel();
    m_entries = std::move(entries);
    m_stubbed = stubbed;
    m_selection.clear();
    m_focused = m_entries.isEmpty() ? -1 : 0;
    endResetModel();

    rewatch();
    ThumbnailCache::instance()->beginBatch(int(m_entries.size()));

    emit countChanged();
    emit selectionChanged();
    emit focusedIndexChanged();
}

int FileSystemModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_entries.size());
}

QVariant FileSystemModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size())
        return {};

    const Entry &e = m_entries.at(index.row());
    switch (role) {
    case NameRole:
        return e.name;
    case PathRole:
        return e.path;
    case KindRole:
        return e.kind;
    case SizeRole:
        return e.size;
    case ModifiedRole:
        return e.modified;
    case ThumbnailRole:
        // Consumed as `image://thumbnails/<kind>/<path>`.
        return QStringLiteral("image://thumbnails/%1/%2").arg(e.kind, e.path);
    case SelectedRole:
        return m_selection.contains(index.row());
    case FocusedRole:
        return index.row() == m_focused;
    case DurationRole:
        return e.duration;
    case DimensionsRole:
        return e.dimensions;
    case FormatRole:
        return e.format;
    default:
        return {};
    }
}

QHash<int, QByteArray> FileSystemModel::roleNames() const
{
    return {
        {NameRole, "name"},
        {PathRole, "path"},
        {KindRole, "kind"},
        {SizeRole, "size"},
        {ModifiedRole, "modified"},
        {ThumbnailRole, "thumbnail"},
        {SelectedRole, "selected"},
        {FocusedRole, "focused"},
        {DurationRole, "duration"},
        {DimensionsRole, "dimensions"},
        {FormatRole, "format"},
    };
}

qint64 FileSystemModel::selectionBytes() const
{
    qint64 total = 0;
    for (int i : m_selection) {
        if (i >= 0 && i < m_entries.size())
            total += m_entries.at(i).size;
    }
    return total;
}

void FileSystemModel::emitSelectionFor(int index)
{
    if (index < 0 || index >= m_entries.size())
        return;
    const QModelIndex mi = this->index(index, 0);
    emit dataChanged(mi, mi, {SelectedRole});
}

void FileSystemModel::setFocusedIndex(int index)
{
    if (index == m_focused)
        return;
    const int previous = m_focused;
    m_focused = (index >= 0 && index < m_entries.size()) ? index : -1;
    for (int row : {previous, m_focused}) {
        if (row >= 0 && row < m_entries.size()) {
            const QModelIndex mi = this->index(row, 0);
            emit dataChanged(mi, mi, {FocusedRole});
        }
    }
    emit focusedIndexChanged();
}

void FileSystemModel::selectOnly(int index)
{
    const QSet<int> previous = m_selection;
    m_selection.clear();
    if (index >= 0 && index < m_entries.size())
        m_selection.insert(index);
    for (int row : previous)
        emitSelectionFor(row);
    emitSelectionFor(index);
    setFocusedIndex(index);
    emit selectionChanged();
}

void FileSystemModel::toggleSelected(int index)
{
    if (index < 0 || index >= m_entries.size())
        return;
    if (m_selection.contains(index))
        m_selection.remove(index);
    else
        m_selection.insert(index);
    emitSelectionFor(index);
    setFocusedIndex(index);
    emit selectionChanged();
}

void FileSystemModel::selectRange(int from, int to, bool additive)
{
    if (m_entries.isEmpty())
        return;
    const int lo = qBound(0, qMin(from, to), int(m_entries.size()) - 1);
    const int hi = qBound(0, qMax(from, to), int(m_entries.size()) - 1);

    const QSet<int> previous = m_selection;
    if (!additive)
        m_selection.clear();
    for (int i = lo; i <= hi; ++i)
        m_selection.insert(i);

    for (int row : previous)
        emitSelectionFor(row);
    for (int i = lo; i <= hi; ++i)
        emitSelectionFor(i);

    setFocusedIndex(to);
    emit selectionChanged();
}

void FileSystemModel::selectIndices(const QList<int> &indices, bool additive)
{
    const QSet<int> previous = m_selection;
    if (!additive)
        m_selection.clear();
    for (int i : indices) {
        if (i >= 0 && i < m_entries.size())
            m_selection.insert(i);
    }
    for (int row : previous)
        emitSelectionFor(row);
    for (int row : m_selection)
        emitSelectionFor(row);
    if (!indices.isEmpty())
        setFocusedIndex(indices.last());
    emit selectionChanged();
}

void FileSystemModel::selectAll()
{
    m_selection.clear();
    for (int i = 0; i < m_entries.size(); ++i)
        m_selection.insert(i);
    if (!m_entries.isEmpty())
        emit dataChanged(index(0, 0), index(int(m_entries.size()) - 1, 0), {SelectedRole});
    emit selectionChanged();
}

void FileSystemModel::clearSelection()
{
    const QSet<int> previous = m_selection;
    m_selection.clear();
    for (int row : previous)
        emitSelectionFor(row);
    emit selectionChanged();
}

bool FileSystemModel::isSelected(int index) const
{
    return m_selection.contains(index);
}

QList<int> FileSystemModel::selectedIndices() const
{
    QList<int> out(m_selection.cbegin(), m_selection.cend());
    std::sort(out.begin(), out.end());
    return out;
}

QStringList FileSystemModel::selectedPaths() const
{
    QStringList out;
    for (int i : selectedIndices())
        out << m_entries.at(i).path;
    return out;
}

QVariantMap FileSystemModel::get(int row) const
{
    QVariantMap map;
    if (row < 0 || row >= m_entries.size())
        return map;
    const QModelIndex mi = index(row, 0);
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        map.insert(QString::fromUtf8(it.value()), data(mi, it.key()));
    return map;
}

void FileSystemModel::refresh()
{
    reload();
}

void FileSystemModel::goUp()
{
    if (m_folder.isEmpty())
        return;
    QDir dir(m_folder);
    if (dir.cdUp())
        setFolder(dir.absolutePath());
}

void FileSystemModel::enter(int index)
{
    if (index < 0 || index >= m_entries.size())
        return;
    const Entry &e = m_entries.at(index);
    if (e.kind != QLatin1String("folder"))
        return;
    if (m_stubbed) {
        // Nothing on disk to descend into; let QML react (tab title, etc).
        emit enterDirectory(e.path);
        return;
    }
    setFolder(e.path);
    emit enterDirectory(e.path);
}

int FileSystemModel::trashSelected()
{
    const QList<int> rows = selectedIndices();
    int done = 0;
    for (int i = rows.size() - 1; i >= 0; --i) {
        const int row = rows.at(i);
        Entry &e = m_entries[row];
        if (m_stubbed || QFile::moveToTrash(e.path)) {
            beginRemoveRows(QModelIndex(), row, row);
            m_entries.removeAt(row);
            endRemoveRows();
            ++done;
        }
    }
    m_selection.clear();
    setFocusedIndex(m_entries.isEmpty() ? -1 : 0);
    emit countChanged();
    emit selectionChanged();
    return done;
}

int FileSystemModel::deleteSelected()
{
    const QList<int> rows = selectedIndices();
    int done = 0;
    for (int i = rows.size() - 1; i >= 0; --i) {
        const int row = rows.at(i);
        Entry &e = m_entries[row];
        if (m_stubbed || QFile::remove(e.path)) {
            beginRemoveRows(QModelIndex(), row, row);
            m_entries.removeAt(row);
            endRemoveRows();
            ++done;
        }
    }
    m_selection.clear();
    setFocusedIndex(m_entries.isEmpty() ? -1 : 0);
    emit countChanged();
    emit selectionChanged();
    return done;
}
