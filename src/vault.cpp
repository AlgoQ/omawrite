#include "vault.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

namespace {

// Obsidian's own vault: the root came from a .obsidian marker, and pasted
// images follow the vault's attachmentFolderPath so Obsidian finds them too.
class ObsidianAdapter final : public Vault::Adapter {
public:
    Vault::Kind kind() const override { return Vault::Kind::Obsidian; }

    QString attachmentFolder(const Vault &vault) const override {
        const QString root = vault.root();
        QString setting;
        QFile appConfig(QDir(root).filePath(QStringLiteral(".obsidian/app.json")));
        if (appConfig.open(QIODevice::ReadOnly)) {
            setting = QJsonDocument::fromJson(appConfig.readAll()).object()
                          .value(QStringLiteral("attachmentFolderPath")).toString().trimmed();
        }
        if (setting.isEmpty() || setting == QStringLiteral("/"))
            return root;
        if (setting == QStringLiteral(".") || setting.startsWith(QStringLiteral("./"))) {
            const QString base = vault.noteDirectory().isEmpty() ? root : vault.noteDirectory();
            return QDir::cleanPath(base + QLatin1Char('/') + setting.mid(1));
        }
        return QDir::cleanPath(root + QLatin1Char('/') + setting);
    }
};

// A folder of Markdown files with no vault marker: images go beside the note,
// in a folder of their own, and remain plain relative Markdown links.
class PlainFolderAdapter final : public Vault::Adapter {
public:
    Vault::Kind kind() const override { return Vault::Kind::PlainFolder; }

    QString attachmentFolder(const Vault &vault) const override {
        const QString folder = vault.noteDirectory();
        return folder.isEmpty() ? QString()
                                : QDir(folder).filePath(QStringLiteral("_attachments"));
    }
};

} // namespace

void Vault::setNoteFile(const QString &absolutePath) {
    const QString cleaned = absolutePath.isEmpty() ? QString()
                                                   : QFileInfo(absolutePath).absoluteFilePath();
    if (m_noteFile == cleaned)
        return;
    m_noteFile = cleaned;
    m_rootKnown = false;
}

QString Vault::noteDirectory() const {
    return m_noteFile.isEmpty() ? QString() : QFileInfo(m_noteFile).absolutePath();
}

void Vault::setFallbackFolder(const QString &absolutePath) {
    const QString cleaned = absolutePath.isEmpty() ? QString()
                                                   : QDir(absolutePath).absolutePath();
    m_fallbackFolderProvider = {};
    if (m_fallbackFolder == cleaned)
        return;
    m_fallbackFolder = cleaned;
    m_rootKnown = false;
}

void Vault::setFallbackFolderProvider(FolderProvider provider) {
    m_fallbackFolderProvider = std::move(provider);
    m_rootKnown = false;
}

QString Vault::fallbackFolder() const {
    return m_fallbackFolderProvider ? m_fallbackFolderProvider() : m_fallbackFolder;
}

// A slipbox is often nested (ai/, drawings/), and [[links]] resolve across the
// whole vault, so the marker is looked for above the note rather than expected
// beside it.
QString Vault::computeRoot() const {
    const QString start = noteDirectory().isEmpty() ? fallbackFolder() : noteDirectory();
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

QString Vault::root() const {
    if (!m_rootKnown) {
        m_root = computeRoot();
        m_rootKnown = true;
    }
    return m_root;
}

bool Vault::refreshRoot() const {
    const bool wasKnown = m_rootKnown;
    const QString previous = m_root;
    m_rootKnown = false;
    m_root = computeRoot();
    m_rootKnown = true;
    return !wasKnown || previous != m_root;
}

Vault::Kind Vault::kind() const {
    const QString path = root();
    return !path.isEmpty() && QDir(QDir(path).filePath(QStringLiteral(".obsidian"))).exists()
        ? Kind::Obsidian : Kind::PlainFolder;
}

const Vault::Adapter &Vault::adapter() const {
    static const ObsidianAdapter obsidian;
    static const PlainFolderAdapter plain;
    return kind() == Kind::Obsidian
        ? static_cast<const Vault::Adapter &>(obsidian)
        : static_cast<const Vault::Adapter &>(plain);
}

QString Vault::treeRoot() const {
    const QString path = root();
    return path.isEmpty() ? QDir::homePath() : path;
}

QString Vault::relativePath(const QString &absolutePath) const {
    const QString path = root();
    if (path.isEmpty() || absolutePath.isEmpty())
        return {};

    const QString relative =
        QDir(path).relativeFilePath(QDir::cleanPath(absolutePath));
    if (relative == QStringLiteral(".") || relative == QStringLiteral("..")
            || relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative))
        return {};
    return relative;
}

bool Vault::contains(const QString &absolutePath) const {
    return !relativePath(absolutePath).isEmpty();
}

QString Vault::absolutePath(const QString &relativePath) const {
    const QString path = root();
    if (path.isEmpty() || relativePath.isEmpty() || QDir::isAbsolutePath(relativePath)
            || relativePath.contains(QStringLiteral("..")))
        return {};
    return QDir(path).filePath(relativePath);
}

QString Vault::treeRelativePath(const QString &absolutePath) const {
    const QString path = treeRoot();
    if (path.isEmpty() || absolutePath.isEmpty())
        return {};

    const QString relative =
        QDir(path).relativeFilePath(QDir::cleanPath(absolutePath));
    if (relative == QStringLiteral(".") || relative == QStringLiteral("..")
            || relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative))
        return {};
    return relative;
}

QString Vault::sourceDirectory() const {
    const QString folder = noteDirectory();
    return folder.isEmpty() ? root() : folder;
}

QString Vault::documentFolder() const {
    const QString folder = noteDirectory();
    return folder.isEmpty() ? fallbackFolder() : folder;
}

QString Vault::resolveNote(const QString &target, const QString &sourceDir,
                           const QStringList &knownNotes) const {
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

    const QString path = root();
    if (path.isEmpty())
        return {};

    const QDir rootDirectory(path);
    const QString source = sourceDir.isEmpty() ? path : sourceDir;
    const QStringList suffixes{QStringLiteral(".md"), QStringLiteral(".markdown")};
    // Obsidian finds a note beside the one holding the link first, and a
    // canonical path keeps a symlinked folder from reading as outside.
    for (const QString &suffix : suffixes) {
        const QFileInfo beside(QDir(source).filePath(name + suffix));
        if (!beside.isFile())
            continue;
        const QString relative = relativePath(beside.canonicalFilePath());
        if (!relative.isEmpty())
            return relative;
    }
    for (const QString &suffix : suffixes) {
        const QFileInfo atRoot(rootDirectory.filePath(name + suffix));
        if (atRoot.isFile())
            return relativePath(atRoot.canonicalFilePath());
    }

    // Obsidian resolves a bare name anywhere in the vault. Ambiguity is not
    // guessed at: two notes with the same name leave the link unresolved.
    const QString wanted = QFileInfo(name).fileName();
    QString match;
    for (const QString &note : knownNotes) {
        if (QFileInfo(note).completeBaseName().compare(wanted, Qt::CaseInsensitive) != 0)
            continue;
        if (!match.isEmpty() && match != note)
            return {};
        match = note;
    }
    return match;
}

QVariantList Vault::listFolder(const QString &relativePath) const {
    if (relativePath.contains(QStringLiteral("..")) || QDir::isAbsolutePath(relativePath))
        return {};

    const QDir root(treeRoot());
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

QString Vault::attachmentDirectory(bool *vault) const {
    const bool isVault = kind() == Kind::Obsidian;
    if (vault)
        *vault = isVault;
    return adapter().attachmentFolder(*this);
}
