#include "backend.h"

#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QImage>
#include <QImageReader>
#include <QtConcurrent/QtConcurrentRun>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QDesktopServices>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHash>
#include <QKeySequence>
#include <QMimeData>
#include <QProcess>
#include <QPrintDialog>
#include <QPrinter>
#include <QQuickTextDocument>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextStream>
#include <QUrl>
#include <QVariantMap>
#include <QWindow>

#include <algorithm>
#include <functional>
#include <iterator>
#include <vector>

#include "markdownhighlighter.h"

constexpr qreal typoraLineHeightPercent = 140;

static QTextBlockFormat lineHeightFormat() {
    QTextBlockFormat format;
    format.setLineHeight(typoraLineHeightPercent, QTextBlockFormat::ProportionalHeight);
    return format;
}

// A list item's wrapped lines belong under its text, not under the marker or
// the page margin. Qt's block indent moves every line, and a matching negative
// first-line indent puts the marker itself back where it was.
static QTextBlockFormat hangingIndentFormat(const QString &text, const QFontMetricsF &metrics,
                                            const QFont &font) {
    QTextBlockFormat format;
    static const QRegularExpression listItemRe(
        QStringLiteral("^([ \\t]*(?:[-+*]|\\d+[.)])[ \\t]+)"));
    const QRegularExpressionMatch match = listItemRe.match(text);
    if (!match.hasMatch() || MarkdownHighlighter::isThematicBreak(text)) {
        // Explicit zeros clear an indent left by a line that used to be an item.
        format.setIndent(0);
        format.setTextIndent(0);
        return format;
    }

    QString prefix = match.captured(1);
    // Measure tabs as the four-column stops the editor draws them at.
    QString measured;
    int column = 0;
    for (const QChar character : prefix) {
        if (character == QLatin1Char('\t')) {
            const int spaces = 4 - column % 4;
            measured.append(QString(spaces, QLatin1Char(' ')));
            column += spaces;
        } else {
            measured.append(character);
            ++column;
        }
    }

    qreal indent = metrics.horizontalAdvance(measured);

    // A task's checkbox replaces "[ ]" with the width it is drawn at. The
    // whitespace after it stays, so measure that too and wrapped lines start
    // under the task text rather than under the checkbox.
    const MarkdownHighlighter::TaskMarker task = MarkdownHighlighter::taskMarker(text);
    if (task.isValid()) {
        int contentStart = task.start + task.length;
        while (contentStart < text.length() && text.at(contentStart).isSpace())
            ++contentStart;
        indent += MarkdownHighlighter::taskCheckboxWidth(font)
            + metrics.horizontalAdvance(text.mid(task.start + task.length,
                                                 contentStart - task.start - task.length));
    }

    format.setIndent(qRound(indent));
    format.setTextIndent(-indent);
    return format;
}
const QString lastSaveDirectorySetting = QStringLiteral("file/lastSaveDirectory");
const QString textZoomSetting = QStringLiteral("text/zoom");

// The user's own text size, multiplied onto the desktop's text scale.
// Browser-sized steps, with a wider range for small and large displays.
constexpr qreal textZoomSteps[] = {0.5, 0.67, 0.75, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
constexpr int textZoomStepCount = int(std::size(textZoomSteps));

const QString editorFontSetting = QStringLiteral("editor/font");
const QString scrollSpeedSetting = QStringLiteral("scroll/speed");
const QString treeModeSetting = QStringLiteral("fileTree/mode");

// Wheel-scroll multiplier and tree source, shared by the editor Flickable and
// the file viewer's list. Twice as fast as the toolkit default reads better
// next to the eased Chromium-style scrolling, so 2.0 is the default.
constexpr double defaultScrollSpeed = 2.0;
constexpr double minScrollSpeed = 0.25;
constexpr double maxScrollSpeed = 4.0;

// Every rebindable action: the id QML binds to, the label the dialogs show,
// and the sequences a fresh settings file behaves as. The settings dialog and
// the shortcuts reference both read this table, so it is the only place that
// names a default key.
struct ShortcutDef {
    const char *id;
    const char *description;
    // A vector, not an initializer_list: the list's backing array would be a
    // temporary, dangling once this static table is built.
    std::vector<const char *> defaults;
};

static const QList<ShortcutDef> shortcutDefs = {
    {"save", "Save", {"Ctrl+S"}},
    {"save-as", "Save As", {"Ctrl+Shift+S"}},
    {"toggle-sidebar", "File Viewer (Show / Hide)", {"Ctrl+O"}},
    {"quick-open", "Open Other File", {"Ctrl+Shift+O"}},
    {"tab-1", "Go to Tab 1", {"Ctrl+1"}},
    {"tab-2", "Go to Tab 2", {"Ctrl+2"}},
    {"tab-3", "Go to Tab 3", {"Ctrl+3"}},
    {"tab-4", "Go to Tab 4", {"Ctrl+4"}},
    {"tab-5", "Go to Tab 5", {"Ctrl+5"}},
    {"tab-6", "Go to Tab 6", {"Ctrl+6"}},
    {"tab-7", "Go to Tab 7", {"Ctrl+7"}},
    {"tab-8", "Go to Tab 8", {"Ctrl+8"}},
    {"tab-9", "Go to Tab 9 (Last)", {"Ctrl+9"}},
    {"next-tab", "Next Tab", {"Ctrl+Tab"}},
    {"prev-tab", "Previous Tab", {"Ctrl+Shift+Tab"}},
    {"close-tab", "Close Tab", {"Ctrl+W"}},
    {"backlinks", "Linked Mentions (Backlinks)", {"Ctrl+Shift+B"}},
    {"new-window", "New Window", {"Ctrl+N"}},
    {"print", "Print", {"Ctrl+P"}},
    {"copy-html", "Copy as HTML", {"Ctrl+Shift+C"}},
    {"fullscreen", "Fullscreen", {"Meta+F", "F11"}},
    {"undo", "Undo", {"Ctrl+Z"}},
    {"redo", "Redo", {"Ctrl+Shift+Z", "Ctrl+Y"}},
    {"find", "Find", {"Ctrl+F"}},
    {"find-replace", "Find & Replace", {"Ctrl+H"}},
    {"find-next", "Next Match in Find", {"Ctrl+G"}},
    {"bold", "Bold", {"Ctrl+B"}},
    {"italic", "Italic", {"Ctrl+I"}},
    {"strikethrough", "Strikethrough", {"Ctrl+Shift+X"}},
    {"insert-link", "Insert Link", {"Ctrl+K"}},
    {"follow-link", "Follow Link", {"Ctrl+Return", "Ctrl+Enter"}},
    {"toggle-task", "Toggle Checkbox", {"Ctrl+Shift+Return", "Ctrl+Shift+Enter"}},
    {"zoom-in", "Increase Text Size", {"Ctrl+=", "Ctrl++"}},
    {"zoom-out", "Decrease Text Size", {"Ctrl+-"}},
    {"zoom-reset", "Reset Text Size", {"Ctrl+0"}},
    {"shortcuts-dialog", "Shortcuts", {"Ctrl+?", "Ctrl+/", "F1"}},
};

static QString shortcutSettingsKey(const QString &id) {
    return QStringLiteral("shortcuts/") + id;
}

// Noto ships a separate family per script, which buries every other font in
// the picker. Offer only the core Latin families; the rest still serve as
// fallbacks for whatever scripts a document contains.
const QStringList keptNotoFamilies = {QStringLiteral("Noto Sans"),
                                      QStringLiteral("Noto Sans Mono"),
                                      QStringLiteral("Noto Serif")};

// Fontconfig's generic aliases just point at another listed family.
const QStringList genericFontAliases = {QStringLiteral("Monospace"), QStringLiteral("Mono"),
                                        QStringLiteral("Sans Serif"), QStringLiteral("Sans"),
                                        QStringLiteral("Serif"), QStringLiteral("System-ui")};

// Symbol fonts often claim Latin coverage because they map their glyphs onto
// ASCII, so they can only be told apart by name. D050000L is URW's Dingbats.
const QRegularExpression symbolFontName(
    QStringLiteral("symbol|dingbat|emoji|icon|awesome|^D050000L$"),
    QRegularExpression::CaseInsensitiveOption);

namespace {

// A shade of `from` carried part of the way towards `to`.
QColor blend(const QColor &from, const QColor &to, qreal amount) {
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * amount,
                            from.greenF() + (to.greenF() - from.greenF()) * amount,
                            from.blueF() + (to.blueF() - from.blueF()) * amount);
}

}

namespace {

// One top-level frontmatter key and the lines it owns. firstLine and lastLine
// index the fenced block's own lines (the opening "---" is line 0), so a
// rewrite can splice just this key and leave comments, order, and unknown
// keys untouched.
struct FrontmatterEntry {
    QString key;
    QStringList values;
    bool isList = false;
    bool isMapping = false;
    int firstLine = -1;
    int lastLine = -1;
};

QString unquoteYaml(QString value) {
    value = value.trimmed();
    if (value.size() >= 2
            && ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
                || (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\''))))) {
        value = value.mid(1, value.size() - 2);
        value.replace(QStringLiteral("\\\""), QStringLiteral("\""));
        value.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
    }
    return value;
}

// A tag's or link's displayed form: no leading #, no [[ ]], alias, or heading.
QString cleanMetadataValue(QString value, bool link) {
    value = unquoteYaml(value);
    if (link) {
        if (value.startsWith(QStringLiteral("[["))) {
            const int close = value.indexOf(QStringLiteral("]]"));
            if (close > 2)
                value = value.mid(2, close - 2);
        }
        const int alias = value.indexOf(QLatin1Char('|'));
        if (alias >= 0)
            value.truncate(alias);
        const int heading = value.indexOf(QLatin1Char('#'));
        if (heading >= 0)
            value.truncate(heading);
    } else if (value.startsWith(QLatin1Char('#'))) {
        value.remove(0, 1);
    }
    return value.trimmed();
}

QStringList frontmatterLines(const QString &frontmatter) {
    QString text = frontmatter;
    if (text.endsWith(QLatin1Char('\n')))
        text.chop(1);
    return text.isEmpty() ? QStringList() : text.split(QLatin1Char('\n'));
}

int frontmatterClose(const QStringList &lines) {
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line == QStringLiteral("---") || line == QStringLiteral("..."))
            return i;
    }
    return -1;
}

QList<FrontmatterEntry> parseFrontmatter(const QString &frontmatter) {
    QList<FrontmatterEntry> entries;
    if (!frontmatter.startsWith(QStringLiteral("---")))
        return entries;
    const QStringList lines = frontmatterLines(frontmatter);
    const int close = frontmatterClose(lines);
    if (close < 0)
        return entries;

    for (int i = 1; i < close;) {
        const QString line = lines.at(i);
        const bool nested = line.startsWith(QLatin1Char(' ')) || line.startsWith(QLatin1Char('\t'));
        const int colon = line.indexOf(QLatin1Char(':'));
        if (line.trimmed().isEmpty() || nested || line.startsWith(QLatin1Char('#')) || colon <= 0) {
            ++i;
            continue;
        }
        FrontmatterEntry entry;
        entry.key = line.left(colon).trimmed();
        entry.firstLine = i;
        const QString rest = line.mid(colon + 1).trimmed();
        if (rest.startsWith(QLatin1Char('{'))) {
            // A mapping value (some vaults put one under up:). Shown but not
            // edited: rewriting it would drop fields this editor cannot see.
            entry.isMapping = true;
            entry.lastLine = i;
            entries.append(entry);
            ++i;
            continue;
        }
        if (rest.startsWith(QLatin1Char('['))) {
            entry.isList = true;
            QString flow = rest.mid(1);
            if (flow.endsWith(QLatin1Char(']')))
                flow.chop(1);
            for (const QString &part : flow.split(QLatin1Char(','))) {
                if (!part.trimmed().isEmpty())
                    entry.values.append(unquoteYaml(part));
            }
            entry.lastLine = i;
            entries.append(entry);
            ++i;
            continue;
        }
        if (rest.isEmpty()) {
            entry.isList = true;
            int j = i + 1;
            while (j < close) {
                const QString item = lines.at(j).trimmed();
                if (!item.startsWith(QLatin1Char('-')))
                    break;
                const QString value = unquoteYaml(item.mid(1));
                if (!value.isEmpty())
                    entry.values.append(value);
                ++j;
            }
            entry.lastLine = j - 1;
            entries.append(entry);
            i = j;
            continue;
        }
        entry.values = {unquoteYaml(rest)};
        entry.lastLine = i;
        entries.append(entry);
        ++i;
    }
    return entries;
}

QString yamlScalar(const QString &value) {
    const QString trimmed = value.trimmed();
    const bool needsQuotes = trimmed.isEmpty() || trimmed != value
        || trimmed.contains(QLatin1Char(':')) || trimmed.contains(QLatin1Char('#'))
        || trimmed.contains(QLatin1Char('[')) || trimmed.contains(QLatin1Char(']'))
        || trimmed.contains(QLatin1Char('{')) || trimmed.contains(QLatin1Char('}'))
        || trimmed.contains(QLatin1Char(',')) || trimmed.contains(QLatin1Char('"'))
        || trimmed.contains(QLatin1Char('\n'))
        || trimmed.startsWith(QLatin1Char('-')) || trimmed.startsWith(QLatin1Char('&'))
        || trimmed.startsWith(QLatin1Char('*')) || trimmed.startsWith(QLatin1Char('!'))
        || trimmed.startsWith(QLatin1Char('|')) || trimmed.startsWith(QLatin1Char('>'));
    if (!needsQuotes)
        return trimmed;
    QString escaped = trimmed;
    escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    escaped.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString composeFrontmatter(const QStringList &lines) {
    if (lines.isEmpty())
        return {};
    // lines already carries the two fence lines.
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

// True when the frontmatter list held nothing but its two fences.
bool frontmatterIsEmpty(const QStringList &lines) {
    const int close = frontmatterClose(lines);
    return close < 0 || close == 1;
}

} // namespace

QString Backend::normalizedLinkUrl(const QString &clipboardText) {
    QString candidate = clipboardText.trimmed();
    static const QRegularExpression lineBreakRe(QStringLiteral("[\\r\\n]"));
    const int lineBreak = candidate.indexOf(lineBreakRe);
    if (lineBreak >= 0)
        candidate = candidate.left(lineBreak).trimmed();

    if (candidate.isEmpty())
        return {};

    if (candidate.startsWith(QStringLiteral("www."), Qt::CaseInsensitive))
        candidate.prepend(QStringLiteral("https://"));

    static const QRegularExpression schemeRe(
        QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:"));
    if (!schemeRe.match(candidate).hasMatch())
        return {};

    const QUrl url(candidate);
    if (!url.isValid() || url.scheme().isEmpty())
        return {};

    const QString scheme = url.scheme().toLower();
    const bool webUrl = scheme == QStringLiteral("http")
        || scheme == QStringLiteral("https")
        || scheme == QStringLiteral("ftp");
    if (webUrl && url.host().isEmpty())
        return {};

    if (!webUrl && scheme != QStringLiteral("mailto"))
        return {};

    return url.toString();
}

Backend::Backend(QObject *parent) : QObject(parent) {
    // A settings file truncated by a crash reads back as an empty value, and
    // both that and a NaN would clamp to the smallest size rather than fall
    // back to the default.
    bool zoomOk = false;
    const qreal storedZoom = QSettings().value(textZoomSetting, 1.0).toDouble(&zoomOk);
    m_textZoom = zoomOk && storedZoom > 0 ? qBound(textZoomSteps[0], storedZoom,
                                                   textZoomSteps[textZoomStepCount - 1])
                                          : 1.0;

    const QString savedFont = QSettings().value(editorFontSetting).toString();
    m_editorFont = !savedFont.isEmpty() && QFontDatabase::hasFamily(savedFont)
        ? savedFont
        : defaultEditorFont();

    bool speedOk = false;
    const double storedSpeed = QSettings().value(scrollSpeedSetting, defaultScrollSpeed)
                                   .toDouble(&speedOk);
    m_scrollSpeed = speedOk && storedSpeed > 0
        ? qBound(minScrollSpeed, storedSpeed, maxScrollSpeed)
        : defaultScrollSpeed;

    const QString storedMode = QSettings().value(treeModeSetting, QStringLiteral("links")).toString();
    m_treeMode = storedMode == QStringLiteral("folders") ? storedMode : QStringLiteral("links");

    const QString stateDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(stateDirectory);
    // Claim an orphaned snapshot before taking an empty slot. This ensures a
    // crash in window 2 is still recovered even if window 1 exited normally.
    for (int pass = 0; pass < 2 && !m_recoveryLock; ++pass) {
        for (int slot = 0; slot < 100; ++slot) {
            const QString base = QDir(stateDirectory).filePath(
                QStringLiteral("recovery-%1").arg(slot));
            const bool snapshotExists = QFileInfo::exists(base + QStringLiteral(".json"));
            if ((pass == 0) != snapshotExists)
                continue;
            auto lock = std::make_unique<QLockFile>(base + QStringLiteral(".lock"));
            if (lock->tryLock()) {
                m_recoveryPath = base + QStringLiteral(".json");
                m_recoveryLock = std::move(lock);
                break;
            }
        }
    }
    m_wordCountTimer.setSingleShot(true);
    m_wordCountTimer.setInterval(120);
    connect(&m_wordCountTimer, &QTimer::timeout, this, &Backend::refreshWordCount);
    connect(&m_wordCountWatcher, &QFutureWatcher<int>::finished, this, [this] {
        if (!m_wordCountStale)
            setWordCount(m_wordCountWatcher.result());
        if (m_wordCountAgain) {
            m_wordCountAgain = false;
            refreshWordCount();
        }
    });
    m_recoveryTimer.setSingleShot(true);
    m_recoveryTimer.setInterval(750);
    connect(&m_recoveryTimer, &QTimer::timeout, this, &Backend::writeRecovery);
    m_autoSaveTimer.setSingleShot(true);
    m_autoSaveTimer.setInterval(500);
    connect(&m_autoSaveTimer, &QTimer::timeout, this, &Backend::autoSave);
    // Tools that replace a file by removing it and writing it again leave it
    // briefly missing, so let the burst settle before deciding what happened.
    m_fileCheckTimer.setSingleShot(true);
    m_fileCheckTimer.setInterval(100);
    connect(&m_fileCheckTimer, &QTimer::timeout, this, &Backend::checkWatchedFile);
    connect(&m_fileWatcher, &QFileSystemWatcher::fileChanged, &m_fileCheckTimer,
            qOverload<>(&QTimer::start));
    connect(&m_fileWatcher, &QFileSystemWatcher::directoryChanged, &m_fileCheckTimer,
            qOverload<>(&QTimer::start));

    loadOmarchyTheme();
    watchOmarchyTheme();
    // Switching themes touches several watched paths in a row, and the theme
    // folder can be briefly missing, so reload once the burst has settled.
    m_themeReloadTimer.setSingleShot(true);
    m_themeReloadTimer.setInterval(100);
    connect(&m_themeReloadTimer, &QTimer::timeout, this, [this]() {
        loadOmarchyTheme();
        watchOmarchyTheme();
    });
    connect(&m_themeWatcher, &QFileSystemWatcher::fileChanged, &m_themeReloadTimer,
            qOverload<>(&QTimer::start));
    connect(&m_themeWatcher, &QFileSystemWatcher::directoryChanged, &m_themeReloadTimer,
            qOverload<>(&QTimer::start));
}

Backend::~Backend() = default;

void Backend::setParentWindow(QWindow *window) {
    m_parentWindow = window;
}

QUrl Backend::documentFolder() const {
    if (m_fileUrl.isLocalFile())
        return QUrl::fromLocalFile(QFileInfo(m_fileUrl.toLocalFile()).absolutePath());

    const QString lastFolder = QSettings().value(lastSaveDirectorySetting).toString();
    return lastFolder.isEmpty() ? QUrl() : QUrl::fromLocalFile(lastFolder);
}

QString Backend::fileName() const {
    if (!m_fileUrl.isValid() || m_fileUrl.isEmpty())
        return QStringLiteral("Untitled.md");

    if (m_fileUrl.isLocalFile()) {
        const QFileInfo info(m_fileUrl.toLocalFile());
        if (!info.fileName().isEmpty())
            return info.fileName();
    }

    const QString name = m_fileUrl.fileName();
    return name.isEmpty() ? QStringLiteral("Untitled.md") : name;
}

void Backend::setDarkMode(bool darkMode) {
    m_systemDarkMode = darkMode;
    loadOmarchyTheme();
}

void Backend::setDesktopTextScale(qreal textScale) {
    if (qFuzzyCompare(m_desktopTextScale, textScale))
        return;

    m_desktopTextScale = textScale;
    emit textScaleChanged();
}

void Backend::increaseTextSize() {
    setTextZoom(steppedTextZoom(1));
}

void Backend::decreaseTextSize() {
    setTextZoom(steppedTextZoom(-1));
}

void Backend::resetTextSize() {
    setTextZoom(1.0);
}

// The neighbouring rung, starting from whichever rung the current zoom sits
// closest to; the ends of the ladder hold.
qreal Backend::steppedTextZoom(int direction) const {
    int closest = 0;
    for (int step = 1; step < textZoomStepCount; ++step) {
        if (qAbs(textZoomSteps[step] - m_textZoom) < qAbs(textZoomSteps[closest] - m_textZoom))
            closest = step;
    }

    return textZoomSteps[qBound(0, closest + direction, textZoomStepCount - 1)];
}

void Backend::setTextZoom(qreal zoom) {
    // Report the size even when the ladder has run out, so a keypress that
    // changes nothing still says why.
    setStatus(QStringLiteral("Text size %1%").arg(qRound(zoom * 100)));
    if (qFuzzyCompare(m_textZoom, zoom))
        return;

    m_textZoom = zoom;
    QSettings().setValue(textZoomSetting, zoom);
    emit textScaleChanged();
}

void Backend::setScrollSpeed(double speed) {
    // A wheel that barely moves, or one that jumps pages, is a misconfiguration
    // rather than a preference, so clamp instead of refusing.
    const double clamped = qBound(minScrollSpeed, speed, maxScrollSpeed);
    if (qFuzzyCompare(m_scrollSpeed, clamped))
        return;

    m_scrollSpeed = clamped;
    QSettings().setValue(scrollSpeedSetting, clamped);
    emit scrollSpeedChanged();
}

void Backend::setTreeMode(const QString &mode) {
    const QString cleaned = mode == QStringLiteral("folders") ? mode : QStringLiteral("links");
    if (m_treeMode == cleaned)
        return;

    m_treeMode = cleaned;
    QSettings().setValue(treeModeSetting, cleaned);
    emit treeModeChanged();
}

QStringList Backend::shortcutIds() const {
    QStringList ids;
    for (const ShortcutDef &def : shortcutDefs)
        ids.append(QString::fromLatin1(def.id));
    return ids;
}

QString Backend::shortcutDescription(const QString &id) const {
    for (const ShortcutDef &def : shortcutDefs) {
        if (id == QString::fromLatin1(def.id))
            return QString::fromLatin1(def.description);
    }
    return {};
}

QStringList Backend::shortcutDefaults(const QString &id) const {
    for (const ShortcutDef &def : shortcutDefs) {
        if (id != QString::fromLatin1(def.id))
            continue;
        QStringList defaults;
        for (const char *sequence : def.defaults)
            defaults.append(QString::fromLatin1(sequence));
        return defaults;
    }
    return {};
}

QStringList Backend::shortcutSequences(const QString &id) const {
    if (shortcutDefaults(id).isEmpty())
        return {};
    const QStringList stored = QSettings().value(shortcutSettingsKey(id)).toStringList();
    if (stored.isEmpty())
        return shortcutDefaults(id);
    // An override edited by hand outside the settings dialog must not leave a
    // dead key: anything unparseable falls back to the defaults wholesale.
    for (const QString &sequence : stored) {
        if (QKeySequence(sequence).isEmpty())
            return shortcutDefaults(id);
    }
    return stored;
}

bool Backend::setShortcut(const QString &id, const QString &text) {
    if (shortcutDefaults(id).isEmpty())
        return false;
    // Commas separate alternates ("Ctrl+Shift+Z, Ctrl+Y"); a single empty or
    // unparseable entry rejects the whole override, so half a key never sticks.
    QStringList sequences;
    for (const QString &part : text.split(QLatin1Char(','))) {
        const QString sequence = part.trimmed();
        if (sequence.isEmpty() || QKeySequence(sequence).isEmpty())
            return false;
        sequences.append(QKeySequence(sequence).toString(QKeySequence::PortableText));
    }
    if (sequences.isEmpty())
        return false;
    QSettings().setValue(shortcutSettingsKey(id), sequences);
    emit shortcutsChanged();
    return true;
}

void Backend::resetShortcut(const QString &id) {
    if (shortcutDefaults(id).isEmpty())
        return;
    QSettings().remove(shortcutSettingsKey(id));
    emit shortcutsChanged();
}

void Backend::resetAllShortcuts() {
    for (const ShortcutDef &def : shortcutDefs)
        QSettings().remove(shortcutSettingsKey(QString::fromLatin1(def.id)));
    emit shortcutsChanged();
}

QVariantList Backend::shortcutRows() const {
    QVariantList rows;
    for (const ShortcutDef &def : shortcutDefs) {
        const QString id = QString::fromLatin1(def.id);
        const QStringList sequences = shortcutSequences(id);
        rows.append(QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("description"), QString::fromLatin1(def.description)},
            {QStringLiteral("sequence"), sequences.join(QStringLiteral(", "))},
            {QStringLiteral("isDefault"),
             sequences == shortcutDefaults(id)}});
    }
    return rows;
}

QVariantMap Backend::shortcutMap() const {
    QVariantMap map;
    for (const ShortcutDef &def : shortcutDefs) {
        const QString id = QString::fromLatin1(def.id);
        map.insert(id, shortcutSequences(id));
    }
    return map;
}

void Backend::attachDocument(QObject *textDocument) {
    auto *quickDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    if (!quickDocument || !quickDocument->textDocument()) {
        setStatus(QStringLiteral("Could not attach the Markdown renderer."));
        return;
    }

    if (m_highlighter)
        delete m_highlighter.data();

    m_document = quickDocument->textDocument();
    m_lastDocumentText = m_document->toPlainText();
    m_highlighter = new MarkdownHighlighter(m_document);
    m_highlighter->setTheme(m_darkMode, m_themeBackground, m_themeForeground, m_themeAccent,
                            m_themeCodeBackground);
    m_highlighter->setCaret(m_caretPosition);

    connect(m_document, &QTextDocument::contentsChange, this,
            [this](int position, int, int charsAdded) {
                if (m_formattingTypography || m_loading)
                    return;
                m_lastChangePos = position;
                m_lastChangeAdded = charsAdded;
            });

    applyDocumentTypography();
    restoreRecovery();
}

void Backend::openDialog() {
    emit openDialogRequested();
}

void Backend::open(const QUrl &url) {
    if (!url.isLocalFile()) {
        setStatus(QStringLiteral("Only local files can be opened."));
        return;
    }

    const QString targetName = QFileInfo(url.toLocalFile()).fileName();
    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus(QStringLiteral("Could not open %1.").arg(targetName));
        return;
    }

    const QByteArray raw = file.readAll();
    // Text mode drops carriage returns, matching how the file is compared
    // and saved later.
    file.setTextModeEnabled(true);
    file.seek(0);
    const QByteArray contents = file.readAll();
    // Before the text is laid out: image embeds resolve against the document's
    // folder, and laying out text without it would find none of them.
    setFileUrl(url);
    loadDocumentText(QString::fromUtf8(contents));
    clearRecovery();
    setKnownFileContents(contents, true);
    m_autoSaveSuspended = false;
    watchCurrentFile();
    setModified(false);
    if (savesUnchanged(raw)) {
        setStatus(QStringLiteral("Opened %1").arg(fileName()));
    } else {
        // Saving writes UTF-8 with plain line endings, which would change
        // bytes this file holds. Leave that to an explicit save.
        suspendAutoSave();
        setStatus(QStringLiteral("Opened %1. Auto-save is off to keep its encoding and line endings.")
                      .arg(fileName()));
    }
}

void Backend::save() {
    if (!m_fileUrl.isValid() || m_fileUrl.isEmpty()) {
        saveAsDialog();
        return;
    }

    saveTo(m_fileUrl);
}

void Backend::saveAsDialog() {
    emit saveDialogRequested(suggestedSaveUrl());
}

void Backend::saveAs(const QUrl &url) {
    saveTo(url);
}

void Backend::discardRecovery() {
    clearRecovery();
    // Discarded changes must never reach the file, even if the open or close
    // that follows fails and leaves them in the editor. Opening a file or an
    // explicit save lifts this.
    suspendAutoSave();
}

// A file named on the command line need not exist yet: `omawrite new.md`
// starts a blank document that the first save writes to that path. Opening
// from the file picker or reloading still requires the file to be there.
void Backend::openOrStartFile(const QUrl &requestedUrl) {
    // A relative path names a file in the directory Omawrite was started
    // from; a recovery draft must not resolve it against another one later.
    const QFileInfo info(requestedUrl.toLocalFile());
    const QUrl url = requestedUrl.isLocalFile()
        ? QUrl::fromLocalFile(info.absoluteFilePath()) : requestedUrl;
    if (m_modified) {
        // A recovered draft fills this window; opening over it would lose it.
        setStatus(QStringLiteral("Recovered unsaved changes; %1 was not opened.")
                      .arg(info.fileName()));
        return;
    }
    if (!url.isLocalFile() || info.exists() || !info.absoluteDir().exists()) {
        open(url);
        return;
    }

    setFileUrl(url);
    loadDocumentText(QString());
    clearRecovery();
    // Nothing has been read, so no text counts as saved, and auto-save holds
    // off until an explicit save creates the file (see autoSave()).
    setKnownFileContents(QByteArray(), false);
    watchCurrentFile();
    setModified(false);
    setStatus(QStringLiteral("New file %1").arg(fileName()));
}

void Backend::reloadFromDisk() {
    if (m_fileUrl.isLocalFile())
        open(m_fileUrl);
}

void Backend::keepExternalVersion() {
    QFile file(m_fileUrl.toLocalFile());
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray raw = file.readAll();
        file.setTextModeEnabled(true);
        file.seek(0);
        setKnownFileContents(file.readAll(), true);
        // Keeping our text over an edited file is a deliberate choice to
        // overwrite it, so auto-save may resume, unless saving would change
        // the file's encoding or line endings. A removed file stays removed
        // until the writer saves it again.
        m_autoSaveSuspended = !savesUnchanged(raw);
    } else {
        setKnownFileContents(QByteArray(), false);
    }
    setModified(true);
    scheduleRecovery();
    scheduleAutoSave();
    watchCurrentFile();
    setStatus(QStringLiteral("Kept your version"));
}

QFont Backend::printFont(const QFont &editorFont, qreal screenDpi) {
    // The editor sizes its font in pixels, which a printer takes as device dots.
    // Convert to points at the screen's DPI so the page matches the editor.
    QFont font = editorFont;
    if (font.pixelSize() <= 0)
        return font;

    const qreal dpi = screenDpi > 0.0 ? screenDpi : 96.0;
    font.setPointSizeF(font.pixelSize() * 72.0 / dpi);
    return font;
}

void Backend::printDocument() {
    if (!m_document) {
        setStatus(QStringLiteral("There is no document to print."));
        return;
    }

    QPrinter printer(QPrinter::HighResolution);
    QPrintDialog dialog(&printer);
    dialog.setWindowTitle(QStringLiteral("Print %1").arg(fileName()));
    dialog.winId();
    if (dialog.windowHandle() && m_parentWindow)
        dialog.windowHandle()->setTransientParent(m_parentWindow);

    if (dialog.exec() == QDialog::Accepted) {
        QTextDocument rendered;
        const QScreen *screen = m_parentWindow ? m_parentWindow->screen()
                                               : QGuiApplication::primaryScreen();
        rendered.setDefaultFont(printFont(m_document->defaultFont(),
                                          screen ? screen->logicalDotsPerInchY() : 0.0));
        rendered.setMarkdown(currentDocumentText());
        rendered.print(&printer);
    }
}

void Backend::newWindow() {
    const bool started = QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                                 QStringList());
    if (!started)
        setStatus(QStringLiteral("Could not open a new window."));
}

void Backend::copyAsHtml() {
    QTextDocument rendered;
    rendered.setMarkdown(currentDocumentText());

    // Drop Qt's fixed fonts and margins from the body and blocks, so the pasted
    // text takes on the destination's own typeface and paragraph spacing. A
    // task list's <li> carries a class before its style, which stays.
    QString html = rendered.toHtml();
    static const QRegularExpression blockStyle(
        QStringLiteral("<(body|p|h[1-6]|ul|ol|li)(\\s+[^>]*?)?\\s+style=\"[^\"]*\""));
    html.replace(blockStyle, QStringLiteral("<\\1\\2"));

    auto *mimeData = new QMimeData;
    mimeData->setHtml(html);
    mimeData->setText(rendered.toPlainText());
    QGuiApplication::clipboard()->setMimeData(mimeData);
    setStatus(QStringLiteral("Copied as HTML."));
}

QString Backend::clipboardUrl() const {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return {};

    const QMimeData *mimeData = clipboard->mimeData();
    if (!mimeData)
        return {};

    if (mimeData->hasUrls()) {
        const QList<QUrl> urls = mimeData->urls();
        for (const QUrl &url : urls) {
            const QString normalized = normalizedLinkUrl(url.toString());
            if (!normalized.isEmpty())
                return normalized;
        }
    }

    if (!mimeData->hasText())
        return {};

    return normalizedLinkUrl(mimeData->text());
}

QString Backend::clipboardText() const {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return {};

    const QMimeData *mimeData = clipboard->mimeData();
    return mimeData && mimeData->hasText() ? mimeData->text() : QString();
}

bool Backend::editorTextChanged() {
    if (m_loading || m_formattingTypography)
        return false;

    const QString text = currentDocumentText();
    if (text == m_lastDocumentText)
        return false;
    m_lastDocumentText = text;

    reapplyTypographyToChange();

    scheduleWordCount();

    const QString full = fullDocumentText();
    const QString &baseline = m_lastKnownFileText;
    // An empty baseline means a pristine untitled document, but only when there
    // is no file behind it. Once there is one, an unknown baseline is unknown
    // rather than empty, and emptying the editor is a change like any other.
    const bool baselineKnown = m_hasKnownFileContents
        || !m_fileUrl.isValid() || m_fileUrl.isEmpty();

    if (baselineKnown && full == baseline) {
        setModified(false);
        clearRecovery();
        if (m_hasKnownFileContents)
            setStatus(QStringLiteral("Saved %1").arg(fileName()));
        else
            setStatus(QString());
    } else {
        setModified(true);
        setStatus(QStringLiteral("Unsaved"));
        scheduleRecovery();
        scheduleAutoSave();
    }

    return true;
}

void Backend::setCaretPosition(int position) {
    m_caretPosition = position;
    if (m_highlighter)
        m_highlighter->setCaret(position);
}

// The emphasis span of the marker's kind that the caret sits in or against,
// in document positions, so a toggle with nothing selected can act on it.
QVariantMap Backend::emphasisSpanAt(int position, const QString &marker) const {
    if (!m_document)
        return {};

    const QTextBlock block =
        m_document->findBlock(qBound(0, position, m_document->characterCount() - 1));
    if (!block.isValid() || block.userState() >= MarkdownHighlighter::InsideFence
            || block.previous().userState() >= MarkdownHighlighter::InsideFence)
        return {};

    using Kind = MarkdownHighlighter::InlineKind;
    const Kind kind = marker == QStringLiteral("**") ? Kind::Bold
        : marker == QStringLiteral("~~")              ? Kind::Strikethrough
                                                      : Kind::Italic;
    const int offset = position - block.position();
    const QList<MarkdownHighlighter::InlineMarkup> markup =
        MarkdownHighlighter::inlineMarkup(block.text());
    for (const auto &item : markup) {
        const int start = item.markers[0].start;
        const int end = item.markers[1].start + item.markers[1].length;
        if (item.kind != kind || offset < start || offset > end)
            continue;
        // The visible text ends before the chain of nested emphasis closing
        // with it, as the italic "*" inside "***word***".
        const int contentEnd = item.content.start + item.content.length;
        int textEnd = contentEnd;
        for (bool moved = true; moved;) {
            moved = false;
            for (const auto &nested : markup) {
                const auto &closing = nested.markers[1];
                if (closing.start >= item.content.start && closing.length > 0
                        && closing.start + closing.length == textEnd) {
                    textEnd = closing.start;
                    moved = true;
                }
            }
        }
        const int base = block.position();
        return {{QStringLiteral("start"), base + start},
                {QStringLiteral("end"), base + end},
                {QStringLiteral("contentStart"), base + item.content.start},
                {QStringLiteral("contentEnd"), base + contentEnd},
                {QStringLiteral("textEnd"), base + textEnd}};
    }
    return {};
}

QString Backend::linkUrlAt(int position) const {
    if (!m_document)
        return {};

    const QTextBlock block =
        m_document->findBlock(qBound(0, position, m_document->characterCount() - 1));
    if (!block.isValid())
        return {};

    // Fenced code is literal, so nothing in it is a link.
    if (block.userState() >= MarkdownHighlighter::InsideFence
            || block.previous().userState() >= MarkdownHighlighter::InsideFence)
        return {};

    return MarkdownHighlighter::linkUrlAt(block.text(), position - block.position());
}

// Every task item's marker, in document order, for the editor's checkboxes.
QVariantList Backend::taskMarkers() const {
    QVariantList markers;
    if (!m_document)
        return markers;

    for (QTextBlock block = m_document->begin(); block.isValid(); block = block.next()) {
        if (block.userState() >= MarkdownHighlighter::InsideFence)
            continue;
        const MarkdownHighlighter::TaskMarker task =
            MarkdownHighlighter::taskMarker(block.text());
        if (!task.isValid())
            continue;
        markers.append(QVariantMap{{QStringLiteral("position"), block.position() + task.start},
                                   {QStringLiteral("checked"), task.checked}});
    }
    return markers;
}

// Toggles the checkbox on the line at position. The replacement is one edit,
// so a single undo puts the checkbox back.
bool Backend::toggleTaskAt(int position) {
    if (!m_document)
        return false;

    const QTextBlock block =
        m_document->findBlock(qBound(0, position, m_document->characterCount() - 1));
    if (!block.isValid() || block.userState() >= MarkdownHighlighter::InsideFence)
        return false;

    const MarkdownHighlighter::TaskMarker task = MarkdownHighlighter::taskMarker(block.text());
    if (!task.isValid())
        return false;

    replaceRange(block.position() + task.start + 1,
                 block.position() + task.start + 2,
                 task.checked ? QStringLiteral(" ") : QStringLiteral("x"));
    return true;
}

void Backend::openExternalUrl(const QString &url) {
    const QUrl target = QUrl::fromUserInput(url.trimmed());
    const QString scheme = target.scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")
            && scheme != QStringLiteral("mailto")) {
        setStatus(QStringLiteral("Cannot open %1").arg(url));
        return;
    }
    if (!QDesktopServices::openUrl(target))
        setStatus(QStringLiteral("Could not open %1").arg(url));
}

// The note's folder, or the nearest folder above it that holds an Obsidian
// vault marker. A slipbox is often nested (ai/, drawings/), and [[links]]
// resolve across the whole vault, not just beside the current file.
QString Backend::notesRoot() const {
    QString start;
    if (m_fileUrl.isLocalFile())
        start = QFileInfo(m_fileUrl.toLocalFile()).absolutePath();
    else
        start = QSettings().value(lastSaveDirectorySetting).toString();
    if (start.isEmpty())
        return {};

    QDir directory(start);
    while (true) {
        if (QDir(directory.filePath(QStringLiteral(".obsidian"))).exists())
            return directory.absolutePath();
        if (!directory.cdUp())
            break;
    }
    return QDir(start).absolutePath();
}

void Backend::refreshNoteIndex() {
    m_notesRoot = notesRoot();
    m_notePaths.clear();
    m_noteMeta.clear();
    m_notesIndexed = true;
    if (m_notesRoot.isEmpty())
        return;

    const QDir root(m_notesRoot);
    QDirIterator iterator(m_notesRoot, {QStringLiteral("*.md"), QStringLiteral("*.markdown")},
                          QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString relative = root.relativeFilePath(iterator.next());
        // Obsidian keeps its settings and trash in dot folders; neither holds
        // notes the writer means to link to.
        if (relative.startsWith(QLatin1Char('.'))
                || relative.contains(QStringLiteral("/.")))
            continue;

        m_notePaths.append(relative);
    }

    std::sort(m_notePaths.begin(), m_notePaths.end(), [](const QString &a, const QString &b) {
        const QString aName = QFileInfo(a).completeBaseName();
        const QString bName = QFileInfo(b).completeBaseName();
        const int byName = aName.compare(bName, Qt::CaseInsensitive);
        return byName != 0 ? byName < 0 : a.compare(b, Qt::CaseInsensitive) < 0;
    });

    // One head read per note, so the link tree and future panels never scan
    // the vault again for what the index already knows.
    for (const QString &path : m_notePaths)
        m_noteMeta.insert(path, readNoteFrontmatter(root.filePath(path)));
}

QVariantList Backend::noteSuggestions(const QString &query) {
    if (!m_notesIndexed || m_notesRoot != notesRoot())
        refreshNoteIndex();

    QHash<QString, int> nameCounts;
    for (const QString &path : m_notePaths)
        ++nameCounts[QFileInfo(path).completeBaseName().toCaseFolded()];

    const QString needle = query.trimmed().toCaseFolded();
    struct Suggestion {
        QString path;
        QString name;
        QString insert;
        int score;
    };
    QList<Suggestion> matches;
    for (const QString &path : m_notePaths) {
        const QString name = QFileInfo(path).completeBaseName();
        int score = 0;
        if (!needle.isEmpty()) {
            const int nameAt = name.toCaseFolded().indexOf(needle);
            const int pathAt = path.toCaseFolded().indexOf(needle);
            if (nameAt < 0 && pathAt < 0)
                continue;
            score = nameAt >= 0 ? nameAt : 1000 + pathAt;
        }
        QString stem = path;
        const int slash = stem.lastIndexOf(QLatin1Char('/'));
        const int dot = stem.lastIndexOf(QLatin1Char('.'));
        if (dot > slash)
            stem.truncate(dot);
        const bool duplicate = nameCounts.value(name.toCaseFolded()) > 1;
        const QString insert = duplicate && stem.contains(QLatin1Char('/')) ? stem : name;
        matches.append({path, name, insert, score});
    }

    std::sort(matches.begin(), matches.end(), [](const Suggestion &a, const Suggestion &b) {
        if (a.score != b.score)
            return a.score < b.score;
        const int byName = a.name.compare(b.name, Qt::CaseInsensitive);
        return byName != 0 ? byName < 0 : a.path.compare(b.path, Qt::CaseInsensitive) < 0;
    });

    QVariantList suggestions;
    const int limit = 50;
    for (int i = 0; i < matches.size() && i < limit; ++i) {
        suggestions.append(QVariantMap{
            {QStringLiteral("name"), matches[i].name},
            {QStringLiteral("path"), matches[i].path},
            {QStringLiteral("insert"), matches[i].insert},
            {QStringLiteral("url"),
             QUrl::fromLocalFile(QDir(m_notesRoot).filePath(matches[i].path))}});
    }
    return suggestions;
}

// A frontmatter value with its quotes dropped. For up: links the brackets,
// aliases, and headings come off too, leaving the bare note target.
static QString cleanFrontmatterValue(QString value, bool link) {
    value = value.trimmed();
    if (value.size() >= 2
            && ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
                || (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\'')))))
        value = value.mid(1, value.size() - 2).trimmed();
    if (!link)
        return value;
    if (value.startsWith(QStringLiteral("[["))) {
        const int close = value.indexOf(QStringLiteral("]]"));
        if (close > 2)
            value = value.mid(2, close - 2);
    }
    const int alias = value.indexOf(QLatin1Char('|'));
    if (alias >= 0)
        value.truncate(alias);
    const int heading = value.indexOf(QLatin1Char('#'));
    if (heading >= 0)
        value.truncate(heading);
    return value.trimmed();
}

// The top-level up: and tags:/tag: keys of a note's frontmatter. Only the
// head of the file is read: the block always opens the note, and a vault-wide
// index cannot afford whole files. A mapping under up: (an object, not
// parents) is left alone rather than guessed at.
Backend::NoteMeta Backend::readNoteFrontmatter(const QString &absolutePath) {
    NoteMeta meta;
    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly))
        return meta;
    const QList<QByteArray> lines = file.read(4096).split('\n');
    if (lines.isEmpty() || lines.constFirst().trimmed() != "---")
        return meta;
    int end = -1;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray trimmed = lines.at(i).trimmed();
        if (trimmed == "---" || trimmed == "...") {
            end = i;
            break;
        }
    }
    if (end < 0)
        return meta;

    for (int i = 1; i < end;) {
        const QString line = QString::fromUtf8(lines.at(i));
        if (line.isEmpty() || line.at(0).isSpace() || !line.contains(QLatin1Char(':'))) {
            ++i;
            continue;
        }
        const QString key = line.left(line.indexOf(QLatin1Char(':'))).trimmed();
        const bool isUp = key == QStringLiteral("up");
        if (!isUp && key != QStringLiteral("tags") && key != QStringLiteral("tag")) {
            ++i;
            continue;
        }
        const QString rest = line.mid(line.indexOf(QLatin1Char(':')) + 1).trimmed();
        QStringList *out = isUp ? &meta.ups : &meta.tags;
        if (rest.isEmpty()) {
            // A block list: the indented "- item" lines that follow the key.
            // Anything else (a nested mapping's "key: value") ends it.
            ++i;
            while (i < end) {
                const QString item = QString::fromUtf8(lines.at(i)).trimmed();
                if (!item.startsWith(QLatin1Char('-')))
                    break;
                const QString value = cleanFrontmatterValue(item.mid(1), isUp);
                if (!value.isEmpty())
                    out->append(value);
                ++i;
            }
            continue;
        }
        if (isUp && rest.startsWith(QLatin1Char('{'))) {
            ++i;
            continue;
        }
        if (rest.startsWith(QLatin1Char('['))) {
            QString flow = rest.mid(1);
            if (flow.endsWith(QLatin1Char(']')))
                flow.chop(1);
            for (const QString &part : flow.split(QLatin1Char(','))) {
                const QString value = cleanFrontmatterValue(part, isUp);
                if (!value.isEmpty())
                    out->append(value);
            }
            ++i;
            continue;
        }
        const QString value = cleanFrontmatterValue(rest, isUp);
        if (!value.isEmpty())
            out->append(value);
        ++i;
    }
    return meta;
}

QString Backend::fullDocumentText() const {
    return m_frontmatter + currentDocumentText();
}

// What the properties panel draws: one row per top-level key, in file order,
// with tags and up: links resolved to their readable targets.
QVariantList Backend::documentProperties() const {
    QVariantList rows;
    for (const FrontmatterEntry &entry : parseFrontmatter(m_frontmatter)) {
        if (entry.isMapping)
            continue;

        QString kind = QStringLiteral("text");
        QStringList values = entry.values;
        if (entry.key == QStringLiteral("tags") || entry.key == QStringLiteral("tag")) {
            kind = QStringLiteral("tags");
            for (QString &value : values)
                value = cleanMetadataValue(value, false);
        } else if (entry.key == QStringLiteral("up")) {
            kind = QStringLiteral("links");
            for (QString &value : values)
                value = cleanMetadataValue(value, true);
        } else if (entry.isList) {
            kind = QStringLiteral("list");
            for (QString &value : values)
                value = cleanMetadataValue(value, false);
        } else if (values.size() == 1
                   && (values.constFirst() == QStringLiteral("true")
                       || values.constFirst() == QStringLiteral("false"))) {
            kind = QStringLiteral("checkbox");
        } else if (values.size() == 1) {
            bool number = false;
            values.constFirst().toDouble(&number);
            static const QRegularExpression dateRe(
                QStringLiteral("^\\d{4}-\\d{2}-\\d{2}$"));
            if (number)
                kind = QStringLiteral("number");
            else if (dateRe.match(values.constFirst()).hasMatch())
                kind = QStringLiteral("date");
        }

        QString display = values.join(QStringLiteral(", "));
        if (kind == QStringLiteral("checkbox"))
            display = values.constFirst();
        rows.append(QVariantMap{
            {QStringLiteral("key"), entry.key},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("values"), values},
            {QStringLiteral("display"), display}});
    }
    return rows;
}

// A change made from the properties panel. The body did not move, so this
// mirrors the dirty-state half of editorTextChanged() without restyling text.
void Backend::frontmatterEdited() {
    emit propertiesChanged();

    const QString full = fullDocumentText();
    const bool baselineKnown = m_hasKnownFileContents
        || !m_fileUrl.isValid() || m_fileUrl.isEmpty();
    if (baselineKnown && full == m_lastKnownFileText) {
        setModified(false);
        clearRecovery();
        if (m_hasKnownFileContents)
            setStatus(QStringLiteral("Saved %1").arg(fileName()));
        else
            setStatus(QString());
        return;
    }
    setModified(true);
    setStatus(QStringLiteral("Unsaved"));
    scheduleRecovery();
    scheduleAutoSave();
}

// Replaces one key's lines, creating the key at the end when it is missing.
// Lines before and after it, comments included, are left alone.
static bool writeFrontmatterKey(QString *frontmatter, const QString &key,
                                const QStringList &block) {
    QStringList lines = frontmatterLines(*frontmatter);
    const int close = frontmatterClose(lines);
    if (close < 0)
        return false;

    const QList<FrontmatterEntry> entries = parseFrontmatter(*frontmatter);
    int first = -1;
    int last = -1;
    for (const FrontmatterEntry &entry : entries) {
        if (entry.key == key) {
            first = entry.firstLine;
            last = entry.lastLine;
            break;
        }
    }

    QStringList updated;
    for (int i = 0; i < lines.size(); ++i) {
        if (first >= 0 && i == first) {
            updated.append(block);
            i = last;
            continue;
        }
        if (first < 0 && i == close) {
            updated.append(block);
            updated.append(lines.at(i));
            continue;
        }
        updated.append(lines.at(i));
    }
    *frontmatter = composeFrontmatter(updated);
    return true;
}

static QStringList tagBlock(const QString &key, const QStringList &tags) {
    QStringList block{key + QLatin1Char(':')};
    for (const QString &tag : tags)
        block.append(QStringLiteral("  - ") + yamlScalar(tag));
    return block;
}

static QStringList linkBlock(const QString &key, const QStringList &targets) {
    QStringList block{key + QLatin1Char(':')};
    for (const QString &target : targets)
        block.append(QStringLiteral("  - \"[[") + target + QStringLiteral("]]\""));
    return block;
}

static QStringList scalarBlock(const QString &key, const QString &value) {
    return {value.isEmpty() ? key + QLatin1Char(':')
                            : key + QStringLiteral(": ") + yamlScalar(value)};
}

static QStringList valuesForBlock(const QString &value) {
    QStringList values;
    for (const QString &part : value.split(QLatin1Char(','))) {
        const QString cleaned = part.trimmed();
        if (!cleaned.isEmpty() && !values.contains(cleaned))
            values.append(cleaned);
    }
    return values;
}

bool Backend::setPropertyValue(const QString &key, const QString &value) {
    const QString cleanedKey = key.trimmed();
    if (cleanedKey.isEmpty() || cleanedKey.contains(QLatin1Char(':'))
            || cleanedKey.contains(QLatin1Char('\n')))
        return false;

    QStringList lines = frontmatterLines(m_frontmatter);
    if (frontmatterClose(lines) < 0)
        return addProperty(cleanedKey, value);

    QStringList block;
    if (cleanedKey == QStringLiteral("up")) {
        block = linkBlock(cleanedKey, valuesForBlock(value));
    } else if (cleanedKey == QStringLiteral("tags") || cleanedKey == QStringLiteral("tag")) {
        block = tagBlock(cleanedKey, valuesForBlock(value));
    } else {
        // Keep a list a list: editing a list property's text still writes
        // items rather than folding it into one comma-joined string.
        bool wasList = false;
        for (const FrontmatterEntry &entry : parseFrontmatter(m_frontmatter)) {
            if (entry.key == cleanedKey) {
                wasList = entry.isList;
                break;
            }
        }
        block = wasList ? tagBlock(cleanedKey, valuesForBlock(value))
                        : scalarBlock(cleanedKey, value);
    }

    if (!writeFrontmatterKey(&m_frontmatter, cleanedKey, block))
        return false;
    frontmatterEdited();
    return true;
}

bool Backend::addProperty(const QString &key, const QString &value) {
    const QString cleanedKey = key.trimmed();
    if (cleanedKey.isEmpty() || cleanedKey.contains(QLatin1Char(':'))
            || cleanedKey.contains(QLatin1Char('\n'))
            || value.contains(QLatin1Char('\n')))
        return false;

    if (frontmatterClose(frontmatterLines(m_frontmatter)) < 0) {
        // The note has no frontmatter yet: open one with this property.
        m_frontmatter = QStringLiteral("---\n---\n");
    }

    QStringList block;
    if (cleanedKey == QStringLiteral("up"))
        block = linkBlock(cleanedKey, valuesForBlock(value));
    else if (cleanedKey == QStringLiteral("tags") || cleanedKey == QStringLiteral("tag"))
        block = tagBlock(cleanedKey, valuesForBlock(value));
    else
        block = scalarBlock(cleanedKey, value);

    if (!writeFrontmatterKey(&m_frontmatter, cleanedKey, block))
        return false;
    frontmatterEdited();
    return true;
}

bool Backend::removeProperty(const QString &key) {
    QStringList lines = frontmatterLines(m_frontmatter);
    const int close = frontmatterClose(lines);
    if (close < 0)
        return false;

    const QList<FrontmatterEntry> entries = parseFrontmatter(m_frontmatter);
    int first = -1;
    int last = -1;
    for (const FrontmatterEntry &entry : entries) {
        if (entry.key == key) {
            first = entry.firstLine;
            last = entry.lastLine;
            break;
        }
    }
    if (first < 0)
        return false;

    QStringList updated;
    for (int i = 0; i < lines.size(); ++i) {
        if (i >= first && i <= last)
            continue;
        updated.append(lines.at(i));
    }
    m_frontmatter = frontmatterIsEmpty(updated) ? QString() : composeFrontmatter(updated);
    frontmatterEdited();
    return true;
}

bool Backend::addTag(const QString &tag) {
    QString cleaned = tag.trimmed();
    if (cleaned.startsWith(QLatin1Char('#')))
        cleaned.remove(0, 1);
    if (cleaned.isEmpty() || cleaned.contains(QRegularExpression(
            QStringLiteral("\\s|,"))))
        return false;

    if (frontmatterClose(frontmatterLines(m_frontmatter)) < 0)
        m_frontmatter = QStringLiteral("---\n---\n");

    QString tagsKey = QStringLiteral("tags");
    QStringList tags;
    for (const FrontmatterEntry &entry : parseFrontmatter(m_frontmatter)) {
        if (entry.key == QStringLiteral("tags") || entry.key == QStringLiteral("tag")) {
            tagsKey = entry.key;
            for (const QString &value : entry.values)
                tags.append(cleanMetadataValue(value, false));
            break;
        }
    }
    if (tags.contains(cleaned))
        return false;
    tags.append(cleaned);

    if (!writeFrontmatterKey(&m_frontmatter, tagsKey, tagBlock(tagsKey, tags)))
        return false;
    frontmatterEdited();
    return true;
}

bool Backend::removeTag(const QString &tag) {
    QString cleaned = tag.trimmed();
    if (cleaned.startsWith(QLatin1Char('#')))
        cleaned.remove(0, 1);

    QString tagsKey = QStringLiteral("tags");
    QStringList tags;
    bool found = false;
    for (const FrontmatterEntry &entry : parseFrontmatter(m_frontmatter)) {
        if (entry.key == QStringLiteral("tags") || entry.key == QStringLiteral("tag")) {
            tagsKey = entry.key;
            for (const QString &value : entry.values) {
                const QString current = cleanMetadataValue(value, false);
                if (current == cleaned)
                    found = true;
                else
                    tags.append(current);
            }
            break;
        }
    }
    if (!found)
        return false;

    if (tags.isEmpty()) {
        return removeProperty(tagsKey);
    }
    if (!writeFrontmatterKey(&m_frontmatter, tagsKey, tagBlock(tagsKey, tags)))
        return false;
    frontmatterEdited();
    return true;
}

bool Backend::removePropertyValue(const QString &key, const QString &value) {
    const bool link = key == QStringLiteral("up");
    QStringList values;
    bool found = false;
    for (const FrontmatterEntry &entry : parseFrontmatter(m_frontmatter)) {
        if (entry.key != key)
            continue;
        for (const QString &raw : entry.values) {
            const QString current = cleanMetadataValue(raw, link);
            if (current == value)
                found = true;
            else
                values.append(current);
        }
        break;
    }
    if (!found)
        return false;

    if (values.isEmpty())
        return removeProperty(key);

    QStringList block = link ? linkBlock(key, values) : tagBlock(key, values);
    if (!writeFrontmatterKey(&m_frontmatter, key, block))
        return false;
    frontmatterEdited();
    return true;
}

QVariantMap Backend::noteFrontmatter(const QString &relativePath) {
    if (!m_notesIndexed || m_notesRoot != notesRoot())
        refreshNoteIndex();
    const NoteMeta meta = m_noteMeta.value(relativePath);
    return QVariantMap{{QStringLiteral("ups"), meta.ups}, {QStringLiteral("tags"), meta.tags}};
}

// Where an up: target points, as an index-relative path. Beside the source
// note wins, then the vault root, then a name unique across the vault — the
// same order [[ links resolve in — so the tree nests notes where following
// their links would land.
QString Backend::resolveUpTarget(const QString &target, const QString &sourceDir) const {
    QString name = target.trimmed();
    if (name.isEmpty())
        return {};

    const int heading = name.indexOf(QLatin1Char('#'));
    if (heading >= 0)
        name.truncate(heading);
    name = name.trimmed();
    if (name.endsWith(QStringLiteral(".markdown"), Qt::CaseInsensitive))
        name.chop(9);
    else if (name.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive))
        name.chop(3);
    if (name.isEmpty() || name.contains(QStringLiteral(".."))
            || QDir::isAbsolutePath(name))
        return {};

    const QDir root(m_notesRoot);
    const QStringList suffixes{QStringLiteral(".md"), QStringLiteral(".markdown")};
    for (const QString &suffix : suffixes) {
        const QFileInfo beside(QDir(sourceDir).filePath(name + suffix));
        if (!beside.isFile())
            continue;
        const QString relative = root.relativeFilePath(beside.canonicalFilePath());
        if (!relative.startsWith(QStringLiteral("..")))
            return relative;
    }
    for (const QString &suffix : suffixes) {
        const QFileInfo atRoot(root.filePath(name + suffix));
        if (atRoot.isFile())
            return root.relativeFilePath(atRoot.canonicalFilePath());
    }

    const QString wanted = QFileInfo(name).fileName();
    QString match;
    for (const QString &path : m_notePaths) {
        if (QFileInfo(path).completeBaseName().compare(wanted, Qt::CaseInsensitive) != 0)
            continue;
        // Two notes sharing a bare name leave the parent unresolved instead
        // of nesting under the wrong one.
        if (!match.isEmpty() && match != path)
            return {};
        match = path;
    }
    return match;
}

// Generated exports, not knowledge notes: they bulk up the hierarchy without
// ever being linked as parents.
static bool excludedFromVirtTree(const QString &relativePath) {
    return relativePath == QStringLiteral("drawings")
        || relativePath.startsWith(QStringLiteral("drawings/"));
}

QVariantList Backend::virtFolderTree() {
    if (!m_notesIndexed || m_notesRoot != notesRoot())
        refreshNoteIndex();
    QVariantList tree;
    if (m_notesRoot.isEmpty())
        return tree;

    const QDir root(m_notesRoot);
    const auto byName = [](const QString &a, const QString &b) {
        const int name = QFileInfo(a).completeBaseName().compare(
            QFileInfo(b).completeBaseName(), Qt::CaseInsensitive);
        return name != 0 ? name < 0 : a.compare(b, Qt::CaseInsensitive) < 0;
    };

    QHash<QString, QStringList> childrenOf;
    QSet<QString> hasParent;
    for (const QString &path : m_notePaths) {
        if (excludedFromVirtTree(path))
            continue;
        const QString sourceDir = root.filePath(QFileInfo(path).path());
        for (const QString &up : m_noteMeta.value(path).ups) {
            const QString parent = resolveUpTarget(up, sourceDir);
            if (parent.isEmpty() || parent == path || excludedFromVirtTree(parent))
                continue;
            if (!childrenOf[parent].contains(path))
                childrenOf[parent].append(path);
            hasParent.insert(path);
        }
    }
    for (auto it = childrenOf.begin(); it != childrenOf.end(); ++it)
        std::sort(it.value().begin(), it.value().end(), byName);

    const std::function<QVariantMap(const QString &, const QSet<QString> &)> build =
        [&](const QString &path, const QSet<QString> &ancestors) {
            QVariantList kids;
            QSet<QString> next = ancestors;
            next.insert(path);
            for (const QString &child : childrenOf.value(path)) {
                // A link cycle rejoins an ancestor: show the note once, where
                // it is first reached, rather than recursing forever.
                if (next.contains(child))
                    continue;
                kids.append(build(child, next));
            }
            return QVariantMap{
                {QStringLiteral("name"), QFileInfo(path).completeBaseName()},
                {QStringLiteral("path"), path},
                {QStringLiteral("url"), QUrl::fromLocalFile(root.filePath(path))},
                {QStringLiteral("tags"), m_noteMeta.value(path).tags},
                {QStringLiteral("childCount"), childrenOf.value(path).size()},
                {QStringLiteral("children"), kids}};
        };

    // A note whose parents resolve nests under each of them; anything without
    // one — orphans included — stays a root, like VirtFolder shows them.
    QStringList roots;
    for (const QString &path : m_notePaths) {
        if (!excludedFromVirtTree(path) && !hasParent.contains(path))
            roots.append(path);
    }
    std::sort(roots.begin(), roots.end(), byName);

    QSet<QString> placed;
    const std::function<void(const QVariantList &)> markPlaced = [&](const QVariantList &rows) {
        for (const QVariant &row : rows) {
            placed.insert(row.toMap().value(QStringLiteral("path")).toString());
            markPlaced(row.toMap().value(QStringLiteral("children")).toList());
        }
    };
    for (const QString &path : roots) {
        if (placed.contains(path))
            continue;
        tree.append(build(path, QSet<QString>()));
        markPlaced(tree.constLast().toMap().value(QStringLiteral("children")).toList());
        placed.insert(path);
    }
    // A pure cycle has no roots at all; lift its members up so no note hides.
    for (const QString &path : m_notePaths) {
        if (!excludedFromVirtTree(path) && !placed.contains(path)) {
            tree.append(build(path, QSet<QString>()));
            markPlaced(tree.constLast().toMap().value(QStringLiteral("children")).toList());
            placed.insert(path);
        }
    }
    return tree;
}

QString Backend::fileTreeRoot() const {
    const QString root = notesRoot();
    return root.isEmpty() ? QDir::homePath() : root;
}

QVariantList Backend::listFolder(const QString &relativePath) const {
    if (relativePath.contains(QStringLiteral("..")) || QDir::isAbsolutePath(relativePath))
        return {};

    const QDir root(fileTreeRoot());
    const QDir folder(relativePath.isEmpty() ? root.path() : root.filePath(relativePath));
    const QFileInfoList entries = folder.entryInfoList(
        {QStringLiteral("*")}, QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
        QDir::DirsFirst | QDir::IgnoreCase | QDir::Name);

    QVariantList rows;
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        if (name.startsWith(QLatin1Char('.')))
            continue;
        const bool isDir = entry.isDir();
        const QString suffix = entry.suffix().toLower();
        if (!isDir && suffix != QStringLiteral("md") && suffix != QStringLiteral("markdown"))
            continue;
        rows.append(QVariantMap{
            {QStringLiteral("name"), isDir ? name : entry.completeBaseName()},
            {QStringLiteral("path"), root.relativeFilePath(entry.absoluteFilePath())},
            {QStringLiteral("url"), QUrl::fromLocalFile(entry.absoluteFilePath())},
            {QStringLiteral("isDir"), isDir}});
    }
    return rows;
}

QString Backend::fileTreeCurrentPath() const {
    if (!m_fileUrl.isLocalFile())
        return {};
    const QString relative =
        QDir(fileTreeRoot()).relativeFilePath(QFileInfo(m_fileUrl.toLocalFile()).absoluteFilePath());
    return relative.startsWith(QStringLiteral("..")) ? QString() : relative;
}

QVariantList Backend::backlinks() {
    if (!m_fileUrl.isLocalFile())
        return {};
    if (!m_notesIndexed || m_notesRoot != notesRoot())
        refreshNoteIndex();
    if (m_notesRoot.isEmpty())
        return {};

    const QDir root(m_notesRoot);
    const QString currentPath = QFileInfo(m_fileUrl.toLocalFile()).canonicalFilePath();
    const QString currentRelative = root.relativeFilePath(currentPath);
    if (currentRelative.startsWith(QStringLiteral("..")))
        return {};
    const QString currentName = QFileInfo(currentRelative).completeBaseName();
    const QString currentDir = QFileInfo(currentPath).absolutePath();

    QString currentStem = currentRelative;
    const int dot = currentStem.lastIndexOf(QLatin1Char('.'));
    if (dot > currentStem.lastIndexOf(QLatin1Char('/')))
        currentStem.truncate(dot);
    currentStem = currentStem.toCaseFolded();

    QHash<QString, int> nameCounts;
    for (const QString &path : m_notePaths)
        ++nameCounts[QFileInfo(path).completeBaseName().toCaseFolded()];
    const QString foldedName = currentName.toCaseFolded();
    // A bare [[name]] reaches a note beside the one holding it, at the vault
    // root, or anywhere when no other note shares the name; resolveNote
    // follows the same order.
    const bool bareNameIsUnique = nameCounts.value(foldedName) <= 1;
    const bool atRoot = !currentStem.contains(QLatin1Char('/'));

    const auto stripExtension = [](QString name) {
        if (name.endsWith(QStringLiteral(".markdown"), Qt::CaseInsensitive))
            name.chop(9);
        else if (name.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive))
            name.chop(3);
        return name;
    };
    // Whether a link written in the note at sourceDir points at the open one.
    const auto pointsHere = [&](QString target, const QString &sourceDir) {
        const int heading = target.indexOf(QLatin1Char('#'));
        if (heading >= 0)
            target.truncate(heading);
        target = stripExtension(target.trimmed()).toCaseFolded();
        if (target.isEmpty())
            return false;
        if (!target.contains(QLatin1Char('/')))
            return target == foldedName
                && (bareNameIsUnique || atRoot || sourceDir == currentDir);
        if (target == currentStem)
            return true;
        const QString relativeToSource =
            root.relativeFilePath(QDir(sourceDir).filePath(target)).toCaseFolded();
        return relativeToSource == currentStem;
    };

    static const QRegularExpression wikiRe(QStringLiteral("\\[\\[([^\\]\\n]+)\\]\\]"));
    static const QRegularExpression markdownLinkRe(
        QStringLiteral("\\[[^\\]\\n]*\\]\\(\\s*(?:<([^>\\n]+)>|([^\\s()<>]+))[^)\\n]*\\)"));
    static const QRegularExpression fenceRe(QStringLiteral("^\\s{0,3}(```|~~~)"));

    QVariantList sources;
    for (const QString &path : m_notePaths) {
        if (path == currentRelative)
            continue;
        QFile file(root.filePath(path));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString text = QString::fromUtf8(file.readAll());
        // Cheap rejection: a link has to spell the note's name somewhere.
        if (!text.contains(currentName, Qt::CaseInsensitive))
            continue;

        const QString sourceDir = QFileInfo(root.filePath(path)).absolutePath();
        QVariantList mentions;
        bool inFence = false;
        int lineNumber = 0;
        for (const QStringView line : QStringView(text).split(QLatin1Char('\n'))) {
            ++lineNumber;
            const QString lineText = line.toString();
            if (fenceRe.match(lineText).hasMatch()) {
                inFence = !inFence;
                continue;
            }
            if (inFence)
                continue;

            bool linked = false;
            QRegularExpressionMatchIterator wiki = wikiRe.globalMatch(lineText);
            while (wiki.hasNext() && !linked) {
                QString target = wiki.next().captured(1);
                const int pipe = target.indexOf(QLatin1Char('|'));
                if (pipe >= 0)
                    target.truncate(pipe);
                linked = pointsHere(target, sourceDir);
            }
            QRegularExpressionMatchIterator markdown = markdownLinkRe.globalMatch(lineText);
            while (markdown.hasNext() && !linked) {
                const QRegularExpressionMatch match = markdown.next();
                const QString destination = QUrl::fromPercentEncoding(
                    (match.capturedStart(1) >= 0 ? match.captured(1) : match.captured(2))
                        .toUtf8());
                if (destination.contains(QStringLiteral("://"))
                        || !(destination.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive)
                             || destination.contains(QStringLiteral(".md#"), Qt::CaseInsensitive)
                             || destination.endsWith(QStringLiteral(".markdown"),
                                                     Qt::CaseInsensitive)))
                    continue;
                // Markdown links are paths from the note holding them.
                QString relative = destination.left(destination.indexOf(QLatin1Char('#')) < 0
                                                        ? destination.size()
                                                        : destination.indexOf(QLatin1Char('#')));
                relative = QDir::cleanPath(QDir(sourceDir).filePath(relative));
                linked = QFileInfo(relative).canonicalFilePath() == currentPath;
            }
            if (!linked)
                continue;

            QString shown = lineText.trimmed();
            if (shown.size() > 240)
                shown = shown.left(240) + QStringLiteral("…");
            mentions.append(QVariantMap{{QStringLiteral("line"), lineNumber},
                                        {QStringLiteral("text"), shown}});
        }
        if (mentions.isEmpty())
            continue;

        sources.append(QVariantMap{
            {QStringLiteral("name"), QFileInfo(path).completeBaseName()},
            {QStringLiteral("path"), path},
            {QStringLiteral("url"), QUrl::fromLocalFile(root.filePath(path))},
            {QStringLiteral("count"), int(mentions.size())},
            {QStringLiteral("mentions"), mentions}});
    }
    // m_notePaths is already sorted by name, so the sources are too.
    return sources;
}

QUrl Backend::resolveNote(const QString &target) const {
    QString name = target.trimmed();
    if (name.isEmpty())
        return {};

    const int heading = name.indexOf(QLatin1Char('#'));
    if (heading >= 0)
        name.truncate(heading);
    name = name.trimmed();
    if (name.endsWith(QStringLiteral(".markdown"), Qt::CaseInsensitive))
        name.chop(9);
    else if (name.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive))
        name.chop(3);
    if (name.isEmpty() || name.contains(QStringLiteral(".."))
            || QDir::isAbsolutePath(name))
        return {};

    const QStringList suffixes{QStringLiteral(".md"), QStringLiteral(".markdown")};
    const QDir current = m_fileUrl.isLocalFile()
        ? QFileInfo(m_fileUrl.toLocalFile()).absoluteDir()
        : QDir(notesRoot());
    for (const QString &suffix : suffixes) {
        const QFileInfo beside(current.filePath(name + suffix));
        if (beside.exists() && beside.isFile())
            return QUrl::fromLocalFile(beside.canonicalFilePath());
    }

    const QString root = notesRoot();
    if (root.isEmpty())
        return {};
    for (const QString &suffix : suffixes) {
        const QFileInfo atRoot(QDir(root).filePath(name + suffix));
        if (atRoot.exists() && atRoot.isFile())
            return QUrl::fromLocalFile(atRoot.canonicalFilePath());
    }

    // Obsidian resolves a bare name anywhere in the vault. Ambiguity is not
    // guessed at: two notes with the same name leave the link unresolved.
    const QString wanted = QFileInfo(name).fileName();
    QString match;
    QDirIterator iterator(root, {QStringLiteral("*.md"), QStringLiteral("*.markdown")},
                          QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        if (QFileInfo(iterator.fileName()).completeBaseName().compare(wanted,
                                                                      Qt::CaseInsensitive) != 0)
            continue;
        const QString candidate = iterator.fileInfo().canonicalFilePath();
        if (!match.isEmpty() && match != candidate)
            return {};
        match = candidate;
    }
    return match.isEmpty() ? QUrl() : QUrl::fromLocalFile(match);
}

QVariantMap Backend::wikiLinkAt(int position) const {
    const QVariantMap none{{QStringLiteral("kind"), QStringLiteral("none")}};
    if (!m_document)
        return none;

    const QTextBlock block =
        m_document->findBlock(qBound(0, position, m_document->characterCount() - 1));
    if (!block.isValid() || block.userState() >= MarkdownHighlighter::InsideFence
            || block.previous().userState() >= MarkdownHighlighter::InsideFence)
        return none;

    const QString target =
        MarkdownHighlighter::wikiLinkTargetAt(block.text(), position - block.position());
    if (target.isEmpty())
        return none;

    const QUrl url = resolveNote(target);
    if (url.isEmpty()) {
        return {{QStringLiteral("kind"), QStringLiteral("missing")},
                {QStringLiteral("target"), target}};
    }
    return {{QStringLiteral("kind"), QStringLiteral("file")},
            {QStringLiteral("target"), target},
            {QStringLiteral("url"), url}};
}

void Backend::reportMissingNote(const QString &target) {
    setStatus(QStringLiteral("No note named %1").arg(target));
}

// Where a pasted image goes. In an Obsidian vault that is the vault's own
// attachment folder setting, so Obsidian finds what Omawrite pastes:
// "/" or unset is the vault root, "./x" sits beside the note, and anything
// else is a folder in the vault. Elsewhere it is _attachments beside the
// document, and an untitled one outside a vault has no folder yet.
QString Backend::attachmentDirectory(bool *vault) const {
    const QString root = notesRoot();
    const bool isVault = !root.isEmpty()
        && QDir(QDir(root).filePath(QStringLiteral(".obsidian"))).exists();
    if (vault)
        *vault = isVault;
    const QString documentDir = m_fileUrl.isLocalFile()
        ? QFileInfo(m_fileUrl.toLocalFile()).absolutePath() : QString();

    if (!isVault)
        return documentDir.isEmpty()
            ? QString() : QDir(documentDir).filePath(QStringLiteral("_attachments"));

    QString setting;
    QFile appConfig(QDir(root).filePath(QStringLiteral(".obsidian/app.json")));
    if (appConfig.open(QIODevice::ReadOnly)) {
        setting = QJsonDocument::fromJson(appConfig.readAll()).object()
                      .value(QStringLiteral("attachmentFolderPath")).toString().trimmed();
    }
    if (setting.isEmpty() || setting == QStringLiteral("/"))
        return root;
    if (setting == QStringLiteral(".") || setting.startsWith(QStringLiteral("./"))) {
        const QString base = documentDir.isEmpty() ? root : documentDir;
        return QDir::cleanPath(base + QLatin1Char('/') + setting.mid(1));
    }
    return QDir::cleanPath(root + QLatin1Char('/') + setting);
}

// A file name in folder that is not taken yet: "name.png", then "name 1.png".
static QString freeFileName(const QDir &folder, const QString &wanted) {
    const QFileInfo info(wanted);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    QString name = wanted;
    for (int i = 1; QFileInfo::exists(folder.filePath(name)); ++i)
        name = QStringLiteral("%1 %2%3").arg(base).arg(i).arg(suffix);
    return name;
}

// Saves what the clipboard holds as an image file and returns the Markdown
// that embeds it. A screenshot or a browser's "Copy Image" arrives as image
// data; a file manager's copy as file URLs, which are copied in. Text on the
// clipboard wins over image data, so a spreadsheet's cells paste as text
// rather than as a picture of them.
QVariantMap Backend::pasteClipboardImage() {
    const QVariantMap notHandled{{QStringLiteral("handled"), false}};
    const QVariantMap handledNothing{{QStringLiteral("handled"), true},
                                     {QStringLiteral("text"), QString()}};
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *mimeData = clipboard ? clipboard->mimeData() : nullptr;
    if (!mimeData)
        return notHandled;

    QStringList sourceFiles;
    if (mimeData->hasUrls()) {
        for (const QUrl &url : mimeData->urls()) {
            if (!url.isLocalFile())
                continue;
            const QFileInfo info(url.toLocalFile());
            if (info.isFile() && MarkdownHighlighter::isImageFileName(info.fileName()))
                sourceFiles.append(info.absoluteFilePath());
        }
    }
    const bool imageData = sourceFiles.isEmpty() && mimeData->hasImage() && !mimeData->hasText();
    if (sourceFiles.isEmpty() && !imageData)
        return notHandled;

    bool vault = false;
    const QString folderPath = attachmentDirectory(&vault);
    if (folderPath.isEmpty()) {
        setStatus(QStringLiteral("Save the document first, so the image has a folder to go in."));
        return handledNothing;
    }
    if (!QDir().mkpath(folderPath)) {
        setStatus(QStringLiteral("Could not create %1.").arg(folderPath));
        return handledNothing;
    }
    const QDir folder(folderPath);
    const QString root = notesRoot();

    QStringList written;
    if (imageData) {
        // PNG bytes as the source sent them; anything else is encoded here.
        QByteArray bytes = mimeData->data(QStringLiteral("image/png"));
        if (bytes.isEmpty()) {
            const QImage image = qvariant_cast<QImage>(mimeData->imageData());
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            if (image.isNull() || !image.save(&buffer, "PNG")) {
                setStatus(QStringLiteral("Could not read the image on the clipboard."));
                return handledNothing;
            }
        }
        // Obsidian's own name for a pasted image, so a vault reads alike
        // whichever app pasted into it.
        const QString name = freeFileName(folder, QStringLiteral("Pasted image %1.png")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss"))));
        QSaveFile file(folder.filePath(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()
                || !file.commit()) {
            setStatus(QStringLiteral("Could not save %1.").arg(name));
            return handledNothing;
        }
        written.append(folder.filePath(name));
    } else {
        for (const QString &source : sourceFiles) {
            // An image already in the vault is linked where it is.
            if (vault && !QDir(root).relativeFilePath(source).startsWith(QStringLiteral(".."))) {
                written.append(source);
                continue;
            }
            const QString name = freeFileName(folder, QFileInfo(source).fileName());
            if (!QFile::copy(source, folder.filePath(name))) {
                setStatus(QStringLiteral("Could not copy %1.").arg(QFileInfo(source).fileName()));
                return handledNothing;
            }
            written.append(folder.filePath(name));
        }
    }

    m_imagePaths.clear();
    QStringList embeds;
    for (const QString &path : written) {
        if (vault) {
            // Obsidian finds a bare name anywhere in the vault; a file
            // outside the attachment folder keeps its path to stay unambiguous.
            const QString target = QFileInfo(path).absolutePath() == folder.absolutePath()
                ? QFileInfo(path).fileName() : QDir(root).relativeFilePath(path);
            embeds.append(QStringLiteral("![[%1]]").arg(target));
        } else {
            const QString relative =
                QFileInfo(m_fileUrl.toLocalFile()).absoluteDir().relativeFilePath(path);
            embeds.append(QStringLiteral("![](%1)").arg(
                QString::fromUtf8(QUrl::toPercentEncoding(relative, "/"))));
        }
    }
    setStatus(written.size() == 1 ? QStringLiteral("Pasted %1").arg(QFileInfo(written.first()).fileName())
                                  : QStringLiteral("Pasted %1 images").arg(written.size()));
    return {{QStringLiteral("handled"), true},
            {QStringLiteral("text"), embeds.join(QLatin1Char('\n'))}};
}

// The file an embed names. A Markdown path is relative to the document; an
// Obsidian name is looked for beside it, in the attachment folder, at the
// vault root, and then anywhere in the vault, as Obsidian does.
QString Backend::resolveImage(const QString &target, bool wiki) const {
    const QString documentDir = m_fileUrl.isLocalFile()
        ? QFileInfo(m_fileUrl.toLocalFile()).absolutePath() : QString();
    const QString key = documentDir + QLatin1Char('\n') + (wiki ? QLatin1Char('w') : QLatin1Char('m'))
        + target;
    const auto cached = m_imagePaths.constFind(key);
    if (cached != m_imagePaths.constEnd() && (cached->isEmpty() || QFileInfo::exists(*cached)))
        return *cached;

    const auto existing = [](const QString &path) {
        const QFileInfo info(path);
        return info.isFile() ? info.absoluteFilePath() : QString();
    };
    const QString root = notesRoot();
    const QString base = documentDir.isEmpty() ? root : documentDir;
    QString found;

    if (!wiki) {
        QString path = target;
        const QUrl url(target);
        if (url.isLocalFile())
            path = url.toLocalFile();
        else if (!url.scheme().isEmpty() && url.scheme().size() > 1)
            path.clear(); // A web image is not fetched.
        else
            path = QUrl::fromPercentEncoding(target.toUtf8());
        if (!path.isEmpty()) {
            if (path.startsWith(QStringLiteral("~/")))
                path = QDir::home().filePath(path.mid(2));
            found = QDir::isAbsolutePath(path) ? existing(path)
                : base.isEmpty() ? QString() : existing(QDir(base).filePath(path));
        }
    } else if (!root.isEmpty() || !base.isEmpty()) {
        const QStringList folders{base, attachmentDirectory(nullptr), root};
        for (const QString &folder : folders) {
            if (!folder.isEmpty())
                found = existing(QDir(folder).filePath(target));
            if (!found.isEmpty())
                break;
        }
        if (found.isEmpty() && !root.isEmpty()) {
            const QString wanted = QFileInfo(target).fileName();
            QDirIterator iterator(root, QDir::Files, QDirIterator::Subdirectories);
            while (iterator.hasNext()) {
                const QString path = iterator.next();
                if (iterator.fileName().compare(wanted, Qt::CaseInsensitive) == 0
                        && !QDir(root).relativeFilePath(path).startsWith(QLatin1Char('.'))) {
                    found = iterator.fileInfo().absoluteFilePath();
                    break;
                }
            }
        }
    }

    m_imagePaths.insert(key, found);
    return found;
}

int Backend::imagePadding() const {
    if (!m_document)
        return 0;
    return qRound(QFontMetricsF(m_document->defaultFont()).height() * 0.3);
}

QSize Backend::blockImageSize(const QTextBlock &block, QString *path, QSize *natural) const {
    if (!block.isValid() || block.userState() >= MarkdownHighlighter::InsideFence
            || block.previous().userState() >= MarkdownHighlighter::InsideFence)
        return {};
    const MarkdownHighlighter::ImageEmbed embed = MarkdownHighlighter::imageEmbed(block.text());
    if (!embed.isValid())
        return {};
    const QString file = resolveImage(embed.target, embed.wiki);
    if (file.isEmpty())
        return {};

    auto pixels = m_imageSizes.constFind(file);
    if (pixels == m_imageSizes.constEnd())
        pixels = m_imageSizes.insert(file, QImageReader(file).size());
    if (!pixels->isValid() || pixels->isEmpty())
        return {};

    // Drawn at its size in logical pixels, so a screenshot from a HiDPI
    // screen comes back at the size it was on screen, never wider than the
    // writing column, and no taller than that column is wide.
    const qreal ratio = m_parentWindow && m_parentWindow->devicePixelRatio() > 0
        ? m_parentWindow->devicePixelRatio() : 1.0;
    if (natural)
        *natural = QSize(qMax(1, qRound(pixels->width() / ratio)),
                         qMax(1, qRound(pixels->height() / ratio)));
    const qreal areaWidth = m_imageAreaWidth > 0 ? m_imageAreaWidth
        : (m_document && m_document->textWidth() > 0 ? m_document->textWidth() : 600);
    qreal width = embed.width > 0 ? embed.width : pixels->width() / ratio;
    width = qMin(width, areaWidth);
    qreal height = width * pixels->height() / pixels->width();
    if (height > areaWidth) {
        height = areaWidth;
        width = height * pixels->width() / pixels->height();
    }
    if (path)
        *path = file;
    return QSize(qMax(1, qRound(width)), qMax(1, qRound(height)));
}

qreal Backend::blockImageMargin(const QTextBlock &block) const {
    const QSize size = blockImageSize(block);
    return size.isEmpty() ? 0 : size.height() + 2 * imagePadding();
}

// Every image line, in document order, for the editor to draw them.
QVariantList Backend::imageEmbeds() const {
    QVariantList images;
    if (!m_document)
        return images;
    const int padding = imagePadding();
    for (QTextBlock block = m_document->begin(); block.isValid(); block = block.next()) {
        if (!block.text().contains(QStringLiteral("![")))
            continue;
        QString path;
        QSize natural;
        const QSize size = blockImageSize(block, &path, &natural);
        if (size.isEmpty())
            continue;
        images.append(QVariantMap{
            {QStringLiteral("blockStart"), block.position()},
            {QStringLiteral("blockEnd"), block.position() + block.length() - 1},
            {QStringLiteral("imageUrl"), QUrl::fromLocalFile(path)},
            {QStringLiteral("imageName"), QFileInfo(path).fileName()},
            {QStringLiteral("imageWidth"), size.width()},
            {QStringLiteral("imageHeight"), size.height()},
            {QStringLiteral("naturalWidth"), natural.width()},
            {QStringLiteral("naturalHeight"), natural.height()},
            {QStringLiteral("imagePad"), padding}});
    }
    return images;
}

// The image an embed under position names, so Ctrl+Click can open it.
QUrl Backend::imageUrlAt(int position) const {
    if (!m_document)
        return {};
    const QTextBlock block =
        m_document->findBlock(qBound(0, position, m_document->characterCount() - 1));
    const MarkdownHighlighter::ImageEmbed embed = MarkdownHighlighter::imageEmbed(block.text());
    const int offset = position - block.position();
    if (!embed.isValid() || offset < embed.start || offset >= embed.start + embed.length)
        return {};
    QString path;
    if (blockImageSize(block, &path).isEmpty())
        return {};
    return QUrl::fromLocalFile(path);
}

void Backend::openImage(const QUrl &url) {
    if (!url.isLocalFile() || !QDesktopServices::openUrl(url))
        setStatus(QStringLiteral("Could not open %1").arg(url.fileName()));
}

// The writing column's full width. Image lines reserve their height from it,
// so it is measured again when the font or text size changes it.
void Backend::setImageAreaWidth(int width) {
    if (width == m_imageAreaWidth)
        return;
    m_imageAreaWidth = width;
    applyHangingIndents(false);
}

int Backend::cursorPositionStep(int position, int direction) const
{
    if (!m_document || direction == 0)
        return position;

    const int documentLength = m_document->characterCount() - 1;
    position = qBound(0, position, documentLength);

    if (direction > 0) {
        if (position >= documentLength)
            return documentLength;
    } else if (position <= 0) {
        return 0;
    }

    const QTextBlock block = m_document->findBlock(position);
    if (!block.isValid())
        return position;

    const int blockStart = block.position();
    const int blockTextLength = block.text().size();
    // A block's text excludes its paragraph separator, so this is the last
    // cursor position inside the block, just before the separator.
    const int blockEnd = blockStart + blockTextLength;

    if (direction > 0) {
        // The separator is the next valid cursor position past a block; the
        // layout only knows the block's own characters.
        if (position >= blockEnd)
            return position + 1;
    } else if (position <= blockStart) {
        return position - 1;
    }

    // A fresh layout over the block's own text is always in sync with the
    // document, unlike the document's display layout, whose text() cannot be
    // checked for staleness. Building lines is unnecessary for cursor
    // stepping and dominates the cost, so the layout is left unformatted.
    // QTextLayout steps by whole grapheme clusters, so surrogate pairs, skin
    // tone modifiers and ZWJ sequences are crossed in a single move.
    QTextLayout layout(block.text());
    const int local = position - blockStart;
    const int stepped = direction > 0
        ? layout.nextCursorPosition(local, QTextLayout::SkipCharacters)
        : layout.previousCursorPosition(local, QTextLayout::SkipCharacters);
    return blockStart + qBound(0, stepped, blockTextLength);
}

void Backend::setSearchHighlight(const QString &query, int currentMatchStart) {
    if (m_highlighter)
        m_highlighter->setSearch(query, currentMatchStart);
}

// Found by the highlighter's own matcher, so the offsets agree with what it
// paints, diacritics and all.
QVariantList Backend::searchMatches(const QString &query) const {
    QVariantList matches;
    if (query.isEmpty())
        return matches;
    for (const int start : MarkdownHighlighter::findMatches(currentDocumentText(), query))
        matches.append(start);
    return matches;
}

// One edit command, so a single undo takes the whole replacement back.
void Backend::replaceRange(int start, int end, const QString &text) {
    if (!m_document)
        return;
    const int last = m_document->characterCount() - 1;
    QTextCursor cursor(m_document);
    cursor.beginEditBlock();
    cursor.setPosition(qBound(0, start, last));
    cursor.setPosition(qBound(0, end, last), QTextCursor::KeepAnchor);
    cursor.insertText(text);
    cursor.endEditBlock();
}

// A block's state says what follows its line, so a fence line itself is only
// code on its inner side: past an opening fence's backticks or tildes, or anywhere
// before the end of a closing one.
bool Backend::insideFence(int position) const {
    if (!m_document)
        return false;
    const QTextBlock block =
        m_document->findBlock(qBound(0, position, m_document->characterCount() - 1));
    if (!block.isValid())
        return false;
    const bool opensOrContinues = block.userState() >= MarkdownHighlighter::InsideFence;
    const bool afterOpening = block.previous().userState() >= MarkdownHighlighter::InsideFence;
    const int offset = position - block.position();
    if (opensOrContinues && !afterOpening) {
        static const QRegularExpression fenceMark(QStringLiteral("[`~]"));
        return offset >= block.text().indexOf(fenceMark) + 3;
    }
    if (afterOpening && !opensOrContinues)
        return offset < block.text().length();
    return opensOrContinues;
}

QVariantMap Backend::windowGeometry() const {
    QSettings settings;
    return {{QStringLiteral("x"), settings.value(QStringLiteral("window/x"), -1)},
            {QStringLiteral("y"), settings.value(QStringLiteral("window/y"), -1)},
            {QStringLiteral("width"), settings.value(QStringLiteral("window/width"), 1280)},
            {QStringLiteral("height"), settings.value(QStringLiteral("window/height"), 820)},
            {QStringLiteral("maximized"), settings.value(QStringLiteral("window/maximized"), false)}};
}

void Backend::saveWindowGeometry(int x, int y, int width, int height, bool maximized) {
    QSettings settings;
    if (!maximized) {
        settings.setValue(QStringLiteral("window/x"), x);
        settings.setValue(QStringLiteral("window/y"), y);
        settings.setValue(QStringLiteral("window/width"), width);
        settings.setValue(QStringLiteral("window/height"), height);
    }
    settings.setValue(QStringLiteral("window/maximized"), maximized);
}

QString Backend::defaultEditorFont() {
    return QStringLiteral("IBM Plex Mono");
}

void Backend::setEditorFont(const QString &family) {
    if (family.isEmpty() || family == m_editorFont)
        return;

    m_editorFont = family;
    QSettings().setValue(editorFontSetting, family);
    emit editorFontChanged();
}

QStringList Backend::availableFonts() const {
    // Leave out icon, emoji, and other symbol fonts, along with anything that
    // cannot set the Latin text the editor is written in.
    QStringList families;
    for (const QString &family : QFontDatabase::families(QFontDatabase::Latin)) {
        if (QFontDatabase::isPrivateFamily(family)
                || QFontDatabase::writingSystems(family).contains(QFontDatabase::Symbol))
            continue;
        families.append(family);
    }
    return selectableFontFamilies(families);
}

QStringList Backend::selectableFontFamilies(const QStringList &families) {
    QStringList selectable;
    static const QRegularExpression foundrySuffix(QStringLiteral("\\s*\\[[^\\]]*\\]$"));
    for (QString family : families) {
        // The same family from two foundries is listed as "Name [Foundry]".
        family.remove(foundrySuffix);
        if (family.startsWith(QStringLiteral("Noto ")) && !keptNotoFamilies.contains(family))
            continue;
        if (genericFontAliases.contains(family, Qt::CaseInsensitive)
                || family.contains(symbolFontName))
            continue;
        if (family == defaultEditorFont() || selectable.contains(family, Qt::CaseInsensitive))
            continue;
        selectable.append(family);
    }
    std::sort(selectable.begin(), selectable.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    selectable.prepend(defaultEditorFont());
    return selectable;
}

void Backend::documentFontChanged() {
    if (m_highlighter)
        m_highlighter->refreshFont();
    applyHangingIndents(false);
}

// Splits a note into its YAML frontmatter (fences included) and the body the
// editor shows. Only a block that opens the file with --- and closes with ---
// or ... counts; a stray rule at the top stays prose.
static void splitFrontmatter(const QString &text, QString *frontmatter, QString *body) {
    *frontmatter = QString();
    *body = text;
    if (!text.startsWith(QStringLiteral("---\n")))
        return;

    int lineStart = text.indexOf(QLatin1Char('\n')) + 1;
    while (lineStart < text.size()) {
        int lineEnd = text.indexOf(QLatin1Char('\n'), lineStart);
        if (lineEnd < 0)
            lineEnd = text.size();
        const QString line = text.mid(lineStart, lineEnd - lineStart).trimmed();
        if (line == QStringLiteral("---") || line == QStringLiteral("...")) {
            const int end = lineEnd < text.size() ? lineEnd + 1 : text.size();
            *frontmatter = text.left(end);
            *body = text.mid(end);
            return;
        }
        if (lineEnd == text.size())
            break;
        lineStart = lineEnd + 1;
    }
}

void Backend::loadDocumentText(const QString &text) {
    if (!m_document) {
        setStatus(QStringLiteral("Could not attach the Markdown renderer."));
        return;
    }

    QString body;
    splitFrontmatter(text, &m_frontmatter, &body);
    m_loading = true;
    m_document->setPlainText(body);
    m_lastDocumentText = body;
    m_loading = false;

    applyDocumentTypography();
    m_wordCountTimer.stop();
    m_wordCountStale = true;
    m_wordCountAgain = false;
    setWordCount(countWords(body));
    emit propertiesChanged();
}

void Backend::setFileUrl(const QUrl &url) {
    if (m_fileUrl == url)
        return;

    m_fileUrl = url;
    m_imagePaths.clear();
    emit fileUrlChanged();
}

void Backend::setModified(bool modified) {
    if (m_modified == modified)
        return;

    m_modified = modified;
    emit modifiedChanged();
}

void Backend::setStatus(const QString &status) {
    if (m_status == status)
        return;

    m_status = status;
    emit statusChanged();
}

void Backend::saveTo(const QUrl &url) {
    if (!url.isLocalFile()) {
        setStatus(QStringLiteral("Only local files can be saved."));
        emit saveFailed();
        return;
    }

    const QString targetName = QFileInfo(url.toLocalFile()).fileName();
    QSaveFile file(url.toLocalFile());
    // Without a temporary file beside it, as in a folder only the file itself
    // may be written in, write the file in place rather than fail.
    file.setDirectWriteFallback(true);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setStatus(QStringLiteral("Could not save %1.").arg(targetName));
        emit saveFailed();
        return;
    }

    const QByteArray contents = fullDocumentText().toUtf8();
    file.write(contents);

    // QSaveFile commits by replacing the target. Stop watching the old inode
    // before that replacement so our own write is not classified as external.
    unwatchFiles();

    // commit() flushes, fsyncs, and atomically renames the temp file into place,
    // returning false (and leaving the original untouched) on any write error.
    if (!file.commit()) {
        watchCurrentFile();
        setStatus(QStringLiteral("Could not write %1.").arg(targetName));
        emit saveFailed();
        return;
    }

    setKnownFileContents(contents, true);
    m_autoSaveSuspended = false;
    m_autoSaveTimer.stop();
    setFileUrl(url);
    watchCurrentFile();
    QSettings().setValue(lastSaveDirectorySetting,
                         QFileInfo(url.toLocalFile()).absolutePath());
    setModified(false);
    setStatus(QStringLiteral("Saved %1").arg(fileName()));
    clearRecovery();
    emit saveSucceeded();
}

void Backend::scheduleRecovery() {
    m_recoveryTimer.start();
}

void Backend::scheduleAutoSave() {
    // Only auto-save documents with a file name. Untitled drafts are left to
    // manual save so auto-save never silently opens a file picker or creates a
    // file the writer did not name; crash recovery still covers them. A named
    // file that does not exist yet waits for an explicit save (see autoSave()).
    if (m_fileUrl.isLocalFile() && !m_autoSaveSuspended && !m_autoSaveHeld)
        m_autoSaveTimer.start();
}

void Backend::holdAutoSave(bool held) {
    m_autoSaveHeld = held;
    if (held)
        m_autoSaveTimer.stop();
    else if (m_modified)
        scheduleAutoSave();
}

void Backend::autoSave() {
    if (!m_modified || !m_fileUrl.isLocalFile() || m_autoSaveSuspended || m_autoSaveHeld)
        return;

    // Never write over a version we have not seen. The watcher normally
    // catches outside edits first, but it can miss some (or fire late), so
    // check the file itself right before replacing it.
    const bool exists = QFileInfo::exists(m_fileUrl.toLocalFile());
    if (!m_hasKnownFileContents) {
        // A removed file, a new one not yet saved, or a recovered draft whose
        // file was gone waits for an explicit save rather than being created.
        suspendAutoSave();
        if (exists)
            emit externalChangeDetected(false, m_modified);
        return;
    }
    if (!diskMatchesKnownContents()) {
        reportExternalChange(!exists);
        return;
    }

    saveTo(m_fileUrl);
}

void Backend::suspendAutoSave() {
    // Held until the writer reloads, keeps their version, or saves explicitly.
    m_autoSaveTimer.stop();
    m_autoSaveSuspended = true;
}

// Whether the file still holds what we last read or wrote, read as open()
// reads it so carriage returns compare equal.
bool Backend::diskMatchesKnownContents() const {
    if (!m_hasKnownFileContents)
        return false;
    QFile file(m_fileUrl.toLocalFile());
    return file.open(QIODevice::ReadOnly | QIODevice::Text)
        && file.readAll() == m_lastKnownFileContents;
}

void Backend::reportExternalChange(bool deleted) {
    // Whatever is on disk now, it is not what we last read, so the baseline is
    // unknown until the writer picks a version. A deletion counts: the old
    // text is not saved anywhere either.
    setKnownFileContents(QByteArray(), false);
    suspendAutoSave();
    emit externalChangeDetected(deleted, m_modified);
}

// Saving writes UTF-8 with plain line endings; anything else on disk would be
// rewritten by it.
bool Backend::savesUnchanged(const QByteArray &raw) {
    return !raw.contains('\r') && QString::fromUtf8(raw).toUtf8() == raw;
}

void Backend::setKnownFileContents(const QByteArray &contents, bool known) {
    m_lastKnownFileContents = contents;
    m_hasKnownFileContents = known;
    // Every caller reads in text mode or writes plain line endings, so there
    // are no carriage returns to strip.
    m_lastKnownFileText = known ? QString::fromUtf8(contents) : QString();
}

void Backend::writeRecovery() {
    if (!m_modified)
        return;
    const QString path = m_recoveryPath;
    if (path.isEmpty())
        return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    const QJsonObject recovery{{QStringLiteral("fileUrl"), m_fileUrl.toString()},
                               {QStringLiteral("text"), fullDocumentText()}};
    file.write(QJsonDocument(recovery).toJson(QJsonDocument::Compact));
    file.commit();
}

void Backend::restoreRecovery() {
    QFile file(m_recoveryPath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    if (!json.isObject() || !json.object().contains(QStringLiteral("text"))) {
        // Unreadable now means unreadable every launch; do not keep claiming it.
        file.remove();
        return;
    }
    const QJsonObject recovery = json.object();
    const QUrl recoveredUrl(recovery.value(QStringLiteral("fileUrl")).toString());
    // Before the text is laid out, so image embeds resolve against the
    // recovered document's folder.
    setFileUrl(recoveredUrl);
    loadDocumentText(recovery.value(QStringLiteral("text")).toString());
    QFile diskFile(recoveredUrl.toLocalFile());
    if (recoveredUrl.isLocalFile() && diskFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setKnownFileContents(diskFile.readAll(), true);
    } else {
        setKnownFileContents(QByteArray(), false);
    }
    // The file may have moved on since this draft was written, and the draft
    // alone cannot tell. Leave overwriting it to an explicit save.
    m_autoSaveSuspended = recoveredUrl.isLocalFile();
    watchCurrentFile();
    setModified(true);
    setStatus(QStringLiteral("Recovered unsaved changes"));
}

void Backend::clearRecovery() {
    m_recoveryTimer.stop();
    QFile::remove(m_recoveryPath);
}

void Backend::unwatchFiles() {
    m_fileCheckTimer.stop();
    const QStringList watched = m_fileWatcher.files() + m_fileWatcher.directories();
    if (!watched.isEmpty())
        m_fileWatcher.removePaths(watched);
}

// A missing file cannot be watched, so watch its folder instead to notice it
// coming back.
void Backend::watchCurrentFile() {
    unwatchFiles();
    if (!m_fileUrl.isLocalFile())
        return;
    const QFileInfo info(m_fileUrl.toLocalFile());
    m_fileMissing = !info.exists();
    if (info.exists())
        m_fileWatcher.addPath(info.filePath());
    else if (info.dir().exists())
        m_fileWatcher.addPath(info.dir().path());
}

void Backend::checkWatchedFile() {
    // Atomic saves replace the watched inode, which drops it from the
    // watcher, so re-arm it either way. Our own save is not an outside edit.
    const bool wasMissing = m_fileMissing;
    watchCurrentFile();
    if (diskMatchesKnownContents())
        return;
    // A file that was already missing has been reported, or was never there.
    // One that turns up again is an outside edit like any other, even while
    // the writer is still looking at the removal.
    if (m_fileMissing && wasMissing)
        return;
    reportExternalChange(m_fileMissing);
}

// A quoted TOML value ends at its closing quote; whatever trails it is an inline comment.
QString Backend::tomlValue(const QString &text) {
    const QString value = text.trimmed();
    if (value.isEmpty())
        return value;

    const QChar quote = value.front();
    if (quote != QLatin1Char('"') && quote != QLatin1Char('\''))
        return value;

    const int close = value.indexOf(quote, 1);
    return close < 0 ? value : value.mid(1, close - 1);
}

void Backend::loadOmarchyTheme() {
    const QString colorsPath = QDir::homePath()
        + QStringLiteral("/.local/state/omarchy/current/theme/colors.toml");
    QString themeMode;
    QString background;
    QString foreground;
    QString accent;
    QString selection;
    QString selectionBackground;
    QString selectionForeground;
    QString brightForeground;
    QString lighterBackground;
    QFile file(colorsPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;

            const int equals = line.indexOf(QLatin1Char('='));
            if (equals < 0)
                continue;

            const QString key = line.left(equals).trimmed();
            const QString value = tomlValue(line.mid(equals + 1));

            if (key == QStringLiteral("mode"))
                themeMode = value;
            else if (key == QStringLiteral("background"))
                background = value;
            else if (key == QStringLiteral("foreground"))
                foreground = value;
            else if (key == QStringLiteral("accent"))
                accent = value;
            else if (key == QStringLiteral("selection"))
                selection = value;
            else if (key == QStringLiteral("selection_background"))
                selectionBackground = value;
            else if (key == QStringLiteral("selection_foreground"))
                selectionForeground = value;
            else if (key == QStringLiteral("bright_foreground"))
                brightForeground = value;
            else if (key == QStringLiteral("lighter_background"))
                lighterBackground = value;
        }
    }

    const bool wasDark = m_darkMode;
    const QStringList previousColors{m_themeBackground, m_themeForeground, m_themeAccent,
                                     m_themeSelection, m_themeSelectionForeground,
                                     m_themeCodeBackground};

    bool themeIsDark = m_systemDarkMode;
    if (themeMode == QStringLiteral("dark")) {
        themeIsDark = true;
    } else if (themeMode == QStringLiteral("light")) {
        themeIsDark = false;
    } else {
        const QColor parsedBackground(background);
        if (parsedBackground.isValid()) {
            const double luminance = 0.299 * parsedBackground.redF()
                + 0.587 * parsedBackground.greenF() + 0.114 * parsedBackground.blueF();
            themeIsDark = luminance < 0.5;
        }
    }
    m_darkMode = themeIsDark;

    // The defaults follow the mode the theme resolved to, so a colour we cannot
    // read costs that one colour rather than the contrast of the whole editor.
    m_themeBackground = m_darkMode ? QStringLiteral("#101010") : QStringLiteral("#ffffff");
    m_themeForeground = m_darkMode ? QStringLiteral("#eeeeee") : QStringLiteral("#222324");
    m_themeAccent = m_darkMode ? QStringLiteral("#5584aa") : QStringLiteral("#2077b2");
    m_themeSelection = m_darkMode ? QStringLiteral("#186a9a") : QStringLiteral("#2077b2");

    // An unparseable colour reaches QML as black, so keep the default instead.
    // Later calls win, so each fallback is applied before what it falls back from.
    const auto applyColor = [](QString &target, const QString &value) {
        if (QColor(value).isValid())
            target = value;
    };
    applyColor(m_themeBackground, background);
    applyColor(m_themeForeground, foreground);
    applyColor(m_themeAccent, accent);
    applyColor(m_themeSelection, selectionBackground);
    applyColor(m_themeSelection, selection);
    m_themeSelectionForeground = m_themeForeground;
    applyColor(m_themeSelectionForeground, brightForeground);
    applyColor(m_themeSelectionForeground, selectionForeground);

    // Omarchy themes name a lighter background for panels like the one code
    // sits on, so use the shade the theme chose. Not every theme sets the key,
    // and a few set it to the page background, which would leave code with no
    // panel at all: mix a shade of the page towards the text for those. Mixing,
    // unlike lightening, still moves on a pure black background.
    const QColor page(m_themeBackground);
    const QColor lighter(lighterBackground);
    m_themeCodeBackground = (lighter.isValid() && lighter != page
                                 ? lighter
                                 : blend(page, QColor(m_themeForeground), 0.06)).name();

    if (m_highlighter) {
        m_highlighter->setTheme(m_darkMode, m_themeBackground, m_themeForeground,
                                m_themeAccent, m_themeCodeBackground);
    }

    if (m_darkMode != wasDark)
        emit darkModeChanged();
    const QStringList colors{m_themeBackground, m_themeForeground, m_themeAccent,
                             m_themeSelection, m_themeSelectionForeground,
                             m_themeCodeBackground};
    if (colors != previousColors)
        emit themeColorsChanged();
}

void Backend::watchOmarchyTheme() {
    const QStringList watched = m_themeWatcher.files() + m_themeWatcher.directories();
    if (!watched.isEmpty())
        m_themeWatcher.removePaths(watched);

    const QString currentDir = QDir::homePath()
        + QStringLiteral("/.local/state/omarchy/current");
    const QString themeDir = currentDir + QStringLiteral("/theme");
    const QString colorsPath = themeDir + QStringLiteral("/colors.toml");

    if (QDir(currentDir).exists())
        m_themeWatcher.addPath(currentDir);
    if (QDir(themeDir).exists())
        m_themeWatcher.addPath(themeDir);
    if (QFile::exists(colorsPath))
        m_themeWatcher.addPath(colorsPath);
}

QUrl Backend::suggestedSaveUrl() const {
    if (m_fileUrl.isLocalFile())
        return m_fileUrl;

    const QString savedDirectory = QSettings().value(lastSaveDirectorySetting).toString();
    const QDir directory = savedDirectory.isEmpty() || !QDir(savedDirectory).exists()
        ? QDir::home()
        : QDir(savedDirectory);
    return QUrl::fromLocalFile(
        directory.filePath(suggestedFileName(currentDocumentText())));
}

QString Backend::currentDocumentText() const {
    return m_document ? m_document->toPlainText() : QString();
}

int Backend::countWords(const QString &text) {
    // Only letters and numbers: PCRE2 reads \p{Han} as Script_Extensions, which
    // puts 。 、 「 and the CJK radicals in Han and would count each as a word.
    // One per thread, as loading a file counts here while a worker may too.
    thread_local const QRegularExpression cjkRe(
        QStringLiteral("(?=[\\p{L}\\p{N}])[\\p{Han}\\p{Hiragana}\\p{Katakana}]"));
    thread_local const QRegularExpression wordRe(
        QStringLiteral("[\\p{L}\\p{N}]+(?:['-][\\p{L}\\p{N}]+)*"));
    // A task marker is punctuation and state, not a word: the "x" of "[x]"
    // should count the same as the space of "[ ]".
    thread_local const QRegularExpression taskRe(
        QStringLiteral("^([ \\t]*(?:[-+*]|\\d+[.)])[ \\t]+)\\[[ xX]\\](?=[ \\t]|\\r?$)"),
        QRegularExpression::MultilineOption);

    QString counted = text;
    counted.replace(taskRe, QStringLiteral("\\1"));

    // Count unspaced Han and kana characters individually, then replace them
    // so they remain boundaries between surrounding space-delimited words.
    int count = 0;

    QRegularExpressionMatchIterator cjkIt = cjkRe.globalMatch(counted);
    while (cjkIt.hasNext()) {
        cjkIt.next();
        ++count;
    }

    QString nonCjkText = counted;
    nonCjkText.replace(cjkRe, QStringLiteral(" "));

    QRegularExpressionMatchIterator it = wordRe.globalMatch(nonCjkText);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

QString Backend::suggestedFileName(const QString &text) {
    QString name = text.section(QLatin1Char('\n'), 0, 0).trimmed();
    name.replace(QRegularExpression(QStringLiteral("[/\\x00-\\x1f\\x7f]")),
                 QStringLiteral("-"));
    name = name.left(120).trimmed();
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        name = QStringLiteral("Untitled");
    if (!name.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive))
        name += QStringLiteral(".md");
    return name;
}

void Backend::setWordCount(int words) {
    if (m_wordCount == words)
        return;

    m_wordCount = words;
    emit wordCountChanged();
}

void Backend::refreshWordCount() {
    // One count at a time; edits made while it runs get one more after it.
    if (m_wordCountWatcher.isRunning()) {
        m_wordCountAgain = true;
        return;
    }
    // editorTextChanged keeps m_lastDocumentText current, and handing the
    // worker that shared copy saves reading the whole document out again.
    m_wordCountStale = false;
    m_wordCountWatcher.setFuture(QtConcurrent::run(&Backend::countWords, m_lastDocumentText));
}

void Backend::scheduleWordCount() {
    m_wordCountTimer.start();
}

void Backend::applyDocumentTypography() {
    if (!m_document)
        return;

    // A full pass is only used for freshly loaded/attached documents, so it is
    // safe to drop undo history here (re-enabling clears the stack anyway).
    const bool undoEnabled = m_document->isUndoRedoEnabled();
    m_document->setUndoRedoEnabled(false);

    m_formattingTypography = true;
    QTextCursor cursor(m_document);
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(lineHeightFormat());
    m_formattingTypography = false;

    m_document->setUndoRedoEnabled(undoEnabled);
    applyHangingIndents(true);
}

// Block indents are measured in pixels, so a typeface change has to measure
// them again. Undo is left alone: the writer's history is worth more than the
// one formatting step this adds.
void Backend::applyHangingIndents(bool clearUndo) {
    if (!m_document)
        return;

    // One document unit per pixel, so a block's indent is the exact width of
    // its list marker rather than a multiple of Qt's default 40px step.
    m_document->setIndentWidth(1.0);

    const QFontMetricsF metrics(m_document->defaultFont());
    const QFont font = m_document->defaultFont();
    const auto wantedFormat = [this, &metrics, &font](const QTextBlock &block) {
        QTextBlockFormat format = hangingIndentFormat(block.text(), metrics, font);
        format.setBottomMargin(blockImageMargin(block));
        return format;
    };
    QList<QTextBlock> changed;
    for (QTextBlock block = m_document->begin(); block.isValid(); block = block.next()) {
        const QTextBlockFormat current = block.blockFormat();

        const QTextBlockFormat wanted = wantedFormat(block);
        if (current.indent() == wanted.indent()
                && qFuzzyCompare(current.textIndent() + 1, wanted.textIndent() + 1)
                && qFuzzyCompare(current.bottomMargin() + 1, wanted.bottomMargin() + 1))
            continue;
        changed.append(block);
    }
    if (changed.isEmpty())
        return;

    const bool undoEnabled = m_document->isUndoRedoEnabled();
    if (clearUndo)
        m_document->setUndoRedoEnabled(false);

    m_formattingTypography = true;
    QTextCursor cursor(m_document);
    if (!clearUndo)
        cursor.beginEditBlock();
    for (const QTextBlock &block : changed) {
        cursor.setPosition(block.position());
        cursor.mergeBlockFormat(wantedFormat(block));
    }
    if (!clearUndo)
        cursor.endEditBlock();
    // The highlighter hides an image line's text only once room is made
    // for its image, so look at those lines again.
    if (m_highlighter) {
        for (const QTextBlock &block : changed)
            m_highlighter->rehighlightBlock(block);
    }
    m_formattingTypography = false;

    if (clearUndo)
        m_document->setUndoRedoEnabled(undoEnabled);
}

void Backend::reapplyTypographyToChange() {
    if (!m_document)
        return;

    // Format only the block(s) touched by the last edit instead of the whole
    // document, and fold the change into the preceding edit command so a single
    // undo reverts both the text and its formatting.
    const int maxPos = m_document->characterCount() - 1;
    const int start = qBound(0, m_lastChangePos, maxPos);
    const int end = qBound(start, m_lastChangePos + m_lastChangeAdded, maxPos);

    const QFontMetricsF metrics(m_document->defaultFont());
    const QFont font = m_document->defaultFont();
    const auto wantedFormat = [this, &metrics, &font](const QTextBlock &block) {
        const QTextBlockFormat indent = hangingIndentFormat(block.text(), metrics, font);
        QTextBlockFormat format = lineHeightFormat();
        format.setIndent(indent.indent());
        format.setTextIndent(indent.textIndent());
        format.setBottomMargin(blockImageMargin(block));
        return format;
    };

    // Blocks that already have it are left alone: an undo brings their format
    // back with the text, and merging anyway would push a command that clears
    // the redo stack.
    bool needed = false;
    for (QTextBlock block = m_document->findBlock(start);
         block.isValid() && block.position() <= end; block = block.next()) {
        const QTextBlockFormat current = block.blockFormat();
        const QTextBlockFormat wanted = wantedFormat(block);
        if (current.lineHeightType() != wanted.lineHeightType()
                || current.lineHeight() != wanted.lineHeight()
                || current.indent() != wanted.indent()
                || !qFuzzyCompare(current.textIndent() + 1, wanted.textIndent() + 1)
                || !qFuzzyCompare(current.bottomMargin() + 1, wanted.bottomMargin() + 1)) {
            needed = true;
            break;
        }
    }
    if (!needed)
        return;

    m_formattingTypography = true;
    QTextCursor cursor(m_document);
    cursor.joinPreviousEditBlock();
    for (QTextBlock block = m_document->findBlock(start);
         block.isValid() && block.position() <= end; block = block.next()) {
        cursor.setPosition(block.position());
        cursor.mergeBlockFormat(wantedFormat(block));
    }
    cursor.endEditBlock();
    if (m_highlighter) {
        for (QTextBlock block = m_document->findBlock(start);
             block.isValid() && block.position() <= end; block = block.next())
            m_highlighter->rehighlightBlock(block);
    }
    m_formattingTypography = false;
}
