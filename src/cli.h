#pragma once

#include <QString>
#include <QStringList>
#include <optional>

// Command line handling that runs before the GUI starts, so `omawrite --help`
// and `omawrite --version` answer in the terminal instead of opening a window.
namespace Cli {

QString usage();

// The checkout `omawrite --update` rebuilds from: OMAWRITE_SOURCE_DIR from the
// environment, else the directory this binary was built in. Empty when that
// folder has no bin/install, e.g. after the checkout was moved or deleted.
QString sourceDirectory();

// The exit code main should return when the arguments are answered without
// starting the app, or nothing when Omawrite should open as usual. Anything
// else, Qt's own options included, is left for the app.
std::optional<int> handleArguments(const QStringList &arguments, const QString &version);
std::optional<int> handleArguments(int argc, char *argv[], const QString &version);

}
