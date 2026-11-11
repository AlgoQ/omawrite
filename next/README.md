# Omawrite Next

A slipbox-first Markdown editor. Rust owns the vault; TypeScript owns the
writing surface; Tauri 2 is the only host. There is no Electron anywhere in
this tree.

This is the working rewrite described in
[`../docs/rewrite-spike.md`](../docs/rewrite-spike.md). The Qt app at the
repository root is still the packaged 0.7.x release; `next/` is where the next
one is being built.

## What works today

- **Slipbox vaults**: finds the nearest `.obsidian` ancestor, or treats a plain
  folder of notes as a vault. Attachment folders follow `.obsidian/app.json`.
- **Warm note index**: one walk of the vault gives paths, `up:`/`tags`/aliases,
  quick-open suggestions, backlinks, and full-text search.
- **VirtFolder tree**: built from `up:` links, with fold state, child counts,
  tags, filtering, and a current-note highlight. `drawings/` stays out.
- **CodeMirror 6 editor**: GFM Markdown, live tables, task checkboxes, images,
  wiki links, heading/emphasis/quote/code styling, and raw Markdown on the
  block holding the caret.
- **Command palette**: commands and notes in one list, with a preview of the
  selected note and keyboard navigation.
- **Tabs and history**: per-tab editor state, dirty markers, back/forward.
- **Frontmatter as properties**: tags, `up:` parents, list values, checkboxes,
  numbers, dates and text, edited through the Rust frontmatter writer so
  comments and unknown keys survive.
- **Move and copy**: tree and tab context menus, collision-safe names, inbound
  and outbound `[[wikilinks]]` and `up:` rewriting, relative Markdown path
  rewriting, and tabs/history that follow a move.
- **Notes from anywhere**: open a Markdown file outside the slipbox and move or
  copy it in without losing the link context.
- **Settings**: scroll speed (2× default), text size, theme, and rebindable
  shortcuts, stored in `~/.config/omawrite-next/settings.json`.
- **Sidebar gestures**: `Ctrl+B` toggles the file tree, `Ctrl+Shift+B` toggles
  backlinks, `Ctrl+Alt+Left` closes the file tree, and `Ctrl+Alt+Right` closes
  backlinks.
- **Print**: `Ctrl+Shift+P` prints the full note through a print-only view.
- **Theme**: follows Omarchy's `colors.toml`.

## Run

Requirements: Rust 1.90+, Node 20.19+/22.12+, and the Tauri 2 Linux packages
(`webkit2gtk-4.1`, GTK 3, `libsoup3`, `librsvg`, `openssl`, `base-devel`).

```sh
cd next
npm install
OMAWRITE_VAULT_ROOT=/path/to/slipbox npm run tauri:dev
```

Without `OMAWRITE_VAULT_ROOT`, the app uses the last vault, then
`~/Documents/slipbox`, then the working directory.

To open a note outside the vault:

```sh
OMAWRITE_NOTE=/tmp/note.md OMAWRITE_VAULT_ROOT=/path/to/slipbox \
  npm run tauri:dev
```

Or open it from the app with `Ctrl+O`; the note gets a tab and a banner offering
**Move to slipbox** / **Copy to slipbox**.

Build a package:

```sh
npm run tauri:build
# src-tauri/target/release/bundle/deb/
```

## Test

```sh
cargo test -p omawrite-core   # vault, index, frontmatter, links, note ops
npm test                      # TypeScript parsers, commands, keys
npm run build                 # type-check and bundle the frontend
```

## Shortcuts

| Action | Keys |
| --- | --- |
| Command palette | `Ctrl+P` |
| Search notes | `Ctrl+Shift+O` |
| Open a note anywhere | `Ctrl+O` |
| Save / save as | `Ctrl+S` / `Ctrl+Shift+S` |
| New note | `Ctrl+N` |
| Toggle file tree | `Ctrl+B` |
| Toggle backlinks | `Ctrl+Shift+B` |
| **Close file tree** | `Ctrl+Alt+Left` |
| **Close backlinks** | `Ctrl+Alt+Right` |
| Back / forward | `Alt+Left` / `Alt+Right` |
| Print | `Ctrl+Shift+P` |
| Settings | `Ctrl+,` |
| Bold / italic / code | `Ctrl+Alt+B` / `Ctrl+I` / `Ctrl+E` |
| Wiki link / Markdown link | `Ctrl+Shift+K` / `Ctrl+K` |

All of these are editable in Settings.

## Architecture

```
next/
  crates/omawrite-core/   Rust: vault, index, frontmatter, links, note ops
  src-tauri/              Tauri commands, watcher, settings, attachments
  src/
    editor.ts             CodeMirror 6 setup, per-tab state, paste/scroll
    live-preview/         tables, tasks, images, wiki links, markers
    tree.ts               up: tree, folds, filtering
    palette.ts            command + note palette with preview
    properties.ts         frontmatter panel
    backlinks.ts          backlink panel
    settings.ts           settings modal and key capture
    commands.ts           action registry shared by palette, keys, settings
    keymap.ts             default and user key sequences
    main.ts               session, tabs, save, moves, dialogs
```

The boundary is deliberately narrow: TypeScript never walks the vault and never
guesses how a wikilink resolves. It calls a command, receives serde-shaped
data, and renders it. The Rust core is usable without Tauri, so the same
library can later power a CLI, a sync daemon, or a test harness.

### Why not fork Helix?

Helix was the reference, not the base. Its useful ideas map cleanly onto this
stack:

| Helix | Here |
| --- | --- |
| `helix-view::Editor` + `Document` + `View` | tab state + per-tab CM6 `EditorState` |
| `MappableCommand` and the command registry | `src/commands.ts` action ids |
| keymap trie + user overrides | `keymap.ts` + settings key overrides |
| theme scopes and Omarchy colors | CSS variables from `colors.toml` |
| fuzzy picker | command palette over `suggest()` |
| rope + transactions | CM6's persistent state and transactions |
| tree-sitter | Lezer Markdown |

A fork would have meant replacing the TUI compositor, the terminal renderer,
and the key input layer before a single Markdown-specific feature could land,
and Helix has no plugin system yet. CM6 already provides the editor model that
Obsidian itself uses, including decorations, widgets, atomic ranges and a
Lezer GFM tree. The result is a GUI editor with live preview, images and
tables, not a terminal editor with a Markdown mode.

### Plugins

The intended plugin seam is CM6's extension model for editor behaviour, plus a
capability-checked host in Rust for anything that touches the filesystem,
network, or another process. Plugins get a manifest, a set of commands, and
explicit scopes; they do not get raw `fs` access. That is the next layer, not
part of this foundation.

## Known gaps

- The first vault walk is synchronous. A worker-thread index with progressive
  updates is the next performance step for very large vaults.
- Relative Markdown links are rewritten on move; bare `up:` values and
  wikilinks are rewritten too. Links inside non-UTF-8 notes are left alone.
- Table cells render inline Markdown but are not yet directly editable; put the
  caret in the table to get the raw source.
- Images are loaded through a Rust command as data URLs. A streaming asset
  protocol will be better for very large images.
- The editor shell is one window; split panes and a real plugin host are on the
  roadmap.
- Print uses a plain-text print view. Rendered HTML/PDF export is a follow-up.
