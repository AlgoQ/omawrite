#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTextCursor>

class QFont;

class MarkdownHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit MarkdownHighlighter(QTextDocument *document);

    void setTheme(bool darkMode, const QString &background, const QString &foreground,
                  const QString &accent, const QString &codeBackground);
    void setSearch(const QString &query, int currentMatchStart);
    void setCaret(int position);
    void refreshFont();

    struct Span {
        int start;
        int length;
    };

    enum class InlineKind { Bold, Italic, Strikethrough, Link, WikiLink };

    // Carried from block to block so a fenced run of code knows it is inside
    // one. Stored on the block, which is where Backend reads it.
    // Inside a fence the state is InsideFence plus twice the fence characters
    // past three that opened it, plus one for tildes, since only a fence of the
    // same character at least that long closes it.
    enum BlockState { Prose = 0, InsideFence = 1 };

    struct InlineMarkup {
        InlineKind kind;
        Span content;
        Span markers[2];
        Span destination{};
    };

    // A GitHub-style task marker, "[ ]" or "[x]", after a list marker.
    struct TaskMarker {
        int start = -1;
        int length = 0;
        bool checked = false;
        bool isValid() const { return start >= 0; }
    };

    // Single source of truth for inline markdown spans: the highlighter uses it
    // to style content and hide markers, and Backend to find what the caret
    // sits in.
    static QList<InlineMarkup> inlineMarkup(const QString &text);
    static QString linkUrlAt(const QString &text, int position);
    static QString wikiLinkTargetAt(const QString &text, int position);
    static TaskMarker taskMarker(const QString &text);

    // A line that holds nothing but one image embed: Obsidian's
    // ![[image.png]] or ![[image.png|300]], or Markdown's ![alt](path).
    // Such a line is shown as the image itself while the caret is elsewhere.
    struct ImageEmbed {
        int start = -1;
        int length = 0;
        QString target;
        bool wiki = false;
        // A display width asked for with ![[image.png|300]], or 0.
        int width = 0;
        bool isValid() const { return start >= 0; }
    };
    static ImageEmbed imageEmbed(const QString &text);
    static bool isImageFileName(const QString &name);

    // The width a hidden task marker reserves for its drawn checkbox, so the
    // highlighter and the editor's hanging indents measure it alike.
    static qreal taskCheckboxWidth(const QFont &font);

    // Inline code spans, backticks included.
    static QList<Span> codeSpans(const QString &text);
    static int openingFenceLength(const QString &text);
    static bool isThematicBreak(const QString &text);
    static bool closesFence(const QString &text, int openingLength,
                            QChar marker = QLatin1Char('`'));

    // Where query occurs in text, ignoring case and, for the query's plain
    // Latin and Greek letters, diacritics: "tara" finds "țară". An accented
    // letter in the query matches only itself, though ş and ș, ţ and ț count
    // as one. Folding keeps one UTF-16 unit per unit, so offsets and lengths
    // are the text's own.
    static QList<int> findMatches(const QString &text, const QString &query);

protected:
    void highlightBlock(const QString &text) override;

private:
    void rebuildFormats();
    void rehighlightAll();
    void highlightMarkers(const QString &text);
    void highlightInline(const QString &text);
    void highlightSearch(const QString &text);

    bool m_darkMode = true;
    QString m_customBackground;
    QString m_customForeground;
    QString m_customAccent;
    QString m_customCodeBackground;
    QTextCharFormat m_markerFormat;
    QTextCharFormat m_hiddenMarkerFormat;
    // Hides a task marker while reserving the width of its drawn checkbox.
    QTextCharFormat m_taskMarkerFormat;
    QTextCharFormat m_headingFormat;
    QTextCharFormat m_boldFormat;
    QTextCharFormat m_italicFormat;
    QTextCharFormat m_codeFormat;
    QTextCharFormat m_fenceFormat;
    QTextCharFormat m_quoteFormat;
    QTextCharFormat m_linkFormat;
    QString m_searchQuery;
    int m_currentMatchStart = -1;
    // Moves with edits, so it still finds the block the current match was
    // painted in after text above it changes.
    QTextCursor m_currentMatch;
    // The markers of whatever markup the caret is in or against are shown,
    // so they can be seen and edited.
    QTextCursor m_caret;
    QTextCharFormat m_searchFormat;
    QTextCharFormat m_currentSearchFormat;
};
