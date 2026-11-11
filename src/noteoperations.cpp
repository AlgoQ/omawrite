#include "noteoperations.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

#include <functional>

namespace {

QString cleanAbsolute(const QString &path)
{
    if (path.isEmpty())
        return {};
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool samePath(const QString &a, const QString &b)
{
    return !a.isEmpty() && !b.isEmpty()
        && cleanAbsolute(a) == cleanAbsolute(b);
}

QString stripNoteExtension(QString path)
{
    if (path.endsWith(QStringLiteral(".markdown"), Qt::CaseInsensitive))
        path.chop(9);
    else if (path.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive))
        path.chop(3);
    return path;
}

QString noteExtension(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix();
    return suffix.isEmpty() ? QStringLiteral(".md") : QStringLiteral(".") + suffix;
}

// A link's target is everything before its alias and heading. Keeping those
// in place is what lets [[Note#Heading|shown as]] survive a move untouched
// around its target.
int targetEnd(const QString &inner)
{
    const int pipe = inner.indexOf(QLatin1Char('|'));
    const int heading = inner.indexOf(QLatin1Char('#'));
    if (pipe < 0)
        return heading;
    if (heading < 0)
        return pipe;
    return qMin(pipe, heading);
}

bool insideInlineCode(const QString &line, int position)
{
    int ticks = 0;
    for (int i = 0; i < position && i < line.size(); ++i) {
        if (line.at(i) == QLatin1Char('`'))
            ++ticks;
    }
    return ticks % 2 == 1;
}

// Rewrites [[targets]] in body text, leaving fenced code and inline code
// literal the way the editor shows them. `replacement` returns the new bare
// target, or an empty string when the link should stay as it is.
int rewriteWikiLinks(QString *text,
                     const std::function<QString(const QString &)> &replacement)
{
    static const QRegularExpression wikiRe(QStringLiteral("\\[\\[([^\\]\\n]+)\\]\\]"));
    QStringList lines = text->split(QLatin1Char('\n'));
    bool inFence = false;
    int changed = 0;

    for (QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QStringLiteral("```"))
                || trimmed.startsWith(QStringLiteral("~~~"))) {
            inFence = !inFence;
            continue;
        }
        if (inFence || !line.contains(QStringLiteral("[[")))
            continue;

        QString rewritten;
        int copied = 0;
        QRegularExpressionMatchIterator matches = wikiRe.globalMatch(line);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            if (insideInlineCode(line, match.capturedStart()))
                continue;

            const QString inner = match.captured(1);
            const int stop = targetEnd(inner);
            const QString beforeTarget = stop < 0 ? inner : inner.left(stop);
            const QString afterTarget = stop < 0 ? QString() : inner.mid(stop);

            int first = 0;
            while (first < beforeTarget.size() && beforeTarget.at(first).isSpace())
                ++first;
            int last = beforeTarget.size();
            while (last > first && beforeTarget.at(last - 1).isSpace())
                --last;
            const QString bare = beforeTarget.mid(first, last - first);
            if (bare.isEmpty())
                continue;

            QString fresh = replacement(bare);
            if (fresh.isEmpty() || fresh == bare)
                continue;

            rewritten += line.mid(copied, match.capturedStart() - copied);
            rewritten += QStringLiteral("[[") + beforeTarget.left(first) + fresh
                + beforeTarget.mid(last) + afterTarget + QStringLiteral("]]");
            copied = match.capturedEnd();
            ++changed;
        }
        if (copied > 0) {
            rewritten += line.mid(copied);
            line = rewritten;
        }
    }

    *text = lines.join(QLatin1Char('\n'));
    return changed;
}

// Rewrites one YAML value, keeping the whitespace and quotes around it. Flow
// lists are split on commas so each up: target can be resolved on its own.
// Bracketed links are left to rewriteWikiLinks.
int rewriteYamlValue(QString *value,
                     const std::function<QString(const QString &)> &replacement);

int rewriteYamlScalar(const QString &raw,
                      const std::function<QString(const QString &)> &replacement,
                      QString *fresh)
{
    int first = 0;
    while (first < raw.size() && raw.at(first).isSpace())
        ++first;
    int last = raw.size();
    while (last > first && raw.at(last - 1).isSpace())
        --last;
    if (first == last)
        return 0;

    const QChar quote = raw.at(first);
    const bool quoted = (quote == QLatin1Char('"') || quote == QLatin1Char('\''))
        && last - first >= 2 && raw.at(last - 1) == quote;
    const int from = quoted ? first + 1 : first;
    const int to = quoted ? last - 1 : last;
    QString inner = raw.mid(from, to - from).trimmed();
    if (inner.isEmpty() || inner.startsWith(QStringLiteral("[["))
            || inner.startsWith(QLatin1Char('{')) || inner.startsWith(QLatin1Char('[')))
        return 0;

    // An up: value can carry a heading or alias just like a wikilink, and
    // both stay around the target that is replaced.
    const int stop = targetEnd(inner);
    const QString beforeTarget = stop < 0 ? inner : inner.left(stop);
    const QString afterTarget = stop < 0 ? QString() : inner.mid(stop);
    int firstBare = 0;
    while (firstBare < beforeTarget.size() && beforeTarget.at(firstBare).isSpace())
        ++firstBare;
    int lastBare = beforeTarget.size();
    while (lastBare > firstBare && beforeTarget.at(lastBare - 1).isSpace())
        --lastBare;
    const QString bare = beforeTarget.mid(firstBare, lastBare - firstBare);
    if (bare.isEmpty())
        return 0;

    const QString replaced = replacement(bare);
    if (replaced.isEmpty() || replaced == bare)
        return 0;

    const QString updated = beforeTarget.left(firstBare) + replaced
        + beforeTarget.mid(lastBare) + afterTarget;
    *fresh = raw.left(from) + updated + raw.mid(to);
    return 1;
}

int rewriteYamlValue(QString *value,
                     const std::function<QString(const QString &)> &replacement)
{
    const QString trimmed = value->trimmed();
    if (trimmed.startsWith(QStringLiteral("[[")))
        return 0;

    if (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']'))) {
        const int open = value->indexOf(QLatin1Char('['));
        const int close = value->lastIndexOf(QLatin1Char(']'));
        if (open < 0 || close <= open)
            return 0;

        const QString inside = value->mid(open + 1, close - open - 1);
        QStringList parts = inside.split(QLatin1Char(','));
        int changed = 0;
        for (QString &part : parts) {
            QString fresh;
            changed += rewriteYamlScalar(part, replacement, &fresh);
            if (!fresh.isEmpty())
                part = fresh;
        }
        if (changed == 0)
            return 0;
        *value = value->left(open + 1) + parts.join(QStringLiteral(","))
            + value->mid(close);
        return changed;
    }

    QString fresh;
    const int changed = rewriteYamlScalar(*value, replacement, &fresh);
    if (changed > 0)
        *value = fresh;
    return changed;
}

// Bare up: values predate the [[link]] spelling and still appear in old
// notes, so they are resolved and rewritten too. Only the top-level up: key
// is touched; tags and other frontmatter are none of this module's business.
int rewriteUpValues(QString *text,
                    const std::function<QString(const QString &)> &replacement)
{
    QStringList lines = text->split(QLatin1Char('\n'));
    if (lines.isEmpty() || lines.constFirst().trimmed() != QStringLiteral("---"))
        return 0;

    int close = -1;
    for (int i = 1; i < lines.size(); ++i) {
        const QString trimmed = lines.at(i).trimmed();
        if (trimmed == QStringLiteral("---") || trimmed == QStringLiteral("...")) {
            close = i;
            break;
        }
    }
    if (close < 0)
        return 0;

    bool inUpBlock = false;
    int changed = 0;
    for (int i = 1; i < close; ++i) {
        const QString line = lines.at(i);
        const bool indented = !line.isEmpty() && line.at(0).isSpace();
        const QString trimmed = line.trimmed();

        if (!indented) {
            const int colon = line.indexOf(QLatin1Char(':'));
            if (colon < 0) {
                inUpBlock = false;
                continue;
            }
            const QString key = line.left(colon).trimmed();
            if (key != QStringLiteral("up")) {
                inUpBlock = false;
                continue;
            }

            QString value = line.mid(colon + 1);
            if (value.trimmed().isEmpty()) {
                inUpBlock = true;
                continue;
            }
            inUpBlock = false;
            changed += rewriteYamlValue(&value, replacement);
            lines[i] = line.left(colon + 1) + value;
            continue;
        }

        if (!inUpBlock)
            continue;
        if (!trimmed.startsWith(QLatin1Char('-'))) {
            inUpBlock = false;
            continue;
        }
        const int dash = line.indexOf(QLatin1Char('-'));
        QString value = line.mid(dash + 1);
        changed += rewriteYamlValue(&value, replacement);
        lines[i] = line.left(dash + 1) + value;
    }

    if (changed > 0)
        *text = lines.join(QLatin1Char('\n'));
    return changed;
}

bool readUtf8(const QString &path, QString *text, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Could not read %1.").arg(QFileInfo(path).fileName());
        return false;
    }
    const QByteArray raw = file.readAll();
    *text = QString::fromUtf8(raw);
    if (text->toUtf8() != raw) {
        *error = QStringLiteral("%1 is not UTF-8; its links were left alone.")
                     .arg(QFileInfo(path).fileName());
        return false;
    }
    return true;
}

bool writeUtf8(const QString &path, const QString &text, QString *error)
{
    QSaveFile file(path);
    // A folder that lets the note be replaced but not a temp file beside it
    // still has to accept the move, as saving does elsewhere.
    file.setDirectWriteFallback(true);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        *error = QStringLiteral("Could not write %1.").arg(QFileInfo(path).fileName());
        return false;
    }
    const QByteArray contents = text.toUtf8();
    if (file.write(contents) != contents.size() || !file.commit()) {
        *error = QStringLiteral("Could not write %1.").arg(QFileInfo(path).fileName());
        return false;
    }
    return true;
}

} // namespace

NoteOperations::NoteOperations(QString vaultRoot, QStringList notePaths)
    : m_root(cleanAbsolute(vaultRoot))
{
    for (const QString &path : notePaths) {
        const QString cleaned = QDir::cleanPath(path);
        if (!cleaned.isEmpty() && !m_notes.contains(cleaned))
            m_notes.append(cleaned);
    }
}

QString NoteOperations::uniqueDestinationPath(const QString &destinationFolder,
                                              const QString &fileName)
{
    const QDir folder(destinationFolder);
    if (!folder.exists())
        return folder.filePath(fileName);

    const QString suffix = QFileInfo(fileName).suffix();
    const QString extension = suffix.isEmpty() ? QString() : QStringLiteral(".") + suffix;
    QString stem = fileName;
    if (!extension.isEmpty())
        stem.chop(extension.size());

    QString candidate = folder.filePath(fileName);
    for (int n = 1; QFileInfo::exists(candidate); ++n)
        candidate = folder.filePath(QStringLiteral("%1 %2%3").arg(stem).arg(n).arg(extension));
    return candidate;
}

QStringList NoteOperations::absoluteNotePaths() const
{
    QStringList paths;
    const QDir root(m_root);
    for (const QString &relative : m_notes) {
        if (!relative.startsWith(QStringLiteral("..")))
            paths.append(cleanAbsolute(root.filePath(relative)));
    }
    return paths;
}

QString NoteOperations::vaultRelative(const QString &absolutePath) const
{
    if (!inVault(absolutePath))
        return {};
    return QDir(m_root).relativeFilePath(cleanAbsolute(absolutePath));
}

bool NoteOperations::inVault(const QString &absolutePath) const
{
    if (m_root.isEmpty())
        return false;
    const QString relative = QDir(m_root).relativeFilePath(cleanAbsolute(absolutePath));
    return relative != QStringLiteral("..")
        && !relative.startsWith(QStringLiteral("../"))
        && !QDir::isAbsolutePath(relative);
}

// How a [[target]] resolves from a note: beside it, then the vault root, then
// a bare name that only one note in the vault carries. This mirrors
// Backend::resolveNote and resolveUpTarget, which own the same rules for the
// tree and the editor; the module only needs the answer, not the path taken.
QString NoteOperations::findNote(const QString &target, const QString &fromPath,
                                 const QStringList &notes) const
{
    QString name = target.trimmed();
    const int stop = targetEnd(name);
    if (stop >= 0)
        name.truncate(stop);
    name = name.trimmed();
    name = stripNoteExtension(name);
    if (name.isEmpty() || name.contains(QStringLiteral(".."))
            || QDir::isAbsolutePath(name))
        return {};

    const auto matches = [&notes](const QString &candidate) {
        const QString absolute = cleanAbsolute(candidate);
        for (const QString &note : notes) {
            if (cleanAbsolute(note) == absolute)
                return true;
        }
        return false;
    };

    const QStringList suffixes{QStringLiteral(".md"), QStringLiteral(".markdown")};
    const QDir fromDir = QFileInfo(fromPath).absoluteDir();
    for (const QString &suffix : suffixes) {
        const QString beside = cleanAbsolute(fromDir.filePath(name + suffix));
        if (matches(beside))
            return beside;
    }
    const QDir root(m_root);
    for (const QString &suffix : suffixes) {
        const QString atRoot = cleanAbsolute(root.filePath(name + suffix));
        if (matches(atRoot))
            return atRoot;
    }

    const QString wanted = QFileInfo(name).fileName();
    QString found;
    for (const QString &note : notes) {
        if (QFileInfo(note).completeBaseName().compare(wanted, Qt::CaseInsensitive) != 0)
            continue;
        const QString candidate = cleanAbsolute(note);
        if (!found.isEmpty() && found != candidate)
            return {};
        found = candidate;
    }
    return found;
}

// The shortest target that still resolves to targetPath from fromPath under
// the given note set. A bare name wins when the rules find it; otherwise the
// vault-relative path does. A root note shadowed by a same-named note beside
// the link has no unambiguous spelling in the current resolver, so the bare
// name is kept as the best of what remains.
QString NoteOperations::bestTarget(const QString &targetPath, const QString &fromPath,
                                   const QStringList &notes, bool withExtension) const
{
    const QString target = cleanAbsolute(targetPath);
    const QString stem = QFileInfo(target).completeBaseName();
    const QString extension = withExtension ? noteExtension(target) : QString();

    QStringList candidates;
    candidates.append(stem + extension);
    const QString relative = stripNoteExtension(vaultRelative(target));
    if (!relative.isEmpty() && relative != stem)
        candidates.append(relative + extension);
    if (!withExtension
            && target == cleanAbsolute(QDir(m_root).filePath(stem + noteExtension(target))))
        candidates.append(QStringLiteral("./") + stem);

    for (const QString &candidate : candidates) {
        if (findNote(candidate, fromPath, notes) == target)
            return candidate;
    }
    return (relative.isEmpty() ? stem : relative) + extension;
}

NoteOperations::Result NoteOperations::moveNote(const QString &sourcePath,
                                                const QString &destinationFolder) const
{
    return transfer(sourcePath, destinationFolder, true);
}

NoteOperations::Result NoteOperations::copyNote(const QString &sourcePath,
                                                const QString &destinationFolder) const
{
    return transfer(sourcePath, destinationFolder, false);
}

NoteOperations::Result NoteOperations::transfer(const QString &sourcePath,
                                                const QString &destinationFolder,
                                                bool moving) const
{
    Result result;
    const QString source = cleanAbsolute(sourcePath);
    const QString folder = cleanAbsolute(destinationFolder);
    result.sourcePath = source;

    const QFileInfo sourceInfo(source);
    if (!sourceInfo.isFile()) {
        result.error = QStringLiteral("%1 is not a note.").arg(sourceInfo.fileName());
        return result;
    }
    // Read before any rename: QFileInfo answers for the path it was built
    // from, which stops existing when the note moves.
    const QFile::Permissions permissions = sourceInfo.permissions();
    if (folder.isEmpty()) {
        result.error = QStringLiteral("There is no destination folder.");
        return result;
    }
    if (!QDir().mkpath(folder)) {
        result.error = QStringLiteral("Could not create %1.").arg(folder);
        return result;
    }

    // Moving a note back into its own folder is already where it belongs.
    // Copying still makes another note, with the collision-safe name.
    if (moving && QFileInfo(source).absolutePath() == folder) {
        result.success = true;
        result.destinationPath = source;
        return result;
    }

    QString contents;
    // A note that is not UTF-8 cannot be rewritten as text, but it can still
    // be moved or copied byte for byte; only its own links stay stale.
    QString readError;
    const bool textReadable = readUtf8(source, &contents, &readError);

    const QString destination = uniqueDestinationPath(folder, sourceInfo.fileName());
    result.destinationPath = destination;

    QStringList beforeNotes = absoluteNotePaths();
    if (!beforeNotes.contains(source))
        beforeNotes.append(source);

    QStringList afterNotes;
    for (const QString &note : beforeNotes) {
        if (moving && samePath(note, source))
            continue;
        afterNotes.append(note);
    }
    afterNotes.append(destination);

    // Links written in the note itself: after the move they are read from the
    // new folder, so a target that used to resolve beside the old one may
    // need its vault-relative spelling.
    const QString oldFrom = source;
    const QString newFrom = destination;
    int changedLinks = 0;
    if (textReadable) {
        changedLinks += rewriteWikiLinks(&contents, [&](const QString &target) {
            const QString before = findNote(target, oldFrom, beforeNotes);
            if (before.isEmpty())
                return QString();
            const QString wanted = samePath(before, source) ? destination : before;
            const QString after = findNote(target, newFrom, afterNotes);
            if (samePath(after, wanted))
                return QString();
            const bool withExtension = target.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive)
                || target.endsWith(QStringLiteral(".markdown"), Qt::CaseInsensitive);
            return bestTarget(wanted, newFrom, afterNotes, withExtension);
        });
        changedLinks += rewriteUpValues(&contents, [&](const QString &target) {
            const QString before = findNote(target, oldFrom, beforeNotes);
            if (before.isEmpty())
                return QString();
            const QString wanted = samePath(before, source) ? destination : before;
            const QString after = findNote(target, newFrom, afterNotes);
            if (samePath(after, wanted))
                return QString();
            const bool withExtension = target.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive)
                || target.endsWith(QStringLiteral(".markdown"), Qt::CaseInsensitive);
            return bestTarget(wanted, newFrom, afterNotes, withExtension);
        });
    }

    QString error;
    bool sourceRemoved = false;
    if (textReadable) {
        if (!writeUtf8(destination, contents, &error)) {
            result.error = error;
            return result;
        }
    } else {
        result.warnings.append(readError);
        if (moving) {
            if (QFile::rename(source, destination)) {
                sourceRemoved = true;
            } else {
                // The destination can be on another filesystem, where a
                // rename is not possible; copy and remove instead.
                if (!QFile::copy(source, destination)) {
                    result.error = QStringLiteral("Could not write %1.")
                                       .arg(QFileInfo(destination).fileName());
                    return result;
                }
                if (!QFile::remove(source)) {
                    QFile::remove(destination);
                    result.error = QStringLiteral("Could not remove %1.")
                                       .arg(sourceInfo.fileName());
                    return result;
                }
                sourceRemoved = true;
            }
        } else if (!QFile::copy(source, destination)) {
            result.error = QStringLiteral("Could not write %1.")
                               .arg(QFileInfo(destination).fileName());
            return result;
        }
    }
    QFile::setPermissions(destination, permissions);
    result.changedFiles.append(destination);

    if (moving && !sourceRemoved && !QFile::remove(source)) {
        // The old note could not be taken away, so undo the new one rather
        // than leave two copies of the note behind.
        QFile::remove(destination);
        result.changedFiles.clear();
        result.error = QStringLiteral("Could not remove %1.").arg(sourceInfo.fileName());
        return result;
    }

    // Links in other notes: a move breaks the ones that named the old path;
    // a copy leaves them alone unless the new note makes a bare name
    // ambiguous, in which case they are pointed at the source explicitly.
    const QDir root(m_root);
    for (const QString &relative : m_notes) {
        if (relative.startsWith(QStringLiteral("..")))
            continue;
        const QString notePath = cleanAbsolute(root.filePath(relative));
        if (samePath(notePath, source))
            continue;

        QString text;
        if (!readUtf8(notePath, &text, &error)) {
            result.warnings.append(error);
            continue;
        }

        int changed = 0;
        changed += rewriteWikiLinks(&text, [&](const QString &target) {
            if (!samePath(findNote(target, notePath, beforeNotes), source))
                return QString();
            const QString after = findNote(target, notePath, afterNotes);
            const QString wanted = moving ? destination : source;
            if (samePath(after, wanted))
                return QString();
            const bool withExtension = target.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive)
                || target.endsWith(QStringLiteral(".markdown"), Qt::CaseInsensitive);
            return bestTarget(wanted, notePath, afterNotes, withExtension);
        });
        changed += rewriteUpValues(&text, [&](const QString &target) {
            if (!samePath(findNote(target, notePath, beforeNotes), source))
                return QString();
            const QString after = findNote(target, notePath, afterNotes);
            const QString wanted = moving ? destination : source;
            if (samePath(after, wanted))
                return QString();
            const bool withExtension = target.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive)
                || target.endsWith(QStringLiteral(".markdown"), Qt::CaseInsensitive);
            return bestTarget(wanted, notePath, afterNotes, withExtension);
        });
        if (changed == 0)
            continue;

        if (!writeUtf8(notePath, text, &error)) {
            result.warnings.append(error);
            continue;
        }
        changedLinks += changed;
        result.changedFiles.append(notePath);
    }

    result.success = true;
    result.changedLinks = changedLinks;
    return result;
}
