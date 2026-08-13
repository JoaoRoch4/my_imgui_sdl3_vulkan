#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QSet>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

/*!
 * Flat listing of one directory, with selection state and a "focused" row that
 * drives the details panel. When the folder does not exist (or is empty on
 * first run) the model falls back to a stub listing so the window renders
 * populated instead of blank.
 */
class FileSystemModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY folderChanged)
    Q_PROPERTY(QString folderName READ folderName NOTIFY folderChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int focusedIndex READ focusedIndex WRITE setFocusedIndex NOTIFY focusedIndexChanged)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY selectionChanged)
    Q_PROPERTY(qint64 selectionBytes READ selectionBytes NOTIFY selectionChanged)
    Q_PROPERTY(bool stubbed READ stubbed NOTIFY folderChanged)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        PathRole,
        KindRole,
        SizeRole,
        ModifiedRole,
        ThumbnailRole,
        SelectedRole,
        FocusedRole,
        DurationRole,   //!< video only, pre-formatted "m:ss"
        DimensionsRole, //!< "1920 × 1080" or empty
        FormatRole      //!< uppercase extension, shown as the preview badge
    };
    Q_ENUM(Roles)

    explicit FileSystemModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString folder() const { return m_folder; }
    void setFolder(const QString &path);
    QString folderName() const;
    bool stubbed() const { return m_stubbed; }

    int focusedIndex() const { return m_focused; }
    void setFocusedIndex(int index);

    int selectionCount() const { return int(m_selection.size()); }
    qint64 selectionBytes() const;

    // --- selection -------------------------------------------------------
    Q_INVOKABLE void selectOnly(int index);
    Q_INVOKABLE void toggleSelected(int index);
    Q_INVOKABLE void selectRange(int from, int to, bool additive = false);
    Q_INVOKABLE void selectIndices(const QList<int> &indices, bool additive = false);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE bool isSelected(int index) const;
    Q_INVOKABLE QList<int> selectedIndices() const;
    Q_INVOKABLE QStringList selectedPaths() const;
    //! All roles of one row as a map, for panels that follow the focused item.
    Q_INVOKABLE QVariantMap get(int index) const;

    // --- navigation / mutation ------------------------------------------
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void goUp();
    Q_INVOKABLE void enter(int index);
    //! Moves the selection to the trash (stub rows are just dropped).
    Q_INVOKABLE int trashSelected();
    //! Deletes the selection from disk (stub rows are just dropped).
    Q_INVOKABLE int deleteSelected();

signals:
    void folderChanged();
    void countChanged();
    void focusedIndexChanged();
    void selectionChanged();
    void enterDirectory(const QString &path);

private:
    struct Entry {
        QString name;
        QString path;
        QString kind; // folder | image | video | audio | other
        qint64 size = 0;
        QDateTime modified;
        QString duration;
        QString dimensions;
        QString format;
    };

    void reload();
    void rewatch();
    void emitSelectionFor(int index);
    static QString kindForSuffix(const QString &suffix);
    static QList<Entry> stubEntries();

    QString m_folder;
    QList<Entry> m_entries;
    QSet<int> m_selection;
    int m_focused = -1;
    bool m_stubbed = false;

    QFileSystemWatcher m_watcher;
    QTimer m_coalesce; //!< watcher events arrive in bursts; reload once
};
