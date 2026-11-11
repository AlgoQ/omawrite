#include "noteindex.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <functional>

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

NoteIndex::NoteIndex(QObject *parent) : QObject(parent) {
    m_settleTimer.setSingleShot(true);
    m_settleTimer.setInterval(100);
    connect(&m_settleTimer, &QTimer::timeout, this, [this] {
        if (m_dirty)
            emit changed();
    });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
            [this] { invalidate(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this,
            [this] { invalidate(); });
}

void NoteIndex::invalidate() {
    m_dirty = true;
    if (m_loaded)
        m_settleTimer.start();
}

void NoteIndex::noteSaved(const QString &relativePath) {
    if (!m_loaded || m_root.isEmpty())
        return;
    for (Entry &entry : m_entries) {
        if (entry.path != relativePath)
            continue;
        entry.meta = readFrontmatter(QDir(m_root).filePath(relativePath));
        m_meta.insert(entry.path, entry.meta);
        return;
    }
    // A note that was not in the snapshot yet, as after a save-as into the
    // vault: let the next read walk once and place it properly.
    invalidate();
}

bool NoteIndex::refresh(const QString &root, bool force) {
    if (!force && m_loaded && !m_dirty && root == m_root)
        return false;

    m_root = root;
    m_entries.clear();
    m_paths.clear();
    m_meta.clear();
    m_nameCounts.clear();

    if (!m_root.isEmpty()) {
        const QDir rootDirectory(m_root);
        QDirIterator iterator(m_root, {QStringLiteral("*.md"), QStringLiteral("*.markdown")},
                              QDir::Files, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString relative = rootDirectory.relativeFilePath(iterator.next());
            // Obsidian keeps its settings and trash in dot folders; neither
            // holds notes the writer means to link to.
            if (relative.startsWith(QLatin1Char('.'))
                    || relative.contains(QStringLiteral("/.")))
                continue;

            Entry entry;
            entry.path = relative;
            entry.name = QFileInfo(relative).completeBaseName();
            entry.foldedName = entry.name.toCaseFolded();
            entry.foldedPath = entry.path.toCaseFolded();
            entry.meta = readFrontmatter(rootDirectory.filePath(relative));
            m_entries.append(entry);
        }

        std::sort(m_entries.begin(), m_entries.end(), [](const Entry &a, const Entry &b) {
            const int byName = a.name.compare(b.name, Qt::CaseInsensitive);
            return byName != 0 ? byName < 0 : a.path.compare(b.path, Qt::CaseInsensitive) < 0;
        });
        for (const Entry &entry : m_entries) {
            m_paths.append(entry.path);
            m_meta.insert(entry.path, entry.meta);
            ++m_nameCounts[entry.foldedName];
        }
    }

    m_dirty = false;
    m_loaded = true;
    watchFolders();
    return true;
}

NoteIndex::NoteMeta NoteIndex::meta(const QString &relativePath) const {
    return m_meta.value(relativePath);
}

QVariantList NoteIndex::suggestions(const QString &query) const {
    const QString needle = query.trimmed().toCaseFolded();
    struct Suggestion {
        QString path;
        QString name;
        QString insert;
        int score;
    };
    QList<Suggestion> matches;
    for (const Entry &entry : m_entries) {
        int score = 0;
        if (!needle.isEmpty()) {
            const int nameAt = entry.foldedName.indexOf(needle);
            const int pathAt = entry.foldedPath.indexOf(needle);
            if (nameAt < 0 && pathAt < 0)
                continue;
            score = nameAt >= 0 ? nameAt : 1000 + pathAt;
        }
        QString stem = entry.path;
        const int slash = stem.lastIndexOf(QLatin1Char('/'));
        const int dot = stem.lastIndexOf(QLatin1Char('.'));
        if (dot > slash)
            stem.truncate(dot);
        const bool duplicate = m_nameCounts.value(entry.foldedName) > 1;
        const QString insert = duplicate && stem.contains(QLatin1Char('/')) ? stem : entry.name;
        matches.append({entry.path, entry.name, insert, score});
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
             QUrl::fromLocalFile(QDir(m_root).filePath(matches[i].path))}});
    }
    return suggestions;
}

QVariantList NoteIndex::backlinks(const QString &currentRelativePath) const {
    if (m_root.isEmpty() || currentRelativePath.isEmpty()
            || currentRelativePath.startsWith(QStringLiteral("../")))
        return {};

    const QDir root(m_root);
    const QString currentFile = root.filePath(currentRelativePath);
    const QString canonical = QFileInfo(currentFile).canonicalFilePath();
    const QString currentPath = canonical.isEmpty() ? QFileInfo(currentFile).absoluteFilePath()
                                                    : canonical;
    const QString currentName = QFileInfo(currentRelativePath).completeBaseName();
    const QString currentDir = QFileInfo(currentPath).absolutePath();

    QString currentStem = currentRelativePath;
    const int dot = currentStem.lastIndexOf(QLatin1Char('.'));
    if (dot > currentStem.lastIndexOf(QLatin1Char('/')))
        currentStem.truncate(dot);
    currentStem = currentStem.toCaseFolded();

    const QString foldedName = currentName.toCaseFolded();
    // A bare [[name]] reaches a note beside the one holding it, at the vault
    // root, or anywhere when no other note shares the name; resolveNote
    // follows the same order.
    const bool bareNameIsUnique = m_nameCounts.value(foldedName) <= 1;
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
    for (const Entry &entry : m_entries) {
        if (entry.path == currentRelativePath)
            continue;
        QFile file(root.filePath(entry.path));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString text = QString::fromUtf8(file.readAll());
        // Cheap rejection: a link has to spell the note's name somewhere.
        if (!text.contains(currentName, Qt::CaseInsensitive))
            continue;

        const QString sourceDir = QFileInfo(root.filePath(entry.path)).absolutePath();
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
                const QString target = QFileInfo(relative).canonicalFilePath();
                linked = (target.isEmpty() ? QFileInfo(relative).absoluteFilePath() : target)
                    == currentPath;
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
            {QStringLiteral("name"), entry.name},
            {QStringLiteral("path"), entry.path},
            {QStringLiteral("url"), QUrl::fromLocalFile(root.filePath(entry.path))},
            {QStringLiteral("count"), int(mentions.size())},
            {QStringLiteral("mentions"), mentions}});
    }
    // The snapshot is sorted by name, so the sources are too.
    return sources;
}

NoteIndex::NoteMeta NoteIndex::readFrontmatter(const QString &absolutePath) {
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

// Every folder is watched, not just the root, so a note moved between
// folders inside the vault still reaches the next refresh. Dot folders hold
// Obsidian's own state and are not part of the link graph.
void NoteIndex::watchFolders() {
    const QStringList watched = m_watcher.directories() + m_watcher.files();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    if (m_root.isEmpty())
        return;

    QStringList folders;
    const std::function<void(const QString &)> collect = [&](const QString &path) {
        folders.append(path);
        const QFileInfoList subfolders = QDir(path).entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
        for (const QFileInfo &folder : subfolders) {
            if (folder.fileName().startsWith(QLatin1Char('.')))
                continue;
            collect(folder.absoluteFilePath());
        }
    };
    collect(m_root);
    m_watcher.addPaths(folders);
}
