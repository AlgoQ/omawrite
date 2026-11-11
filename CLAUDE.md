# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Omawrite is a distraction-free Markdown writing app for Omarchy, built with Qt 6 (Qt Quick/QML front end, C++ back end) and packaged for Arch.

## Commands

```sh
./bin/build                 # qmake6 + make into build/ → build/omawrite
./bin/test                  # builds tests/tests.pro into build-tests/ and runs it offscreen
./build/omawrite FILE.md    # run the local build
./bin/install               # build + makepkg -fsi (installs system package, asks for sudo)
```

Run a single test (after `./bin/test` has built once):

```sh
cd build-tests && make -j"$(nproc)" && QT_QPA_PLATFORM=offscreen ./tst_omawrite <testFunctionName>
./tst_omawrite -functions   # list test names
```

There is no linter. The version is the `OMAWRITE_VERSION` define in `src/main.cpp` (also in `pkgbuild/PKGBUILD`).

## Architecture

- **`src/main.cpp`** creates one `Backend` and one `SystemTheme`, exposes the backend to QML as the context property `backend`, and loads `qrc:/Main.qml`. Every window is a separate process (`Ctrl+N` → `Backend::newWindow` spawns a new instance).
- **`src/Main.qml`** holds almost all of the UI: the `TextEdit` editor (`objectName: "sourceEditor"`), its `Keys.onPressed` handling (paste, smart Return, list indent, wiki popup navigation), search/replace bar, quick-open, `[[` wiki suggestions, task checkboxes drawn over hidden `[ ]` markers, the inline properties panel (`objectName: "propertiesPanel"`, above the editor), footer, and the portal file dialogs. `EditorMutations.js` holds pure text-edit helpers (wrap/unwrap emphasis, range replace as one undo step).
- **`Backend` (`src/backend.{h,cpp}`)** owns the document state. In `Component.onCompleted` the editor calls `backend.attachDocument(textDocument)`, so the backend works directly on the editor's `QTextDocument` — loading (`loadDocumentText`), saving, auto-save, crash-recovery drafts (lock file + recovery path), external-change watching, word count on a worker thread, print/copy-as-HTML, clipboard helpers, the notes index for `[[` links, and Omarchy theme loading (`~/.config/omarchy/current/theme`, watched for changes).
  - QML signals the backend on each edit via `backend.editorTextChanged()`; that drives modified/status, recovery, auto-save, and `reapplyTypographyToChange()`.
  - Typography (line height, hanging indents for list items in pixel units) lives in **block formats**, not in the text. Edits made with `m_formattingTypography`/`m_loading` set are ignored by change tracking. Formatting changes are folded into the preceding edit block (`joinPreviousEditBlock`) so one undo reverts text + formatting.
  - The saved file is always the editor's plain text (`TextEdit.PlainText`) with the note's YAML frontmatter in front of it; there is no rich document model.
  - **Properties**: `loadDocumentText` splits a leading `---` … `---`/`...` block out of the file and keeps it in `m_frontmatter` (fences included); the editor holds only the body, and `fullDocumentText()` joins the two again for save, auto-save, recovery drafts, and the dirty check. `documentProperties()` parses that block into rows (`key`, `kind` of `tags`/`links`/`list`/`checkbox`/`number`/`date`/`text`, `values`, `display`) and the inline panel in Main.qml draws them. Edits go through `addTag`/`removeTag`/`addProperty`/`removeProperty`/`setPropertyValue`/`removePropertyValue`, which rewrite only the named key's lines (spliced by `writeFrontmatterKey`) and call `frontmatterEdited()` to mark the document dirty and schedule save. Comments, key order, and keys the panel does not understand are preserved. A file with no frontmatter stays body-only until a property is added, which opens a fenced block for it.
- **`MarkdownHighlighter`** (`QSyntaxHighlighter`) renders Markdown in place: it styles content and hides markers except around the caret (`setCaret`). Its static helpers (`inlineMarkup`, `linkUrlAt`, `wikiLinkTargetAt`, `taskMarker`, `imageEmbed`, `codeSpans`) are the single source of truth for span parsing — Backend uses them too, so add parsing there rather than duplicating regexes. A line holding only an image embed is drawn as the image itself while the caret is elsewhere. A table's source lines are made transparent while the caret is outside it, which hides them but keeps their line heights; the QML table is drawn over them and Backend reserves any extra height below the last line.
- **`BlockRenderer`** (`src/blockrenderer.{h,cpp}`) is the seam for blocks drawn as themselves rather than as Markdown. It parses GFM pipe tables (header, delimiter alignment, body rows, source block range), measures columns, wrapped row heights and the table's total height for the writing column, and skips fenced code. `Backend::renderedBlocks()` turns that into the editor's overlay model with a `kind` field; `Main.qml` draws each kind (`tableOverlay`, rows as `tableOverlayRow`) and shows the raw source when the caret is anywhere in the block. Images keep their older single-line overlay; a heading, image or task block can join the same model with its own `kind` and delegate. The policy is: the block containing the caret shows raw Markdown, every other rendered block draws itself.
- **Images** paste from the clipboard as files, never as data URLs: inside an Obsidian vault they go into the vault's `attachmentFolderPath` (falling back to the vault root) and are embedded as `![[name.png]]`; beside a plain note they go into `_attachments` and are embedded as `![](relative/path.png)`. Backend reserves the image's height as a block bottom margin and Main.qml draws the picture over it. Clicking a picture opens it in `ImageViewer.qml`, a full-window viewer that steps through the note's images with `←`/`→` or its chevrons; closing it leaves the caret on the embed for editing.
- **`SystemTheme`** reads dark mode and desktop text scale (portal / GNOME `text-scaling-factor` / `omarchy display text size`) over D-Bus. Final text scale = desktop scale × user zoom (`Ctrl+=`), stored in `QSettings` (org `Omacom`, `~/.config/Omacom/omawrite.conf`).
- **`cli.cpp`** answers `--help` / `--version` before a `QApplication` exists.
- New QML files and fonts must be listed in `src/resources.qrc`; new C++ sources in both `omawrite.pro` and `tests/tests.pro`.

## Tests

`tests/tst_omawrite.cpp` is one QtTest class (`OmawriteTest`, a `friend` of `Backend` so tests can reach private members). Two styles:
- Pure unit tests on `Backend` / `MarkdownHighlighter` statics.
- UI tests that load `../src/Main.qml` from disk via `QFINDTESTDATA` into a `QQmlEngine` with a real `Backend` set as the `backend` context property, then find items by `objectName` and drive them with `QTest::keyClick`, `QMetaObject::invokeMethod`, and `QGuiApplication::clipboard()`. Anything a test needs to reach must have an `objectName`.

`QStandardPaths::setTestModeEnabled(true)` keeps settings and recovery files out of the real config.

## Conventions

- Keyboard shortcuts live in the `shortcutDefs` registry in `src/backend.cpp`: `Main.qml` binds every `Shortcut` to it, `SettingsDialog.qml` edits it, `ShortcutsDialog.qml` lists it live, and `README.md` (Shortcuts) describes the defaults. Change the registry, not the copies.
- Comments explain *why* in full sentences; follow the existing style.
- Untitled drafts are never auto-saved; files whose bytes would change on save (non-UTF-8, CRLF) open with auto-save suspended. Keep these guarantees when touching save paths.
