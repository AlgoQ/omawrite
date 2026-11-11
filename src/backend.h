#pragma once

#include <QObject>
#include <QPointer>
#include <QByteArray>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QHash>
#include <QPair>
#include <QSize>
#include <QUrl>
#include <QVariantList>
#include <memory>

class MarkdownHighlighter;
class QFont;
class QTextBlock;
class QTextDocument;
class QWindow;
class QLockFile;

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl fileUrl READ fileUrl NOTIFY fileUrlChanged)
    Q_PROPERTY(QUrl documentFolder READ documentFolder NOTIFY fileUrlChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY fileUrlChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int wordCount READ wordCount NOTIFY wordCountChanged)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    Q_PROPERTY(qreal textScale READ textScale NOTIFY textScaleChanged)
    Q_PROPERTY(QString themeBackground READ themeBackground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeForeground READ themeForeground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeAccent READ themeAccent NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeSelection READ themeSelection NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeSelectionForeground READ themeSelectionForeground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString editorFont READ editorFont WRITE setEditorFont NOTIFY editorFontChanged)
    Q_PROPERTY(QStringList availableFonts READ availableFonts CONSTANT)
    // The note's YAML frontmatter as Obsidian shows it: rows of keyed values,
    // tags and up: links included. The raw block is kept out of the editor and
    // joined back on save.
    Q_PROPERTY(bool hasProperties READ hasProperties NOTIFY propertiesChanged)
    Q_PROPERTY(QVariantList documentProperties READ documentProperties NOTIFY propertiesChanged)
    Q_PROPERTY(double scrollSpeed READ scrollSpeed WRITE setScrollSpeed NOTIFY scrollSpeedChanged)
    // The file viewer's source: "links" follows the notes' up: links like
    // Obsidian's VirtFolder, "folders" shows the folders on disk.
    Q_PROPERTY(QString treeMode READ treeMode WRITE setTreeMode NOTIFY treeModeChanged)
    // The active key bindings by action id, so the Shortcut items and the
    // settings dialog read the same values and follow overrides live.
    Q_PROPERTY(QVariantMap shortcutMap READ shortcutMap NOTIFY shortcutsChanged)

public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    void setParentWindow(QWindow *window);

    QUrl fileUrl() const { return m_fileUrl; }
    QUrl documentFolder() const;
    QString fileName() const;

    bool modified() const { return m_modified; }
    QString status() const { return m_status; }
    int wordCount() const { return m_wordCount; }
    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool darkMode);
    // What the desktop asks for, times what the user asked for on top of it.
    qreal textScale() const { return m_desktopTextScale * m_textZoom; }
    void setDesktopTextScale(qreal textScale);
    QString themeBackground() const { return m_themeBackground; }
    QString themeForeground() const { return m_themeForeground; }
    QString themeAccent() const { return m_themeAccent; }
    QString themeSelection() const { return m_themeSelection; }
    QString themeCodeBackground() const { return m_themeCodeBackground; }
    QString themeSelectionForeground() const { return m_themeSelectionForeground; }
    QString editorFont() const { return m_editorFont; }
    void setEditorFont(const QString &family);
    QStringList availableFonts() const;
    static QString defaultEditorFont();
    static QStringList selectableFontFamilies(const QStringList &families);
    static int countWords(const QString &text);
    static QString normalizedLinkUrl(const QString &clipboardText);
    static QString suggestedFileName(const QString &text);
    static QString tomlValue(const QString &text);
    static QFont printFont(const QFont &editorFont, qreal screenDpi);

    Q_INVOKABLE void attachDocument(QObject *textDocument);
    Q_INVOKABLE void openDialog();
    Q_INVOKABLE void open(const QUrl &url);
    void openOrStartFile(const QUrl &url);
    Q_INVOKABLE void save();
    Q_INVOKABLE void saveAsDialog();
    Q_INVOKABLE void saveAs(const QUrl &url);
    Q_INVOKABLE void discardRecovery();
    Q_INVOKABLE void reloadFromDisk();
    Q_INVOKABLE void keepExternalVersion();
    Q_INVOKABLE void increaseTextSize();
    Q_INVOKABLE void decreaseTextSize();
    Q_INVOKABLE void resetTextSize();
    Q_INVOKABLE void holdAutoSave(bool held);
    Q_INVOKABLE void printDocument();
    Q_INVOKABLE void copyAsHtml();
    Q_INVOKABLE void newWindow();
    Q_INVOKABLE QString clipboardUrl() const;
    Q_INVOKABLE QString clipboardText() const;
    Q_INVOKABLE bool editorTextChanged();
    Q_INVOKABLE void setCaretPosition(int position);
    Q_INVOKABLE QString linkUrlAt(int position) const;
    Q_INVOKABLE QVariantList taskMarkers() const;
    Q_INVOKABLE bool toggleTaskAt(int position);
    Q_INVOKABLE void openExternalUrl(const QString &url);
    Q_INVOKABLE QVariantMap wikiLinkAt(int position) const;
    Q_INVOKABLE void reportMissingNote(const QString &target);
    Q_INVOKABLE void refreshNoteIndex();
    Q_INVOKABLE QVariantList noteSuggestions(const QString &query);
    // The frontmatter a note carries: its up: parents (wikilink targets with
    // brackets, aliases, and headings stripped) and its tags.
    Q_INVOKABLE QVariantMap noteFrontmatter(const QString &relativePath);
    // The slipbox as a link tree: notes nested under the notes their up:
    // field names, orphans as roots, drawings left out. Each row holds its
    // name, path, url, tags, direct childCount, and nested children rows.
    Q_INVOKABLE QVariantList virtFolderTree();
    // The notes in the slipbox that link to the open one, each with the lines
    // that do: Obsidian's "Linked mentions". Reads from disk, so a note that
    // links here only in unsaved text elsewhere is not counted.
    Q_INVOKABLE QVariantList backlinks();
    // The file viewer's folder tree. The root is the vault (or the open
    // note's folder), or the home folder when nothing is open. listFolder
    // gives one level: folders first, then Markdown notes, as paths relative
    // to the root. Dot folders are left out, as in the [[ suggestions.
    Q_INVOKABLE QString fileTreeRoot() const;
    Q_INVOKABLE QVariantList listFolder(const QString &relativePath) const;
    // The open note's path from the root, or empty when it is not under it.
    Q_INVOKABLE QString fileTreeCurrentPath() const;
    Q_INVOKABLE QVariantMap pasteClipboardImage();
    // Every image line, in document order, with the size it is drawn at in
    // the editor and its own pixel size for the full-window viewer.
    Q_INVOKABLE QVariantList imageEmbeds() const;
    Q_INVOKABLE QUrl imageUrlAt(int position) const;
    Q_INVOKABLE void openImage(const QUrl &url);
    Q_INVOKABLE void setImageAreaWidth(int width);
    // The blocks the editor draws as themselves rather than as Markdown:
    // tables today, each with the rows, alignment and measured geometry the
    // QML overlay needs. Later block kinds join this same list.
    Q_INVOKABLE QVariantList renderedBlocks() const;
    // The writing column's width for rendered blocks. Block formats reserve
    // their height from it, so it is measured again when the column changes.
    Q_INVOKABLE void setTableAreaWidth(int width);
    Q_INVOKABLE QVariantMap emphasisSpanAt(int position, const QString &marker) const;
    // Properties editing. Each rewrite touches only the named key's lines and
    // keeps the rest of the frontmatter, comments and order included.
    Q_INVOKABLE bool addTag(const QString &tag);
    Q_INVOKABLE bool removeTag(const QString &tag);
    Q_INVOKABLE bool removePropertyValue(const QString &key, const QString &value);
    Q_INVOKABLE bool setPropertyValue(const QString &key, const QString &value);
    Q_INVOKABLE bool addProperty(const QString &key, const QString &value);
    Q_INVOKABLE bool removeProperty(const QString &key);
    // Rebindable keys. shortcutSequences falls back to the built-in defaults
    // until setShortcut stores an override; resetShortcut drops it again.
    // shortcutRows feeds the settings and shortcuts dialogs: one map per
    // action with its id, description, and sequences joined for display.
    bool hasProperties() const { return !m_frontmatter.isEmpty(); }
    QVariantList documentProperties() const;
    Q_INVOKABLE QStringList shortcutIds() const;
    Q_INVOKABLE QString shortcutDescription(const QString &id) const;
    Q_INVOKABLE QStringList shortcutSequences(const QString &id) const;
    Q_INVOKABLE QStringList shortcutDefaults(const QString &id) const;
    Q_INVOKABLE bool setShortcut(const QString &id, const QString &text);
    Q_INVOKABLE void resetShortcut(const QString &id);
    Q_INVOKABLE void resetAllShortcuts();
    Q_INVOKABLE QVariantList shortcutRows() const;
    double scrollSpeed() const { return m_scrollSpeed; }
    void setScrollSpeed(double speed);
    QString treeMode() const { return m_treeMode; }
    void setTreeMode(const QString &mode);
    QVariantMap shortcutMap() const;    Q_INVOKABLE int cursorPositionStep(int position, int direction) const;
    Q_INVOKABLE void setSearchHighlight(const QString &query, int currentMatchStart);
    Q_INVOKABLE QVariantList searchMatches(const QString &query) const;
    Q_INVOKABLE void replaceRange(int start, int end, const QString &text);
    Q_INVOKABLE bool insideFence(int position) const;
    Q_INVOKABLE QVariantMap windowGeometry() const;
    Q_INVOKABLE void saveWindowGeometry(int x, int y, int width, int height, bool maximized);
    Q_INVOKABLE void documentFontChanged();

signals:
    void fileUrlChanged();
    void modifiedChanged();
    void statusChanged();
    void wordCountChanged();
    void darkModeChanged();
    void textScaleChanged();
    void themeColorsChanged();
    void editorFontChanged();
    void saveFailed();
    void openDialogRequested();
    void saveDialogRequested(const QUrl &suggestedUrl);
    void saveSucceeded();
    void externalChangeDetected(bool deleted, bool locallyModified);
    void scrollSpeedChanged();
    void propertiesChanged();
    void treeModeChanged();
    void shortcutsChanged();

private:
    friend class OmawriteTest;

    void loadDocumentText(const QString &text);
    QString fullDocumentText() const;
    void frontmatterEdited();
    void applyHangingIndents(bool clearUndo);
    QString notesRoot() const;
    QUrl resolveNote(const QString &target) const;
    // A note's frontmatter parents and tags, by index-relative path. Read
    // with the index, so the link tree never re-reads the vault per row.
    struct NoteMeta {
        QStringList ups;
        QStringList tags;
    };
    // An up: target resolved against the note that names it, as an
    // index-relative path. Empty when it names nothing unambiguous.
    QString resolveUpTarget(const QString &target, const QString &sourceDir) const;
    static NoteMeta readNoteFrontmatter(const QString &absolutePath);
    QString attachmentDirectory(bool *vault) const;
    QString resolveImage(const QString &target, bool wiki) const;
    // Where an image line's picture is and the size it is drawn at, or an
    // empty size when the block is not one or its file cannot be read.
    // natural receives the picture's own size in logical pixels.
    QSize blockImageSize(const QTextBlock &block, QString *path = nullptr,
                         QSize *natural = nullptr) const;
    qreal blockImageMargin(const QTextBlock &block) const;
    int imagePadding() const;
    // The width a rendered block is drawn at: the writing column once QML has
    // measured it, then the document's own width, then a sane fallback.
    int blockAreaWidth() const;
    qreal steppedTextZoom(int direction) const;
    void setTextZoom(qreal zoom);
    void setFileUrl(const QUrl &url);
    void setModified(bool modified);
    void setStatus(const QString &status);
    void saveTo(const QUrl &url);
    QUrl suggestedSaveUrl() const;
    QString currentDocumentText() const;
    void setWordCount(int words);
    void refreshWordCount();
    void scheduleWordCount();
    void applyDocumentTypography();
    void reapplyTypographyToChange();
    void scheduleRecovery();
    void writeRecovery();
    void scheduleAutoSave();
    void autoSave();
    void suspendAutoSave();
    void restoreRecovery();
    void clearRecovery();
    bool diskMatchesKnownContents() const;
    void reportExternalChange(bool deleted);
    static bool savesUnchanged(const QByteArray &raw);
    void setKnownFileContents(const QByteArray &contents, bool known);
    void unwatchFiles();
    void watchCurrentFile();
    void checkWatchedFile();
    void loadOmarchyTheme();
    void watchOmarchyTheme();

    QUrl m_fileUrl;
    bool m_modified = false;
    QString m_status;
    int m_wordCount = 0;
    bool m_darkMode = true;
    // The desktop's light/dark setting, used when the Omarchy theme names none.
    bool m_systemDarkMode = true;
    qreal m_desktopTextScale = 1.0;
    qreal m_textZoom = 1.0;
    bool m_loading = false;
    bool m_formattingTypography = false;
    int m_lastChangePos = 0;
    int m_lastChangeAdded = 0;
    QTimer m_wordCountTimer;
    // Counting a large document stalls typing, so counts run on a worker
    // thread. A load outdates the count in flight, and its result is dropped.
    QFutureWatcher<int> m_wordCountWatcher;
    bool m_wordCountStale = false;
    bool m_wordCountAgain = false;
    QTimer m_recoveryTimer;
    QTimer m_autoSaveTimer;
    QFileSystemWatcher m_fileWatcher;
    QTimer m_fileCheckTimer;
    bool m_fileMissing = false;
    QPointer<QTextDocument> m_document;
    QPointer<QWindow> m_parentWindow;
    QPointer<MarkdownHighlighter> m_highlighter;
    int m_caretPosition = 0;
    QString m_lastDocumentText;
    // The note's YAML frontmatter, fences included, kept out of the editor so
    // properties can be drawn in its place; currentDocumentText() is the body.
    QString m_frontmatter;
    QByteArray m_lastKnownFileContents;
    QString m_lastKnownFileText;
    bool m_hasKnownFileContents = false;
    bool m_autoSaveSuspended = false;
    bool m_autoSaveHeld = false;
    QString m_recoveryPath;
    std::unique_ptr<QLockFile> m_recoveryLock;
    // Markdown notes beside the open document, cached for [[ suggestions.
    QString m_notesRoot;
    QStringList m_notePaths;
    bool m_notesIndexed = false;
    QHash<QString, NoteMeta> m_noteMeta;
    // Wheel-scroll multiplier for the editor and the file viewer, and which
    // tree the viewer shows. Both persist across windows.
    double m_scrollSpeed = 2.0;
    QString m_treeMode = QStringLiteral("links");
    // Pasted and embedded images: resolved paths by target, pixel sizes by
    // path, and the widest an image is drawn in the editor.
    mutable QHash<QString, QString> m_imagePaths;
    mutable QHash<QString, QSize> m_imageSizes;
    int m_imageAreaWidth = 0;
    int m_tableAreaWidth = 0;
    // The block ranges tables occupied after the last geometry pass. An edit
    // that dissolves a table still has to clear the margins and hidden styles
    // its old lines carried, so they are remembered here.
    QList<QPair<int, int>> m_tableBlockRanges;

    QString m_themeBackground;
    QString m_themeForeground;
    QString m_themeAccent;
    QString m_themeSelection;
    QString m_themeCodeBackground;
    QString m_themeSelectionForeground;
    QString m_editorFont;
    QFileSystemWatcher m_themeWatcher;
    QTimer m_themeReloadTimer;
};
