#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

// One warm snapshot of a vault's notes: the walk, the up:/tags frontmatter,
// the search used by quick open and [[ suggestions, and backlinks. Readers
// share the snapshot, so opening quick open again or following a link does
// not walk and parse the vault all over. A watcher on the vault's folders
// marks it stale when notes move outside Omawrite; invalidation() is the hook
// for changes Omawrite makes itself.
class NoteIndex : public QObject {
    Q_OBJECT

public:
    // A note's frontmatter parents and tags, by root-relative path.
    struct NoteMeta {
        QStringList ups;
        QStringList tags;
    };

    struct Entry {
        QString path;
        QString name;
        QString foldedName;
        QString foldedPath;
        NoteMeta meta;
    };

    explicit NoteIndex(QObject *parent = nullptr);

    // Marks the snapshot stale; the next refresh rereads the vault. A settle
    // timer announces it once the burst of watcher events has passed, so the
    // open file viewer can redraw without waiting for something to ask.
    void invalidate();
    bool isDirty() const { return m_dirty; }

    // Re-reads one note's frontmatter after Omawrite saved it. Backlinks read
    // note bodies from disk when asked, so only the metadata needs updating;
    // a full walk would make every auto-save pay for the whole vault.
    void noteSaved(const QString &relativePath);

    // Re-walks only when the root changed or the snapshot is stale, so a
    // refresh per quick open or per link is cheap once the index is warm.
    bool refresh(const QString &root, bool force = false);

    QString root() const { return m_root; }
    const QList<Entry> &entries() const { return m_entries; }
    QStringList paths() const { return m_paths; }
    NoteMeta meta(const QString &relativePath) const;
    QVariantList suggestions(const QString &query) const;
    // The notes that link to the one at currentRelativePath, with the lines
    // that do. Empty when the note is outside the indexed vault.
    QVariantList backlinks(const QString &currentRelativePath) const;

    // The top-level up: and tags:/tag: keys of a note's frontmatter. Only the
    // head of the file is read, since a vault-wide walk cannot afford whole
    // files. A mapping under up: (an object, not parents) is left alone.
    static NoteMeta readFrontmatter(const QString &absolutePath);

signals:
    // An external change has settled. Readers should refresh and redraw.
    void changed();

private:
    void watchFolders();

    QString m_root;
    QList<Entry> m_entries;
    QStringList m_paths;
    QHash<QString, NoteMeta> m_meta;
    // Notes sharing a bare name need their folder in a [[link]], so the count
    // is kept from the walk rather than rebuilt on every keystroke.
    QHash<QString, int> m_nameCounts;
    bool m_dirty = true;
    bool m_loaded = false;
    QFileSystemWatcher m_watcher;
    QTimer m_settleTimer;
};
