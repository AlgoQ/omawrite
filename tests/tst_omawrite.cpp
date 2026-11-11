#include <QtTest>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QSignalSpy>
#include <QSemaphore>
#include <QFont>
#include <QImage>
#include <QImageReader>
#include <QRegularExpression>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QStandardPaths>
#include <QClipboard>
#include <QMimeData>
#include "cli.h"

#include "backend.h"
#include "markdownhighlighter.h"
#include "noteindex.h"
#include "vault.h"

// Counts QML "Binding loop detected" warnings while it is alive, forwarding
// everything to the previous handler so test output stays intact.
class BindingLoopCounter {
public:
    BindingLoopCounter() {
        s_count = 0;
        m_previous = qInstallMessageHandler(handle);
        s_previous = m_previous;
    }
    ~BindingLoopCounter() { qInstallMessageHandler(m_previous); }
    int count() const { return s_count; }

private:
    static void handle(QtMsgType type, const QMessageLogContext &context, const QString &message) {
        if (message.contains(QLatin1String("Binding loop detected")))
            ++s_count;
        if (s_previous)
            s_previous(type, context, message);
    }
    static inline int s_count = 0;
    static inline QtMessageHandler s_previous = nullptr;
    QtMessageHandler m_previous = nullptr;
};

class OmawriteTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QCoreApplication::setApplicationVersion(QStringLiteral("1.2.3-test"));
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_settingsDirectory.isValid());
        QQuickStyle::setStyle(QStringLiteral("Material"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           m_settingsDirectory.path());
    }

    void countsWords() {
        QCOMPARE(Backend::countWords(QStringLiteral("one two-three don't 42")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("你好世界")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("你好 世界")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("中文 iPad AI")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("foo中bar")), 3);
        QCOMPARE(Backend::countWords(QStringLiteral("日本語テスト")), 6);
        QCOMPARE(Backend::countWords(QStringLiteral("你好，世界。")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("コーヒー")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("か\u3099")), 1); // が, decomposed
        QCOMPARE(Backend::countWords(QStringLiteral("𠀀𠀁 foo")), 3);
        QCOMPARE(Backend::countWords(QStringLiteral("안녕하세요 세계")), 2);
        QCOMPARE(Backend::countWords(QString()), 0);
        // A task's checkbox is state, not a word, checked or not.
        QCOMPARE(Backend::countWords(QStringLiteral("- [ ] one\n- [x] two")), 2);
        QCOMPARE(Backend::countWords(QStringLiteral("- [x] done\nplain [x] text")), 4);
    }

    void findsTaskMarkers() {
        const auto marker = [](const QString &text) {
            return MarkdownHighlighter::taskMarker(text);
        };

        const auto unchecked = marker(QStringLiteral("- [ ] one"));
        QVERIFY(unchecked.isValid());
        QCOMPARE(unchecked.start, 2);
        QCOMPARE(unchecked.length, 3);
        QVERIFY(!unchecked.checked);

        const auto checked = marker(QStringLiteral("   1. [X] done"));
        QVERIFY(checked.isValid());
        QCOMPARE(checked.start, 6);
        QVERIFY(checked.checked);

        const auto bare = marker(QStringLiteral("* [ ]"));
        QVERIFY(bare.isValid());
        QCOMPARE(bare.start, 2);

        // The marker needs a list item and whitespace (or the line end) after
        // the closing bracket.
        QVERIFY(!marker(QStringLiteral("- [ ]task")).isValid());
        QVERIFY(!marker(QStringLiteral("plain [ ] text")).isValid());
        QVERIFY(!marker(QStringLiteral("- [y] task")).isValid());
        QVERIFY(!marker(QStringLiteral("> - [ ] quote")).isValid());
    }

    void findsImageEmbeds() {
        const auto embed = [](const QString &text) {
            return MarkdownHighlighter::imageEmbed(text);
        };

        // Obsidian's embed, with and without a width, and Markdown's, whose
        // path may hold spaces or a title.
        const auto wiki = embed(QStringLiteral("![[Screenshot 2026-11-10.png]]"));
        QVERIFY(wiki.isValid());
        QVERIFY(wiki.wiki);
        QCOMPARE(wiki.target, QStringLiteral("Screenshot 2026-11-10.png"));
        QCOMPARE(wiki.start, 0);
        QCOMPARE(wiki.width, 0);

        const auto sized = embed(QStringLiteral("  ![[diagram.png|300]]  "));
        QVERIFY(sized.isValid());
        QCOMPARE(sized.target, QStringLiteral("diagram.png"));
        QCOMPARE(sized.width, 300);

        const auto markdown = embed(QStringLiteral("![alt](<images/a b.png> \"title\")"));
        QVERIFY(markdown.isValid());
        QVERIFY(!markdown.wiki);
        QCOMPARE(markdown.target, QStringLiteral("images/a b.png"));

        const auto plain = embed(QStringLiteral("![](assets/pic.webp)"));
        QVERIFY(plain.isValid());
        QCOMPARE(plain.target, QStringLiteral("assets/pic.webp"));

        // Only a line holding an embed is drawn as an image, and only an
        // image file, so links and notes stay text.
        QVERIFY(!embed(QStringLiteral("![[Note]]")).isValid());
        QVERIFY(!embed(QStringLiteral("before ![[pic.png]]")).isValid());
        QVERIFY(!embed(QStringLiteral("![]()")).isValid());
        QVERIFY(!embed(QStringLiteral("![](page.html)")).isValid());
        QVERIFY(MarkdownHighlighter::isImageFileName(QStringLiteral("PIC.JPEG")));
        QVERIFY(!MarkdownHighlighter::isImageFileName(QStringLiteral("note.md")));
    }

    void searchIgnoresDiacriticsTheQueryLeavesOut() {
        using Matches = QList<int>;
        const auto find = [](const QString &text, const QString &query) {
            return MarkdownHighlighter::findMatches(text, query);
        };

        // Plain letters find their accented forms, in either case.
        QCOMPARE(find(QStringLiteral("Țară și ŞTIRI"), QStringLiteral("tara")), Matches({0}));
        QCOMPARE(find(QStringLiteral("Țară și ŞTIRI"), QStringLiteral("si")), Matches({5}));
        QCOMPARE(find(QStringLiteral("Țară și ŞTIRI"), QStringLiteral("stiri")), Matches({8}));
        QCOMPARE(find(QStringLiteral("în câmp"), QStringLiteral("in camp")), Matches({0}));
        QCOMPARE(find(QStringLiteral("élève, Łódź, Straße"), QStringLiteral("eleve")), Matches({0}));
        QCOMPARE(find(QStringLiteral("élève, Łódź, Straße"), QStringLiteral("lodz")), Matches({7}));

        // An accented letter in the query matches only itself, though the
        // cedilla and comma-below forms of s and t are one letter.
        QCOMPARE(find(QStringLiteral("tara țară"), QStringLiteral("țară")), Matches({5}));
        QCOMPARE(find(QStringLiteral("tara Ţară"), QStringLiteral("țară")), Matches({5}));
        QCOMPARE(find(QStringLiteral("mama mamă"), QStringLiteral("mamă")), Matches({5}));

        // Scripts whose marks make different letters keep them.
        QCOMPARE(find(QStringLiteral("й и"), QStringLiteral("и")), Matches({2}));
        QCOMPARE(find(QStringLiteral("が か"), QStringLiteral("か")), Matches({2}));
        QCOMPARE(find(QStringLiteral("한 하"), QStringLiteral("하")), Matches({2}));

        // Letters beyond the Basic Multilingual Plane still ignore case.
        QCOMPARE(find(QStringLiteral("x 𐐀𐐁"), QStringLiteral("𐐨𐐩")), Matches({2}));
        QCOMPARE(find(QStringLiteral("x 𐐨𐐩"), QStringLiteral("𐐀𐐁")), Matches({2}));

        // Matches do not overlap, and a near miss does not hide the next one.
        QCOMPARE(find(QStringLiteral("aaa"), QStringLiteral("aa")), Matches({0}));
        QCOMPARE(find(QStringLiteral("tțara"), QStringLiteral("țara")), Matches({1}));
    }

    void restylesTheWholeDocumentWithItsLayoutFinished() {
        // Qt Quick's render thread lays out any block left without a layout,
        // starting the layout's timers from the wrong thread.
        QTextDocument document;
        document.setTextWidth(600);
        QStringList lines;
        for (int i = 0; i < 3000; ++i)
            lines << QStringLiteral("line %1 with **bold** and a [link](u)").arg(i);
        document.setPlainText(lines.join(QLatin1Char('\n')));
        MarkdownHighlighter highlighter(&document);
        document.documentLayout()->documentSize();

        const auto unlaidBlocks = [&document] {
            int count = 0;
            for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
                if (block.layout()->lineCount() == 0)
                    ++count;
            }
            return count;
        };
        highlighter.setSearch(QStringLiteral("line"), -1);
        QCOMPARE(unlaidBlocks(), 0);
        highlighter.setSearch(QString(), -1);
        QCOMPARE(unlaidBlocks(), 0);
        highlighter.setTheme(false, QString(), QString(), QString(), QString());
        QCOMPARE(unlaidBlocks(), 0);
    }

    void normalizesLinks() {
        QCOMPARE(Backend::normalizedLinkUrl(QStringLiteral("www.example.com/path")),
                 QStringLiteral("https://www.example.com/path"));
        QCOMPARE(Backend::normalizedLinkUrl(QStringLiteral("mailto:writer@example.com")),
                 QStringLiteral("mailto:writer@example.com"));
        QVERIFY(Backend::normalizedLinkUrl(QStringLiteral("example.com")).isEmpty());
        QVERIFY(Backend::normalizedLinkUrl(QStringLiteral("file:///tmp/private")).isEmpty());
    }

    void suggestsSafeNames() {
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("My first draft\nBody")),
                 QStringLiteral("My first draft.md"));
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("A/B")), QStringLiteral("A-B.md"));
        QCOMPARE(Backend::suggestedFileName(QString()), QStringLiteral("Untitled.md"));
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("Already.md")),
                 QStringLiteral("Already.md"));
    }

    void answersHelpBeforeOpeningAWindow() {
        const QString usage = Cli::usage();
        QVERIFY(usage.contains(QStringLiteral("omawrite [FILE]")));
        QVERIFY(usage.contains(QStringLiteral("-h, --help")));
        QVERIFY(usage.contains(QStringLiteral("-v, --version")));

        const QString version = QStringLiteral("1.2.3");
        const auto answer = [&](const QStringList &arguments) {
            return Cli::handleArguments(QStringList{QStringLiteral("omawrite")} + arguments,
                                        version);
        };
        QCOMPARE(answer({QStringLiteral("--help")}), std::optional<int>(0));
        QCOMPARE(answer({QStringLiteral("-h")}), std::optional<int>(0));
        QCOMPARE(answer({QStringLiteral("--version")}), std::optional<int>(0));
        QCOMPARE(answer({QStringLiteral("-v")}), std::optional<int>(0));

        // Everything else is the app's: no arguments, a file, and Qt's own
        // options, which a leading dash must not turn into an error.
        QCOMPARE(answer({}), std::nullopt);
        QCOMPARE(answer({QStringLiteral("draft.md")}), std::nullopt);
        QCOMPARE(answer({QStringLiteral("-platform"), QStringLiteral("offscreen")}),
                 std::nullopt);
        QCOMPARE(answer({QStringLiteral("-notes.md")}), std::nullopt);
        QVERIFY(usage.contains(QStringLiteral("--update")));
    }

    void findsTheCheckoutToUpdateFrom() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray previous = qgetenv("OMAWRITE_SOURCE_DIR");

        // A folder without bin/install is not a checkout, and --update says
        // so instead of running anything.
        qputenv("OMAWRITE_SOURCE_DIR", dir.path().toLocal8Bit());
        QCOMPARE(Cli::sourceDirectory(), QString());
        QCOMPARE(Cli::handleArguments({QStringLiteral("omawrite"), QStringLiteral("--update")},
                                      QStringLiteral("1.2.3")),
                 std::optional<int>(1));

        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("bin")));
        QFile script(QDir(dir.path()).filePath(QStringLiteral("bin/install")));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/usr/bin/env sh\n");
        script.close();
        QVERIFY(script.setPermissions(script.permissions() | QFileDevice::ExeOwner));
        QCOMPARE(Cli::sourceDirectory(), QDir(dir.path()).absolutePath());

        if (previous.isNull())
            qunsetenv("OMAWRITE_SOURCE_DIR");
        else
            qputenv("OMAWRITE_SOURCE_DIR", previous);
    }

    void printsInPointsNotPixels() {
        QFont editorFont;
        editorFont.setPixelSize(20);
        const QFont printed = Backend::printFont(editorFont, 96.0);
        QCOMPARE(printed.pixelSize(), -1);
        QCOMPARE(printed.pointSizeF(), 15.0);
        QCOMPARE(Backend::printFont(editorFont, 0.0).pointSizeF(), 15.0);

        QFont pointFont;
        pointFont.setPointSizeF(11.0);
        QCOMPARE(Backend::printFont(pointFont, 144.0).pointSizeF(), 11.0);
    }

    void keepsNotoFromCrowdingTheFontList() {
        const QStringList fonts = Backend::selectableFontFamilies({
            QStringLiteral("Noto Sans Devanagari"), QStringLiteral("Liberation Serif"),
            QStringLiteral("Noto Serif"), QStringLiteral("Noto Sans Tamil UI"),
            QStringLiteral("adwaita Sans"), QStringLiteral("IBM Plex Mono"),
            QStringLiteral("Noto Sans"), QStringLiteral("Noto Sans Mono"),
            QStringLiteral("Liberation Serif"), QStringLiteral("Monospace"),
            QStringLiteral("Standard Symbols PS"), QStringLiteral("D050000L"),
            QStringLiteral("Noto Color Emoji"), QStringLiteral("Nimbus Sans [UKWN]"),
            QStringLiteral("Nimbus Sans [URW ]")});
        QCOMPARE(fonts, QStringList({QStringLiteral("IBM Plex Mono"),
                                     QStringLiteral("adwaita Sans"),
                                     QStringLiteral("Liberation Serif"),
                                     QStringLiteral("Nimbus Sans"),
                                     QStringLiteral("Noto Sans"),
                                     QStringLiteral("Noto Sans Mono"),
                                     QStringLiteral("Noto Serif")}));
    }

    void remembersEditorFont() {
        const QString installed = Backend().availableFonts().value(1);
        QVERIFY(!installed.isEmpty());

        {
            Backend backend;
            QCOMPARE(backend.editorFont(), Backend::defaultEditorFont());
            QSignalSpy fontSpy(&backend, &Backend::editorFontChanged);
            backend.setEditorFont(installed);
            QCOMPARE(fontSpy.count(), 1);
        }

        QCOMPARE(Backend().editorFont(), installed);

        // A remembered font that has since been uninstalled falls back.
        QSettings().setValue(QStringLiteral("editor/font"), QStringLiteral("No Such Font"));
        QCOMPARE(Backend().editorFont(), Backend::defaultEditorFont());
        QSettings().remove(QStringLiteral("editor/font"));
    }

    void findsStrikethroughOutsideCode() {
        const auto markup = MarkdownHighlighter::inlineMarkup(QStringLiteral("a ~~gone~~ b"));
        QCOMPARE(markup.size(), 1);
        QCOMPARE(markup.at(0).kind, MarkdownHighlighter::InlineKind::Strikethrough);
        QCOMPARE(markup.at(0).content.start, 4);
        QCOMPARE(markup.at(0).content.length, 4);

        // Tildes in code, beside space, or in longer runs stay literal.
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("`~~x~~`")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("a ~~ b ~~ c")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("~~~x~~~")).isEmpty());
    }

    void findsWikilinks() {
        const QString text = QStringLiteral(
            "See [[Note]] and [[Other|alias]] and [[Third#Heading]].");
        const auto markup = MarkdownHighlighter::inlineMarkup(text);
        QCOMPARE(markup.size(), 3);

        QCOMPARE(markup.at(0).kind, MarkdownHighlighter::InlineKind::WikiLink);
        QCOMPARE(markup.at(0).content.start, int(text.indexOf(QStringLiteral("Note"))));
        QCOMPARE(markup.at(0).content.length, 4);
        QCOMPARE(markup.at(0).destination.start, int(text.indexOf(QStringLiteral("Note"))));
        QCOMPARE(markup.at(0).destination.length, 4);

        QCOMPARE(markup.at(1).kind, MarkdownHighlighter::InlineKind::WikiLink);
        QCOMPARE(markup.at(1).content.start, int(text.indexOf(QStringLiteral("alias"))));
        QCOMPARE(markup.at(1).content.length, 5);
        QCOMPARE(markup.at(1).destination.start, int(text.indexOf(QStringLiteral("Other"))));
        QCOMPARE(markup.at(1).destination.length, 5);
        QCOMPARE(markup.at(1).markers[0].start,
                 int(text.indexOf(QStringLiteral("[[Other"))));

        QCOMPARE(markup.at(2).destination.start, int(text.indexOf(QStringLiteral("Third"))));
        QCOMPARE(markup.at(2).destination.length, 5);
        QCOMPARE(markup.at(2).content.length,
                 int(QStringLiteral("Third#Heading").size()));
    }

    void keepsWikilinksLiteralInCode() {
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("`[[Note]]`")).isEmpty());

        const QString text = QStringLiteral("See [[Note|alias]].");
        QCOMPARE(MarkdownHighlighter::wikiLinkTargetAt(text, int(text.indexOf(QStringLiteral("Note")))),
                 QStringLiteral("Note"));
        QCOMPARE(MarkdownHighlighter::wikiLinkTargetAt(text, int(text.indexOf(QStringLiteral("alias")))),
                 QStringLiteral("Note"));
        QVERIFY(MarkdownHighlighter::wikiLinkTargetAt(text, 0).isEmpty());
    }

    void suggestsNotesAcrossTheSlipbox() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral(".obsidian")));
        QVERIFY(root.mkpath(QStringLiteral("ai")));
        const auto write = [&root](const QString &name) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write("# note\n");
            return true;
        };
        QVERIFY(write(QStringLiteral("Note.md")));
        QVERIFY(write(QStringLiteral("ai/Note.md")));
        QVERIFY(write(QStringLiteral("ai/skill list.md")));
        QVERIFY(write(QStringLiteral("Branding.md")));
        QVERIFY(write(QStringLiteral("current.md")));

        Backend backend;
        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("current.md"))));
        QCOMPARE(backend.documentFolder(), QUrl::fromLocalFile(root.path()));
        backend.refreshNoteIndex();

        const QVariantList all = backend.noteSuggestions(QString());
        QCOMPARE(all.size(), 5);
        QCOMPARE(all.constFirst().toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Branding"));

        const QVariantList notes = backend.noteSuggestions(QStringLiteral("note"));
        QCOMPARE(notes.size(), 2);
        bool sawRoot = false;
        bool sawNested = false;
        for (const QVariant &value : notes) {
            const QVariantMap note = value.toMap();
            const QString path = note.value(QStringLiteral("path")).toString();
            if (path == QStringLiteral("Note.md")) {
                QCOMPARE(note.value(QStringLiteral("insert")).toString(), QStringLiteral("Note"));
                sawRoot = true;
            } else if (path == QStringLiteral("ai/Note.md")) {
                QCOMPARE(note.value(QStringLiteral("insert")).toString(),
                         QStringLiteral("ai/Note"));
                sawNested = true;
            }
        }
        QVERIFY(sawRoot);
        QVERIFY(sawNested);

        QCOMPARE(backend.resolveNote(QStringLiteral("Branding")),
                 QUrl::fromLocalFile(root.filePath(QStringLiteral("Branding.md"))));
        QCOMPARE(backend.resolveNote(QStringLiteral("ai/Note")),
                 QUrl::fromLocalFile(root.filePath(QStringLiteral("ai/Note.md"))));
        QCOMPARE(backend.resolveNote(QStringLiteral("skill list")),
                 QUrl::fromLocalFile(root.filePath(QStringLiteral("ai/skill list.md"))));
        QCOMPARE(backend.resolveNote(QStringLiteral("Note")),
                 QUrl::fromLocalFile(root.filePath(QStringLiteral("Note.md"))));
        QVERIFY(backend.resolveNote(QStringLiteral("missing")).isEmpty());
    }

    void listsNotesThatLinkToTheOpenNote() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral(".obsidian")));
        QVERIFY(root.mkpath(QStringLiteral("ai")));
        const auto write = [&root](const QString &name, const QByteArray &body) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write(body);
            return true;
        };
        QVERIFY(write(QStringLiteral("Target.md"), "# target\n"));
        QVERIFY(write(QStringLiteral("Plain.md"), "see [[Target]] and [[target|alias]]\nnothing\n"));
        QVERIFY(write(QStringLiteral("ai/Nested.md"),
                      "![[Target#Heading]]\n[md](../Target.md)\n```\n[[Target]]\n```\n"));
        QVERIFY(write(QStringLiteral("Other.md"), "[[Targets]] and [[ai/Target]]\n"));

        Backend backend;
        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("Target.md"))));
        const QVariantList links = backend.backlinks();
        QCOMPARE(links.size(), 2);
        const QVariantMap nested = links.at(0).toMap();
        QCOMPARE(nested.value(QStringLiteral("name")).toString(), QStringLiteral("Nested"));
        // The embed and the Markdown link count; the fenced example does not.
        QCOMPARE(nested.value(QStringLiteral("count")).toInt(), 2);
        const QVariantMap plain = links.at(1).toMap();
        QCOMPARE(plain.value(QStringLiteral("name")).toString(), QStringLiteral("Plain"));
        // Two links on one line are one mention of the note's text.
        QCOMPARE(plain.value(QStringLiteral("count")).toInt(), 1);
        QCOMPARE(plain.value(QStringLiteral("mentions")).toList().constFirst().toMap()
                     .value(QStringLiteral("line")).toInt(), 1);
    }

    void vaultFindsItsRootsNotesAndAttachments() {
        QTemporaryDir vaultDirectory;
        QVERIFY(vaultDirectory.isValid());
        QDir root(vaultDirectory.path());
        QVERIFY(root.mkpath(QStringLiteral(".obsidian")));
        QVERIFY(root.mkpath(QStringLiteral("ai/deep")));
        QVERIFY(root.mkpath(QStringLiteral("one")));
        QVERIFY(root.mkpath(QStringLiteral("two")));
        QVERIFY(root.mkpath(QStringLiteral("sub")));
        const auto write = [&root](const QString &name, const QByteArray &body) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write(body);
            return true;
        };
        QVERIFY(write(QStringLiteral("Top.md"), "# Top\n"));
        QVERIFY(write(QStringLiteral("ai/deep/Sibling.md"), "# Sibling\n"));
        QVERIFY(write(QStringLiteral("ai/deep/Note.md"), "# Note\n"));
        QVERIFY(write(QStringLiteral("sub/Deep.md"), "# Deep\n"));
        QVERIFY(write(QStringLiteral("one/Shared.md"), "# One\n"));
        QVERIFY(write(QStringLiteral("two/Shared.md"), "# Two\n"));
        QVERIFY(write(QStringLiteral(".obsidian/app.json"),
                      "{\"attachmentFolderPath\": \"attachments\"}\n"));

        const QString notePath = root.filePath(QStringLiteral("ai/deep/Note.md"));
        Vault vault;
        vault.setNoteFile(notePath);
        QCOMPARE(vault.root(), root.absolutePath());
        QVERIFY(vault.isObsidianVault());
        QCOMPARE(vault.treeRoot(), root.absolutePath());
        QVERIFY(vault.contains(notePath));
        QVERIFY(!vault.contains(QDir::tempPath()));
        QCOMPARE(vault.relativePath(notePath), QStringLiteral("ai/deep/Note.md"));
        QCOMPARE(vault.treeRelativePath(notePath), QStringLiteral("ai/deep/Note.md"));
        QCOMPARE(vault.sourceDirectory(), root.filePath(QStringLiteral("ai/deep")));
        QCOMPARE(vault.documentFolder(), root.filePath(QStringLiteral("ai/deep")));

        const QString sourceDir = root.filePath(QStringLiteral("ai/deep"));
        const QStringList notes{QStringLiteral("Top.md"), QStringLiteral("sub/Deep.md"),
                                QStringLiteral("ai/deep/Sibling.md"),
                                QStringLiteral("one/Shared.md"), QStringLiteral("two/Shared.md")};
        QCOMPARE(vault.resolveNote(QStringLiteral("Sibling"), sourceDir, notes),
                 QStringLiteral("ai/deep/Sibling.md"));
        QCOMPARE(vault.resolveNote(QStringLiteral("Top"), sourceDir, notes),
                 QStringLiteral("Top.md"));
        QCOMPARE(vault.resolveNote(QStringLiteral("sub/Deep.md"), sourceDir, notes),
                 QStringLiteral("sub/Deep.md"));
        QVERIFY(vault.resolveNote(QStringLiteral("Shared"), sourceDir, notes).isEmpty());

        bool isVault = false;
        QCOMPARE(vault.attachmentDirectory(&isVault),
                 root.filePath(QStringLiteral("attachments")));
        QVERIFY(isVault);
        QVERIFY(write(QStringLiteral(".obsidian/app.json"),
                      "{\"attachmentFolderPath\": \"./_attachments\"}\n"));
        QCOMPARE(vault.attachmentDirectory(nullptr),
                 root.filePath(QStringLiteral("ai/deep/_attachments")));
        QVERIFY(write(QStringLiteral(".obsidian/app.json"),
                      "{\"attachmentFolderPath\": \"/\"}\n"));
        QCOMPARE(vault.attachmentDirectory(nullptr), root.absolutePath());

        // A plain folder is its own root, and pasted images sit beside the
        // note; an untitled document outside a vault has no folder yet.
        QTemporaryDir plainDirectory;
        QVERIFY(plainDirectory.isValid());
        QDir plainRoot(plainDirectory.path());
        QVERIFY(plainRoot.mkpath(QStringLiteral("nested")));
        const QString plainNote = plainRoot.filePath(QStringLiteral("nested/Plain.md"));
        QFile plainFile(plainNote);
        QVERIFY(plainFile.open(QIODevice::WriteOnly));
        plainFile.write("# Plain\n");
        plainFile.close();

        Vault plain;
        plain.setNoteFile(plainNote);
        QCOMPARE(plain.root(), plainRoot.filePath(QStringLiteral("nested")));
        QVERIFY(!plain.isObsidianVault());
        QVERIFY(plain.contains(plainNote));
        QCOMPARE(plain.resolveNote(QStringLiteral("Plain"), plain.sourceDirectory(),
                                   QStringList{QStringLiteral("nested/Plain.md")}),
                 QStringLiteral("Plain.md"));
        QCOMPARE(plain.attachmentDirectory(nullptr),
                 plainRoot.filePath(QStringLiteral("nested/_attachments")));

        Vault untitled;
        untitled.setFallbackFolder(plainRoot.absolutePath());
        QCOMPARE(untitled.root(), plainRoot.absolutePath());
        QVERIFY(untitled.attachmentDirectory(nullptr).isEmpty());
    }

    void indexStaysWarmUntilInvalidated() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral(".obsidian")));
        const auto write = [&root](const QString &name, const QByteArray &body) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write(body);
            return true;
        };
        QVERIFY(write(QStringLiteral("First.md"), "---\ntags:\n  - warm\n---\n# First\n"));

        NoteIndex index;
        QVERIFY(index.refresh(root.absolutePath()));
        QCOMPARE(index.entries().size(), 1);
        QCOMPARE(index.meta(QStringLiteral("First.md")).tags,
                 (QStringList{QStringLiteral("warm")}));

        // A clean snapshot answers without reading the folder again: the
        // note added behind its back is not there until something says so.
        QVERIFY(write(QStringLiteral("Second.md"), "# Second\n"));
        QVERIFY(!index.refresh(root.absolutePath()));
        QCOMPARE(index.entries().size(), 1);

        index.invalidate();
        QVERIFY(index.refresh(root.absolutePath()));
        QCOMPARE(index.entries().size(), 2);
        QCOMPARE(index.entries().constLast().path, QStringLiteral("Second.md"));
        QCOMPARE(index.suggestions(QStringLiteral("second")).size(), 1);
    }

    void indexSuggestsAndFindsBacklinksAfterSavesAndMoves() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral(".obsidian")));
        const auto write = [&root](const QString &name, const QByteArray &body) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write(body);
            return true;
        };
        QVERIFY(write(QStringLiteral("Target.md"), "# Target\n"));
        QVERIFY(write(QStringLiteral("Source.md"), "see [[Target]]\n"));
        QVERIFY(write(QStringLiteral("Other.md"), "# Other\n"));

        Backend backend;
        QQuickTextDocument *document = attachTextEdit(&backend);
        QVERIFY(document);

        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("Source.md"))));
        QCOMPARE(backend.vaultRoot(), root.absolutePath());
        QCOMPARE(backend.noteSuggestions(QStringLiteral("Target")).size(), 1);

        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("Target.md"))));
        QCOMPARE(backend.backlinks().size(), 1);

        // Saving a link makes the index notice it: backlinks read the same
        // snapshot the tree and [[ suggestions do.
        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("Other.md"))));
        document->textDocument()->setPlainText(QStringLiteral("[[Target]]\n"));
        backend.save();
        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("Target.md"))));
        QCOMPARE(backend.backlinks().size(), 2);

        // A note saved into the vault joins the snapshot without a restart.
        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("Other.md"))));
        document->textDocument()->setPlainText(QStringLiteral("[[Target]]\n"));
        backend.saveAs(QUrl::fromLocalFile(root.filePath(QStringLiteral("Fresh.md"))));
        const QVariantList fresh = backend.noteSuggestions(QStringLiteral("Fresh"));
        QCOMPARE(fresh.size(), 1);
        QCOMPARE(fresh.constFirst().toMap().value(QStringLiteral("path")).toString(),
                 QStringLiteral("Fresh.md"));

        // A move outside Omawrite reaches the watcher, and every reader of
        // the snapshot follows it.
        QVERIFY(root.mkpath(QStringLiteral("sub")));
        QVERIFY(QFile::rename(root.filePath(QStringLiteral("Target.md")),
                              root.filePath(QStringLiteral("sub/Target.md"))));
        const QUrl moved = QUrl::fromLocalFile(root.filePath(QStringLiteral("sub/Target.md")));
        QTRY_COMPARE(backend.resolveNote(QStringLiteral("Target")), moved);
        const QVariantList targets = backend.noteSuggestions(QStringLiteral("Target"));
        QCOMPARE(targets.size(), 1);
        QCOMPARE(targets.constFirst().toMap().value(QStringLiteral("path")).toString(),
                 QStringLiteral("sub/Target.md"));

        backend.open(moved);
        QCOMPARE(backend.backlinks().size(), 3);
    }

    void treeNoteRowsOpenAndChevronsFold() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral(".obsidian")));
        const auto write = [&root](const QString &name, const QByteArray &body) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write(body);
            return true;
        };
        QVERIFY(write(QStringLiteral("Parent.md"), "# Parent\n"));
        QVERIFY(write(QStringLiteral("Child.md"),
                      "---\nup:\n  - \"[[Parent]]\"\n---\n# Child\n"));
        QVERIFY(write(QStringLiteral("Other.md"), "# Other\n"));

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("Other.md"))));
        QObject *list = window->findChild<QObject *>(QStringLiteral("fileViewerList"));
        QVERIFY(list);
        // Other and Parent are roots; Parent's child starts folded.
        QTRY_COMPARE(list->property("count").toInt(), 2);
        const auto tabCount = [window]() { return window->property("tabs").toList().size(); };
        QCOMPARE(tabCount(), 1);

        // The parent row opens the parent; it does not fold its children.
        QMetaObject::invokeMethod(window, "activateTreeRow", Q_ARG(QVariant, 1));
        QTRY_COMPARE(backend.fileName(), QStringLiteral("Parent.md"));
        QCOMPARE(tabCount(), 2);
        QCOMPARE(list->property("count").toInt(), 2);

        // The chevron is the fold control: it unfolds and folds without
        // taking the row's note away. Delegate items are owned by the view,
        // not the window, so they are found through the item tree.
        const std::function<QQuickItem *(QQuickItem *, const QString &)> findItem =
            [&](QQuickItem *parent, const QString &name) -> QQuickItem * {
                for (QQuickItem *child : parent->childItems()) {
                    if (child->objectName() == name)
                        return child;
                    if (QQuickItem *found = findItem(child, name))
                        return found;
                }
                return nullptr;
            };
        QQuickItem *chevron = findItem(window->contentItem(), QStringLiteral("treeChevron1"));
        QVERIFY(chevron);
        const auto clickChevron = [window](QQuickItem *item) {
            const QPointF scene =
                item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, scene.toPoint());
        };
        clickChevron(chevron);
        QTRY_COMPARE(list->property("count").toInt(), 3);
        QCOMPARE(backend.fileName(), QStringLiteral("Parent.md"));
        chevron = findItem(window->contentItem(), QStringLiteral("treeChevron1"));
        QVERIFY(chevron);
        clickChevron(chevron);
        QTRY_COMPARE(list->property("count").toInt(), 2);

        // Right and Left still fold and unfold from the keyboard, and Enter
        // opens the row's note.
        QMetaObject::invokeMethod(list, "forceActiveFocus");
        list->setProperty("currentIndex", 1);
        QTest::keyClick(window, Qt::Key_Right);
        QTRY_COMPARE(list->property("count").toInt(), 3);
        QTest::keyClick(window, Qt::Key_Left);
        QTRY_COMPARE(list->property("count").toInt(), 2);
    }

    void openDialogStartsAtTheVaultAndOpensTabs() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral(".obsidian")));
        QVERIFY(root.mkpath(QStringLiteral("sub")));
        const auto write = [&root](const QString &name) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write("# note\n");
            return true;
        };
        QVERIFY(write(QStringLiteral("sub/Current.md")));
        QVERIFY(write(QStringLiteral("RootNote.md")));

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("sub/Current.md"))));
        backend.openDialog();

        QObject *dialog = window->findChild<QObject *>(QStringLiteral("openFileDialog"));
        QVERIFY(dialog);
        QTRY_COMPARE(dialog->property("currentFolder").toUrl().toLocalFile(),
                     root.absolutePath());

        // A file picked here joins the tabs, like a tree or wiki-link open.
        dialog->setProperty("selectedFile",
                            QUrl::fromLocalFile(root.filePath(QStringLiteral("RootNote.md"))));
        QVERIFY(QMetaObject::invokeMethod(dialog, "accepted"));
        QTRY_COMPARE(backend.fileName(), QStringLiteral("RootNote.md"));
        QCOMPARE(window->property("tabs").toList().size(), 2);
    }

    void treeRootAndCurrentPathNotifyLive() {
        QTemporaryDir firstDirectory;
        QTemporaryDir secondDirectory;
        QVERIFY(firstDirectory.isValid());
        QVERIFY(secondDirectory.isValid());
        QDir first(firstDirectory.path());
        QDir second(secondDirectory.path());
        QVERIFY(first.mkpath(QStringLiteral(".obsidian")));
        QVERIFY(second.mkpath(QStringLiteral(".obsidian")));
        const auto write = [](const QDir &root, const QString &name) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write("# note\n");
            return true;
        };
        QVERIFY(write(first, QStringLiteral("First.md")));
        QVERIFY(write(first, QStringLiteral("Second.md")));
        QVERIFY(write(second, QStringLiteral("Third.md")));

        Backend backend;
        QSignalSpy rootSpy(&backend, &Backend::fileTreeRootChanged);
        QSignalSpy vaultSpy(&backend, &Backend::vaultRootChanged);
        QSignalSpy pathSpy(&backend, &Backend::fileTreeCurrentPathChanged);

        backend.open(QUrl::fromLocalFile(first.filePath(QStringLiteral("First.md"))));
        QCOMPARE(backend.fileTreeRoot(), first.absolutePath());
        QCOMPARE(backend.fileTreeCurrentPath(), QStringLiteral("First.md"));
        QVERIFY(pathSpy.count() > 0);

        // Another note in the same vault moves the highlight, not the root.
        rootSpy.clear();
        vaultSpy.clear();
        pathSpy.clear();
        backend.open(QUrl::fromLocalFile(first.filePath(QStringLiteral("Second.md"))));
        QTRY_VERIFY(pathSpy.count() > 0);
        QCOMPARE(rootSpy.count(), 0);
        QCOMPARE(vaultSpy.count(), 0);
        QCOMPARE(backend.fileTreeCurrentPath(), QStringLiteral("Second.md"));

        // A note in another vault changes both, and the viewer header follows.
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        QObject *header = window->findChild<QObject *>(QStringLiteral("fileViewerRoot"));
        QVERIFY(header);
        QTRY_COMPARE(header->property("text").toString(), first.dirName());

        backend.open(QUrl::fromLocalFile(second.filePath(QStringLiteral("Third.md"))));
        QTRY_VERIFY(rootSpy.count() > 0);
        QVERIFY(vaultSpy.count() > 0);
        QCOMPARE(backend.vaultRoot(), second.absolutePath());
        QCOMPARE(backend.fileTreeRoot(), second.absolutePath());
        QCOMPARE(backend.fileTreeCurrentPath(), QStringLiteral("Third.md"));
        QTRY_COMPARE(header->property("text").toString(), second.dirName());
    }

    void opensLinkedNotesInTabsWithoutDuplicates() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        for (const char *name : {"A.md", "B.md", "C.md"}) {
            QFile file(root.filePath(QString::fromLatin1(name)));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("# note\n\nsome text\n");
        }
        const auto url = [&root](const char *name) {
            return QUrl::fromLocalFile(root.filePath(QString::fromLatin1(name)));
        };

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        const auto tabCount = [window]() { return window->property("tabs").toList().size(); };
        QObject *strip = window->findChild<QObject *>(QStringLiteral("tabStrip"));
        QVERIFY(strip);

        backend.open(url("A.md"));
        QCOMPARE(tabCount(), 1);
        QVERIFY(!strip->property("visible").toBool());

        QObject *tabEditor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(tabEditor);
        tabEditor->setProperty("cursorPosition", 12);
        QMetaObject::invokeMethod(window, "openInTab", Q_ARG(QVariant, url("B.md")));
        QTRY_COMPARE(backend.fileName(), QStringLiteral("B.md"));
        QMetaObject::invokeMethod(window, "openInTab", Q_ARG(QVariant, url("C.md")));
        QTRY_COMPARE(backend.fileName(), QStringLiteral("C.md"));
        QCOMPARE(tabCount(), 3);
        QCOMPARE(window->property("activeTab").toInt(), 2);
        QVERIFY(strip->property("visible").toBool());

        // A note already in a tab is switched to, not opened a second time.
        QMetaObject::invokeMethod(window, "openInTab", Q_ARG(QVariant, url("A.md")));
        QTRY_COMPARE(backend.fileName(), QStringLiteral("A.md"));
        QCOMPARE(tabCount(), 3);
        QCOMPARE(window->property("activeTab").toInt(), 0);
        // The caret is where it was left in that tab.
        QTRY_COMPARE(tabEditor->property("cursorPosition").toInt(), 12);

        QTest::keyClick(window, Qt::Key_2, Qt::ControlModifier);
        QTRY_COMPARE(backend.fileName(), QStringLiteral("B.md"));
        QTest::keyClick(window, Qt::Key_9, Qt::ControlModifier);
        QTRY_COMPARE(backend.fileName(), QStringLiteral("C.md"));

        QTest::keyClick(window, Qt::Key_W, Qt::ControlModifier);
        QTRY_COMPARE(backend.fileName(), QStringLiteral("B.md"));
        QCOMPARE(tabCount(), 2);
        QCOMPARE(window->property("activeTab").toInt(), 1);
    }

    void goesBackAndForwardThroughOpenedFiles() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        const auto write = [&root](const QString &name) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write("# note\n");
            return true;
        };
        QVERIFY(write(QStringLiteral("First.md")));
        QVERIFY(write(QStringLiteral("Second.md")));
        const auto url = [&root](const QString &name) {
            return QUrl::fromLocalFile(root.filePath(name));
        };

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        backend.open(url(QStringLiteral("First.md")));
        QTRY_COMPARE(backend.fileUrl(), url(QStringLiteral("First.md")));
        backend.open(url(QStringLiteral("Second.md")));
        QTRY_COMPARE(backend.fileUrl(), url(QStringLiteral("Second.md")));

        // Two files were visited, so a step back is available and lights the
        // footer arrow.
        QObject *backButton = window->findChild<QObject *>(QStringLiteral("historyBackButton"));
        QObject *forwardButton = window->findChild<QObject *>(QStringLiteral("historyForwardButton"));
        QVERIFY(backButton);
        QVERIFY(forwardButton);
        QTRY_VERIFY(backButton->property("opacity").toReal() > 0.9);

        QTest::keyClick(window, Qt::Key_Left, Qt::AltModifier);
        QTRY_COMPARE(backend.fileUrl(), url(QStringLiteral("First.md")));
        QTRY_VERIFY(forwardButton->property("opacity").toReal() > 0.9);

        QTest::keyClick(window, Qt::Key_Right, Qt::AltModifier);
        QTRY_COMPARE(backend.fileUrl(), url(QStringLiteral("Second.md")));
        QTRY_VERIFY(forwardButton->property("opacity").toReal() < 0.9);
    }

    void showsAFileViewerOfTheSlipbox() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral(".obsidian")));
        QVERIFY(root.mkpath(QStringLiteral("ai/deep")));
        const auto write = [&root](const QString &name) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write("# note\n");
            return true;
        };
        QVERIFY(write(QStringLiteral("Top.md")));
        QVERIFY(write(QStringLiteral("ai/Nested.md")));
        QVERIFY(write(QStringLiteral("ai/deep/Deeper.md")));
        QVERIFY(write(QStringLiteral("ai/image.png")));
        QVERIFY(write(QStringLiteral("zeta.markdown")));

        Backend backend;
        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("ai/deep/Deeper.md"))));
        QCOMPARE(backend.fileTreeRoot(), QDir(dir.path()).absolutePath());
        QCOMPARE(backend.fileTreeCurrentPath(), QStringLiteral("ai/deep/Deeper.md"));

        // One level at a time: folders first, then notes, no dot folders and
        // nothing that is not Markdown.
        const QVariantList top = backend.listFolder(QString());
        QCOMPARE(top.size(), 3);
        QCOMPARE(top.at(0).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("ai"));
        QVERIFY(top.at(0).toMap().value(QStringLiteral("isDir")).toBool());
        QCOMPARE(top.at(1).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Top"));
        QCOMPARE(top.at(2).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("zeta"));
        QCOMPARE(backend.listFolder(QStringLiteral("ai")).size(), 2);
        QVERIFY(backend.listFolder(QStringLiteral("../")).isEmpty());

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *viewer = window->findChild<QObject *>(QStringLiteral("fileViewer"));
        QObject *list = window->findChild<QObject *>(QStringLiteral("fileViewerList"));
        QVERIFY(viewer);
        QVERIFY(list);
        // Opened with a file, so the viewer waits for Ctrl+B, which also
        // unfolds the way to the open note.
        QVERIFY(!viewer->property("visible").toBool());
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        QTRY_VERIFY(viewer->property("visible").toBool());
        // Links mode is the default: every note is a root here, sorted by
        // name, so Deeper, Nested, Top, zeta.
        QCOMPARE(backend.treeMode(), QStringLiteral("links"));
        QTRY_COMPARE(list->property("count").toInt(), 4);

        // Opening a note from the tree puts it in a tab.
        QMetaObject::invokeMethod(window, "activateTreeRow", Q_ARG(QVariant, 2));
        QTRY_COMPARE(backend.fileName(), QStringLiteral("Top.md"));
        QCOMPARE(window->property("tabs").toList().size(), 2);

        // Folders mode still walks the disk: folders first, then notes, no
        // dot folders and nothing that is not Markdown.
        QObject *foldersButton = window->findChild<QObject *>(QStringLiteral("treeModeFolders"));
        QVERIFY(foldersButton);
        QVERIFY(QMetaObject::invokeMethod(foldersButton, "clicked"));
        QTRY_COMPARE(list->property("count").toInt(), 3);

        // A folder folds open and shut again.
        QMetaObject::invokeMethod(window, "activateTreeRow", Q_ARG(QVariant, 0));
        QTRY_COMPARE(list->property("count").toInt(), 5);
        QMetaObject::invokeMethod(window, "activateTreeRow", Q_ARG(QVariant, 0));
        QTRY_COMPARE(list->property("count").toInt(), 3);

        // The header's collapse button hides the tree without the keyboard.
        QObject *collapseButton = window->findChild<QObject *>(QStringLiteral("treeCollapse"));
        QVERIFY(collapseButton);
        QVERIFY(QMetaObject::invokeMethod(collapseButton, "clicked"));
        QTRY_VERIFY(!viewer->property("visible").toBool());

        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        QTRY_VERIFY(viewer->property("visible").toBool());
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        QTRY_VERIFY(!viewer->property("visible").toBool());
    }

    void settingsDialogChangesScrollSpeed() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        const auto write = [&root](const QString &name) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write("# note\n");
            return true;
        };
        QVERIFY(write(QStringLiteral("Note.md")));

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("Note.md"))));
        QObject *settingsButton = window->findChild<QObject *>(QStringLiteral("settingsButton"));
        QObject *dialog = window->findChild<QObject *>(QStringLiteral("settingsDialog"));
        QVERIFY(settingsButton);
        QVERIFY(dialog);
        QVERIFY(!dialog->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(settingsButton, "clicked"));
        QTRY_VERIFY(dialog->property("opened").toBool());

        // The dialog lists every rebindable action, and the speed field edits
        // the shared setting the editor and the tree both read.
        QVERIFY(dialog->property("rows").toList().size() >= 30);
        QObject *speedField = dialog->findChild<QObject *>(QStringLiteral("scrollSpeedField"));
        QVERIFY(speedField);
        QCOMPARE(speedField->property("text").toString(), QStringLiteral("2.0"));
        speedField->setProperty("text", QStringLiteral("1.5"));
        QVERIFY(QMetaObject::invokeMethod(speedField, "editingFinished"));
        QTRY_COMPARE(backend.scrollSpeed(), 1.5);

        // Key edits apply live through the bindings: Ctrl+B stops toggling
        // the viewer until the key is reset. The blank window opens the
        // viewer by itself, so close it and the modal dialog first.
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("opened").toBool());
        QObject *viewer = window->findChild<QObject *>(QStringLiteral("fileViewer"));
        QVERIFY(viewer);
        QVERIFY(viewer->property("visible").toBool());
        QMetaObject::invokeMethod(window, "toggleSidebar");
        QTRY_VERIFY(!viewer->property("visible").toBool());
        QObject *sidebarShortcut =
            window->findChild<QObject *>(QStringLiteral("toggleSidebarShortcut"));
        QVERIFY(sidebarShortcut);
        QCOMPARE(sidebarShortcut->property("sequence").toString(), QStringLiteral("Ctrl+B"));
        QVERIFY(backend.setShortcut(QStringLiteral("toggle-sidebar"),
                                    QStringLiteral("Ctrl+E")));
        QTRY_COMPARE(sidebarShortcut->property("sequence").toString(),
                     QStringLiteral("Ctrl+E"));
        backend.resetShortcut(QStringLiteral("toggle-sidebar"));
        QTRY_COMPARE(sidebarShortcut->property("sequence").toString(),
                     QStringLiteral("Ctrl+B"));
        backend.setScrollSpeed(2.0);
    }

    void commandPaletteSearchesCommandsAndFiles() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral(".obsidian")));
        const auto write = [&root](const QString &name, const QByteArray &text) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write(text);
            return true;
        };
        QVERIFY(write(QStringLiteral("Branding.md"), "# Branding\n"));
        QVERIFY(write(QStringLiteral("Note.md"), "# Note\n"));
        QVERIFY(write(QStringLiteral("current.md"), "# Current\n"));

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *palette = window->findChild<QObject *>(QStringLiteral("commandPalette"));
        QObject *field = window->findChild<QObject *>(QStringLiteral("commandPaletteField"));
        QObject *list = window->findChild<QObject *>(QStringLiteral("commandPaletteList"));
        QVERIFY(palette);
        QVERIFY(field);
        QVERIFY(list);
        const auto rows = [palette] { return palette->property("rows").toList(); };
        const auto type = [window](const QString &text) {
            for (const QChar character : text)
                QTest::keyClick(window, character.toLatin1());
        };

        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("current.md"))));

        // Ctrl+P opens on the command list, plus every note in the slipbox.
        QTest::keyClick(window, Qt::Key_P, Qt::ControlModifier);
        QTRY_VERIFY(palette->property("visible").toBool());
        QTRY_VERIFY(field->property("activeFocus").toBool());
        QCOMPARE(field->property("placeholderText").toString(),
                 QStringLiteral("Search commands and files…"));
        QTRY_VERIFY(rows().size() > 4);
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        QCOMPARE(rows().constFirst().toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("command"));

        // The arrow keys move the selection and Page Down jumps by a screen.
        QTest::keyClick(window, Qt::Key_Down);
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        QTest::keyClick(window, Qt::Key_PageDown);
        QVERIFY(list->property("currentIndex").toInt() > 1);
        QTest::keyClick(window, Qt::Key_PageUp);
        QVERIFY(list->property("currentIndex").toInt() >= 0);

        // A query narrows files through the backend and marks the row as one.
        type(QStringLiteral("brand"));
        QCOMPARE(field->property("text").toString(), QStringLiteral("brand"));
        QTRY_COMPARE(rows().size(), 1);
        const QVariantMap file = rows().constFirst().toMap();
        QCOMPARE(file.value(QStringLiteral("kind")).toString(), QStringLiteral("file"));
        QCOMPARE(file.value(QStringLiteral("label")).toString(), QStringLiteral("Branding"));
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(backend.fileName(), QStringLiteral("Branding.md"));
        QVERIFY(!palette->property("visible").toBool());

        // Commands run through the same runAction the keyboard uses.
        const qreal scale = backend.textScale();
        QTest::keyClick(window, Qt::Key_P, Qt::ControlModifier);
        QTRY_VERIFY(palette->property("visible").toBool());
        type(QStringLiteral("increase text"));
        QTRY_COMPARE(rows().size(), 1);
        const QVariantMap command = rows().constFirst().toMap();
        QCOMPARE(command.value(QStringLiteral("kind")).toString(), QStringLiteral("command"));
        QCOMPARE(command.value(QStringLiteral("id")).toString(), QStringLiteral("zoom-in"));
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(backend.textScale() > scale);
        QVERIFY(!palette->property("visible").toBool());
        backend.resetTextSize();

        // Ctrl+Shift+O is the same palette restricted to files.
        QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(palette->property("visible").toBool());
        QVERIFY(palette->property("fileMode").toBool());
        QCOMPARE(field->property("placeholderText").toString(),
                 QStringLiteral("Open other file…"));
        QTRY_COMPARE(rows().size(), 3);
        for (const QVariant &row : rows())
            QCOMPARE(row.toMap().value(QStringLiteral("kind")).toString(),
                     QStringLiteral("file"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!palette->property("visible").toBool());
    }

    void alignsWrappedListLinesWithTheirText() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        auto *quickDocument = qobject_cast<QQuickTextDocument *>(
            editor->property("textDocument").value<QObject *>());
        QVERIFY(quickDocument);
        QTextDocument *document = quickDocument->textDocument();

        editor->setProperty("text", QStringLiteral(
            "- a list item with enough words to wrap at this width and keep going "
            "until it has several lines to check\n"
            "    - a nested item with enough words to wrap at this width and keep "
            "going until it has several lines to check\n"
            "- [ ] a task item with enough words to wrap at this width and keep "
            "going until it has several lines to check"));
        document->documentLayout()->documentSize();

        int listItems = 0;
        for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
            const QTextBlockFormat format = block.blockFormat();
            QVERIFY(format.indent() > 0);
            QVERIFY(format.textIndent() < 0);
            QVERIFY(block.layout()->lineCount() > 1);
            QVERIFY(block.layout()->lineAt(1).x() > 0);
            QVERIFY(qAbs(block.layout()->lineAt(1).x() - format.indent()) < 1.0);
            ++listItems;
        }
        QCOMPARE(listItems, 3);
    }

    void clickingTheEditorStillPlacesTheCaret() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        auto *item = qobject_cast<QQuickItem *>(editor);
        QVERIFY(item);

        editor->setProperty("text", QStringLiteral("hello world"));
        QTest::qWait(20);
        QRectF box;
        QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, box),
                                  Q_ARG(int, 6));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          item->mapToScene(box.center()).toPoint());
        QVERIFY(qAbs(editor->property("cursorPosition").toInt() - 6) <= 1);
    }

    void draggingTheEditorStillSelectsText() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        auto *item = qobject_cast<QQuickItem *>(editor);
        QVERIFY(item);

        editor->setProperty("text", QStringLiteral("hello world"));
        QTest::qWait(20);
        QRectF first;
        QRectF last;
        QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, first),
                                  Q_ARG(int, 1));
        QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, last),
                                  Q_ARG(int, 7));
        const QPoint from = item->mapToScene(first.center()).toPoint();
        const QPoint to = item->mapToScene(last.center()).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(window, to);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to);
        QVERIFY(editor->property("selectionEnd").toInt()
                > editor->property("selectionStart").toInt());
    }

    void ctrlClicksWikiLinks() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral(".obsidian")));
        QFile current(root.filePath(QStringLiteral("current.md")));
        QVERIFY(current.open(QIODevice::WriteOnly));
        current.write("See [[Other]] here\n");
        current.close();
        QFile other(root.filePath(QStringLiteral("Other.md")));
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.write("# Other\n");
        other.close();

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        auto *item = qobject_cast<QQuickItem *>(editor);
        QVERIFY(item);

        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("current.md"))));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("See [[Other]] here\n"));
        QTest::qWait(20);
        QRectF box;
        QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, box),
                                  Q_ARG(int, 8));
        QTest::mouseClick(window, Qt::LeftButton, Qt::ControlModifier,
                          item->mapToScene(box.center()).toPoint());
        QTRY_COMPARE(backend.fileName(), QStringLiteral("Other.md"));
    }

    void undoesAnEditInOneStep() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        editor->setProperty("text", QStringLiteral("hello world"));
        QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, 5));
        QMetaObject::invokeMethod(editor, "wrapSelection", Q_ARG(QVariant, QStringLiteral("**")));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("**hello** world"));
        QMetaObject::invokeMethod(editor, "undo");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("hello world"));
    }

    void keepsLineHeightAcrossUndoAndPaste() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        auto *quickDocument = qobject_cast<QQuickTextDocument *>(
            editor->property("textDocument").value<QObject *>());
        QVERIFY(quickDocument);
        QTextDocument *document = quickDocument->textDocument();
        const auto allLinesSpaced = [document] {
            for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
                if (block.blockFormat().lineHeight() != 140)
                    return false;
            }
            return true;
        };

        // Undoing a deleted line leaves redo working.
        editor->setProperty("text", QStringLiteral("a\nb\nc"));
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 2), Q_ARG(int, 4));
        QTest::keyClick(window, Qt::Key_Delete);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("a\nc"));
        QMetaObject::invokeMethod(editor, "undo");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("a\nb\nc"));
        QVERIFY(document->isRedoAvailable());

        // Lines written over others keep the line height, however many there are.
        editor->setProperty("text", QStringLiteral("aa\nbb\ncc"));
        QGuiApplication::clipboard()->setText(QStringLiteral("X\nY"));
        QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, 5));
        QMetaObject::invokeMethod(editor, "paste");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("X\nY\ncc"));
        QVERIFY(allLinesSpaced());
    }

    void pastesAClipboardImageIntoAnObsidianVault() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral(".obsidian")));
        QVERIFY(root.mkpath(QStringLiteral("attachments")));
        {
            QFile config(root.filePath(QStringLiteral(".obsidian/app.json")));
            QVERIFY(config.open(QIODevice::WriteOnly));
            config.write("{\"attachmentFolderPath\": \"attachments\"}");
        }
        const QString notePath = root.filePath(QStringLiteral("Inbox.md"));
        {
            QFile note(notePath);
            QVERIFY(note.open(QIODevice::WriteOnly));
            note.write("# Inbox\n");
        }

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        backend.open(QUrl::fromLocalFile(notePath));
        QCOMPARE(backend.fileName(), QStringLiteral("Inbox.md"));
        editor->setProperty("cursorPosition", editor->property("text").toString().length());

        QImage screenshot(64, 32, QImage::Format_ARGB32);
        screenshot.fill(Qt::red);
        QGuiApplication::clipboard()->setImage(screenshot);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        QTest::keyClick(window, Qt::Key_V, Qt::ControlModifier);

        // The image is saved under Obsidian's own attachment folder and
        // embedded the way Obsidian writes it, so the vault reads alike in
        // either app.
        QTRY_VERIFY2(editor->property("text").toString().contains(QStringLiteral("![[Pasted image")),
                     qPrintable(editor->property("text").toString()));
        const QString text = editor->property("text").toString();
        static const QRegularExpression embedRe(
            QStringLiteral("!\\[\\[(Pasted image \\d{14}(?: \\d+)?\\.png)\\]\\]"));
        const QRegularExpressionMatch embed = embedRe.match(text);
        QVERIFY2(embed.hasMatch(), qPrintable(text));

        const QFileInfo image(root.filePath(QStringLiteral("attachments/") + embed.captured(1)));
        QVERIFY2(image.isFile(), qPrintable(image.absoluteFilePath()));
        QCOMPARE(QImageReader(image.absoluteFilePath()).size(), QSize(64, 32));

        // The note shows the image itself, and the line keeps the room for it.
        QCOMPARE(backend.imageEmbeds().size(), 1);
        QObject *previews = window->findChild<QObject *>(QStringLiteral("imagePreviews"));
        QVERIFY(previews);
        QTRY_COMPARE(previews->property("count").toInt(), 1);
        QQuickItem *preview = nullptr;
        QVERIFY(QMetaObject::invokeMethod(previews, "itemAt",
                                          Q_RETURN_ARG(QQuickItem *, preview), Q_ARG(int, 0)));
        QVERIFY(preview);
        QCOMPARE(preview->objectName(), QStringLiteral("imagePreview"));
        QCOMPARE(preview->property("imageUrl").toUrl(), QUrl::fromLocalFile(image.absoluteFilePath()));
        auto *quickDocument = qobject_cast<QQuickTextDocument *>(
            editor->property("textDocument").value<QObject *>());
        QVERIFY(quickDocument);
        QTRY_VERIFY(quickDocument->textDocument()->lastBlock().blockFormat().bottomMargin() > 0);
    }

    void pastesClipboardImagesBesidePlainNotes() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString notePath = QDir(dir.path()).filePath(QStringLiteral("Note.md"));
        {
            QFile note(notePath);
            QVERIFY(note.open(QIODevice::WriteOnly));
            note.write("# Note\n");
        }

        Backend backend;
        backend.open(QUrl::fromLocalFile(notePath));

        QImage image(20, 10, QImage::Format_ARGB32);
        image.fill(Qt::blue);
        QGuiApplication::clipboard()->setImage(image);
        const QVariantMap result = backend.pasteClipboardImage();
        QVERIFY(result.value(QStringLiteral("handled")).toBool());

        const QString text = result.value(QStringLiteral("text")).toString();
        static const QRegularExpression embedRe(
            QStringLiteral("^!\\[\\]\\((_attachments/Pasted%20image%20\\d{14}(?:%20\\d+)?\\.png)\\)$"));
        const QRegularExpressionMatch embed = embedRe.match(text);
        QVERIFY2(embed.hasMatch(), qPrintable(text));

        // A folder outside a vault gets a Markdown path, relative to the note.
        const QString path = QDir(dir.path()).filePath(QUrl::fromPercentEncoding(
            embed.captured(1).toUtf8()));
        QVERIFY2(QFileInfo::exists(path), qPrintable(path));
        QCOMPARE(QImageReader(path).size(), QSize(20, 10));
    }

    void pastesImagesAtTheVaultRootByDefault() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral(".obsidian")));
        const QString notePath = root.filePath(QStringLiteral("Note.md"));
        {
            QFile note(notePath);
            QVERIFY(note.open(QIODevice::WriteOnly));
            note.write("# Note\n");
        }

        Backend backend;
        backend.open(QUrl::fromLocalFile(notePath));

        // A new vault has no attachment folder setting, and Obsidian puts
        // pasted images at the vault root.
        QImage image(8, 8, QImage::Format_ARGB32);
        image.fill(Qt::green);
        QGuiApplication::clipboard()->setImage(image);
        const QVariantMap result = backend.pasteClipboardImage();
        QVERIFY(result.value(QStringLiteral("handled")).toBool());

        const QString text = result.value(QStringLiteral("text")).toString();
        static const QRegularExpression embedRe(
            QStringLiteral("^!\\[\\[(Pasted image \\d{14}(?: \\d+)?\\.png)\\]\\]$"));
        const QRegularExpressionMatch embed = embedRe.match(text);
        QVERIFY2(embed.hasMatch(), qPrintable(text));
        QVERIFY2(QFileInfo::exists(root.filePath(embed.captured(1))), qPrintable(text));
    }

    void pastesImagesUnderTheNotesFolderWhenTheVaultAsks() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral(".obsidian")));
        QVERIFY(root.mkpath(QStringLiteral("notes")));
        {
            QFile config(root.filePath(QStringLiteral(".obsidian/app.json")));
            QVERIFY(config.open(QIODevice::WriteOnly));
            config.write("{\"attachmentFolderPath\": \"./_attachments\"}");
        }
        const QString notePath = root.filePath(QStringLiteral("notes/Inbox.md"));
        {
            QFile note(notePath);
            QVERIFY(note.open(QIODevice::WriteOnly));
            note.write("# Inbox\n");
        }

        Backend backend;
        backend.open(QUrl::fromLocalFile(notePath));
        QImage image(16, 16, QImage::Format_ARGB32);
        image.fill(Qt::yellow);
        QGuiApplication::clipboard()->setImage(image);
        const QVariantMap result = backend.pasteClipboardImage();
        QVERIFY(result.value(QStringLiteral("handled")).toBool());

        static const QRegularExpression embedRe(
            QStringLiteral("^!\\[\\[(Pasted image \\d{14}(?: \\d+)?\\.png)\\]\\]$"));
        const QString text = result.value(QStringLiteral("text")).toString();
        const QRegularExpressionMatch embed = embedRe.match(text);
        QVERIFY2(embed.hasMatch(), qPrintable(text));
        QVERIFY2(QFileInfo::exists(root.filePath(
                     QStringLiteral("notes/_attachments/") + embed.captured(1))),
                 qPrintable(text));
    }

    void opensImagesFullWindowAndStepsThroughThem() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        const auto writeImage = [&root](const QString &name, const QColor &color) {
            QImage image(900, 600, QImage::Format_ARGB32);
            image.fill(color);
            return image.save(root.filePath(name));
        };
        QVERIFY(writeImage(QStringLiteral("one.png"), Qt::red));
        QVERIFY(writeImage(QStringLiteral("two.png"), Qt::blue));
        const QString notePath = root.filePath(QStringLiteral("Note.md"));
        {
            QFile note(notePath);
            QVERIFY(note.open(QIODevice::WriteOnly));
            note.write("# Note\n\n![[one.png]]\n\n![[two.png]]\n");
        }

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *previews = window->findChild<QObject *>(QStringLiteral("imagePreviews"));
        QObject *viewer = window->findChild<QObject *>(QStringLiteral("imageViewer"));
        QObject *picture = window->findChild<QObject *>(QStringLiteral("imageViewerPicture"));
        QObject *previous = window->findChild<QObject *>(QStringLiteral("imageViewerPrevious"));
        QObject *next = window->findChild<QObject *>(QStringLiteral("imageViewerNext"));
        QVERIFY(editor);
        QVERIFY(previews);
        QVERIFY(viewer);
        QVERIFY(picture);
        QVERIFY(previous);
        QVERIFY(next);
        QVERIFY(!viewer->property("visible").toBool());

        backend.open(QUrl::fromLocalFile(notePath));
        QTRY_COMPARE(previews->property("count").toInt(), 2);
        QQuickItem *firstPreview = nullptr;
        QVERIFY(QMetaObject::invokeMethod(previews, "itemAt",
                                          Q_RETURN_ARG(QQuickItem *, firstPreview), Q_ARG(int, 0)));
        QVERIFY(firstPreview);
        editor->setProperty("cursorPosition", 0);
        QTest::qWait(30);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          firstPreview->mapToScene(QPointF(firstPreview->width() / 2,
                                                          firstPreview->height() / 2)).toPoint());

        // The viewer covers the whole window and starts on the clicked
        // picture, at its own size rather than stretched to the window.
        QTRY_VERIFY(viewer->property("visible").toBool());
        QTRY_VERIFY(viewer->property("activeFocus").toBool());
        QCOMPARE(qRound(viewer->property("width").toReal()), window->width());
        QCOMPARE(qRound(viewer->property("height").toReal()), window->height());
        QTRY_COMPARE(picture->property("source").toUrl(),
                     QUrl::fromLocalFile(root.filePath(QStringLiteral("one.png"))));
        QCOMPARE(qRound(picture->property("width").toReal()), 900);
        QCOMPARE(qRound(picture->property("height").toReal()), 600);
        QVERIFY(!previous->property("visible").toBool());
        QVERIFY(next->property("visible").toBool());

        // A click on the picture inspects it; only the space around it closes.
        auto *pictureItem = qobject_cast<QQuickItem *>(picture);
        QVERIFY(pictureItem);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          pictureItem->mapToScene(QPointF(pictureItem->width() / 2,
                                                         pictureItem->height() / 2)).toPoint());
        QVERIFY(viewer->property("visible").toBool());

        // The right chevron and the Right key both reach the next picture.
        auto *nextButton = qobject_cast<QQuickItem *>(next);
        QVERIFY(nextButton);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          nextButton->mapToScene(QPointF(nextButton->width() / 2,
                                                        nextButton->height() / 2)).toPoint());
        QTRY_COMPARE(picture->property("source").toUrl(),
                     QUrl::fromLocalFile(root.filePath(QStringLiteral("two.png"))));
        QVERIFY(previous->property("visible").toBool());
        QVERIFY(!next->property("visible").toBool());

        QTest::keyClick(window, Qt::Key_Left);
        QTRY_COMPARE(picture->property("source").toUrl(),
                     QUrl::fromLocalFile(root.filePath(QStringLiteral("one.png"))));

        // Escape closes, returns the caret to the picture's line, and gives
        // the keyboard back to the editor.
        const QVariantList embeds = backend.imageEmbeds();
        QCOMPARE(embeds.size(), 2);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!viewer->property("visible").toBool());
        QTRY_VERIFY(editor->property("activeFocus").toBool());
        QCOMPARE(editor->property("cursorPosition").toInt(),
                 embeds.first().toMap().value(QStringLiteral("blockEnd")).toInt());
    }

    void searchFollowsEditsWithoutTakingTheCaret() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *searchField = window->findChild<QObject *>(QStringLiteral("searchField"));
        QVERIFY(editor);
        QVERIFY(searchField);

        editor->setProperty("text", QStringLiteral("foo bar foo"));
        QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
        searchField->setProperty("text", QStringLiteral("foo"));
        QMetaObject::invokeMethod(window, "flushSearch");
        QCOMPARE(editor->property("selectedText").toString(), QStringLiteral("foo"));

        // Back in the text, typing goes where the caret is.
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        editor->setProperty("cursorPosition", 7);
        QTest::keyClick(window, 'x');
        QTest::keyClick(window, 'y');
        QCOMPARE(editor->property("text").toString(), QStringLiteral("foo barxy foo"));
        QCOMPARE(window->property("searchMatches").toList().size(), 2);

        // The current match's highlight leaves its old block even when text
        // above it is deleted.
        auto *quickDocument = qobject_cast<QQuickTextDocument *>(
            editor->property("textDocument").value<QObject *>());
        QVERIFY(quickDocument);
        QTextDocument *document = quickDocument->textDocument();
        const auto matchBackground = [document](int blockNumber) {
            const QTextBlock block = document->findBlockByNumber(blockNumber);
            for (const QTextLayout::FormatRange &range : block.layout()->formats()) {
                if (range.start == 0 && range.format.hasProperty(QTextFormat::BackgroundBrush))
                    return range.format.background().color();
            }
            return QColor();
        };
        editor->setProperty("text", QStringLiteral("long text\nfoo\nfoo"));
        searchField->setProperty("text", QStringLiteral("x"));
        QMetaObject::invokeMethod(window, "flushSearch");
        searchField->setProperty("text", QStringLiteral("foo"));
        QMetaObject::invokeMethod(window, "flushSearch");
        QMetaObject::invokeMethod(window, "moveSearch", Q_ARG(QVariant, 1));
        const QColor current = matchBackground(2);
        const QColor other = matchBackground(1);
        QVERIFY(current.isValid() && other.isValid() && current != other);
        QMetaObject::invokeMethod(editor, "remove", Q_ARG(int, 0), Q_ARG(int, 10));
        QCOMPARE(window->property("searchMatches").toList().size(), 2);
        QCOMPARE(window->property("searchMatchIndex").toInt(), 0);
        QCOMPARE(matchBackground(0), current);
        QCOMPARE(matchBackground(1), other);

        // Nor does it stay behind when another match moves into its offset.
        editor->setProperty("text", QStringLiteral("foo\nab\nfoo"));
        QMetaObject::invokeMethod(window, "moveSearch", Q_ARG(QVariant, 1));
        QCOMPARE(matchBackground(2), current);
        QMetaObject::invokeMethod(editor, "insert", Q_ARG(int, 0), Q_ARG(QString, QStringLiteral("abcdefg")));
        QCOMPARE(window->property("searchMatches").toList().first().toInt(), 7);
        QCOMPARE(matchBackground(2), other);

        // Matches are found where the highlighter finds them, even past a
        // character whose lowercase is longer.
        editor->setProperty("text", QString::fromUtf8("\xC4\xB0 ab"));
        searchField->setProperty("text", QStringLiteral("ab"));
        QMetaObject::invokeMethod(window, "flushSearch");
        QCOMPARE(editor->property("selectedText").toString(), QStringLiteral("ab"));
    }

    void replacesOneMatchAfterAnother() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *searchField = window->findChild<QObject *>(QStringLiteral("searchField"));
        QObject *replaceField = window->findChild<QObject *>(QStringLiteral("replaceField"));
        QObject *replaceButton = window->findChild<QObject *>(QStringLiteral("replaceButton"));
        QObject *replaceAllButton = window->findChild<QObject *>(QStringLiteral("replaceAllButton"));
        QVERIFY(editor && searchField && replaceField && replaceButton && replaceAllButton);

        editor->setProperty("text", QStringLiteral("foo foo foo"));
        QTest::keyClick(window, Qt::Key_H, Qt::ControlModifier);
        searchField->setProperty("text", QStringLiteral("foo"));
        replaceField->setProperty("text", QStringLiteral("Foo"));

        // A replacement the query still matches is not found again.
        QMetaObject::invokeMethod(replaceButton, "clicked");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("Foo foo foo"));
        QMetaObject::invokeMethod(replaceButton, "clicked");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("Foo Foo foo"));
        QMetaObject::invokeMethod(replaceButton, "clicked");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("Foo Foo Foo"));

        // Replace All is one edit, so one undo takes it back.
        editor->setProperty("text", QStringLiteral("a foo b foo c"));
        replaceField->setProperty("text", QStringLiteral("x"));
        QMetaObject::invokeMethod(replaceAllButton, "clicked");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("a x b x c"));
        QMetaObject::invokeMethod(editor, "undo");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("a foo b foo c"));
    }

    void countsWordsAfterEditsWithoutBlocking() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        editor->setProperty("text", QStringLiteral("one two"));
        QTRY_COMPARE(backend.wordCount(), 2);

        // Hold the thread pool's only thread so a count stays in flight until
        // the gate opens, whatever the timing.
        QThreadPool *pool = QThreadPool::globalInstance();
        const int maxThreads = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        QSemaphore gate;
        QSemaphore held;
        auto hold = [&] {
            pool->start([&] {
                held.release();
                gate.acquire();
            });
            held.acquire();
        };
        const auto restore = qScopeGuard([&] {
            gate.release(2);
            pool->waitForDone();
            pool->setMaxThreadCount(maxThreads);
        });
        auto &counting = backend.m_wordCountWatcher;
        QSignalSpy counted(&counting, &QFutureWatcher<int>::finished);

        // An edit made while a count runs gets one more count after it.
        hold();
        QMetaObject::invokeMethod(editor, "insert", Q_ARG(int, 7), Q_ARG(QString, QStringLiteral(" three")));
        QTRY_VERIFY(counting.isRunning());
        QMetaObject::invokeMethod(editor, "insert", Q_ARG(int, 13), Q_ARG(QString, QStringLiteral(" four")));
        QTRY_VERIFY(!backend.m_wordCountTimer.isActive());
        QCOMPARE(backend.wordCount(), 2);
        gate.release();
        QTRY_COMPARE(counted.count(), 2);
        QCOMPARE(backend.wordCount(), 4);

        // A file loaded while a count runs keeps its own count, and the
        // outdated one is dropped when it finishes.
        hold();
        QMetaObject::invokeMethod(editor, "insert", Q_ARG(int, 18), Q_ARG(QString, QStringLiteral(" five")));
        QTRY_VERIFY(counting.isRunning());
        backend.loadDocumentText(QStringLiteral("alpha"));
        QCOMPARE(backend.wordCount(), 1);
        gate.release();
        QTRY_COMPARE(counted.count(), 3);
        QCOMPARE(backend.wordCount(), 1);
        QVERIFY(!counting.isRunning());
        QVERIFY(!backend.m_wordCountTimer.isActive());
    }

    void searchWaitsForTypingToPause() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *searchField = window->findChild<QObject *>(QStringLiteral("searchField"));
        QObject *replaceField = window->findChild<QObject *>(QStringLiteral("replaceField"));
        QObject *replaceAllButton = window->findChild<QObject *>(QStringLiteral("replaceAllButton"));
        QVERIFY(editor && searchField && replaceField && replaceAllButton);

        // Typing does not search until it pauses.
        editor->setProperty("text", QStringLiteral("foo bar foo"));
        QTest::keyClick(window, Qt::Key_H, Qt::ControlModifier);
        searchField->setProperty("text", QStringLiteral("f"));
        searchField->setProperty("text", QStringLiteral("fo"));
        QCOMPARE(window->property("searchMatches").toList().size(), 0);
        QTRY_COMPARE(window->property("searchMatches").toList().size(), 2);
        QCOMPARE(editor->property("selectedText").toString(), QStringLiteral("fo"));

        // Return before the pause lands on the new query's first match.
        searchField->setProperty("text", QStringLiteral("bar"));
        QMetaObject::invokeMethod(window, "moveSearch", Q_ARG(QVariant, 1));
        QCOMPARE(window->property("searchMatchIndex").toInt(), 0);
        QCOMPARE(editor->property("selectedText").toString(), QStringLiteral("bar"));

        // Replace All before the pause replaces the new query's matches.
        searchField->setProperty("text", QStringLiteral("foo"));
        replaceField->setProperty("text", QStringLiteral("x"));
        QMetaObject::invokeMethod(replaceAllButton, "clicked");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("x bar x"));

        // Closing the bar drops a search still waiting for the pause.
        searchField->setProperty("text", QStringLiteral("bar"));
        QMetaObject::invokeMethod(window, "closeSearch");
        QTest::qWait(300);
        QCOMPARE(editor->property("selectedText").toString(), QString());
    }

    void reopeningTheFindBarSearchesForWhatItHolds() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *searchField = window->findChild<QObject *>(QStringLiteral("searchField"));
        QObject *replaceField = window->findChild<QObject *>(QStringLiteral("replaceField"));
        QObject *replaceAllButton = window->findChild<QObject *>(QStringLiteral("replaceAllButton"));
        QVERIFY(editor && searchField && replaceField && replaceAllButton);

        editor->setProperty("text", QStringLiteral("foo bar foo"));
        QTest::keyClick(window, Qt::Key_H, Qt::ControlModifier);
        searchField->setProperty("text", QStringLiteral("foo"));
        QTRY_COMPARE(window->property("searchMatches").toList().size(), 2);

        // A query changed just before closing the bar is searched on reopening,
        // so Replace All does not cut at the old query's matches.
        searchField->setProperty("text", QStringLiteral("o"));
        QMetaObject::invokeMethod(window, "closeSearch");
        QTest::keyClick(window, Qt::Key_H, Qt::ControlModifier);
        replaceField->setProperty("text", QStringLiteral("x"));
        QMetaObject::invokeMethod(replaceAllButton, "clicked");
        QCOMPARE(editor->property("text").toString(), QStringLiteral("fxx bar fxx"));

        // Edits made while the bar was closed are searched on reopening too.
        QMetaObject::invokeMethod(window, "closeSearch");
        editor->setProperty("text", QStringLiteral("a bar"));
        QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
        searchField->setProperty("text", QStringLiteral("bar"));
        QTRY_COMPARE(window->property("searchMatches").toList().size(), 1);
        QMetaObject::invokeMethod(window, "closeSearch");
        editor->setProperty("text", QStringLiteral("bar bar bar"));
        QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
        QCOMPARE(window->property("searchMatches").toList().size(), 3);
    }

    void reachesAndLeavesTheReplaceFieldFromTheKeyboard() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *replaceField = window->findChild<QObject *>(QStringLiteral("replaceField"));
        QVERIFY(editor && replaceField);

        QTest::keyClick(window, Qt::Key_H, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY(replaceField->property("activeFocus").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!window->property("searchOpen").toBool());

        // Escape closes the bar from the Replace field itself too.
        QTest::keyClick(window, Qt::Key_H, Qt::ControlModifier);
        QMetaObject::invokeMethod(replaceField, "forceActiveFocus");
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!window->property("searchOpen").toBool());

        // A link inserted from the find bar puts the writer back in the text.
        QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_K, Qt::ControlModifier);
        QVERIFY(editor->property("activeFocus").toBool());
    }

    void keepsShortcutsBehindDialogs() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *dialog = window->findChild<QObject *>(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(dialog);

        QMetaObject::invokeMethod(dialog, "open");
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
        QVERIFY(!window->property("searchOpen").toBool());
        QMetaObject::invokeMethod(dialog, "close");
    }

    void togglesTheFontPickerFromItsButton() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *button = window->findChild<QQuickItem *>(QStringLiteral("fontButton"));
        QObject *picker = window->findChild<QObject *>(QStringLiteral("fontPicker"));
        QVERIFY(button && picker);

        const QPoint center = button->mapToScene(
            QPointF(button->width() / 2, button->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center);
        QTRY_VERIFY(picker->property("opened").toBool());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center);
        QTRY_VERIFY(!picker->property("visible").toBool());
    }

    void breaksLinesAfterInlineBackticks() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        // Backticks that open no fence leave the paragraphs below as prose.
        const QString text = QStringLiteral("```x``` inline\n\npara");
        editor->setProperty("text", text);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        editor->setProperty("cursorPosition", text.length());
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(editor->property("text").toString(), text + QStringLiteral("\n\n"));

        // Before a closing fence the caret is still in the code; before an
        // opening one it is not yet.
        editor->setProperty("text", QStringLiteral("```\ncode\n```"));
        editor->setProperty("cursorPosition", 9);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("```\ncode\n\n```"));
        editor->setProperty("text", QStringLiteral("para\n```\ncode\n```"));
        editor->setProperty("cursorPosition", 5);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(editor->property("text").toString(),
                 QStringLiteral("para\n\n\n```\ncode\n```"));

        // Tilde fences hold code the same way.
        editor->setProperty("text", QStringLiteral("~~~\ncode"));
        editor->setProperty("cursorPosition", 8);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("~~~\ncode\n"));
    }

    void togglesEmphasisAtTheCaret() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");

        const auto type = [window](const QString &keys) {
            for (const QChar key : keys)
                QTest::keyClick(window, key.toLatin1());
        };
        const auto bold = [window] {
            QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier | Qt::AltModifier);
        };
        const auto text = [editor] { return editor->property("text").toString(); };

        // Ctrl+B, type, Ctrl+B ends the bold instead of stacking markers.
        editor->setProperty("text", QStringLiteral("a "));
        editor->setProperty("cursorPosition", 2);
        bold();
        type(QStringLiteral("hi"));
        bold();
        type(QStringLiteral(" more"));
        QCOMPARE(text(), QStringLiteral("a **hi** more"));

        // Just past the closing marker, Ctrl+B steps back into the bold.
        editor->setProperty("text", QStringLiteral("a **hi**"));
        editor->setProperty("cursorPosition", 8);
        bold();
        type(QStringLiteral("!"));
        QCOMPARE(text(), QStringLiteral("a **hi!**"));

        // Inside the bold, Ctrl+B takes it off and keeps the caret in place.
        editor->setProperty("text", QStringLiteral("a **hello** b"));
        editor->setProperty("cursorPosition", 6);
        bold();
        QCOMPARE(text(), QStringLiteral("a hello b"));
        QCOMPARE(editor->property("cursorPosition").toInt(), 4);

        // At the end of bold italic text, Ctrl+B steps out rather than unbolding.
        editor->setProperty("text", QStringLiteral("***hello***"));
        editor->setProperty("cursorPosition", 8);
        bold();
        QCOMPARE(text(), QStringLiteral("***hello***"));
        QCOMPARE(editor->property("cursorPosition").toInt(), 11);

        // Through several layers of nesting too.
        editor->setProperty("text", QStringLiteral("**~~*hello*~~**"));
        editor->setProperty("cursorPosition", 10);
        bold();
        QCOMPARE(text(), QStringLiteral("**~~*hello*~~**"));
        QCOMPARE(editor->property("cursorPosition").toInt(), 15);

        // A literal underscore at the end is text, not a closing marker.
        editor->setProperty("text", QStringLiteral("**value_**"));
        editor->setProperty("cursorPosition", 7);
        bold();
        QCOMPARE(text(), QStringLiteral("value_"));

        // Italic works the same way.
        editor->setProperty("text", QStringLiteral("a "));
        editor->setProperty("cursorPosition", 2);
        QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
        type(QStringLiteral("hi"));
        QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
        type(QStringLiteral(" x"));
        QCOMPARE(text(), QStringLiteral("a *hi* x"));
    }

    void togglesEmphasisMarkers() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        // Sets the text, selects [from, to), toggles, and returns the text.
        const auto toggle = [editor](const QString &text, int from, int to,
                                     const QString &marker) {
            editor->setProperty("text", text);
            QMetaObject::invokeMethod(editor, "select", Q_ARG(int, from), Q_ARG(int, to));
            QMetaObject::invokeMethod(editor, "wrapSelection", Q_ARG(QVariant, marker));
            return editor->property("text").toString();
        };
        const QString bold = QStringLiteral("**");
        const QString italic = QStringLiteral("*");

        QCOMPARE(toggle(QStringLiteral("a word b"), 2, 6, bold), QStringLiteral("a **word** b"));
        // Off again, whether the markers sit outside the selection or in it.
        QCOMPARE(toggle(QStringLiteral("a **word** b"), 4, 8, bold), QStringLiteral("a word b"));
        QCOMPARE(toggle(QStringLiteral("a **word** b"), 2, 10, bold), QStringLiteral("a word b"));
        QCOMPARE(toggle(QStringLiteral("a ~~word~~ b"), 4, 8, QStringLiteral("~~")),
                 QStringLiteral("a word b"));
        // An empty pair goes away too.
        QCOMPARE(toggle(QStringLiteral("Hello****."), 7, 7, bold), QStringLiteral("Hello."));

        // The "*" of bold is not italic, and "***" holds both.
        QCOMPARE(toggle(QStringLiteral("**word**"), 2, 6, italic), QStringLiteral("***word***"));
        QCOMPARE(toggle(QStringLiteral("***word***"), 3, 7, italic), QStringLiteral("**word**"));
        QCOMPARE(toggle(QStringLiteral("***word***"), 3, 7, bold), QStringLiteral("*word*"));
        // Selecting part of bold's markers adds italic rather than removing bold.
        QCOMPARE(toggle(QStringLiteral("**word**"), 1, 7, italic), QStringLiteral("***word***"));

        // Separate spans are wrapped, not broken apart.
        QCOMPARE(toggle(QStringLiteral("**a** and **b**"), 0, 15, bold),
                 QStringLiteral("****a** and **b****"));
        // Other emphasis nested inside does not stop italic from coming off.
        QCOMPARE(toggle(QStringLiteral("*a **b** c*"), 1, 10, italic),
                 QStringLiteral("a **b** c"));
    }

    void findsInlineMarkdownRanges() {
        const auto markup = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("**bold** and *italic* and [site](https://example.com)"));
        QCOMPARE(markup.size(), 3);
        QCOMPARE(markup.at(0).content.start, 2);
        QCOMPARE(markup.at(0).content.length, 4);
        QCOMPARE(markup.at(2).content.length, 4);
        QCOMPARE(markup.at(2).markers[0].length, 1);
    }

    void findsLinkUrlAtPosition() {
        const QString text = QStringLiteral("Read [site](https://example.com/docs) now");
        const int linkStart = text.indexOf(QStringLiteral("site"));

        QCOMPARE(MarkdownHighlighter::linkUrlAt(text, linkStart),
                 QStringLiteral("https://example.com/docs"));
        QCOMPARE(MarkdownHighlighter::linkUrlAt(text, linkStart + 3),
                 QStringLiteral("https://example.com/docs"));
        QVERIFY(MarkdownHighlighter::linkUrlAt(text, linkStart - 1).isEmpty());
        QCOMPARE(MarkdownHighlighter::linkUrlAt(text, text.indexOf(QStringLiteral("https"))),
                 QStringLiteral("https://example.com/docs"));

        // A bare URL is clickable too, without its sentence punctuation.
        const QString bare = QStringLiteral("Read https://example.com/docs, then stop");
        QCOMPARE(MarkdownHighlighter::linkUrlAt(bare, int(bare.indexOf(QStringLiteral("example")))),
                 QStringLiteral("https://example.com/docs"));
        QVERIFY(MarkdownHighlighter::linkUrlAt(bare, 0).isEmpty());

        // Escaped punctuation, as the editor writes for parentheses, reads as itself.
        const QString escaped = QStringLiteral(R"([wiki](https://example.com/a\(b\)))");
        QCOMPARE(MarkdownHighlighter::linkUrlAt(escaped, 1),
                 QStringLiteral("https://example.com/a(b)"));
    }

    void showsLinkUrlOnlyOverTheLinkText() {
        // Probing a character past the end of the text warns from QTextCursor.
        QTest::failOnWarning(QRegularExpression(QStringLiteral("out of range")));
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        editor->setProperty("text", QStringLiteral("see [label](https://example.com)\nplain"));

        const auto urlUnder = [editor](qreal x, qreal y) {
            QVariant url;
            QMetaObject::invokeMethod(editor, "linkUrlUnder", Q_RETURN_ARG(QVariant, url),
                                      Q_ARG(QVariant, x), Q_ARG(QVariant, y));
            return url.toString();
        };
        const auto boxAt = [editor](int position) {
            QRectF box;
            QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, box),
                                      Q_ARG(int, position));
            return box;
        };

        // Over the label (the hidden "[" is index 4), in the right half of a
        // character too.
        const QRectF first = boxAt(5);
        const QRectF second = boxAt(6);
        const qreal middleY = first.y() + first.height() / 2;
        QCOMPARE(urlUnder(first.x() + 1, middleY), QStringLiteral("https://example.com"));
        QCOMPARE(urlUnder(second.x() - 1, middleY), QStringLiteral("https://example.com"));
        // Over the plain word before it.
        QCOMPARE(urlUnder(boxAt(0).x() + 1, middleY), QString());
        // Past the end of the line the link ends.
        QCOMPARE(urlUnder(editor->property("width").toReal() - 2, middleY), QString());
        // The right half of the label's last character still counts, though
        // the nearest caret position is past the label.
        QCOMPARE(urlUnder(boxAt(10).x() - 1, middleY), QStringLiteral("https://example.com"));

        // Below a last line that is a link, positionAt() snaps up onto it.
        editor->setProperty("text", QStringLiteral("[label](https://example.com)"));
        const QRectF only = boxAt(1);
        QCOMPARE(urlUnder(only.x() + 1, only.y() + only.height() / 2),
                 QStringLiteral("https://example.com"));
        QCOMPARE(urlUnder(only.x() + 1, only.y() + only.height() * 3), QString());

        // A wide character counts across its whole width.
        editor->setProperty("text", QStringLiteral("[\u5bbd](https://example.com)"));
        const QRectF wide = boxAt(1);
        const qreal wideRight = boxAt(2).x();
        QVERIFY(wideRight - wide.x() > editor->property("font").value<QFont>().pixelSize() * 0.7);
        QCOMPARE(urlUnder(wideRight - 1, wide.y() + wide.height() / 2),
                 QStringLiteral("https://example.com"));

        // A right-to-left label runs the other way across the screen.
        editor->setProperty("text", QStringLiteral("[\u05e9\u05dc\u05d5\u05dd](https://example.org)"));
        const QRectF rtlFirst = boxAt(1);
        const QRectF rtlSecond = boxAt(2);
        QVERIFY(rtlSecond.x() < rtlFirst.x());
        const qreal rtlY = rtlFirst.y() + rtlFirst.height() / 2;
        QCOMPARE(urlUnder((rtlFirst.x() + rtlSecond.x()) / 2, rtlY),
                 QStringLiteral("https://example.org"));
        QCOMPARE(urlUnder(rtlFirst.x() - 1, rtlY), QStringLiteral("https://example.org"));

        // Link-like text inside fenced code is not a link.
        editor->setProperty("text", QStringLiteral("```\n[label](https://example.com)\n```"));
        QCOMPARE(backend.linkUrlAt(5), QString());
    }

    void showsLinkTooltipUrlAsPlainText() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *tooltipText =
            window->findChild<QObject *>(QStringLiteral("linkTooltipText"));
        QVERIFY(tooltipText);

        // A destination Qt would mistake for HTML must still read as itself.
        const QString destination =
            QStringLiteral("<img src=\"https://example.com/pixel\">");
        QVERIFY(Qt::mightBeRichText(destination));
        tooltipText->setProperty("text", destination);
        QCOMPARE(tooltipText->property("text").toString(), destination);
        QCOMPARE(tooltipText->property("textFormat").toInt(), int(Qt::PlainText));
    }

    void leavesInlineCodeLiteral() {
        QVERIFY(MarkdownHighlighter::inlineMarkup(
                    QStringLiteral("see `default_line_height` and `file_name`")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(
                    QStringLiteral("a_b `c_d`")).isEmpty());
        // Word-boundary underscores inside a code span are still literal.
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("`_private_`")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("`a * b * c`")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(
                    QStringLiteral("`[not](a link)`")).isEmpty());

        // Emphasis that merely wraps a code span still applies.
        const auto wrapping = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("**`file_name` only**"));
        QCOMPARE(wrapping.size(), 1);
        QCOMPARE(wrapping.at(0).kind, MarkdownHighlighter::InlineKind::Bold);
        QCOMPARE(wrapping.at(0).content.start, 2);
        QCOMPARE(wrapping.at(0).content.length, 16);
        QCOMPARE(wrapping.at(0).markers[0].length, 2);
        QCOMPARE(wrapping.at(0).markers[1].start, 18);
    }

    void keepsIntrawordUnderscoresLiteral() {
        QVERIFY(MarkdownHighlighter::inlineMarkup(
                    QStringLiteral("snake_case_name and default_line_height")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("a__b__c")).isEmpty());

        // Emphasis still opens at a word boundary.
        const auto underscoreBold =
            MarkdownHighlighter::inlineMarkup(QStringLiteral("say __bold__ here"));
        QCOMPARE(underscoreBold.size(), 1);
        QCOMPARE(underscoreBold.at(0).content.start, 6);
        QCOMPARE(underscoreBold.at(0).content.length, 4);

        const auto emphasis = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("_italic_, **bold** and mid*word*"));
        QCOMPARE(emphasis.size(), 3);
        QCOMPARE(emphasis.at(0).kind, MarkdownHighlighter::InlineKind::Bold);
        QCOMPARE(emphasis.at(0).content.length, 4);
        QCOMPARE(emphasis.at(1).content.start, 1);
        QCOMPARE(emphasis.at(1).content.length, 6);
        QCOMPARE(emphasis.at(2).content.length, 4);
    }

    void respectsEscapedOpeningBackticks() {
        const auto escaped = MarkdownHighlighter::inlineMarkup(
            QStringLiteral(R"(\` *italic* \`)"));
        QCOMPARE(escaped.size(), 1);
        QCOMPARE(escaped.constFirst().kind, MarkdownHighlighter::InlineKind::Italic);
        QVERIFY(MarkdownHighlighter::codeSpans(QStringLiteral(R"(\` *italic* \`)")).isEmpty());

        const QString mixed = QStringLiteral(R"(\` *italic* `*literal*`)");
        QCOMPARE(MarkdownHighlighter::inlineMarkup(mixed).size(), 1);
        const auto code = MarkdownHighlighter::codeSpans(mixed);
        QCOMPARE(code.size(), 1);
        QCOMPARE(mixed.mid(code.constFirst().start, code.constFirst().length),
                 QStringLiteral("`*literal*`"));

        // An escaped backslash leaves the opening backtick active.
        QVERIFY(MarkdownHighlighter::inlineMarkup(
            QStringLiteral(R"(\\`*literal*`)")).isEmpty());
        QCOMPARE(MarkdownHighlighter::codeSpans(
            QStringLiteral(R"(\\`*literal*`)")).size(), 1);

        // Backslashes inside code do not escape its closing delimiter.
        const auto afterCode = MarkdownHighlighter::inlineMarkup(
            QStringLiteral(R"(`code\` *italic*)"));
        QCOMPARE(afterCode.size(), 1);
        QCOMPARE(afterCode.constFirst().kind, MarkdownHighlighter::InlineKind::Italic);
    }

    void keepsEnclosedCodeStyling_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("kind");
        QTest::newRow("italic") << QStringLiteral("*a `code` b*")
                                << int(MarkdownHighlighter::InlineKind::Italic);
        QTest::newRow("bold") << QStringLiteral("**a `code` b**")
                              << int(MarkdownHighlighter::InlineKind::Bold);
        QTest::newRow("link") << QStringLiteral("[a `code` b](https://example.com)")
                              << int(MarkdownHighlighter::InlineKind::Link);
    }

    void keepsEnclosedCodeStyling() {
        QFETCH(QString, text);
        QFETCH(int, kind);
        QTextDocument document;
        document.setPlainText(text);
        MarkdownHighlighter highlighter(&document);
        highlighter.setTheme(true, "#101010", "#eeeeee", "#5584aa", "#23372b");
        highlighter.rehighlight();

        const auto formatAt = [&document](int position) {
            for (const auto &range : document.firstBlock().layout()->formats()) {
                if (position >= range.start && position < range.start + range.length)
                    return range.format;
            }
            return QTextCharFormat();
        };
        const auto markup = MarkdownHighlighter::inlineMarkup(text);
        QCOMPARE(markup.size(), 1);
        QCOMPARE(int(markup.constFirst().kind), kind);
        const auto prose = formatAt(text.indexOf(QLatin1Char('a')));
        QCOMPARE(prose.fontItalic(), kind == int(MarkdownHighlighter::InlineKind::Italic));
        QCOMPARE(prose.fontWeight() == QFont::Bold,
                 kind == int(MarkdownHighlighter::InlineKind::Bold));
        QCOMPARE(prose.fontUnderline(), kind == int(MarkdownHighlighter::InlineKind::Link));
        QCOMPARE(prose.background().style(), Qt::NoBrush);

        const int codeStart = text.indexOf(QLatin1Char('`'));
        for (int i = codeStart; i < codeStart + 6; ++i) {
            const auto code = formatAt(i);
            QCOMPARE(code.background().color(), QColor("#23372b"));
            QVERIFY(!code.fontItalic());
            QVERIFY(code.fontWeight() != QFont::Bold);
            QVERIFY(!code.fontUnderline());
        }
        for (const auto &marker : markup.constFirst().markers)
            QCOMPARE(formatAt(marker.start).fontPointSize(), 1.0);
    }

    void leavesFencedCodeLiteral() {
        QTextDocument document;
        document.setPlainText(QStringLiteral("prose _italic_\n"
                                            "```ruby\n"
                                            "snake_case_name = *value*\n"
                                            "```\n"
                                            "after _italic_\n"));
        MarkdownHighlighter highlighter(&document);
        highlighter.setTheme(true, "#101010", "#eeeeee", "#5584aa", "#23372b");
        highlighter.rehighlight();

        const auto stateOf = [&document](int blockNumber) {
            return document.findBlockByNumber(blockNumber).userState();
        };
        QCOMPARE(stateOf(0), int(MarkdownHighlighter::Prose));
        QCOMPARE(stateOf(1), int(MarkdownHighlighter::InsideFence));
        QCOMPARE(stateOf(2), int(MarkdownHighlighter::InsideFence));
        QCOMPARE(stateOf(3), int(MarkdownHighlighter::Prose));
        QCOMPARE(stateOf(4), int(MarkdownHighlighter::Prose));

        // The fenced line sits on the code panel, with nothing italic and no
        // marker hidden.
        const QList<QTextLayout::FormatRange> fenced =
            document.findBlockByNumber(2).layout()->formats();
        QCOMPARE(fenced.size(), 1);
        QCOMPARE(fenced.constFirst().length, document.findBlockByNumber(2).text().length());
        QVERIFY(fenced.constFirst().format.background().style() != Qt::NoBrush);
        for (const QTextLayout::FormatRange &range : fenced) {
            QVERIFY(!range.format.fontItalic());
            QVERIFY(range.format.foreground().color() != range.format.background().color());
        }

        const QList<QTextLayout::FormatRange> prose =
            document.findBlockByNumber(4).layout()->formats();
        QVERIFY(std::any_of(prose.cbegin(), prose.cend(),
                            [](const QTextLayout::FormatRange &range) {
                                return range.format.fontItalic();
                            }));
    }

    void opensFencesOnlyWhereMarkdownDoes() {
        QTextDocument document;
        document.setPlainText(QStringLiteral("```inline``` and _italic_\n"
                                            "````md\n"
                                            "```\n"
                                            "still code\n"
                                            "````\n"
                                            "after _italic_\n"));
        MarkdownHighlighter highlighter(&document);
        highlighter.setTheme(true, "#101010", "#eeeeee", "#5584aa", "#23372b");
        highlighter.rehighlight();

        const auto stateOf = [&document](int blockNumber) {
            return document.findBlockByNumber(blockNumber).userState();
        };
        // Backticks that close on the same line are inline code, not a fence.
        QCOMPARE(stateOf(0), int(MarkdownHighlighter::Prose));
        // A four-backtick fence is not closed by three.
        QVERIFY(stateOf(1) >= int(MarkdownHighlighter::InsideFence));
        QVERIFY(stateOf(2) >= int(MarkdownHighlighter::InsideFence));
        QVERIFY(stateOf(3) >= int(MarkdownHighlighter::InsideFence));
        QCOMPARE(stateOf(4), int(MarkdownHighlighter::Prose));
        QCOMPARE(stateOf(5), int(MarkdownHighlighter::Prose));

        QCOMPARE(MarkdownHighlighter::openingFenceLength(QStringLiteral("```ruby")), 3);
        QCOMPARE(MarkdownHighlighter::openingFenceLength(QStringLiteral("    ```")), 0);
        QVERIFY(!MarkdownHighlighter::closesFence(QStringLiteral("``` text"), 3));
    }

    void hidesNoMarkersInsideCode() {
        Backend backend;
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nTextEdit {}", QUrl());
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> editor(component.create());
        QVERIFY2(editor, qPrintable(component.errorString()));
        auto *quickDocument = editor->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(quickDocument);
        backend.attachDocument(quickDocument);
        QTextDocument &document = *quickDocument->textDocument();
        auto *highlighter = document.findChild<MarkdownHighlighter *>();
        QVERIFY(highlighter);
        editor->setProperty("text", QStringLiteral("``` **opening**\n"
                                                   "# _literal_ [link](url)\n"
                                                   "```  \n"
                                                   "*prose*\n"
                                                   "`*literal*`\n"
                                                   "```\n"
                                                   "_unterminated_"));
        highlighter->rehighlight();
        const auto hiddenIn = [&](int blockNumber) {
            int hidden = 0;
            for (const auto &range : document.findBlockByNumber(blockNumber).layout()->formats()) {
                if (range.format.fontPointSize() == 1.0)
                    hidden += range.length;
            }
            return hidden;
        };
        for (int blockNumber : {0, 1, 2, 4, 5, 6})
            QCOMPARE(hiddenIn(blockNumber), 0);
        QCOMPARE(hiddenIn(3), 2);

        // Removing a fence rehighlights the following blocks as prose.
        QTextCursor cursor(&document);
        cursor.setPosition(document.findBlockByNumber(5).position());
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        QTRY_COMPARE(document.findBlockByNumber(6).userState(), int(MarkdownHighlighter::Prose));
        QCOMPARE(hiddenIn(6), 2);
    }

    void showsMarkersAroundTheCaret() {
        const QString text = QStringLiteral("a **b** c");
        // Away from the bold, its markers stay hidden.
        for (int caret : {0, 1, 8, 9}) {
            const auto formats = highlightedFormats(text, caret);
            for (int i : {2, 3, 5, 6})
                QCOMPARE(formats.at(i).fontPointSize(), 1.0);
        }
        // In it or against either end, they show, still in the marker colour.
        for (int caret : {2, 3, 4, 5, 7}) {
            const auto formats = highlightedFormats(text, caret);
            for (int i : {2, 3, 5, 6}) {
                QVERIFY(formats.at(i).fontPointSize() != 1.0);
                QCOMPARE(formats.at(i).foreground().color(), QColor(QStringLiteral("#4f525a")));
            }
            QCOMPARE(formats.at(4).fontWeight(), int(QFont::Bold));
        }

        // Everything the caret sits in shows, and nothing beside it.
        const QString nested = QStringLiteral("*a __b__ c* _d_");
        const auto formats = highlightedFormats(nested, 5);
        for (int i : {0, 3, 4, 6, 7, 10})
            QVERIFY(formats.at(i).fontPointSize() != 1.0);
        for (int i : {12, 14})
            QCOMPARE(formats.at(i).fontPointSize(), 1.0);

        // Moving to another line hides them again there.
        QTextDocument document;
        document.setPlainText(QStringLiteral("**a**\nb"));
        MarkdownHighlighter highlighter(&document);
        highlighter.setCaret(1);
        QVERIFY(document.firstBlock().layout()->formats().constFirst().format.fontPointSize() != 1.0);
        highlighter.setCaret(7);
        QCOMPARE(document.firstBlock().layout()->formats().constFirst().format.fontPointSize(), 1.0);
    }

    void hidesTaskCheckboxesAwayFromTheCaret() {
        const QString text = QStringLiteral("- [ ] task");
        // Away from the marker the three characters are hidden, and the run
        // takes the width the editor draws the checkbox at. The caret just
        // past the closing bracket is already out of it.
        for (int caret : {0, 5, 6, 9}) {
            const auto formats = highlightedFormats(text, caret);
            for (int i : {2, 3, 4})
                QCOMPARE(formats.at(i).fontPointSize(), 1.0);
        }

        // Inside it, the raw marker shows so it can be edited.
        for (int caret : {2, 3, 4}) {
            const auto formats = highlightedFormats(text, caret);
            for (int i : {2, 3, 4}) {
                QVERIFY(formats.at(i).fontPointSize() != 1.0);
                QCOMPARE(formats.at(i).foreground().color(), QColor(QStringLiteral("#4f525a")));
            }
        }

        QTextDocument document;
        document.setPlainText(text);
        MarkdownHighlighter highlighter(&document);
        highlighter.setTheme(true, "#101010", "#eeeeee", "#5584aa", "#23372b");
        highlighter.rehighlight();
        const QTextLine line = document.firstBlock().layout()->lineAt(0);
        const qreal reserved = line.cursorToX(5) - line.cursorToX(2);
        QVERIFY(qAbs(reserved - MarkdownHighlighter::taskCheckboxWidth(document.defaultFont())) < 1.0);
    }

    void takesTheCodePanelFromTheTheme() {
        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());

        const QByteArray originalHome = qgetenv("HOME");
        struct HomeRestorer {
            QByteArray value;
            ~HomeRestorer() { qputenv("HOME", value); }
        } restoreHome{originalHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));

        const QString themeDirectory = homeDirectory.path()
            + QStringLiteral("/.local/state/omarchy/current/theme");
        QVERIFY(QDir().mkpath(themeDirectory));

        Backend backend;
        const auto codePanelFor = [&themeDirectory, &backend](const QByteArray &palette) {
            QFile colorsFile(themeDirectory + QStringLiteral("/colors.toml"));
            if (!colorsFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                return QColor();
            colorsFile.write(palette);
            colorsFile.close();
            // Reload the same backend so a missing key cannot inherit the
            // previous theme's panel color.
            backend.setDarkMode(!backend.darkMode());
            return QColor(backend.themeCodeBackground());
        };

        // The shade the theme names for panels wins.
        QCOMPARE(codePanelFor("mode = \"dark\"\n"
                              "background = \"#111c18\"\n"
                              "foreground = \"#c1c497\"\n"
                              "lighter_background = \"#23372b\"\n"),
                 QColor(QStringLiteral("#23372b")));

        // Themes naming none leave code on a shade mixed from the page: a
        // little off the background, and nowhere near the text.
        const QColor mixedDark = codePanelFor("mode = \"dark\"\n"
                                              "background = \"#000000\"\n"
                                              "foreground = \"#ffffff\"\n");
        QVERIFY(mixedDark != QColor(Qt::black));
        QVERIFY(mixedDark.lightness() < 60);

        const QColor mixedLight = codePanelFor("mode = \"light\"\n"
                                               "background = \"#ffffff\"\n"
                                               "foreground = \"#000000\"\n");
        QVERIFY(mixedLight != QColor(Qt::white));
        QVERIFY(mixedLight.lightness() > 195);

        QCOMPARE(codePanelFor("mode = \"light\"\n"
                              "background = \"#ffffff\"\n"
                              "foreground = \"#000000\"\n"
                              "lighter_background = \"garbage\"\n"), mixedLight);

        // A theme whose lighter background is the page itself would leave code
        // with no panel at all, so it falls back to the mix as well.
        QVERIFY(codePanelFor("mode = \"dark\"\n"
                             "background = \"#0c0b0c\"\n"
                             "foreground = \"#fafcfb\"\n"
                             "lighter_background = \"#0c0b0c\"\n")
                != QColor(QStringLiteral("#0c0b0c")));
    }

    void rendersBoldItalicRuns() {
        const QString text = QStringLiteral("a ***word*** b");
        const auto markup = MarkdownHighlighter::inlineMarkup(text);
        QCOMPARE(markup.size(), 2);
        QCOMPARE(markup.at(0).kind, MarkdownHighlighter::InlineKind::Bold);
        QCOMPARE(markup.at(1).kind, MarkdownHighlighter::InlineKind::Italic);
        QCOMPARE(markup.at(1).content.start, 5);
        QCOMPARE(markup.at(1).content.length, 4);

        const auto formats = highlightedFormats(text);
        QVERIFY(formats.at(5).fontItalic());
        QCOMPARE(formats.at(5).fontWeight(), int(QFont::Bold));
        for (int i : {2, 3, 4, 9, 10, 11})
            QCOMPARE(formats.at(i).fontPointSize(), 1.0);
    }

    // Markers nested inside other markup stay hidden, as the caret skips them.
    void hidesMarkersInsideNestedMarkup() {
        const QString linked = QStringLiteral("[**a**](u)");
        const auto linkFormats = highlightedFormats(linked);
        for (int i : {0, 1, 2, 4, 5, 6})
            QCOMPARE(linkFormats.at(i).fontPointSize(), 1.0);
        QCOMPARE(linkFormats.at(3).fontWeight(), int(QFont::Bold));
        QVERIFY(linkFormats.at(3).fontUnderline());

        const QString nested = QStringLiteral("*a __b__ c*");
        const auto nestedFormats = highlightedFormats(nested);
        for (int i : {0, 3, 4, 6, 7, 10})
            QCOMPARE(nestedFormats.at(i).fontPointSize(), 1.0);
        QVERIFY(nestedFormats.at(5).fontItalic());
        QCOMPARE(nestedFormats.at(5).fontWeight(), int(QFont::Bold));
    }

    void honoursBackslashEscapes() {
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("\\*not\\* here")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("\\[x](y)")).isEmpty());
        QVERIFY(MarkdownHighlighter::inlineMarkup(QStringLiteral("\\~~not~~")).isEmpty());
        // An escaped backslash leaves the marker alone.
        QCOMPARE(MarkdownHighlighter::inlineMarkup(QStringLiteral("\\\\*yes*")).size(), 1);
    }

    void leavesThematicBreaksAlone() {
        for (const QString &rule : {QStringLiteral("*****"), QStringLiteral("___"),
                                    QStringLiteral("* * *")}) {
            QVERIFY2(MarkdownHighlighter::inlineMarkup(rule).isEmpty(), qPrintable(rule));
            const auto formats = highlightedFormats(rule);
            for (const QTextCharFormat &format : formats) {
                QVERIFY(format.fontWeight() != QFont::Bold);
                QVERIFY(format.fontPointSize() != 1.0);
            }
        }
    }

    void mergesInlineStylesWithTheBlock() {
        // A heading stays bold under italic, a link, or code.
        const auto heading = highlightedFormats(QStringLiteral("# head *it* [l](u) `c`"));
        QVERIFY(heading.at(8).fontItalic());
        QCOMPARE(heading.at(8).fontWeight(), int(QFont::Bold));
        QCOMPARE(heading.at(13).fontWeight(), int(QFont::Bold));
        QCOMPARE(heading.at(20).fontWeight(), int(QFont::Bold));

        // A quote keeps its colour under bold.
        const auto quote = highlightedFormats(QStringLiteral("> quote **b**"));
        QCOMPARE(quote.at(10).fontWeight(), int(QFont::Bold));
        QCOMPARE(quote.at(10).foreground().color(), quote.at(3).foreground().color());
    }

    void recognisesTildeFences() {
        QTextDocument document;
        document.setPlainText(QStringLiteral("~~~js\n"
                                            "*code*\n"
                                            "```\n"
                                            "~~~\n"
                                            "after *it*\n"));
        MarkdownHighlighter highlighter(&document);
        highlighter.rehighlight();
        const auto stateOf = [&document](int blockNumber) {
            return document.findBlockByNumber(blockNumber).userState();
        };
        QVERIFY(stateOf(0) >= int(MarkdownHighlighter::InsideFence));
        QVERIFY(stateOf(1) >= int(MarkdownHighlighter::InsideFence));
        // Backticks do not close a tilde fence.
        QVERIFY(stateOf(2) >= int(MarkdownHighlighter::InsideFence));
        QCOMPARE(stateOf(3), int(MarkdownHighlighter::Prose));
        QCOMPARE(stateOf(4), int(MarkdownHighlighter::Prose));
    }

    void readsLinkDestinationsWithParentheses() {
        QCOMPARE(MarkdownHighlighter::linkUrlAt(QStringLiteral("[a](b(c))"), 1),
                 QStringLiteral("b(c)"));
        QCOMPARE(MarkdownHighlighter::linkUrlAt(QStringLiteral("[a](b) and (c)"), 1),
                 QStringLiteral("b"));
    }

    void reportsThemeChangesOnlyWhenTheyHappen() {
        ScopedTheme theme("mode = \"dark\"\nbackground = \"#111111\"\n");
        QVERIFY(theme.ok);
        Backend backend;
        QSignalSpy colorsSpy(&backend, &Backend::themeColorsChanged);
        QSignalSpy modeSpy(&backend, &Backend::darkModeChanged);

        // The theme names its mode, so the desktop's setting changes nothing.
        backend.setDarkMode(false);
        QVERIFY(backend.darkMode());
        QCOMPARE(colorsSpy.count(), 0);
        QCOMPARE(modeSpy.count(), 0);
    }

    void ignoresInlineMarkdownInsideCodeSpans() {
        QCOMPARE(MarkdownHighlighter::inlineMarkup(
                     QStringLiteral("`The_brown_fox` jumps")).size(), 0);
        QCOMPARE(MarkdownHighlighter::inlineMarkup(
                     QStringLiteral("`a **b** [c](d)` and `e`")).size(), 0);

        const auto mixed = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("`code_span` then *real* emphasis"));
        QCOMPARE(mixed.size(), 1);
        QCOMPARE(mixed.at(0).kind, MarkdownHighlighter::InlineKind::Italic);
        QCOMPARE(mixed.at(0).content.start, 18);
    }

    // A code span makes its own contents literal; it does not stop markup that
    // merely encloses one. Only a delimiter landing inside the code span does.
    void keepsMarkupThatEnclosesACodeSpan() {
        const auto spanning = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("_a `b` c_"));
        QCOMPARE(spanning.size(), 1);
        QCOMPARE(spanning.at(0).kind, MarkdownHighlighter::InlineKind::Italic);
        QCOMPARE(spanning.at(0).content.start, 1);
        QCOMPARE(spanning.at(0).content.length, 7);

        const auto linked = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("[see `code`](url)"));
        QCOMPARE(linked.size(), 1);
        QCOMPARE(linked.at(0).kind, MarkdownHighlighter::InlineKind::Link);
        QCOMPARE(linked.at(0).content.start, 1);
        QCOMPARE(linked.at(0).content.length, 10);

        const auto wrapped = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("*`code`*"));
        QCOMPARE(wrapped.size(), 1);
        QCOMPARE(wrapped.at(0).kind, MarkdownHighlighter::InlineKind::Italic);

        const auto bold = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("**a `b` c**"));
        QCOMPARE(bold.size(), 1);
        QCOMPARE(bold.at(0).kind, MarkdownHighlighter::InlineKind::Bold);
        QCOMPARE(bold.at(0).content.start, 2);
        QCOMPARE(bold.at(0).content.length, 7);

        // The closing underscore is inside the code span, so it is not a marker.
        QCOMPARE(MarkdownHighlighter::inlineMarkup(
                     QStringLiteral("_a `b_ c` d_")).size(), 0);
    }

    // Emphasis may enclose a code span, so the code pass has to run last for the
    // code span to keep its own styling.
    void stylesAnEnclosedCodeSpanAsCode() {
        QTextDocument document;
        MarkdownHighlighter highlighter(&document);
        document.setPlainText(QStringLiteral("_a `b` c_"));
        // The code panel comes from the theme, as Backend passes it in.
        highlighter.setTheme(true, "#101010", "#eeeeee", "#5584aa", "#23372b");
        // The constructor's own rehighlight is queued, so ask for one directly.
        highlighter.rehighlight();

        const QTextBlock block = document.firstBlock();
        QVERIFY(block.isValid());
        const auto formatAt = [&block](int index) {
            QTextCharFormat found;
            for (const QTextLayout::FormatRange &range : block.layout()->formats()) {
                if (index >= range.start && index < range.start + range.length)
                    found = range.format;
            }
            return found;
        };

        // "a" is italic and carries no code background.
        QVERIFY(formatAt(1).fontItalic());
        QCOMPARE(formatAt(1).background().style(), Qt::NoBrush);

        // The code span keeps the code background and is not italicised.
        QVERIFY(formatAt(4).background().style() != Qt::NoBrush);
        QVERIFY(!formatAt(4).fontItalic());
    }

    // A stray asterisk next to whitespace is not an emphasis delimiter, so it
    // must not swallow a bullet or the operands of arithmetic.
    void keepsWhitespaceDelimitedAsterisksLiteral() {
        // Only `*important*` is markup; the bullet stays a bullet.
        const auto bullet = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("* first *important* point"));
        QCOMPARE(bullet.size(), 1);
        QCOMPARE(bullet.at(0).kind, MarkdownHighlighter::InlineKind::Italic);
        QCOMPARE(bullet.at(0).content.start, 9);
        QCOMPARE(bullet.at(0).content.length, 9);
        QCOMPARE(bullet.at(0).markers[0].start, 8);
        QCOMPARE(bullet.at(0).markers[1].start, 18);

        // Multiplication stays exactly as typed.
        QCOMPARE(MarkdownHighlighter::inlineMarkup(
                     QStringLiteral("2 * 3 * 4")).size(), 0);

        // Real emphasis is unaffected.
        const auto shortWord = MarkdownHighlighter::inlineMarkup(QStringLiteral("*a*"));
        QCOMPARE(shortWord.size(), 1);
        QCOMPARE(shortWord.at(0).kind, MarkdownHighlighter::InlineKind::Italic);
        QCOMPARE(shortWord.at(0).content.start, 1);
        QCOMPARE(shortWord.at(0).content.length, 1);

        const auto longPhrase =
            MarkdownHighlighter::inlineMarkup(QStringLiteral("*longer text*"));
        QCOMPARE(longPhrase.size(), 1);
        QCOMPARE(longPhrase.at(0).content.length, 11);

        const auto mixed = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("**bold** and *italic*"));
        QCOMPARE(mixed.size(), 2);
        QCOMPARE(mixed.at(0).kind, MarkdownHighlighter::InlineKind::Bold);
        QCOMPARE(mixed.at(1).kind, MarkdownHighlighter::InlineKind::Italic);
    }

    void loadsCurrentOmarchyTheme() {
        ScopedTheme theme(
            "mode = \"light\"\n"
            "accent = \"#112233\"\n"
            "selection = \"#445566\"\n"
            "selection_foreground = \"#fafafa\"\n"
            "background = \"#fefefe\"\n"
            "foreground = \"#101010\"\n");
        QVERIFY(theme.ok);

        Backend backend;
        QCOMPARE(backend.themeBackground(), QStringLiteral("#fefefe"));
        QCOMPARE(backend.themeForeground(), QStringLiteral("#101010"));
        QCOMPARE(backend.themeAccent(), QStringLiteral("#112233"));
        QCOMPARE(backend.themeSelection(), QStringLiteral("#445566"));
        QCOMPARE(backend.themeSelectionForeground(), QStringLiteral("#fafafa"));
        QVERIFY(!backend.darkMode());

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QCOMPARE(editor->property("selectionColor").value<QColor>(), QColor("#445566"));
        QCOMPARE(editor->property("selectedTextColor").value<QColor>(), QColor("#fafafa"));
    }

    void fallsBackForSelectionColors_data() {
        QTest::addColumn<QByteArray>("palette");
        QTest::addColumn<QString>("selection");
        QTest::addColumn<QString>("selectionForeground");

        QTest::newRow("bright_foreground")
            << QByteArray("foreground = \"#c0c0c0\"\n"
                          "bright_foreground = \"#ffffff\"\n"
                          "selection = \"#333333\"\n")
            << QStringLiteral("#333333") << QStringLiteral("#ffffff");
        QTest::newRow("foreground")
            << QByteArray("foreground = \"#c0c0c0\"\n"
                          "selection = \"#333333\"\n")
            << QStringLiteral("#333333") << QStringLiteral("#c0c0c0");
        QTest::newRow("selection_background")
            << QByteArray("foreground = \"#c0c0c0\"\n"
                          "selection_background = \"#222222\"\n")
            << QStringLiteral("#222222") << QStringLiteral("#c0c0c0");
    }

    void fallsBackForSelectionColors() {
        QFETCH(QByteArray, palette);
        QFETCH(QString, selection);
        QFETCH(QString, selectionForeground);

        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());

        const QByteArray originalHome = qgetenv("HOME");
        struct HomeRestorer {
            QByteArray value;
            ~HomeRestorer() { qputenv("HOME", value); }
        } restoreHome{originalHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));

        const QString themeDirectory = homeDirectory.path()
            + QStringLiteral("/.local/state/omarchy/current/theme");
        QVERIFY(QDir().mkpath(themeDirectory));

        QFile colorsFile(themeDirectory + QStringLiteral("/colors.toml"));
        QVERIFY(colorsFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QCOMPARE(colorsFile.write(palette), qint64(palette.size()));
        colorsFile.close();

        Backend backend;
        QCOMPARE(backend.themeSelection(), selection);
        QCOMPARE(backend.themeSelectionForeground(), selectionForeground);
    }

    void readsTomlValuesPastInlineComments() {
        // Every colour in every theme is a quoted "#rrggbb": the hash inside the quotes stays.
        QCOMPARE(Backend::tomlValue(QStringLiteral(" \"#1a1b26\" ")), QStringLiteral("#1a1b26"));
        QCOMPARE(Backend::tomlValue(QStringLiteral("\"#EDE6D6\"   # unbleached cloth")),
                 QStringLiteral("#EDE6D6"));
        QCOMPARE(Backend::tomlValue(QStringLiteral("'#445566'# no space before the hash")),
                 QStringLiteral("#445566"));
        QCOMPARE(Backend::tomlValue(QStringLiteral("\"rebecca#purple\"")),
                 QStringLiteral("rebecca#purple"));
        QCOMPARE(Backend::tomlValue(QStringLiteral(" #1a1b26 ")), QStringLiteral("#1a1b26"));
        QCOMPARE(Backend::tomlValue(QStringLiteral("\"#1a1b26")), QStringLiteral("\"#1a1b26"));
        QCOMPARE(Backend::tomlValue(QStringLiteral("\"\"")), QString());
        QCOMPARE(Backend::tomlValue(QString()), QString());
    }

    void keepsDefaultsForColorsItCannotParse() {
        ScopedTheme theme(
            "mode = \"dark\"          # warmed ink\r\n"
            "accent = \"#112233\"     # shell-white pigment\r\n"
            "selection = '#445566'\r\n"
            "background = \"#1a1b17\" # ink, warmed\r\n"
            "foreground = \"unbleached cloth\"\r\n");
        QVERIFY(theme.ok);

        Backend backend;
        QCOMPARE(backend.themeBackground(), QStringLiteral("#1a1b17"));
        QCOMPARE(backend.themeAccent(), QStringLiteral("#112233"));
        QCOMPARE(backend.themeSelection(), QStringLiteral("#445566"));
        QCOMPARE(backend.themeForeground(), QStringLiteral("#eeeeee"));
        QVERIFY(backend.darkMode());
    }

    void takesItsDefaultsFromTheModeTheThemeAsksFor() {
        ScopedTheme theme(
            "mode = \"light\"\n"
            "background = \"#ffffff\"\n"
            "foreground = \"unbleached cloth\"\n");
        QVERIFY(theme.ok);

        Backend backend;
        QVERIFY(!backend.darkMode());
        QCOMPARE(backend.themeForeground(), QStringLiteral("#222324"));
    }

    void ignoresFileWatcherEventsForSavedContents() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("first-save.md"));
        Backend backend;
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        backend.saveAs(QUrl::fromLocalFile(path));
        QVERIFY(QFileInfo::exists(path));

        QFile sameContents(path);
        QVERIFY(sameContents.open(QIODevice::WriteOnly | QIODevice::Truncate));
        sameContents.close();
        QTest::qWait(100);
        QCOMPARE(externalChangeSpy.count(), 0);

        QFile changedContents(path);
        QVERIFY(changedContents.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(changedContents.write("changed elsewhere"), qint64(17));
        changedContents.close();
        QTRY_COMPARE(externalChangeSpy.count(), 1);
    }

    void keepsCursorAndSelectionStableAcrossInsertions() {
        const QString mutationsPath = QFINDTESTDATA("../src/EditorMutations.js");
        QVERIFY(!mutationsPath.isEmpty());

        QQmlEngine engine;
        QQmlComponent component(&engine);
        const QByteArray harness = R"QML(
            import QtQuick
            import "EditorMutations.js" as EditorMutations

            TextEdit {
                function replaceText(start, end, replacement) {
                    remove(start, end);
                    insert(start, replacement);
                }

                property string insertionText
                property int insertionCursor
                property string wrappedText
                property int wrappedSelectionStart
                property int wrappedSelectionEnd

                Component.onCompleted: {
                    text = "alpha omega";
                    cursorPosition = 5;
                    EditorMutations.replaceRange(this, 5, 5, "one\r\ntwo");
                    insertionText = text;
                    insertionCursor = cursorPosition;

                    text = "alpha beta omega";
                    select(6, 10);
                    EditorMutations.replaceRange(this, selectionStart, selectionEnd,
                                                 "**beta**", 2, 6);
                    wrappedText = text;
                    wrappedSelectionStart = selectionStart;
                    wrappedSelectionEnd = selectionEnd;
                }
            }
        )QML";
        const QUrl harnessUrl = QUrl::fromLocalFile(
            QFileInfo(mutationsPath).absolutePath() + QStringLiteral("/MutationHarness.qml"));
        component.setData(harness, harnessUrl);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> editor(component.create());
        QVERIFY2(editor, qPrintable(component.errorString()));

        QCOMPARE(editor->property("insertionText").toString(),
                 QStringLiteral("alphaone\ntwo omega"));
        QCOMPARE(editor->property("insertionCursor").toInt(), 12);
        QCOMPARE(editor->property("wrappedText").toString(),
                 QStringLiteral("alpha **beta** omega"));
        QCOMPARE(editor->property("wrappedSelectionStart").toInt(), 8);
        QCOMPARE(editor->property("wrappedSelectionEnd").toInt(), 12);
    }

    void stepsOverWholeCharactersWhenTypingAndSaving() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        const auto setText = [&](const QString &value) {
            editor->setProperty("text", value);
            editor->setProperty("cursorPosition", 0);
        };
        const auto move = [&](int direction) -> int {
            const bool invoked = QMetaObject::invokeMethod(
                editor, "moveCursorVisibly", Q_ARG(QVariant, direction));
            return invoked ? editor->property("cursorPosition").toInt() : -1;
        };

        // Right steps over the whole emoji, so typing keeps it in the file.
        setText(QStringLiteral("a😀b"));
        editor->setProperty("cursorPosition", 1);
        QCOMPARE(move(1), 3);
        QVERIFY(QMetaObject::invokeMethod(editor, "insert", Q_ARG(int, 3),
                                          Q_ARG(QString, QStringLiteral("x"))));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("a😀xb"));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString savedPath = directory.filePath(QStringLiteral("emoji.md"));
        backend.saveAs(QUrl::fromLocalFile(savedPath));
        QFile saved(savedPath);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(saved.readAll()), QStringLiteral("a😀xb"));

        // Left steps back over the same cluster.
        setText(QStringLiteral("a😀b"));
        editor->setProperty("cursorPosition", 3);
        QCOMPARE(move(-1), 1);

        // Markers show at the caret, so it steps through them like any text.
        setText(QStringLiteral("**bold**"));
        editor->setProperty("cursorPosition", 0);
        QCOMPARE(move(1), 1);
        editor->setProperty("cursorPosition", 8);
        QCOMPARE(move(-1), 7);

        // Markers next to an emoji still step over the whole emoji.
        setText(QStringLiteral("*😀*x"));
        editor->setProperty("cursorPosition", 0);
        QCOMPARE(move(1), 1);
        QCOMPARE(move(1), 3);
        QCOMPARE(move(1), 4);

        // Paragraph separators still move one position at a time.
        setText(QStringLiteral("ab\ncd"));
        editor->setProperty("cursorPosition", 2);
        QCOMPARE(move(1), 3);
        QCOMPARE(move(-1), 2);
    }

    void stepsCursorByWholeGraphemeClusters() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);

        const auto step = [&](const QString &text, int from, int direction) {
            editor->setProperty("text", text);
            return backend.cursorPositionStep(from, direction);
        };

        // Qt 6 treats surrogate pairs, skin tone modifiers, ZWJ sequences,
        // regional indicators and combining marks as single cursor steps.
        QCOMPARE(step(QStringLiteral("a😀b"), 1, 1), 3);
        QCOMPARE(step(QStringLiteral("a😀b"), 3, -1), 1);
        QCOMPARE(step(QStringLiteral("a👍🏽b"), 1, 1), 5);
        QCOMPARE(step(QStringLiteral("a👍🏽b"), 5, -1), 1);
        QCOMPARE(step(QStringLiteral("a👨‍👩‍👧b"), 1, 1), 9);
        QCOMPARE(step(QStringLiteral("a👨‍👩‍👧b"), 9, -1), 1);
        QCOMPARE(step(QStringLiteral("a🇺🇸b"), 1, 1), 5);
        QCOMPARE(step(QStringLiteral("a1\u20e3b"), 1, 1), 3);
        QCOMPARE(step(QStringLiteral("ae\u0301b"), 1, 1), 3);

        // Document limits and paragraph separators are handled explicitly.
        QCOMPARE(step(QStringLiteral("a😀b"), 0, -1), 0);
        QCOMPARE(step(QStringLiteral("a😀b"), 4, 1), 4);
        QCOMPARE(step(QStringLiteral("ab\ncd"), 2, 1), 3);
        QCOMPARE(step(QStringLiteral("ab\ncd"), 3, -1), 2);
        QCOMPARE(step(QStringLiteral("ab\ncd"), 5, 1), 5);
        QCOMPARE(step(QStringLiteral("😀\nb"), 2, 1), 3);
        QCOMPARE(step(QStringLiteral("😀\nb"), 3, -1), 2);
    }

    void savesAndOpensFromFooterButtons() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QVERIFY(window->findChild<QObject *>(QStringLiteral("sourceEditor")));
        QVERIFY(!window->findChild<QObject *>(QStringLiteral("renderedPreview")));
        QVERIFY(!window->findChild<QObject *>(QStringLiteral("modeToggle")));

        QObject *saveButton = window->findChild<QObject *>(QStringLiteral("saveButton"));
        QObject *openButton = window->findChild<QObject *>(QStringLiteral("openButton"));
        QVERIFY(saveButton);
        QVERIFY(openButton);

        QSignalSpy saveDialogSpy(&backend, &Backend::saveDialogRequested);
        QVERIFY(QMetaObject::invokeMethod(saveButton, "clicked"));
        QCOMPARE(saveDialogSpy.count(), 1);

        QSignalSpy openDialogSpy(&backend, &Backend::openDialogRequested);
        QVERIFY(QMetaObject::invokeMethod(openButton, "clicked"));
        QCOMPARE(openDialogSpy.count(), 1);
    }

    void copiesRenderedHtml() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        editor->setProperty("text", QStringLiteral(
            "# Title\n\nFirst **bold**.\n\nSecond.\n\n- [ ] one\n- [x] two"));
        backend.copyAsHtml();

        const QMimeData *mimeData = QGuiApplication::clipboard()->mimeData();
        QVERIFY(mimeData);
        QVERIFY(mimeData->hasHtml());
        const QString html = mimeData->html();
        QVERIFY(html.contains(QStringLiteral("<h1>")));
        QVERIFY(html.contains(QStringLiteral("<p>First ")));
        QVERIFY(html.contains(QStringLiteral("<body>")));
        QVERIFY(html.contains(QStringLiteral("font-weight:700;\">bold</span>")));
        QVERIFY(html.contains(QStringLiteral("class=\"unchecked\"")));
        QVERIFY(html.contains(QStringLiteral("class=\"checked\"")));
        QVERIFY(!html.contains(QStringLiteral("font-family")));
        QVERIFY(!html.contains(QStringLiteral("margin")));
        QCOMPARE(mimeData->text(), QStringLiteral("Title\nFirst bold.\nSecond.\none\ntwo"));
    }

    void flashesScrollBarOnlyWhenPaging() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QObject *linger = window->findChild<QObject *>(QStringLiteral("scrollLinger"));
        QVERIFY(editor);
        QVERIFY(linger);

        QStringList lines;
        for (int i = 0; i < 300; ++i)
            lines << QStringLiteral("line %1").arg(i);
        editor->setProperty("text", lines.join(QLatin1Char('\n')));
        QVERIFY(QMetaObject::invokeMethod(editor, "forceActiveFocus"));
        QTRY_VERIFY(editor->property("activeFocus").toBool());

        // Writing past the bottom edge scrolls along, but leaves the bar hidden.
        editor->setProperty("cursorPosition", editor->property("length").toInt());
        QVERIFY(!linger->property("running").toBool());

        QTest::keyClick(window, Qt::Key_PageUp);
        QVERIFY(linger->property("running").toBool());

        QMetaObject::invokeMethod(linger, "stop");
        QTest::keyClick(window, Qt::Key_Home, Qt::ControlModifier);
        QVERIFY(linger->property("running").toBool());
        QCOMPARE(editor->property("cursorPosition").toInt(), 0);
    }

    void breaksLinesOnReturn() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        QVERIFY(editor->property("activeFocus").toBool());

        auto text = [&] { return editor->property("text").toString(); };
        auto load = [&](const QString &content, int position) {
            // Park the caret first: shrinking the text under a caret near its
            // old end makes QQuickTextEdit warn while it restores it.
            editor->setProperty("cursorPosition", 0);
            editor->setProperty("text", content);
            editor->setProperty("cursorPosition", position);
        };
        auto returnKey = [&] { QTest::keyClick(window, Qt::Key_Return); };

        // Ending a paragraph leaves the blank line that separates it from the
        // next one.
        load(QStringLiteral("one\n\ntwo"), 8);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo\n\n"));

        // On a line that is already blank there is nothing to separate from,
        // so Return is worth one line, not two.
        load(QStringLiteral("one\n\ntwo"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\ntwo"));
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\n\ntwo"));

        // An empty document is a blank line too.
        load(QString(), 0);
        returnKey();
        QCOMPARE(text(), QStringLiteral("\n"));

        // Whitespace left behind on a line still reads as blank.
        load(QStringLiteral("one\n  \ntwo"), 5);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n \n \ntwo"));

        // A selection dragged right to left leaves the caret on the blank line
        // it began on, but the break lands on the line it leaves behind.
        load(QStringLiteral("one\n\ntwo"), 7);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 4));
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\no"));

        // A list carries its marker down, and an item left empty drops the
        // marker to end the list.
        load(QStringLiteral("- item"), 6);
        returnKey();
        QCOMPARE(text(), QStringLiteral("- item\n- "));
        returnKey();
        QCOMPARE(text(), QStringLiteral("- item\n\n"));

        // A task carries an unchecked checkbox down, even from a checked one,
        // and an empty task ends the list. Ordered tasks keep their number.
        load(QStringLiteral("- [ ] item"), 10);
        returnKey();
        QCOMPARE(text(), QStringLiteral("- [ ] item\n- [ ] "));
        returnKey();
        QCOMPARE(text(), QStringLiteral("- [ ] item\n\n"));
        load(QStringLiteral("- [x] done"), 10);
        returnKey();
        QCOMPARE(text(), QStringLiteral("- [x] done\n- [ ] "));
        load(QStringLiteral("1. [ ] one"), 10);
        returnKey();
        QCOMPARE(text(), QStringLiteral("1. [ ] one\n2. [ ] "));

        // Inside a code fence every line is its own, blank ones included.
        load(QStringLiteral("```\ncode\n```"), 8);
        returnKey();
        QCOMPARE(text(), QStringLiteral("```\ncode\n\n```"));
        returnKey();
        QCOMPARE(text(), QStringLiteral("```\ncode\n\n\n```"));
    }

    void togglesCheckboxes() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        QVERIFY(editor->property("activeFocus").toBool());

        auto text = [&] { return editor->property("text").toString(); };
        auto checkboxes = [&]() {
            QList<QQuickItem *> found;
            auto *editorItem = qobject_cast<QQuickItem *>(editor);
            for (QQuickItem *child : editorItem->childItems()) {
                if (child->objectName() == QStringLiteral("taskCheckbox"))
                    found.append(child);
            }
            return found;
        };
        auto firstCheckbox = [&]() -> QQuickItem * {
            const QList<QQuickItem *> found = checkboxes();
            return found.isEmpty() ? nullptr : found.constFirst();
        };

        editor->setProperty("text", QStringLiteral("- [ ] one\n- [x] two"));
        editor->setProperty("cursorPosition", 0);
        QTRY_COMPARE(checkboxes().size(), 2);

        // Clicking the drawn box toggles the Markdown without moving the caret.
        QQuickItem *first = firstCheckbox();
        QVERIFY(first);
        QVERIFY(first->isVisible());
        const int caret = editor->property("cursorPosition").toInt();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          first->mapToScene(QPointF(first->width() / 2,
                                                    first->height() / 2)).toPoint());
        QTRY_COMPARE(text(), QStringLiteral("- [x] one\n- [x] two"));
        QCOMPARE(editor->property("cursorPosition").toInt(), caret);

        // One undo takes the toggle back.
        QMetaObject::invokeMethod(editor, "undo");
        QTRY_COMPARE(text(), QStringLiteral("- [ ] one\n- [x] two"));

        // The raw marker shows, and the box hides, while the caret is inside it.
        editor->setProperty("cursorPosition", 3);
        QTRY_VERIFY(firstCheckbox() && !firstCheckbox()->isVisible());
        editor->setProperty("cursorPosition", 8);
        QTRY_VERIFY(firstCheckbox() && firstCheckbox()->isVisible());

        // The keyboard toggles the item at the caret too.
        QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(text(), QStringLiteral("- [x] one\n- [x] two"));

        // A bare marker at the end of the document still gets a box.
        editor->setProperty("cursorPosition", 0);
        editor->setProperty("text", QStringLiteral("- [ ]"));
        editor->setProperty("cursorPosition", 0);
        QTRY_COMPARE(checkboxes().size(), 1);
        QVERIFY(firstCheckbox() && firstCheckbox()->isVisible());
    }

    void ignoresTasksInsideCodeFences() {
        Backend backend;
        QQuickTextDocument *quickDocument = attachTextEdit(&backend);
        QVERIFY(quickDocument);
        QTextDocument *document = quickDocument->textDocument();
        document->setPlainText(QStringLiteral("```\n- [ ] code\n```\n- [x] real\n"));
        QTRY_VERIFY(document->findBlockByNumber(1).userState()
                    >= int(MarkdownHighlighter::InsideFence));

        const QVariantList markers = backend.taskMarkers();
        QCOMPARE(markers.size(), 1);
        const QVariantMap marker = markers.constFirst().toMap();
        QCOMPARE(marker.value(QStringLiteral("position")).toInt(), 21);
        QVERIFY(marker.value(QStringLiteral("checked")).toBool());

        // A line inside the fence is not a task, so it cannot be toggled.
        QVERIFY(!backend.toggleTaskAt(6));
        QCOMPARE(document->toPlainText(),
                 QStringLiteral("```\n- [ ] code\n```\n- [x] real\n"));
    }

    void nestsListItemsOnTab() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        QVERIFY(editor->property("activeFocus").toBool());

        auto text = [&] { return editor->property("text").toString(); };
        auto caret = [&] { return editor->property("cursorPosition").toInt(); };
        auto load = [&](const QString &content, int position) {
            editor->setProperty("text", content);
            editor->setProperty("cursorPosition", position);
        };
        auto tabKey = [&] { QTest::keyClick(window, Qt::Key_Tab); };
        auto backtabKey = [&] { QTest::keyClick(window, Qt::Key_Backtab, Qt::ShiftModifier); };

        auto selection = [&] {
            return QStringLiteral("%1-%2@%3").arg(editor->property("selectionStart").toInt())
                                             .arg(editor->property("selectionEnd").toInt())
                                             .arg(caret());
        };

        // An item nests four spaces under the item above, the caret rides
        // along, and Shift+Tab puts it back.
        load(QStringLiteral("- one\n- two"), 11);
        tabKey();
        QCOMPARE(text(), QStringLiteral("- one\n    - two"));
        QCOMPARE(caret(), 15);
        backtabKey();
        QCOMPARE(text(), QStringLiteral("- one\n- two"));
        QCOMPARE(caret(), 11);

        // One undo takes a nesting back.
        tabKey();
        QMetaObject::invokeMethod(editor, "undo");
        QCOMPARE(text(), QStringLiteral("- one\n- two"));

        // The same step nests under ordered markers, the empty one Return
        // leaves included, and Shift+Tab steps back out one level.
        load(QStringLiteral("1. one"), 6);
        QTest::keyClick(window, Qt::Key_Return);
        tabKey();
        QCOMPARE(text(), QStringLiteral("1. one\n    2. "));
        load(QStringLiteral("10. a\n- b"), 9);
        tabKey();
        QCOMPARE(text(), QStringLiteral("10. a\n    - b"));
        load(QStringLiteral("1. a\n    - b\n        - c"), 24);
        backtabKey();
        QCOMPARE(text(), QStringLiteral("1. a\n    - b\n    - c"));

        // The first item nests all the same, an outermost one has nowhere to
        // go, and a tab-indented one comes out like a spaced one.
        load(QStringLiteral("- a"), 3);
        tabKey();
        QCOMPARE(text(), QStringLiteral("    - a"));
        backtabKey();
        backtabKey();
        QCOMPARE(text(), QStringLiteral("- a"));
        load(QStringLiteral("- a\n\t- b"), 8);
        backtabKey();
        QCOMPARE(text(), QStringLiteral("- a\n- b"));

        // A selection moves the items it touches together, keeping their
        // depth and its own direction, but not a line it only reaches the
        // start of.
        load(QStringLiteral("- a\n- b\n    - c\n- d"), 16);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 4));
        tabKey();
        QCOMPARE(text(), QStringLiteral("- a\n    - b\n        - c\n- d"));
        QCOMPARE(selection(), QStringLiteral("8-24@8"));

        // Shift+Tab lifts every line by one step, no further than the
        // shallowest line goes, so the nesting within the selection is kept.
        load(QStringLiteral("- a\n  - b\n    - c"), 6);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 17));
        backtabKey();
        QCOMPARE(text(), QStringLiteral("- a\n- b\n  - c"));
        load(QStringLiteral("- a\n    - b\n  - c"), 6);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 17));
        backtabKey();
        QCOMPARE(text(), QStringLiteral("- a\n  - b\n- c"));
        load(QStringLiteral("    - a\n    - b"), 0);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 10));
        backtabKey();
        QCOMPARE(text(), QStringLiteral("- a\n- b"));
        QCOMPARE(selection(), QStringLiteral("0-4@4"));
        load(QStringLiteral("- a\n \t- b\n    - c"), 4);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 15));
        backtabKey();
        QCOMPARE(text(), QStringLiteral("- a\n- b\n- c"));

        // Indented continuation lines move with their item, but a paragraph
        // at the margin is no item, so Tab leaves the list alone.
        load(QStringLiteral("- a\n- b\n  more"), 4);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 14));
        tabKey();
        QCOMPARE(text(), QStringLiteral("- a\n    - b\n      more"));
        load(QStringLiteral("- a\n- b\n\nParagraph"), 4);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 18));
        tabKey();
        QVERIFY(!text().contains(QStringLiteral("    ")));

        // A fence indented under an item moves with it, but one that starts
        // the selection, or a selection running from a list into a code
        // fence, is left alone.
        load(QStringLiteral("- a\n- b\n  ```\n  code\n  ```"), 4);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 26));
        tabKey();
        QCOMPARE(text(), QStringLiteral("- a\n    - b\n      ```\n      code\n      ```"));
        load(QStringLiteral("  ```\n  - x\n  ```"), 0);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 17));
        tabKey();
        QVERIFY(!text().contains(QStringLiteral("    ")));
        load(QStringLiteral("- a\n  ```\n  - x\n  ```"), 0);
        QMetaObject::invokeMethod(editor, "moveCursorSelection", Q_ARG(int, 14));
        tabKey();
        QVERIFY(!text().contains(QStringLiteral("    ")));

        // Outside a list, on a rule, and inside a code fence, Tab types a tab.
        load(QStringLiteral("plain"), 5);
        tabKey();
        QCOMPARE(text(), QStringLiteral("plain\t"));
        load(QStringLiteral("\n- a"), 0);
        tabKey();
        QCOMPARE(text(), QStringLiteral("\t\n- a"));
        load(QStringLiteral("- a\n* * *"), 9);
        tabKey();
        QCOMPARE(text(), QStringLiteral("- a\n* * *\t"));
        load(QStringLiteral("```\n- a\n```"), 7);
        tabKey();
        QCOMPARE(text(), QStringLiteral("```\n- a\t\n```"));
    }

    void closesParagraphBreaksOnBackspace() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "forceActiveFocus");
        QVERIFY(editor->property("activeFocus").toBool());

        auto text = [&] { return editor->property("text").toString(); };
        auto caret = [&] { return editor->property("cursorPosition").toInt(); };
        auto load = [&](const QString &content, int position) {
            editor->setProperty("text", content);
            editor->setProperty("cursorPosition", position);
        };
        auto returnKey = [&] { QTest::keyClick(window, Qt::Key_Return); };
        auto backspaceKey = [&] { QTest::keyClick(window, Qt::Key_Backspace); };

        // Backspace at the head of a paragraph closes the whole break above
        // it, so the two paragraphs join in one press rather than two, and
        // the caret lands where they meet.
        load(QStringLiteral("one\n\ntwo"), 5);
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("onetwo"));
        QCOMPARE(caret(), 3);

        // Ending a paragraph writes both breaks at once, and one Backspace
        // takes both back. A Return on the blank line of "one\n\n\ntwo" can
        // leave this very document with the caret in this very place, so it
        // is not provenance that decides here: it is that the gap is left
        // standing either way, and this is much the commoner press.
        load(QStringLiteral("one\n\ntwo"), 3);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\n\ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo"));
        QCOMPARE(caret(), 3);

        // Which is what a Return inside a wide gap costs: the same rule reads
        // this as the paragraph-ending press it cannot be told apart from, so
        // the writer gets back one blank line fewer than they had. The gap
        // still separates the paragraphs, and one more Return returns it.
        load(QStringLiteral("one\n\n\n\ntwo"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\n\n\ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\ntwo"));
        QCOMPARE(caret(), 3);

        // Return on a blank line writes a single break, so Backspace must
        // take back a single break too. Closing the pair behind the caret
        // would swallow the separator that was there beforehand and leave
        // the two paragraphs with nothing between them.
        load(QStringLiteral("one\n\ntwo"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo"));

        // The same holds however far the gap has grown: each Return is worth
        // one Backspace.
        load(QStringLiteral("one\n\ntwo"), 4);
        returnKey();
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n\n\ntwo"));
        backspaceKey();
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo"));

        // A line with nothing but spaces on it is a blank line, and Return
        // above one writes a single break there too.
        load(QStringLiteral("one\n  \ntwo"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n  \ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n  \ntwo"));

        // On the blank line a document ends with, the two Returns leave the
        // same text behind and no reading of it says which was pressed.
        // Backspace takes one break, because taking two would carry off the
        // line the document already ended with.
        load(QStringLiteral("one\n"), 4);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\n"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n"));

        // Which costs the paragraph that ends a document a second press,
        // and that is the whole of what it costs: nothing is lost on the
        // way through.
        load(QStringLiteral("one\n\ntwo"), 8);
        returnKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo\n\n"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo\n"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n\ntwo"));

        // An empty document is a blank line, so it takes two Returns to open
        // a gap and two Backspaces to close it again.
        load(QString(), 0);
        returnKey();
        returnKey();
        QCOMPARE(text(), QStringLiteral("\n\n"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("\n"));
        backspaceKey();
        QCOMPARE(text(), QString());

        // A document holding nothing but spaces is a blank line as well, and
        // the spaces are text: they must still be there at the end of it.
        load(QStringLiteral("   "), 3);
        returnKey();
        returnKey();
        QCOMPARE(text(), QStringLiteral("   \n\n"));
        backspaceKey();
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("   "));

        // Return at the very head of a document writes both breaks, since
        // the paragraph below it is not blank, and one Backspace takes them.
        load(QStringLiteral("two"), 0);
        returnKey();
        QCOMPARE(text(), QStringLiteral("\n\ntwo"));
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("two"));
        QCOMPARE(caret(), 0);

        // Spaces left on the line above a break do not make the break any
        // less of one: Return between them and the paragraph below wrote
        // both of these, so Backspace still takes both.
        load(QStringLiteral("one\n  \n\ntwo"), 8);
        backspaceKey();
        QCOMPARE(text(), QStringLiteral("one\n  two"));
        QCOMPARE(caret(), 6);
    }

    void unsavedChangesDialogSupportsAltMnemonics() {
        const QString dialogQmlPath = QFINDTESTDATA("../src/UnsavedChangesDialog.qml");
        QVERIFY(!dialogQmlPath.isEmpty());

        QQmlEngine engine;
        engine.addImportPath(QFileInfo(dialogQmlPath).absolutePath());
        QQmlComponent component(&engine);
        const QByteArray harness = R"QML(
            import QtQuick
            import QtQuick.Controls

            ApplicationWindow {
                width: 640
                height: 420
                visible: true

                UnsavedChangesDialog {
                    id: dialog
                    objectName: "unsavedChangesDialog"
                    containerWidth: parent.width
                    containerHeight: parent.height
                }
            }
        )QML";
        component.setData(harness, QUrl::fromLocalFile(
            QFileInfo(dialogQmlPath).absolutePath() + QStringLiteral("/ShortcutHarness.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));

        auto *window = qobject_cast<QQuickWindow *>(root.data());
        QVERIFY(window);
        QObject *dialog = root->findChild<QObject *>(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(dialog);

        QSignalSpy cancelSpy(dialog, SIGNAL(cancelRequested()));
        QSignalSpy discardSpy(dialog, SIGNAL(discardRequested()));
        QSignalSpy saveSpy(dialog, SIGNAL(saveRequested()));

        // Bare letters and Ctrl chords do nothing, so a stray keypress
        // while typing can never discard work.
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_C);
        QTest::keyClick(window, Qt::Key_D);
        QTest::keyClick(window, Qt::Key_S);
        QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(cancelSpy.count(), 0);
        QCOMPARE(discardSpy.count(), 0);
        QCOMPARE(saveSpy.count(), 0);
        QVERIFY(dialog->property("opened").toBool());

        QTest::keyClick(window, Qt::Key_C, Qt::AltModifier);
        QTRY_COMPARE(cancelSpy.count(), 1);
        QVERIFY(!dialog->property("opened").toBool());

        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_D, Qt::AltModifier);
        QTRY_COMPARE(discardSpy.count(), 1);
        QVERIFY(!dialog->property("opened").toBool());

        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_S, Qt::AltModifier);
        QTRY_COMPARE(saveSpy.count(), 1);
        QVERIFY(!dialog->property("opened").toBool());

        // With the dialog closed, the mnemonics are inert.
        QTest::keyClick(window, Qt::Key_C, Qt::AltModifier);
        QTest::keyClick(window, Qt::Key_D, Qt::AltModifier);
        QTest::keyClick(window, Qt::Key_S, Qt::AltModifier);
        QCOMPARE(cancelSpy.count(), 1);
        QCOMPARE(discardSpy.count(), 1);
        QCOMPARE(saveSpy.count(), 1);
    }

    void scalesTextWithDesktopTextSize() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 20);

        // `omarchy display text size 16` sets the GNOME factor to 16/12.
        backend.setDesktopTextScale(16.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 27);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 27);

        backend.setDesktopTextScale(9.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 15);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 15);
    }

    void stepsTextSizeOnTopOfTheDesktopScale() {
        Backend backend;
        QSignalSpy textScaleSpy(&backend, &Backend::textScaleChanged);
        backend.setDesktopTextScale(1.5);
        QCOMPARE(backend.textScale(), 1.5);

        backend.increaseTextSize();
        QCOMPARE(backend.textScale(), 1.5 * 1.1);
        backend.decreaseTextSize();
        backend.decreaseTextSize();
        QCOMPARE(backend.textScale(), 1.5 * 0.9);
        QCOMPARE(backend.status(), QStringLiteral("Text size 90%"));

        // The ends of the ladder hold instead of running away.
        for (int step = 0; step < 8; ++step)
            backend.decreaseTextSize();
        QCOMPARE(backend.textScale(), 1.5 * 0.5);
        const int settledCount = textScaleSpy.count();
        backend.decreaseTextSize();
        QCOMPARE(textScaleSpy.count(), settledCount);

        // A new window picks the size back up.
        Backend reopened;
        QCOMPARE(reopened.textScale(), 0.5);

        for (int step = 0; step < 20; ++step)
            backend.increaseTextSize();
        QCOMPARE(backend.textScale(), 1.5 * 3.0);
        QCOMPARE(Backend().textScale(), 3.0);
        const int maximumCount = textScaleSpy.count();
        backend.increaseTextSize();
        QCOMPARE(textScaleSpy.count(), maximumCount);

        backend.resetTextSize();
        QCOMPARE(backend.textScale(), 1.5);
        QCOMPARE(Backend().textScale(), 1.0);
    }

    void banksTouchpadScrollBeforeChangingTextSize() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *scroll = window->findChild<QObject *>(QStringLiteral("editorScroll"));
        QVERIFY(scroll);

        const auto scrollBy = [scroll](int angle, int pixels) {
            const QVariant wheel = QVariantMap{
                {QStringLiteral("angleDelta"), QVariantMap{{QStringLiteral("y"), angle}}},
                {QStringLiteral("pixelDelta"), QVariantMap{{QStringLiteral("y"), pixels}}}};
            QVERIFY(QMetaObject::invokeMethod(scroll, "zoomByWheel", Q_ARG(QVariant, wheel)));
        };

        // A touchpad trickles in pixels: a nudge is not a whole step yet. Its
        // events carry a synthesized angleDelta as well, as Wayland sends them.
        scrollBy(240, 20);
        scrollBy(240, 20);
        QCOMPARE(backend.textScale(), 1.0);
        scrollBy(240, 20);
        QCOMPARE(backend.textScale(), 1.1);

        // A wheel notch is one step, and reversing drops the banked remainder.
        scrollBy(-120, 0);
        QCOMPARE(backend.textScale(), 1.0);

        backend.resetTextSize();
    }

    void growsTextFromEitherPlusKey() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        QQuickWindow *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        // Qt resolves a shifted = to Ctrl++ rather than to Ctrl+=, and a layout
        // that puts + on its own key never produces Ctrl+= at all.
        QTest::keyClick(window, Qt::Key_Plus, Qt::ControlModifier);
        QCOMPARE(backend.textScale(), 1.1);
        QTest::keyClick(window, Qt::Key_Equal, Qt::ControlModifier);
        QCOMPARE(backend.textScale(), 1.25);
        QTest::keyClick(window, Qt::Key_Minus, Qt::ControlModifier);
        QCOMPARE(backend.textScale(), 1.1);

        // Exercise the actual reset binding as well as zooming out below 100%.
        QTest::keyClick(window, Qt::Key_0, Qt::ControlModifier);
        QCOMPARE(backend.textScale(), 1.0);
        QTest::keyClick(window, Qt::Key_Minus, Qt::ControlModifier);
        QCOMPARE(backend.textScale(), 0.9);
        QTest::keyClick(window, Qt::Key_0, Qt::ControlModifier);
        QCOMPARE(backend.textScale(), 1.0);
    }

    void keepsTextColumnInsideNarrowWindows() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QObject *viewport = editor->parent();
        QVERIFY(viewport);

        // A resize reaches the window through its platform window, so the
        // bindings only see the new width once the events are delivered.
        const int minimumWidth = window->property("minimumWidth").toInt();
        window->setProperty("width", minimumWidth);
        QCoreApplication::processEvents();
        QCOMPARE(window->property("width").toInt(), minimumWidth);
        QVERIFY(editor->property("x").toInt() > 0);

        // A tiling compositor resizes below the minimum the window asks for.
        window->setProperty("width", 394);
        QCoreApplication::processEvents();
        QCOMPARE(window->property("width").toInt(), 394);
        QVERIFY(editor->property("width").toInt()
                <= viewport->property("width").toInt());
        QVERIFY(editor->property("x").toInt() >= 0);
    }

    void shortcutsDialogHasNoBindingLoop() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        // Material's Dialog derives implicitWidth from its content item; a bare
        // Label assigned as contentItem feeds back into that binding and QML
        // reports a binding loop for the dialog on every launch.
        BindingLoopCounter bindingLoops;

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *dialog = window->findChild<QObject *>(QStringLiteral("shortcutsDialog"));
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(dialog->property("implicitWidth").toReal() > 0, true);

        QCOMPARE(bindingLoops.count(), 0);
    }

    void shortcutReferenceFitsTheWindow_data() {
        QTest::addColumn<int>("windowWidth");
        QTest::addColumn<qreal>("scale");
        QTest::newRow("standard") << 1280 << qreal(1);
        QTest::newRow("narrow") << 394 << qreal(1);
        QTest::newRow("small-text") << 720 << qreal(0.5);
        QTest::newRow("large-text") << 720 << qreal(3);
        QTest::newRow("narrow-large-text") << 394 << qreal(3);
    }

    void shortcutReferenceFitsTheWindow() {
        QFETCH(int, windowWidth);
        QFETCH(qreal, scale);
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        Backend backend;
        backend.setDesktopTextScale(scale);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->setMinimumWidth(0);
        window->setWidth(windowWidth);
        window->setHeight(520);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QTRY_VERIFY(window->isActive());
        QObject *dialog = window->findChild<QObject *>(QStringLiteral("shortcutsDialog"));
        QVERIFY(dialog);

        // All aliases open the dialog, and Escape closes it without changing text size.
        for (auto key : {Qt::Key_Slash, Qt::Key_Question, Qt::Key_F1}) {
            QTest::keyClick(window, key, key == Qt::Key_F1 ? Qt::NoModifier : Qt::ControlModifier);
            QTRY_VERIFY(dialog->property("opened").toBool());
            QObject *versionLabel = dialog->findChild<QObject *>(QStringLiteral("versionLabel"));
            QVERIFY(versionLabel);
            QVERIFY(versionLabel->property("visible").toBool());
            QCOMPARE(versionLabel->property("text").toString(),
                     QStringLiteral("Omawrite ") + QCoreApplication::applicationVersion());
            QTest::keyClick(window, Qt::Key_Minus, Qt::ControlModifier);
            QCOMPARE(backend.textScale(), scale);
            QVERIFY(dialog->property("x").toReal() >= 0);
            QVERIFY(dialog->property("y").toReal() >= 0);
            QVERIFY(dialog->property("x").toReal() + dialog->property("width").toReal()
                    <= window->width());
            QVERIFY(dialog->property("y").toReal() + dialog->property("height").toReal()
                    <= window->height());
            auto *grid = dialog->findChild<QQuickItem *>(QStringLiteral("shortcutsGrid"));
            QVERIFY(grid);
            QCoreApplication::processEvents();
            for (auto *row : grid->childItems()) {
                if (!row->inherits("QQuickRowLayout")) continue;
                QVERIFY(row->x() + row->width() <= grid->width() + 1);
                for (auto *label : row->childItems())
                    QVERIFY(label->x() + label->width() <= row->width() + 1);
            }
            QTest::keyClick(window, Qt::Key_Escape);
            QTRY_VERIFY(!dialog->property("visible").toBool());
        }
    }

    void remembersLastSaveDirectory() {
        QTemporaryDir saveDirectory;
        QVERIFY(saveDirectory.isValid());

        const QString savedPath = saveDirectory.filePath(QStringLiteral("first.md"));
        Backend savedDocument;
        savedDocument.saveAs(QUrl::fromLocalFile(savedPath));

        Backend nextDocument;
        QSignalSpy saveDialogSpy(&nextDocument, &Backend::saveDialogRequested);
        nextDocument.saveAsDialog();
        QCOMPARE(saveDialogSpy.count(), 1);

        const QUrl suggestedUrl = saveDialogSpy.takeFirst().constFirst().toUrl();
        QCOMPARE(QFileInfo(suggestedUrl.toLocalFile()).absolutePath(),
                 saveDirectory.path());
        QCOMPARE(QFileInfo(suggestedUrl.toLocalFile()).fileName(),
                 QStringLiteral("Untitled.md"));

        QSettings().setValue(QStringLiteral("file/lastSaveDirectory"),
                             saveDirectory.filePath(QStringLiteral("missing")));
        Backend fallbackDocument;
        QSignalSpy fallbackDialogSpy(&fallbackDocument, &Backend::saveDialogRequested);
        fallbackDocument.saveAsDialog();
        const QUrl fallbackUrl = fallbackDialogSpy.takeFirst().constFirst().toUrl();
        QCOMPARE(QFileInfo(fallbackUrl.toLocalFile()).absolutePath(), QDir::homePath());
    }

    void autoSavesTitledDocumentsAfterEdits() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("draft.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);

        // Give the document a home on disk, then edit it without saving again.
        backend.saveAs(QUrl::fromLocalFile(path));
        QSignalSpy savedSpy(&backend, &Backend::saveSucceeded);
        editor->setProperty("text", QStringLiteral("auto-saved body"));
        QVERIFY(backend.editorTextChanged());

        // The debounced timer should flush to disk with no manual save.
        QTRY_VERIFY(savedSpy.count() >= 1);
        QVERIFY(!backend.modified());

        QFile written(path);
        QVERIFY(written.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(written.readAll()),
                 QStringLiteral("auto-saved body"));
    }

    void doesNotAutoSaveUntitledDocuments() {
        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);

        QSignalSpy savedSpy(&backend, &Backend::saveSucceeded);
        QSignalSpy saveDialogSpy(&backend, &Backend::saveDialogRequested);
        editor->setProperty("text", QStringLiteral("nameless draft"));
        QVERIFY(backend.editorTextChanged());

        // Wait past the auto-save debounce: an untitled draft must never auto-save
        // or silently raise the save picker. Crash recovery still covers it.
        QTest::qWait(1800);
        QCOMPARE(savedSpy.count(), 0);
        QCOMPARE(saveDialogSpy.count(), 0);
        QVERIFY(backend.modified());
        // Leave no draft behind for the next test's backend to recover.
        backend.discardRecovery();
    }

    void holdsAutoSaveWhenTheFileChangesElsewhere() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("shared.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.saveAs(QUrl::fromLocalFile(path));
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        // Another program writes while our auto-save is pending.
        editor->setProperty("text", QStringLiteral("mine"));
        QVERIFY(backend.editorTextChanged());
        writeFile(path, "theirs");

        // Their version survives the debounce, and further typing.
        QTRY_VERIFY(externalChangeSpy.count() >= 1);
        editor->setProperty("text", QStringLiteral("mine, still typing"));
        QVERIFY(backend.editorTextChanged());
        QTest::qWait(1200);
        QCOMPARE(readFile(path), QByteArray("theirs"));
        QVERIFY(backend.modified());

        // Keeping our version is a choice to overwrite, so auto-save resumes.
        backend.keepExternalVersion();
        QTRY_COMPARE(readFile(path), QByteArray("mine, still typing"));
        QVERIFY(!backend.modified());
    }

    void checksTheFileBeforeEachAutoSave() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("unwatched.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.saveAs(QUrl::fromLocalFile(path));
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        // Change the file without the watcher seeing it, then let auto-save run.
        writeFileUnwatched(backend, path, "theirs");
        editor->setProperty("text", QStringLiteral("mine"));
        QVERIFY(backend.editorTextChanged());

        QTRY_VERIFY(externalChangeSpy.count() >= 1);
        QTest::qWait(700);
        QCOMPARE(readFile(path), QByteArray("theirs"));
        QVERIFY(backend.modified());
        backend.discardRecovery();
    }

    void resumesAutoSaveOnlyAfterAnExplicitSave() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("dismissed.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.saveAs(QUrl::fromLocalFile(path));
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        writeFile(path, "theirs");
        QTRY_VERIFY(externalChangeSpy.count() >= 1);

        // The prompt is dismissed (Escape): neither reload nor keep is chosen.
        editor->setProperty("text", QStringLiteral("mine"));
        QVERIFY(backend.editorTextChanged());
        QTest::qWait(1200);
        QCOMPARE(readFile(path), QByteArray("theirs"));

        backend.save();
        QCOMPARE(readFile(path), QByteArray("mine"));

        editor->setProperty("text", QStringLiteral("mine, auto-saved again"));
        QVERIFY(backend.editorTextChanged());
        QTRY_COMPARE(readFile(path), QByteArray("mine, auto-saved again"));
    }

    void resumesAutoSaveAfterReloading() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("reloaded.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.saveAs(QUrl::fromLocalFile(path));
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        writeFile(path, "theirs");
        QTRY_VERIFY(externalChangeSpy.count() >= 1);
        backend.reloadFromDisk();
        QCOMPARE(editor->property("text").toString(), QStringLiteral("theirs"));

        editor->setProperty("text", QStringLiteral("theirs, edited"));
        QVERIFY(backend.editorTextChanged());
        QTRY_COMPARE(readFile(path), QByteArray("theirs, edited"));
    }

    // Saving writes UTF-8 with plain line endings, so a file holding anything
    // else is only rewritten by an explicit save, never by typing.
    void keepsOtherEncodingsAndLineEndingsOutOfAutoSave() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QList<QByteArray> originals{QByteArray("one\r\ntwo\r\n"),
                                          QByteArray("caf\xe9\n")};
        for (const QByteArray &original : originals) {
            const QString path = directory.filePath(QStringLiteral("kept.md"));
            writeFile(path, original);

            Backend backend;
            QScopedPointer<QObject> editor(makeEditor(backend));
            QVERIFY(editor);
            backend.open(QUrl::fromLocalFile(path));
            QVERIFY(backend.status().contains(QStringLiteral("Auto-save is off")));

            editor->setProperty("text", QStringLiteral("changed\n"));
            QVERIFY(backend.editorTextChanged());
            QVERIFY(!backend.m_autoSaveTimer.isActive());
            QTest::qWait(700);
            QCOMPARE(readFile(path), original);

            // Keeping our version over an outside edit does not lift it either.
            backend.keepExternalVersion();
            QVERIFY(!backend.m_autoSaveTimer.isActive());

            // An explicit save still writes.
            backend.save();
            QCOMPARE(readFile(path), QByteArray("changed\n"));
        }
    }

    void holdsAutoSaveWhileTheWriterDecides() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("deciding.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.saveAs(QUrl::fromLocalFile(path));
        QSignalSpy savedSpy(&backend, &Backend::saveSucceeded);

        // Closing right after typing opens the unsaved-changes prompt.
        editor->setProperty("text", QStringLiteral("undecided"));
        QVERIFY(backend.editorTextChanged());
        backend.holdAutoSave(true);
        QTest::qWait(1200);
        QCOMPARE(savedSpy.count(), 0);
        QCOMPARE(readFile(path), QByteArray());

        // Cancel lets auto-save carry on.
        backend.holdAutoSave(false);
        QTRY_COMPARE(readFile(path), QByteArray("undecided"));
    }

    void startsANamedFileThatIsNotThereYet() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("new.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        QSignalSpy externalSpy(&backend, &Backend::externalChangeDetected);

        backend.openOrStartFile(QUrl::fromLocalFile(path));
        QCOMPARE(backend.fileName(), QStringLiteral("new.md"));
        QCOMPARE(backend.status(), QStringLiteral("New file new.md"));
        QVERIFY(!backend.modified());
        QVERIFY(!QFileInfo::exists(path));

        // Typing does not auto-save the file into existence.
        editor->setProperty("text", QStringLiteral("first words"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());
        QTest::qWait(800);
        QVERIFY(!QFileInfo::exists(path));

        // The first save lands on the name given, without asking for one.
        QSignalSpy saveAsSpy(&backend, &Backend::saveDialogRequested);
        backend.save();
        QCOMPARE(saveAsSpy.count(), 0);
        QCOMPARE(readFile(path), QByteArray("first words"));
        QVERIFY(!backend.modified());

        // From then on it is an ordinary file, auto-saved like any other.
        editor->setProperty("text", QStringLiteral("more words"));
        QVERIFY(backend.editorTextChanged());
        QTRY_COMPARE(readFile(path), QByteArray("more words"));
        QCOMPARE(externalSpy.count(), 0);
    }

    void refusesANewFileInAMissingFolder() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("missing/new.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.openOrStartFile(QUrl::fromLocalFile(path));
        QCOMPARE(backend.status(), QStringLiteral("Could not open new.md."));
        QVERIFY(backend.fileName() != QStringLiteral("new.md"));
    }

    void namesARelativeFileByItsFullPath() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString previous = QDir::currentPath();
        QVERIFY(QDir::setCurrent(directory.path()));

        Backend backend;
        backend.openOrStartFile(QUrl::fromLocalFile(QStringLiteral("relative.md")));
        QDir::setCurrent(previous);

        // A recovery draft written later must not depend on the directory.
        QCOMPARE(backend.fileUrl().toLocalFile(),
                 QDir(directory.path()).absoluteFilePath(QStringLiteral("relative.md")));
    }

    void saysWhenARecoveredDraftKeepsAFileFromOpening() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("asked.md"));
        writeFile(path, "on disk");

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        // As after restoring a draft: the window holds unsaved text.
        editor->setProperty("text", QStringLiteral("recovered"));
        QVERIFY(backend.editorTextChanged());

        backend.openOrStartFile(QUrl::fromLocalFile(path));
        QCOMPARE(backend.status(),
                 QStringLiteral("Recovered unsaved changes; asked.md was not opened."));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("recovered"));
        backend.discardRecovery();
    }

    void dropsAPendingCloseWhenItsSaveFails() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        // The writer chose Save in the close prompt, and the save failed.
        window->setProperty("pendingAction", QStringLiteral("close"));
        window->setProperty("awaitingPendingSave", true);
        QSignalSpy failedSpy(&backend, &Backend::saveFailed);
        backend.saveAs(QUrl::fromLocalFile(QStringLiteral("/nonexistent-dir/x.md")));
        QCOMPARE(failedSpy.count(), 1);

        // A later save, such as an auto-save, must not close the window.
        QCOMPARE(window->property("pendingAction").toString(), QString());
        QVERIFY(!window->property("awaitingPendingSave").toBool());
    }

    void savesInAFolderItCannotWriteNewFilesIn() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("shared.md"));
        writeFile(path, "before");
        QFile::setPermissions(directory.path(), QFile::ReadOwner | QFile::ExeOwner);

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.open(QUrl::fromLocalFile(path));
        editor->setProperty("text", QStringLiteral("after"));
        QVERIFY(backend.editorTextChanged());
        backend.save();
        QFile::setPermissions(directory.path(),
                              QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

        QVERIFY(!backend.modified());
        QCOMPARE(readFile(path), QByteArray("after"));
    }

    void removesARecoveryDraftItCannotRead() {
        Backend backend;
        const QString draft = backend.m_recoveryPath;
        QVERIFY(!draft.isEmpty());
        writeFile(draft, "not json");

        // Attaching the document is when a draft is restored.
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        QVERIFY(!QFileInfo::exists(draft));
        QVERIFY(!backend.modified());
    }

    void keepsWatchingAFileReplacedFromOutside() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("watched.md"));
        writeFile(path, "first");

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.open(QUrl::fromLocalFile(path));
        QSignalSpy externalSpy(&backend, &Backend::externalChangeDetected);

        // Editors and sync tools often replace a file rather than write into it.
        const QString replacement = directory.filePath(QStringLiteral("replacement"));
        writeFile(replacement, "second");
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::rename(replacement, path));
        QTRY_VERIFY(externalSpy.count() >= 1);
        QTRY_VERIFY(backend.m_fileWatcher.files().contains(path));
    }

    void keepsTheWritersTextWhenTheChangeDialogIsDismissed() {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport QtQuick.Controls\nApplicationWindow {"
                          " property alias dialog: dialog\n"
                          " ExternalChangeDialog { id: dialog } }",
                          QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY(window);
        QObject *dialog = window->property("dialog").value<QObject *>();
        QVERIFY(dialog);
        QSignalSpy keepSpy(dialog, SIGNAL(keepRequested()));
        QSignalSpy reloadSpy(dialog, SIGNAL(reloadRequested()));

        // Escape closes it without a choice.
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_COMPARE(keepSpy.count(), 1);
        QCOMPARE(reloadSpy.count(), 0);
    }

    void opensAnExistingFileFromTheCommandLine() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("notes.md"));
        writeFile(path, "already here");

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.openOrStartFile(QUrl::fromLocalFile(path));
        QCOMPARE(editor->property("text").toString(), QStringLiteral("already here"));
        QCOMPARE(backend.status(), QStringLiteral("Opened notes.md"));
    }

    void keepsDiscardedChangesOutOfTheFile() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("draft.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        editor->setProperty("text", QStringLiteral("original"));
        QVERIFY(backend.editorTextChanged());
        backend.saveAs(QUrl::fromLocalFile(path));

        // The writer edits, then picks Discard to open a file that fails.
        backend.holdAutoSave(true);
        editor->setProperty("text", QStringLiteral("changes to throw away"));
        QVERIFY(backend.editorTextChanged());
        backend.discardRecovery();
        backend.open(QUrl::fromLocalFile(directory.filePath(QStringLiteral("missing.md"))));
        backend.holdAutoSave(false);

        QTest::qWait(800);
        QCOMPARE(readFile(path), QByteArray("original"));
    }

    void forgetsTheSavedTextWhenTheFileChangedUnseen() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("draft.md"));

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        editor->setProperty("text", QStringLiteral("original"));
        QVERIFY(backend.editorTextChanged());
        backend.saveAs(QUrl::fromLocalFile(path));

        // The file changes without the watcher noticing; the pre-save check
        // catches it.
        QSignalSpy externalSpy(&backend, &Backend::externalChangeDetected);
        writeFileUnwatched(backend, path, "theirs");
        editor->setProperty("text", QStringLiteral("mine"));
        QVERIFY(backend.editorTextChanged());
        QTRY_VERIFY(externalSpy.count() >= 1);

        // Undoing back to the old text is not a return to what is on disk.
        editor->setProperty("text", QStringLiteral("original"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());
        backend.discardRecovery();
    }

    void doesNotAutoSaveARecoveredDraftOverItsFile() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("recovered.md"));
        const QUrl url = QUrl::fromLocalFile(path);

        {
            // A session that ends without saving leaves a recovery draft.
            Backend crashed;
            QScopedPointer<QObject> editor(makeEditor(crashed));
            QVERIFY(editor);
            crashed.saveAs(url);
            editor->setProperty("text", QStringLiteral("draft"));
            crashed.m_autoSaveSuspended = true; // stand in for a crash before auto-save
            QVERIFY(crashed.editorTextChanged());
            QTest::qWait(1000);
        }
        QCOMPARE(readFile(path), QByteArray());

        // The file moves on before Omawrite comes back.
        writeFile(path, "newer on disk");

        // Earlier tests may have left drafts of their own; claim until ours.
        for (int attempt = 0; attempt < 100; ++attempt) {
            Backend restored;
            QScopedPointer<QObject> editor(makeEditor(restored));
            QVERIFY(editor);
            if (restored.fileUrl() != url) {
                restored.discardRecovery();
                continue;
            }

            QCOMPARE(editor->property("text").toString(), QStringLiteral("draft"));
            editor->setProperty("text", QStringLiteral("draft, edited"));
            QVERIFY(restored.editorTextChanged());
            QTest::qWait(1200);
            QCOMPARE(readFile(path), QByteArray("newer on disk"));
            QVERIFY(restored.modified());

            restored.save();
            QCOMPARE(readFile(path), QByteArray("draft, edited"));
            return;
        }
        QFAIL("The recovery draft was not restored.");
    }

    void revertsEmptyDocumentToCleanState() {
        Backend backend;
        QQuickTextDocument *quickDocument = attachTextEdit(&backend);
        QVERIFY(quickDocument);

        QVERIFY(!backend.modified());

        quickDocument->textDocument()->setPlainText(QStringLiteral("hello"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());

        quickDocument->textDocument()->setPlainText(QString());
        QVERIFY(backend.editorTextChanged());
        QVERIFY(!backend.modified());
    }

    void revertsSavedDocumentToCleanState() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("doc.md"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QCOMPARE(file.write("hello"), qint64(5));
        file.close();

        Backend backend;
        QQuickTextDocument *quickDocument = attachTextEdit(&backend);
        QVERIFY(quickDocument);

        backend.open(QUrl::fromLocalFile(path));
        QVERIFY(!backend.modified());

        quickDocument->textDocument()->setPlainText(QStringLiteral("hello world"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());

        quickDocument->textDocument()->setPlainText(QStringLiteral("hello"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(!backend.modified());
    }

    void keepsADocumentUnsavedAfterItsFileDisappears() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("gone.md"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QCOMPARE(file.write("hello"), qint64(5));
        file.close();

        Backend backend;
        QQuickTextDocument *quickDocument = attachTextEdit(&backend);
        QVERIFY(quickDocument);

        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);
        backend.open(QUrl::fromLocalFile(path));
        QVERIFY(!backend.modified());

        QVERIFY(QFile::remove(path));
        QTRY_COMPARE(externalChangeSpy.count(), 1);

        // The writer can dismiss the dialog without picking a version, so the
        // text the deleted file used to hold is not a saved state to return to.
        quickDocument->textDocument()->setPlainText(QStringLiteral("hello world"));
        QVERIFY(backend.editorTextChanged());
        quickDocument->textDocument()->setPlainText(QStringLiteral("hello"));
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());

        // Nor is emptying it: an empty document only reads as clean while there
        // is no file behind it.
        quickDocument->textDocument()->setPlainText(QString());
        QVERIFY(backend.editorTextChanged());
        QVERIFY(backend.modified());
    }

    void treatsAFileRemovedAndWrittenAgainAsChanged() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("replaced.md"));
        writeFile(path, "hello");

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.open(QUrl::fromLocalFile(path));
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        // The watcher sees the file gone before it is written again.
        QVERIFY(QFile::remove(path));
        QTest::qWait(30);
        writeFile(path, "theirs");
        QTRY_COMPARE(externalChangeSpy.count(), 1);
        QCOMPARE(externalChangeSpy.first().at(0).toBool(), false);

        // The new file is watched, so later edits are noticed too.
        writeFile(path, "theirs, again");
        QTRY_COMPARE(externalChangeSpy.count(), 2);
        backend.reloadFromDisk();
        QCOMPARE(editor->property("text").toString(), QStringLiteral("theirs, again"));
    }

    void noticesARemovedFileComingBack() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("back.md"));
        writeFile(path, "hello");

        Backend backend;
        QScopedPointer<QObject> editor(makeEditor(backend));
        QVERIFY(editor);
        backend.open(QUrl::fromLocalFile(path));
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        QVERIFY(QFile::remove(path));
        QTRY_COMPARE(externalChangeSpy.count(), 1);
        QCOMPARE(externalChangeSpy.last().at(0).toBool(), true);

        writeFile(path, "theirs");
        QTRY_COMPARE(externalChangeSpy.count(), 2);
        QCOMPARE(externalChangeSpy.last().at(0).toBool(), false);

        // And removed again before the writer answers.
        QVERIFY(QFile::remove(path));
        QTRY_COMPARE(externalChangeSpy.count(), 3);
        QCOMPARE(externalChangeSpy.last().at(0).toBool(), true);
    }

    void hidesReloadWhileTheFileIsRemoved() {
        const QString dialogPath = QFINDTESTDATA("../src/ExternalChangeDialog.qml");
        QVERIFY(!dialogPath.isEmpty());
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\n"
                          "Window {\n"
                          "    width: 600; height: 400; visible: true\n"
                          "    property alias dialog: dialog\n"
                          "    ExternalChangeDialog { id: dialog; deleted: true }\n"
                          "}\n",
                          QUrl::fromLocalFile(QFileInfo(dialogPath).dir().filePath(
                              QStringLiteral("Harness.qml"))));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject *dialog = window->property("dialog").value<QObject *>();
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QObject *reload = dialog->findChild<QObject *>(QStringLiteral("reloadButton"));
        QVERIFY(reload);

        QVERIFY(!reload->property("visible").toBool());
        dialog->setProperty("deleted", false);
        QVERIFY(reload->property("visible").toBool());

        // Removed while Reload has the focus: Enter keeps the text instead.
        QVERIFY(QMetaObject::invokeMethod(reload, "forceActiveFocus"));
        QTRY_VERIFY(reload->property("activeFocus").toBool());
        dialog->setProperty("deleted", true);
        QObject *keep = dialog->findChild<QObject *>(QStringLiteral("keepButton"));
        QVERIFY(keep);
        QTRY_VERIFY(keep->property("activeFocus").toBool());
    }

    void readsUpsAndTagsFromFrontmatter() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral(".obsidian")));
        QVERIFY(root.mkpath(QStringLiteral("drawings")));
        const auto write = [&root](const QString &name, const QByteArray &body) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write(body);
            return true;
        };
        QVERIFY(write(QStringLiteral("Branding.md"), "---\ntags:\n  - atomic\n---\n# B\n"));
        QVERIFY(write(QStringLiteral("Onboarding.md"),
                      "---\nup:\n  - \"[[Branding]]\"\ntags:\n  - fleeting\n---\n# O\n"));
        QVERIFY(write(QStringLiteral("Single.md"), "---\nup: \"[[Branding]]\"\n---\n# S\n"));
        QVERIFY(write(QStringLiteral("Weird.md"), "---\nup:\n  type: choice\n---\n# W\n"));
        QVERIFY(write(QStringLiteral("Plain.md"), "# no frontmatter\n"));
        QVERIFY(write(QStringLiteral("Tagged.md"), "---\ntags: [atomic, fleeting]\n---\n# T\n"));

        Backend backend;
        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("Branding.md"))));
        backend.refreshNoteIndex();

        const auto meta = [&backend](const QString &path) {
            return backend.noteFrontmatter(path);
        };
        QCOMPARE(meta(QStringLiteral("Branding.md")).value(QStringLiteral("tags")).toStringList(),
                 (QStringList{QStringLiteral("atomic")}));
        QVERIFY(meta(QStringLiteral("Branding.md")).value(QStringLiteral("ups")).toStringList().isEmpty());
        QCOMPARE(meta(QStringLiteral("Onboarding.md")).value(QStringLiteral("ups")).toStringList(),
                 (QStringList{QStringLiteral("Branding")}));
        QCOMPARE(meta(QStringLiteral("Onboarding.md")).value(QStringLiteral("tags")).toStringList(),
                 (QStringList{QStringLiteral("fleeting")}));
        QCOMPARE(meta(QStringLiteral("Single.md")).value(QStringLiteral("ups")).toStringList(),
                 (QStringList{QStringLiteral("Branding")}));
        // An object under up: is not a parent list.
        QVERIFY(meta(QStringLiteral("Weird.md")).value(QStringLiteral("ups")).toStringList().isEmpty());
        QVERIFY(meta(QStringLiteral("Plain.md")).value(QStringLiteral("tags")).toStringList().isEmpty());
        QCOMPARE(meta(QStringLiteral("Tagged.md")).value(QStringLiteral("tags")).toStringList(),
                 (QStringList{QStringLiteral("atomic"), QStringLiteral("fleeting")}));
    }

    void buildsAVirtFolderTreeFromUps() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral(".obsidian")));
        QVERIFY(root.mkpath(QStringLiteral("drawings")));
        const auto write = [&root](const QString &name, const QByteArray &body) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write(body);
            return true;
        };
        QVERIFY(write(QStringLiteral("Branding.md"), "---\ntags:\n  - atomic\n---\n# B\n"));
        QVERIFY(write(QStringLiteral("Zebra.md"),
                      "---\nup:\n  - \"[[Branding]]\"\n---\n# Z\n"));
        QVERIFY(write(QStringLiteral("Apple.md"),
                      "---\nup:\n  - \"[[Branding]]\"\n---\n# A\n"));
        QVERIFY(write(QStringLiteral("Orphan.md"), "# O\n"));
        QVERIFY(write(QStringLiteral("drawings/Sketch.md"),
                      "---\nup:\n  - \"[[Branding]]\"\n---\n# S\n"));

        Backend backend;
        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("Branding.md"))));
        const QVariantList tree = backend.virtFolderTree();

        // Roots, sorted by name: Branding (with children) then Orphan. The
        // drawings note is generated output, not a knowledge note.
        QCOMPARE(tree.size(), 2);
        const QVariantMap branding = tree.at(0).toMap();
        QCOMPARE(branding.value(QStringLiteral("name")).toString(), QStringLiteral("Branding"));
        QCOMPARE(branding.value(QStringLiteral("childCount")).toInt(), 2);
        const QVariantList children = branding.value(QStringLiteral("children")).toList();
        QCOMPARE(children.size(), 2);
        QCOMPARE(children.at(0).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Apple"));
        QCOMPARE(children.at(1).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Zebra"));
        QCOMPARE(tree.at(1).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Orphan"));
    }

    void virtTreeSurvivesALinkCycle() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QVERIFY(root.mkpath(QStringLiteral(".obsidian")));
        const auto write = [&root](const QString &name, const QByteArray &body) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write(body);
            return true;
        };
        QVERIFY(write(QStringLiteral("A.md"), "---\nup:\n  - \"[[B]]\"\n---\n# A\n"));
        QVERIFY(write(QStringLiteral("B.md"), "---\nup:\n  - \"[[A]]\"\n---\n# B\n"));

        Backend backend;
        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("A.md"))));
        const QVariantList tree = backend.virtFolderTree();
        // A nests B under itself; B's back-link to A is cut instead of
        // recursing, so every note still shows up exactly once.
        QCOMPARE(tree.size(), 1);
        int total = 0;
        const std::function<void(const QVariantList &)> count = [&](const QVariantList &rows) {
            for (const QVariant &row : rows) {
                ++total;
                count(row.toMap().value(QStringLiteral("children")).toList());
            }
        };
        count(tree);
        QCOMPARE(total, 2);
    }

    void hidesFrontmatterFromTheEditorButSavesIt() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        const auto write = [&root](const QString &name, const QByteArray &body) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write(body);
            return true;
        };
        QVERIFY(write(QStringLiteral("Branding.md"),
                      "---\ntags:\n  - atomic\n---\n# Branding\n\nBody\n"));

        Backend backend;
        QQuickTextDocument *document = attachTextEdit(&backend);
        QVERIFY(document);
        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("Branding.md"))));
        // The editor holds the body only, so the properties panel can take
        // the YAML's place without the document ever losing it.
        QCOMPARE(document->textDocument()->toPlainText(),
                 QStringLiteral("# Branding\n\nBody\n"));
        QVERIFY(backend.hasProperties());
        const QVariantList rows = backend.documentProperties();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.constFirst().toMap().value(QStringLiteral("key")).toString(),
                 QStringLiteral("tags"));
        QCOMPARE(rows.constFirst().toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("tags"));
        QCOMPARE(rows.constFirst().toMap().value(QStringLiteral("values")).toStringList(),
                 (QStringList{QStringLiteral("atomic")}));

        QVERIFY(backend.addTag(QStringLiteral("fleeting")));
        QVERIFY(backend.removeTag(QStringLiteral("atomic")));
        backend.save();

        const QByteArray saved = readFile(root.filePath(QStringLiteral("Branding.md")));
        QVERIFY(saved.startsWith("---\ntags:\n"));
        QVERIFY(saved.contains("  - fleeting"));
        QVERIFY(!saved.contains("atomic"));
        QVERIFY(saved.endsWith("# Branding\n\nBody\n"));
    }

    void editsPropertiesWithoutLosingOtherFrontmatter() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        const auto write = [&root](const QString &name, const QByteArray &body) {
            QFile file(root.filePath(name));
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.write(body);
            return true;
        };
        QVERIFY(write(QStringLiteral("Note.md"),
                      "---\n# keep this comment\ntags:\n  - atomic\nrating: 3\n---\n"
                      "# Note\n"));
        QVERIFY(write(QStringLiteral("Plain.md"), "# Plain\n"));

        Backend backend;
        QQuickTextDocument *document = attachTextEdit(&backend);
        QVERIFY(document);
        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("Note.md"))));
        QVERIFY(backend.addTag(QStringLiteral("fleeting")));
        backend.save();
        QByteArray saved = readFile(root.filePath(QStringLiteral("Note.md")));
        QVERIFY(saved.contains("# keep this comment"));
        QVERIFY(saved.contains("rating: 3"));
        QVERIFY(saved.contains("  - fleeting"));

        // A note with no frontmatter grows one from + Add property, and the
        // body is untouched.
        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("Plain.md"))));
        QVERIFY(!backend.hasProperties());
        QVERIFY(backend.addProperty(QStringLiteral("status"), QStringLiteral("draft")));
        QVERIFY(backend.hasProperties());
        QCOMPARE(backend.currentDocumentText(), QStringLiteral("# Plain\n"));
        QVERIFY(backend.setPropertyValue(QStringLiteral("status"), QStringLiteral("final")));
        backend.save();
        saved = readFile(root.filePath(QStringLiteral("Plain.md")));
        QCOMPARE(saved, QByteArray("---\nstatus: final\n---\n# Plain\n"));

        QVERIFY(backend.addProperty(QStringLiteral("up"), QStringLiteral("Parent, Other")));
        const QVariantList rows = backend.documentProperties();
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.at(1).toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("links"));
        QCOMPARE(rows.at(1).toMap().value(QStringLiteral("values")).toStringList(),
                 (QStringList{QStringLiteral("Parent"), QStringLiteral("Other")}));
        QVERIFY(backend.removeProperty(QStringLiteral("up")));
        QVERIFY(backend.removeProperty(QStringLiteral("status")));
        QVERIFY(!backend.hasProperties());
    }

    void showsPropertiesInlineAboveTheText() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        QFile file(root.filePath(QStringLiteral("Note.md")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("---\ntags:\n  - atomic\n---\n# Note\n");
        file.close();

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        backend.open(QUrl::fromLocalFile(root.filePath(QStringLiteral("Note.md"))));

        QObject *panel = window->findChild<QObject *>(QStringLiteral("propertiesPanel"));
        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(panel);
        QVERIFY(editor);
        QTRY_VERIFY(panel->property("visible").toBool());
        QVERIFY(panel->property("height").toReal() > 0);
        QCOMPARE(editor->property("text").toString(), QStringLiteral("# Note\n"));
        // The text starts below the properties, not underneath them.
        QVERIFY(editor->property("y").toReal()
                >= panel->property("y").toReal() + panel->property("height").toReal());
    }

    void showsPropertiesAndKeepsYamlOutOfTheEditor() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        const QString notePath = root.filePath(QStringLiteral("Tagged.md"));
        {
            QFile file(notePath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("---\ntags:\n  - atomic\nup:\n  - \"[[Branding]]\"\n---\nBody text\n");
        }

        Backend backend;
        QQuickTextDocument *document = attachTextEdit(&backend);
        QVERIFY(document);
        backend.open(QUrl::fromLocalFile(notePath));

        // The YAML is the panel's model, never part of the writing.
        QCOMPARE(document->textDocument()->toPlainText(), QStringLiteral("Body text\n"));
        QVERIFY(backend.hasProperties());
        const QVariantList rows = backend.documentProperties();
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.at(0).toMap().value(QStringLiteral("key")).toString(),
                 QStringLiteral("tags"));
        QCOMPARE(rows.at(0).toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("tags"));
        QCOMPARE(rows.at(0).toMap().value(QStringLiteral("values")).toStringList(),
                 (QStringList{QStringLiteral("atomic")}));
        QCOMPARE(rows.at(1).toMap().value(QStringLiteral("key")).toString(),
                 QStringLiteral("up"));
        QCOMPARE(rows.at(1).toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("links"));
        QCOMPARE(rows.at(1).toMap().value(QStringLiteral("values")).toStringList(),
                 (QStringList{QStringLiteral("Branding")}));

        // A tag edit rewrites only the tags key and re-saves the whole note.
        QVERIFY(backend.addTag(QStringLiteral("fleeting")));
        QVERIFY(backend.removeTag(QStringLiteral("atomic")));
        QVERIFY(backend.modified());
        QVERIFY(backend.setPropertyValue(QStringLiteral("status"), QStringLiteral("draft")));
        backend.save();

        QFile saved(notePath);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(saved.readAll());
        QVERIFY(text.startsWith(QStringLiteral("---\n")));
        QVERIFY(text.contains(QStringLiteral("  - fleeting")));
        QVERIFY(!text.contains(QStringLiteral("  - atomic")));
        QVERIFY(text.contains(QStringLiteral("status: draft")));
        QVERIFY(text.contains(QStringLiteral("  - \"[[Branding]]\"")));
        QVERIFY(text.endsWith(QStringLiteral("---\nBody text\n")));
        QCOMPARE(document->textDocument()->toPlainText(), QStringLiteral("Body text\n"));
    }

    void addsFrontmatterToANoteThatHadNone() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        const QString notePath = root.filePath(QStringLiteral("Plain.md"));
        {
            QFile file(notePath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("# Plain\n");
        }

        Backend backend;
        QQuickTextDocument *document = attachTextEdit(&backend);
        QVERIFY(document);
        backend.open(QUrl::fromLocalFile(notePath));
        QVERIFY(!backend.hasProperties());

        QVERIFY(backend.addTag(QStringLiteral("atomic")));
        QVERIFY(backend.hasProperties());
        QVERIFY(backend.addProperty(QStringLiteral("status"), QStringLiteral("draft")));
        backend.save();

        QFile saved(notePath);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(saved.readAll());
        QVERIFY(text.startsWith(QStringLiteral("---\ntags:\n  - atomic\nstatus: draft\n---\n")));
        QVERIFY(text.endsWith(QStringLiteral("# Plain\n")));
        QCOMPARE(document->textDocument()->toPlainText(), QStringLiteral("# Plain\n"));
    }

    void removesPropertiesAndTheirLastValue() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        const QString notePath = root.filePath(QStringLiteral("Props.md"));
        {
            QFile file(notePath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("---\ntags:\n  - one\n  - two\nstatus: draft\n---\nbody\n");
        }

        Backend backend;
        QQuickTextDocument *document = attachTextEdit(&backend);
        QVERIFY(document);
        backend.open(QUrl::fromLocalFile(notePath));

        QVERIFY(backend.removePropertyValue(QStringLiteral("tags"), QStringLiteral("one")));
        QCOMPARE(backend.documentProperties().at(0).toMap()
                     .value(QStringLiteral("values")).toStringList(),
                 (QStringList{QStringLiteral("two")}));
        QVERIFY(backend.removeTag(QStringLiteral("two")));
        QVERIFY(backend.removeProperty(QStringLiteral("status")));
        QVERIFY(!backend.hasProperties());

        backend.save();
        QFile saved(notePath);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(saved.readAll()), QStringLiteral("body\n"));
    }

    void editsAListPropertyWithoutFlatteningIt() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir root(dir.path());
        const QString notePath = root.filePath(QStringLiteral("List.md"));
        {
            QFile file(notePath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("---\naliases:\n  - One\n  - Two\n---\nbody\n");
        }

        Backend backend;
        QQuickTextDocument *document = attachTextEdit(&backend);
        QVERIFY(document);
        backend.open(QUrl::fromLocalFile(notePath));
        QCOMPARE(backend.documentProperties().at(0).toMap()
                     .value(QStringLiteral("kind")).toString(),
                 QStringLiteral("list"));

        // Editing the row as text keeps the block a YAML list.
        QVERIFY(backend.setPropertyValue(QStringLiteral("aliases"),
                                         QStringLiteral("One, Three")));
        backend.save();

        QFile saved(notePath);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(saved.readAll()),
                 QStringLiteral("---\naliases:\n  - One\n  - Three\n---\nbody\n"));
    }

    void shortcutDefaultsCanBeOverriddenAndReset() {
        Backend backend;
        QCOMPARE(backend.shortcutSequences(QStringLiteral("toggle-sidebar")),
                 (QStringList{QStringLiteral("Ctrl+B")}));
        QVERIFY(!backend.shortcutIds().isEmpty());
        QVERIFY(!backend.shortcutDescription(QStringLiteral("toggle-sidebar")).isEmpty());

        QVERIFY(backend.setShortcut(QStringLiteral("toggle-sidebar"), QStringLiteral("Ctrl+E")));
        QCOMPARE(backend.shortcutSequences(QStringLiteral("toggle-sidebar")),
                 (QStringList{QStringLiteral("Ctrl+E")}));
        // A second Backend in the same settings scope picks the override up.
        Backend other;
        QCOMPARE(other.shortcutSequences(QStringLiteral("toggle-sidebar")),
                 (QStringList{QStringLiteral("Ctrl+E")}));

        QVERIFY(!backend.setShortcut(QStringLiteral("toggle-sidebar"), QString()));
        QVERIFY(!backend.setShortcut(QStringLiteral("no-such-action"), QStringLiteral("Ctrl+E")));
        QCOMPARE(backend.shortcutSequences(QStringLiteral("toggle-sidebar")),
                 (QStringList{QStringLiteral("Ctrl+E")}));

        backend.resetShortcut(QStringLiteral("toggle-sidebar"));
        QCOMPARE(backend.shortcutSequences(QStringLiteral("toggle-sidebar")),
                 (QStringList{QStringLiteral("Ctrl+B")}));
    }

    void actionRegistryCarriesLabelsAndKeys() {
        Backend backend;
        const QVariantList rows = backend.actionRows();
        QVERIFY(rows.size() >= 40);

        QHash<QString, QVariantMap> byId;
        for (const QVariant &value : rows) {
            const QVariantMap row = value.toMap();
            const QString id = row.value(QStringLiteral("id")).toString();
            QVERIFY(!id.isEmpty());
            QVERIFY(!row.value(QStringLiteral("label")).toString().isEmpty());
            byId.insert(id, row);
        }
        const auto keys = [&byId](const QString &id) {
            return byId.value(id).value(QStringLiteral("sequences")).toStringList();
        };

        QCOMPARE(keys(QStringLiteral("command-palette")),
                 (QStringList{QStringLiteral("Ctrl+P")}));
        QCOMPARE(keys(QStringLiteral("open-file")),
                 (QStringList{QStringLiteral("Ctrl+O")}));
        QCOMPARE(keys(QStringLiteral("toggle-sidebar")),
                 (QStringList{QStringLiteral("Ctrl+B")}));
        QCOMPARE(keys(QStringLiteral("bold")),
                 (QStringList{QStringLiteral("Ctrl+Alt+B")}));
        QCOMPARE(keys(QStringLiteral("backlinks")),
                 (QStringList{QStringLiteral("Ctrl+Shift+B")}));
        QCOMPARE(keys(QStringLiteral("print")),
                 (QStringList{QStringLiteral("Ctrl+Shift+P")}));
        QCOMPARE(keys(QStringLiteral("quick-open")),
                 (QStringList{QStringLiteral("Ctrl+Shift+O")}));
        QCOMPARE(keys(QStringLiteral("save")), (QStringList{QStringLiteral("Ctrl+S")}));
        QCOMPARE(keys(QStringLiteral("save-as")),
                 (QStringList{QStringLiteral("Ctrl+Shift+S")}));
        QCOMPARE(keys(QStringLiteral("close-tab")),
                 (QStringList{QStringLiteral("Ctrl+W")}));

        // The palette's commands without keys still have rows, but the
        // settings dialog must not offer them for rebinding.
        QVERIFY(byId.contains(QStringLiteral("settings")));
        QVERIFY(byId.contains(QStringLiteral("font")));
        QVERIFY(keys(QStringLiteral("settings")).isEmpty());
        QVERIFY(keys(QStringLiteral("font")).isEmpty());
        QVERIFY(!byId.value(QStringLiteral("settings"))
                     .value(QStringLiteral("hasKeys")).toBool());

        int keyed = 0;
        for (const QVariant &value : rows) {
            if (value.toMap().value(QStringLiteral("hasKeys")).toBool())
                ++keyed;
        }
        QCOMPARE(backend.shortcutRows().size(), keyed);
        for (const QVariant &value : backend.shortcutRows()) {
            const QVariantMap row = value.toMap();
            QVERIFY(!row.value(QStringLiteral("sequence")).toString().isEmpty());
            QCOMPARE(row.value(QStringLiteral("label")).toString(),
                     backend.shortcutDescription(row.value(QStringLiteral("id")).toString()));
        }

        // Live keys follow an override in the same row shape.
        QVERIFY(backend.setShortcut(QStringLiteral("bold"),
                                    QStringLiteral("Ctrl+Alt+Shift+B")));
        QVariantMap boldRow;
        for (const QVariant &value : backend.actionRows()) {
            if (value.toMap().value(QStringLiteral("id")).toString()
                    == QStringLiteral("bold"))
                boldRow = value.toMap();
        }
        QCOMPARE(boldRow.value(QStringLiteral("sequences")).toStringList(),
                 (QStringList{QStringLiteral("Ctrl+Alt+Shift+B")}));
        QCOMPARE(boldRow.value(QStringLiteral("defaults")).toStringList(),
                 (QStringList{QStringLiteral("Ctrl+Alt+B")}));
        QVERIFY(!boldRow.value(QStringLiteral("isDefault")).toBool());
        backend.resetShortcut(QStringLiteral("bold"));
    }

    void shortcutsReferenceReadsTheActionRegistry() {
        // There is no second key table left in the dialog: the reference is a
        // view of backend.shortcutRows(), the same registry Settings edits.
        const QString dialogQmlPath = QFINDTESTDATA("../src/ShortcutsDialog.qml");
        QVERIFY(!dialogQmlPath.isEmpty());
        QFile source(dialogQmlPath);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QByteArray dialogSource = source.readAll();
        QVERIFY(!dialogSource.contains("fixedShortcuts"));
        QVERIFY(!dialogSource.contains("editorShortcuts"));

        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *dialog = window->findChild<QObject *>(QStringLiteral("shortcutsDialog"));
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());

        const QVariantList live = dialog->property("shortcuts").toList();
        QCOMPARE(live.size(), backend.shortcutRows().size());
        QStringList labels;
        QStringList keys;
        for (const QVariant &value : live) {
            labels.append(value.toMap().value(QStringLiteral("desc")).toString());
            keys.append(value.toMap().value(QStringLiteral("key")).toString());
        }
        QVERIFY(labels.contains(QStringLiteral("Command Palette")));
        QVERIFY(labels.contains(QStringLiteral("Open File")));
        QVERIFY(labels.contains(QStringLiteral("Bold")));
        QVERIFY(keys.contains(QStringLiteral("Ctrl+P")));
        QVERIFY(keys.contains(QStringLiteral("Ctrl+O")));
        QVERIFY(keys.contains(QStringLiteral("Ctrl+Alt+B")));
        QVERIFY(!keys.contains(QString()));
    }

    void openFileKeySignalsTheDialog() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *openShortcut = window->findChild<QObject *>(QStringLiteral("openFileShortcut"));
        QVERIFY(openShortcut);
        QCOMPARE(openShortcut->property("sequence").toString(), QStringLiteral("Ctrl+O"));

        QSignalSpy openDialogSpy(&backend, &Backend::openDialogRequested);
        QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier);
        QTRY_COMPARE(openDialogSpy.count(), 1);
    }

    void printKeyIsShiftedOffThePalette() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QObject *printShortcut = window->findChild<QObject *>(QStringLiteral("printShortcut"));
        QObject *paletteShortcut =
            window->findChild<QObject *>(QStringLiteral("commandPaletteShortcut"));
        QVERIFY(printShortcut);
        QVERIFY(paletteShortcut);
        QCOMPARE(printShortcut->property("sequence").toString(),
                 QStringLiteral("Ctrl+Shift+P"));
        QCOMPARE(paletteShortcut->property("sequence").toString(), QStringLiteral("Ctrl+P"));

        // Ctrl+P opens the palette rather than printing.
        QTest::keyClick(window, Qt::Key_P, Qt::ControlModifier);
        QTRY_VERIFY(window->findChild<QObject *>(QStringLiteral("commandPalette"))
                        ->property("visible").toBool());
    }

    void scrollSpeedDefaultsToDoubleAndPersists() {
        Backend backend;
        const double original = backend.scrollSpeed();
        QCOMPARE(original, 2.0);

        backend.setScrollSpeed(1.5);
        QCOMPARE(backend.scrollSpeed(), 1.5);
        Backend other;
        QCOMPARE(other.scrollSpeed(), 1.5);

        // Out-of-range values clamp instead of sticking.
        backend.setScrollSpeed(99.0);
        QCOMPARE(backend.scrollSpeed(), 4.0);
        backend.setScrollSpeed(0.0);
        QCOMPARE(backend.scrollSpeed(), 0.25);

        backend.setScrollSpeed(original);
        QCOMPARE(Backend().scrollSpeed(), original);
    }

private:
    // The character formats the highlighter gives a single line of text.
    static QList<QTextCharFormat> highlightedFormats(const QString &text, int caret = -1) {
        QTextDocument document;
        document.setPlainText(text);
        MarkdownHighlighter highlighter(&document);
        highlighter.setTheme(true, "#101010", "#eeeeee", "#5584aa", "#23372b");
        highlighter.rehighlight();
        if (caret >= 0)
            highlighter.setCaret(caret);
        QList<QTextCharFormat> formats(text.length());
        for (const auto &range : document.firstBlock().layout()->formats()) {
            for (int i = range.start; i < range.start + range.length; ++i)
                formats[i] = range.format;
        }
        return formats;
    }

    // Points HOME at a scratch tree holding one colors.toml, and puts it back on the way out.
    struct ScopedTheme {
        explicit ScopedTheme(const QByteArray &palette) {
            const QString themeDirectory = home.path()
                + QStringLiteral("/.local/state/omarchy/current/theme");
            QFile colorsFile(themeDirectory + QStringLiteral("/colors.toml"));
            ok = home.isValid() && QDir().mkpath(themeDirectory)
                && qputenv("HOME", home.path().toUtf8())
                && colorsFile.open(QIODevice::WriteOnly | QIODevice::Text)
                && colorsFile.write(palette) == qint64(palette.size());
        }
        ~ScopedTheme() { qputenv("HOME", originalHome); }

        QTemporaryDir home;
        QByteArray originalHome = qgetenv("HOME");
        bool ok = false;
    };

    // Builds a minimal TextEdit and attaches its document to the backend, so
    // editorTextChanged() sees real edits. The engine outlives the returned
    // editor because it is a data member.
    QObject *makeEditor(Backend &backend) {
        QQmlComponent component(&m_engine);
        component.setData(
            "import QtQuick\nTextEdit { property var doc: textDocument }", QUrl());
        if (!component.isReady()) {
            qWarning() << component.errorString();
            return nullptr;
        }
        QObject *editor = component.create();
        if (editor)
            backend.attachDocument(editor->property("doc").value<QObject *>());
        return editor;
    }

    static void writeFile(const QString &path, const QByteArray &contents) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(contents), qint64(contents.size()));
    }

    // Writes while the backend is not watching, as when an event is missed.
    static void writeFileUnwatched(Backend &backend, const QString &path,
                                   const QByteArray &contents) {
        backend.m_fileWatcher.removePath(path);
        writeFile(path, contents);
    }

    static QByteArray readFile(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }

    // Loads a bare TextEdit, attaches its document to the backend, and returns
    // the QQuickTextDocument. The engine and TextEdit are parented to the
    // backend so they outlive the helper call.
    QQuickTextDocument *attachTextEdit(Backend *backend) {
        auto *engine = new QQmlEngine(backend);
        auto *component = new QQmlComponent(engine);
        component->setData(R"QML(
            import QtQuick
            TextEdit { }
        )QML", QUrl::fromLocalFile(QStringLiteral("Harness.qml")));
        if (!component->isReady())
            return nullptr;

        QObject *editor = component->create();
        if (!editor)
            return nullptr;
        editor->setParent(backend);

        auto *quickDocument = qobject_cast<QQuickTextDocument *>(
            editor->property("textDocument").value<QObject *>());
        if (!quickDocument)
            return nullptr;

        backend->attachDocument(quickDocument);
        return quickDocument;
    }

    QQmlEngine m_engine;
    QTemporaryDir m_settingsDirectory;
};

QTEST_MAIN(OmawriteTest)
#include "tst_omawrite.moc"
