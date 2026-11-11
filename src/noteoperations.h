#pragma once

#include <QString>
#include <QStringList>

// Moving and copying notes inside a vault, with the links around them kept
// working. The module works on paths and the note index alone, so the UI never
// has to know how a wikilink resolves; Backend passes the vault it already
// indexes and gets back what changed.
//
// Only the note files themselves move. Obsidian-style ![[image]] embeds
// resolve across the vault and keep working, but a relative Markdown image
// path such as ![](../_attachments/a.png) still points at the old folder after
// a move. Moving those files would risk taking an attachment another note
// still uses, so they are left alone.
class NoteOperations {
public:
    struct Result {
        bool success = false;
        QString error;
        QString sourcePath;      // absolute path the operation started from
        QString destinationPath; // absolute path the note now lives at
        // How many wikilinks and bare up: values this operation rewrote.
        int changedLinks = 0;
        // Files the operation wrote or moved, the note itself included.
        // Backend reloads the open one when a move rewrote a link in it.
        QStringList changedFiles;
        QStringList warnings;
    };

    // The vault root and its notes as paths relative to it, as
    // Backend::m_notePaths holds them. The index decides which notes can be
    // linked to and which files are scanned for links back.
    NoteOperations(QString vaultRoot, QStringList notePaths);

    Result moveNote(const QString &sourcePath, const QString &destinationFolder) const;
    Result copyNote(const QString &sourcePath, const QString &destinationFolder) const;

    // name.md when it is free in the folder, then name 1.md, name 2.md, and so
    // on. The extension of fileName stays as it was.
    static QString uniqueDestinationPath(const QString &destinationFolder,
                                         const QString &fileName);

private:
    Result transfer(const QString &sourcePath, const QString &destinationFolder,
                    bool moving) const;
    QStringList absoluteNotePaths() const;
    QString findNote(const QString &target, const QString &fromPath,
                     const QStringList &notes) const;
    QString bestTarget(const QString &targetPath, const QString &fromPath,
                       const QStringList &notes, bool withExtension) const;
    QString vaultRelative(const QString &absolutePath) const;
    bool inVault(const QString &absolutePath) const;

    QString m_root;
    QStringList m_notes;
};
