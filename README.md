# Omawrite

A dead-simple Markdown writing app built with Qt Quick and C++ that automatically follows system dark/light mode.

<img width="2948" height="3227" alt="screenshot-2026-06-23_15-24-08" src="https://github.com/user-attachments/assets/4e930c0d-edda-4046-b444-a59eff523329" />
<img width="2948" height="3227" alt="screenshot-2026-06-23_15-23-23" src="https://github.com/user-attachments/assets/8ced7c26-961b-4ded-b263-84403001a951" />


## Install

Install via the Omarchy Package Repository via the `omawrite` package. It's installed by default in new installations of Omarchy (from Quattro forward).

## Command line

`omawrite` opens an empty document and `omawrite FILE` opens a Markdown file.
`omawrite --help` and `omawrite --version` answer in the terminal without opening a window.
`omawrite --update` rebuilds and reinstalls Omawrite from the source checkout it was built in
(it runs `bin/install`, so it asks for sudo), to try changes right away. Set
`OMAWRITE_SOURCE_DIR` to point it at a different checkout.

## Shortcuts

- `Ctrl+S` saves. Unsaved documents use the XDG desktop portal file picker.
- `Ctrl+Shift+S` saves as.
- `Ctrl+O` shows or hides the file viewer beside the page, in the page's own
  background; it also opens by itself when Omawrite starts without a file.
  Links mode (the default) nests notes the way Obsidian's VirtFolder does:
  under the notes their `up:` frontmatter field names, orphans as roots, with
  a child count beside each parent. Generated exports under `drawings/` stay
  out of the tree. Folders mode walks the disk instead. Click a note to open
  it in a tab, or use the arrow keys and `Enter`; `←`/`→` fold and unfold,
  `«` in the header and `Esc` close the tree. "Open another location…" at its
  foot opens the portal picker for files outside the tree.
- `Ctrl+Shift+O` opens a searchable list of the other notes in the slipbox.
- `Ctrl+Shift+B` toggles a Linked mentions panel listing the notes that link to
  the open one, with the lines that do. Click a note to open it.
- Following a link (`Ctrl+Click` or `Ctrl+Enter`) or picking a note from
  `Ctrl+Shift+O` or the Linked mentions panel opens it in a tab, or switches to
  the tab it is already in. The tab strip appears once a second tab is open.
  `Ctrl+1` to `Ctrl+8` go to that tab, `Ctrl+9` to the last, `Ctrl+Tab` and
  `Ctrl+Shift+Tab` step through them, and `Ctrl+W` closes the current one.
  Switching reopens the tab's file, so undo history does not carry across tabs.
- `Ctrl+P` opens the system print dialog.
- `Ctrl+N` opens a new Omawrite window.
- `Ctrl+Z`, `Ctrl+Shift+Z`, and `Ctrl+Y` handle undo and redo.
- `Super+F` toggles fullscreen. Qt maps this key as `Meta+F`.
- `Ctrl+F` searches the document. Use `Enter` or `Ctrl+G` for the next match and `Shift+Enter` for the previous match.
- `Ctrl+H` opens find and replace.
- `Ctrl+B`, `Ctrl+I`, and `Ctrl+Shift+X` toggle bold, italic, and strikethrough Markdown. `Ctrl+K` inserts a link.
- `- [ ]` and `- [x]` are task items. Their checkbox is drawn in place of the
  marker, and a click toggles it. `Ctrl+Shift+Enter` toggles the item at the
  caret. `Enter` on a task continues the list with a fresh checkbox, and
  `Enter` on an empty one ends it.
- Typing `[[` lists the other notes in the slipbox. `Enter` or `Tab` completes
  the link. `Ctrl+Enter` or `Ctrl+Click` follows a `[[wikilink]]` in Omawrite,
  and opens an `https://` or `mailto:` link in the default browser.
- `Tab` and `Shift+Tab` nest and outdent list items. `Shift+Enter` inserts a soft
  line break without starting a new paragraph.
- `Ctrl+V` pastes an image from the clipboard. In an Obsidian vault it goes
  into the vault's attachment folder and is embedded as `![[name.png]]`, so
  Obsidian shows it too; elsewhere it goes into `_attachments` beside the
  note as a Markdown image. A URL pastes as a Markdown link, and anything else
  as text. `Shift+Insert` does the same.
- Click a picture to open it full-window. `←` and `→` (or the chevrons at the
  picture's sides) step through the note's other pictures, and `Escape` closes
  the viewer.
- `Page Up` / `Page Down` move a screen at a time; `Ctrl+Home` / `Ctrl+End`
  jump to the start or end of the document.
- `Ctrl+=` and `Ctrl+-` change the text size, `Ctrl+0` returns to the default.
  `Ctrl+scroll` does the same.
- `Ctrl+Shift+C` copies the rendered document as HTML, with plain text as a fallback.
- `Ctrl+?`, `Ctrl+/`, or `F1` shows the keyboard shortcut reference.
- The gear button in the footer opens Settings: a scroll-speed multiplier
  (2x by default) shared by the editor and the file viewer, and every key
  binding, each editable in place with alternates separated by commas. Edits
  apply immediately and the shortcut reference always shows the live keys.
- In the unsaved-changes dialog, `Alt+C`, `Alt+D`, and `Alt+S` choose Cancel,
  Discard, and Save.

Once a document has been saved to a file, edits are auto-saved to that file a
moment after you stop typing, so `Ctrl+S` is rarely needed. Untitled drafts are
never auto-saved — they wait for an explicit save so Omawrite never picks a file
name for you. Unsaved drafts are recovered after an abnormal exit. Omawrite also
watches open files and warns before an external change can replace local work;
auto-save pauses until you reload, keep your version, or save. A recovered draft
is never auto-saved over its file — save it to keep it.

The font button in the footer picks the writing font from any installed text font,
and Omawrite remembers the choice. IBM Plex Mono is the default.

Text follows the desktop text size — `omarchy display text size`, or GNOME's
`text-scaling-factor` — and re-flows without a restart. The default of 12px leaves
Omawrite at the size it is designed around; larger and smaller sizes scale from there.
`Ctrl+=`, `Ctrl+-`, and `Ctrl+scroll` set your own size on top of that, from 50% to
300% in browser-sized steps. It is remembered for the next window you open.

## Properties

A note's YAML frontmatter is drawn at the top of the page, above the writing,
the way Obsidian shows Properties. The raw YAML never appears in the editor: the
block is held aside while you write and goes back at the top of the file when it
saves, so Obsidian and the VirtFolder tree still read it.

Tags and `up:` links are chips. Click a tag's `×` to remove it, `+` to add one;
click any other value to edit it in place. `+ Add property` appends a key and
`−` beside a row removes it. Only the key being edited is rewritten, leaving
comments, key order, and properties this panel does not understand untouched.
The `⌄` in the heading folds the block away.

## Requirements

- Qt 6: `qt6-base` and `qt6-declarative` (which includes Qt Quick Controls)
- `xdg-desktop-portal` and a portal backend

The IBM Plex Mono font is bundled under the SIL Open Font License 1.1; see
`fonts/OFL.txt`. The font is copyright IBM Corp.

## Build and install

For a local build, install the Qt requirements above, a C++17 compiler such as
`gcc`, and `make`. The build scripts use `qmake6` (or `qmake`), and the tests use
QtTest; both ship in `qt6-base`.

```sh
./bin/build
./bin/test
./build/omawrite
```

`bin/test` runs the Qt test suite with the offscreen platform, so it does not
require an active graphical session. To open a file with the local build:

```sh
./build/omawrite path/to/document.md
```

On Arch Linux or Omarchy, install `base-devel` for the packaging tools, then
build and install the package:

```sh
./bin/install
```

This runs the local build and `makepkg -fsi`, which installs the package through
pacman and may ask for authentication. To build a package archive for inspection
without installing it:

```sh
cd pkgbuild
makepkg -f
```
