# Omawrite Next: the Helix-informed rewrite

Status: implementation, 2026-11-11. The working app lives in
[`../next/`](../next/). The root Qt app remains the 0.7.x release until the
next app reaches parity.

## The decision, restated

The rewrite is **Tauri 2 + a Rust core + a TypeScript shell + CodeMirror 6**.
Electron is excluded. The editor is a real GUI editor, not a terminal editor
with a Markdown mode.

## What Helix actually gave us

Helix is a post-modern modal terminal editor. It is a good reference because it
is small, fast, and unusually clear about the difference between an editor's
model, its commands, and its view. It is not a good base for this product:

- It is a TUI. The compositor, terminal renderer and key layer would all have
  to be replaced before live Markdown, images, tables or a GUI inspector could
  exist.
- It has no plugin system yet. Its own docs still call one a future feature.
- It has no Markdown live-preview model, no GFM table rendering, and no
  attachment/vault semantics.

So we borrowed the parts that survive a UI change.

### 1. One command registry

Helix's `MappableCommand` is the right shape: an action has an id, a title, a
handler, and a key sequence. Omawrite's registry is `src/commands.ts`. The
palette, the global keymap and the Settings key-capture all read the same list,
so a new action cannot appear in one place and be missing in another.

### 2. Key sequences as user data

Helix merges a default keymap with user overrides. Omawrite stores the
overrides in `settings.json`; `sequenceForCommand()` falls back to the default
and the Settings panel records a new sequence by capturing the next key event.

### 3. Document / view / tab

Helix separates the document from the view. Omawrite keeps one `Tab` per note
with its own CodeMirror `EditorState`, selection and scroll offset. Switching
tabs is a `view.setState()`, not a reload, so undo history and selection stay
with the note.

### 4. Theme as data

Helix themes are scoped TOML. Omawrite reads Omarchy's `colors.toml` and maps
the same concepts (background, foreground, accent, selection, muted, border)
onto CSS variables. The editor, chrome, tables and panels all inherit them.

### 5. Rope and transactions

Helix's `helix-core` owns the rope and the transaction model. CodeMirror owns
those in the browser: an immutable `EditorState`, changes as transactions, and
a persistent tree for history. Re-implementing that in Rust would duplicate
the editor's source of truth across the process boundary.

### 6. Tree-sitter versus Lezer

Helix uses tree-sitter for syntax. CodeMirror's Markdown language uses Lezer,
which already ships a GFM tree (tables, task lists, strikethrough) and is the
parser Obsidian's editor uses. The live-preview extensions walk that tree; the
same approach as Helix's syntax layer, with a different parser.

## What we did not copy

- No TUI compositor, no terminal renderer, no modal editing model. Omawrite is
  a writing app with normal selection and a command palette; modal keybindings
  can be a plugin later.
- No Helix source. The Rust core is our own and is deliberately a library, not
  a fork. Helix is MPL-2.0; keeping the dependency boundary clean keeps the
  licensing and upgrade story simple.
- No LSP in the foundation. Helix's LSP integration is excellent, but a
  Markdown slipbox needs vault semantics, links and note operations first.

## The new core

`crates/omawrite-core` is headless and testable without Tauri:

- `Vault`: discovery, containment, attachments, folder listing, link and
  attachment resolution.
- `Frontmatter`: split, property rows, preserving edits, tags and `up:` values.
- `NoteIndex`: warm snapshot, suggestions, backlinks, full-text search, the
  `up:` tree.
- `links`: wikilink and Markdown-link scanning outside code fences.
- `note_ops`: move/copy, collision names, inbound/outbound link rewriting,
  relative Markdown path rewriting, external-note import.
- `settings`: JSON settings at `~/.config/omawrite-next/settings.json`.
- `theme`: Omarchy `colors.toml` reading and blending.

`src-tauri` is a thin command layer: open a vault, read/save a note, create,
move, copy, suggest, search, resolve a link or attachment, edit frontmatter,
read an attachment, paste an image, get/set settings and theme. A `notify`
watcher emits `vault-changed`; the frontend debounces it into a tree/index
refresh.

## The new shell

The TypeScript shell is deliberately ordinary DOM plus CodeMirror:

- `editor.ts` owns the one `EditorView`, per-tab state, autosave hooks, smart
  list Return, formatting commands, and the 2× wheel scroll.
- `live-preview/` owns the CM6 decorations: marker hiding, tables, tasks,
  images, wiki links.
- `tree.ts`, `tabs.ts`, `properties.ts`, `backlinks.ts`, `palette.ts`,
  `settings.ts` render the chrome.
- `main.ts` is the session: open/save/close, external notes, moves, dialogs,
  Tauri events, print and window-close protection.

The directional panel gestures are `Ctrl+Alt+Left` for the file tree and
`Ctrl+Alt+Right` for backlinks. They close the panel on that side and leave the
`Ctrl+B` / `Ctrl+Shift+B` toggles available for reopening.

The rule from the spike holds: plain Markdown is the source of truth, the
rendered view is a set of decorations, and the block containing the caret is
raw Markdown.

## External notes

A note opened outside the slipbox is a first-class tab with `inVault: false`.
The banner offers **Move to slipbox** or **Copy to slipbox**. The Rust core
accepts an absolute source path and a vault destination; on a move it updates
the tab, history and last-note state, rewrites inbound links from indexed notes,
and leaves relative links alone with a warning because the note came from
outside the vault. That is the behaviour the user asked for: open a file
anywhere, then file it into the slipbox.

## Next milestones

1. Move the initial index walk to a worker and stream progress.
2. Plugin host: manifest validation, command registration, capability scopes,
   CM6 extension bundles, and a WASM tier for untrusted code.
3. Rendered print/PDF and HTML export.
4. Split panes, per-tab backlinks, and a real quick-switcher preview.
5. Inline Markdown inside table cells and direct cell editing.
6. A WebKitGTK IME/selection bake-off before committing to the Tauri host on
   Linux; the fallback is Tauri 3 + CEF or a pure-Rust GUI, never Electron.
