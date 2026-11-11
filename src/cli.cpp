#include "cli.h"

#include <QDir>
#include <QFileInfo>
#include <QTextStream>

#include <unistd.h>

QString Cli::usage() {
    return QStringLiteral(
        "Usage: omawrite [FILE]\n"
        "\n"
        "Opens FILE, a Markdown file, or an empty document without one. Omawrite\n"
        "is a graphical app and keeps running until its window is closed.\n"
        "\n"
        "Options:\n"
        "  -h, --help     Show this message and exit\n"
        "  -v, --version  Show the version and exit\n"
        "  --update       Rebuild and reinstall Omawrite from its source checkout, so\n"
        "                 changes made there can be tried right away. Asks for sudo;\n"
        "                 further arguments go to makepkg (e.g. --noconfirm).\n");
}

QString Cli::sourceDirectory() {
    QString directory = qEnvironmentVariable("OMAWRITE_SOURCE_DIR");
#ifdef OMAWRITE_SOURCE_DIR
    if (directory.isEmpty())
        directory = QStringLiteral(OMAWRITE_SOURCE_DIR);
#endif
    if (directory.isEmpty())
        return {};
    const QFileInfo script(QDir(directory).filePath(QStringLiteral("bin/install")));
    return script.isFile() && script.isExecutable() ? QDir(directory).absolutePath() : QString();
}

// Hands the terminal over to bin/install (build, then makepkg -fsi) so its
// output and sudo prompt work as they do when run by hand. Only returns when
// there is nothing to run, with the exit code to report.
static int runUpdate(const QStringList &extraArguments) {
    const QString directory = Cli::sourceDirectory();
    if (directory.isEmpty()) {
        QTextStream(stderr)
            << "omawrite: cannot find the source checkout to update from.\n"
               "Set OMAWRITE_SOURCE_DIR to the folder that holds bin/install.\n";
        return 1;
    }

    QTextStream(stdout) << QStringLiteral("Updating omawrite from %1\n").arg(directory)
                        << Qt::flush;
    const QByteArray script = QDir(directory).filePath(QStringLiteral("bin/install")).toLocal8Bit();
    QList<QByteArray> arguments{QByteArrayLiteral("sh"), script};
    for (const QString &argument : extraArguments)
        arguments.append(argument.toLocal8Bit());
    QList<char *> argv;
    for (QByteArray &argument : arguments)
        argv.append(argument.data());
    argv.append(nullptr);

    execv("/bin/sh", argv.data());
    QTextStream(stderr) << "omawrite: could not run " << script << "\n";
    return 1;
}

std::optional<int> Cli::handleArguments(const QStringList &arguments, const QString &version) {
    if (arguments.size() < 2)
        return std::nullopt;

    const QString &first = arguments.at(1);
    if (first == QLatin1String("-h") || first == QLatin1String("--help")) {
        QTextStream(stdout) << usage();
        return 0;
    }
    if (first == QLatin1String("-v") || first == QLatin1String("--version")) {
        QTextStream(stdout) << QStringLiteral("omawrite %1\n").arg(version);
        return 0;
    }

    if (first == QLatin1String("--update"))
        return runUpdate(arguments.mid(2));

    return std::nullopt;
}

std::optional<int> Cli::handleArguments(int argc, char *argv[], const QString &version) {
    QStringList arguments;
    arguments.reserve(argc);
    for (int index = 0; index < argc; ++index)
        arguments.append(QString::fromLocal8Bit(argv[index]));

    return handleArguments(arguments, version);
}
