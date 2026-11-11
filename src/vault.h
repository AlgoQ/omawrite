#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>

#include <functional>

// The slipbox a note lives in: where its vault root is, which paths are inside
// it, how a [[link]] finds its note, and where a pasted image goes. An
// Obsidian vault (a folder holding .obsidian) and a plain folder of notes
// differ only in how the root is found and where attachments belong, so the
// two flavours sit behind an adapter.
class Vault {
public:
    enum class Kind { Obsidian, PlainFolder };

    class Adapter {
    public:
        virtual ~Adapter() = default;
        virtual Kind kind() const = 0;
        virtual QString attachmentFolder(const Vault &vault) const = 0;
    };

    Vault() = default;

    // The open note, or an empty path for an unsaved document.
    QString noteFile() const { return m_noteFile; }
    void setNoteFile(const QString &absolutePath);
    // The folder only, empty for an unsaved document.
    QString noteDirectory() const;

    // The last save folder, used when no note is open yet. The provider is
    // read on demand, so another window's save is seen without a restart.
    using FolderProvider = std::function<QString()>;
    QString fallbackFolder() const;
    void setFallbackFolder(const QString &absolutePath);
    void setFallbackFolderProvider(FolderProvider provider);

    // The nearest ancestor holding .obsidian, else the note's own folder (or
    // the fallback). Empty when neither is known.
    QString root() const;
    // Re-reads the root, in case .obsidian appeared or went away, and reports
    // whether it changed.
    bool refreshRoot() const;
    Kind kind() const;
    bool isObsidianVault() const { return kind() == Kind::Obsidian; }

    // The folder the file viewer shows: the vault, or the home folder when
    // there is no note and no remembered folder.
    QString treeRoot() const;

    bool contains(const QString &absolutePath) const;
    // absolutePath from the root, or empty when it is outside it.
    QString relativePath(const QString &absolutePath) const;
    QString absolutePath(const QString &relativePath) const;
    // The same path from the file viewer's root, which may be the home folder.
    QString treeRelativePath(const QString &absolutePath) const;

    // The folder links and Markdown paths in the open note resolve against:
    // the note's own folder, or the vault root when it is unsaved.
    QString sourceDirectory() const;
    // The open note's folder, or the remembered one when there is no note.
    QString documentFolder() const;

    // A [[wiki target]] as a root-relative note path. Beside the source note
    // wins, then the vault root, then a bare name that only one note in
    // knownNotes carries; an ambiguous or missing target resolves to nothing.
    QString resolveNote(const QString &target, const QString &sourceDir,
                        const QStringList &knownNotes) const;

    // One level of the file viewer: folders first, then Markdown notes, as
    // paths relative to treeRoot(). Dot folders stay out, as in [[ links.
    QVariantList listFolder(const QString &relativePath) const;

    // Where a pasted image goes. In an Obsidian vault that is its
    // attachmentFolderPath setting; beside a plain note it is _attachments.
    QString attachmentDirectory(bool *vault = nullptr) const;

private:
    const Adapter &adapter() const;
    QString computeRoot() const;

    QString m_noteFile;
    QString m_fallbackFolder;
    FolderProvider m_fallbackFolderProvider;
    mutable QString m_root;
    mutable bool m_rootKnown = false;
};
