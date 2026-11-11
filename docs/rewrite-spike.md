# Rewrite spike: Tauri, Electron, or a pure-Rust GUI

Status: spike, 2026-11-11. Branch `feat/rewrite-spike`. Current app: Omawrite
0.7.2 (Qt 6 / QML / C++17).

## Decision

**Default: Tauri 2 + a Rust core + a TypeScript shell + CodeMirror 6.**

**Fallback: Electron + TypeScript + CodeMirror 6**, reusing the same editor and
shell package behind a small host adapter. Choose this if the WebKitGTK
bake-off below fails, or if the team wants one language and no Rust.

**Not recommended for this product: a pure-Rust GUI (iced or GPUI) with a
Rust/WASM plugin model.** It is the best answer only if "no webview, one small
binary, maximum control" is a hard requirement. For Omawrite it means
rebuilding the editor, live preview, table editing, text layout, IME behavior,
and the plugin ecosystem from scratch, for a product whose users already keep
their notes in an Obsidian vault.

If the destination is TypeScript, do not do a pure-Rust rewrite first. That is
two rewrites, and the second one throws away the GUI layer. If the team wants a
Rust phase for its own sake, make it the core, not the GUI: build the vault,
index, and note operations as a Rust library with a stable command boundary,
then put the TypeScript shell on top. That preserves the useful half of the
Rust work when the UI moves to TypeScript.

This document assumes the rewrite is already decided. If the only goal is more
features and plugins, a `QJSEngine` plugin layer in the current Qt app is a far
cheaper experiment; it will not fix the editor seam or the navigation modules,
which is why it is not the recommendation here.

Why this is the plugin-friendly stack:

- CM6's extension model can be the plugin API for the editor itself. Core
  features and third-party features use the same seam: commands, decorations,
  widgets, completions, keymaps.
- The shell is ordinary TypeScript, so plugin UI can reuse the app's
  components, settings renderer, and command palette instead of inventing a
  second UI toolkit.
- Rust keeps the dangerous parts (paths, writes, network, plugin lifecycle)
  behind one capability-checked boundary, which is easier to audit than
  scattering `fs` calls through plugin code.

## What actually has to be replaced

The current app is small enough to count. These are the modules the review
called out, with their real size and the reason each is hard.

| Area | Where it lives now | Size | Why it is hard |
| --- | --- | --- | --- |
| Actions | `Backend` invokables, `Main.qml` `shortcutDefs`, 38 `Shortcut` items | ~1,000 lines | Every action has editor-state semantics: save/dirty, undo grouping, caret restoration, dialogs, multi-window |
| Index | `notesRoot`, `refreshNoteIndex`, `noteSuggestions`, `noteFrontmatter`, `virtFolderTree`, `backlinks`, `resolveNote` | ~700 lines | Obsidian link resolution is subtle: bare names resolve beside the note, at the root, or only when unique; fences are skipped; `up:` targets resolve against the source note |
| Vault | root detection, `fileTreeRoot`, `listFolder`, `attachmentDirectory`, `resolveImage`, file watching | ~500 lines | Path safety, symlinks, dot folders, vault-root discovery by nearest `.obsidian` ancestor, attachment folder settings |
| Note operations | `loadDocumentText`, `saveTo`, autosave, recovery lock, external-change detection, frontmatter rewriting | ~900 lines | Preserves comments, key order, unknown keys; untitled notes never auto-save; non-UTF-8/CRLF files suspend autosave; crash recovery |
| Editor seam | `MarkdownHighlighter` (820 lines) plus the `TextEdit` overlays in `Main.qml` | ~1,200 lines | Caret-aware marker hiding, inline spans, code spans, fences, wikilinks, task checkboxes, image blocks, hanging indents, diacritic-folding search |
| Shell | `Main.qml` | 3,358 lines, 40 `objectName`s | Tabs, history, quick open, tree, backlinks panel, properties panel, search/replace, settings, dialogs, scroll physics |
| Tests | `tests/tst_omawrite.cpp` | 5,043 lines, 139 test functions | They are the behavior spec, not just regression cover |

Total source plus tests is about 14,900 lines. The rewrite is not dominated by
the shell; it is dominated by the editor seam and the vault/index semantics.

## The comparison

Scores are relative to this product, not general-purpose rankings.

| Criterion | (a) Tauri 2 + Rust + TS + CM6 | (b) Electron + TS + CM6 | (c) Pure Rust GUI + Rust/WASM plugins |
| --- | --- | --- | --- |
| Markdown live preview and tables | Strong primitives; CM6 is the editor Obsidian itself uses; table widget is custom but proven | Same editor as (a); shell does not change editor maturity | Weak; no CM6-class decoration/widget layer; live preview and tables are custom rendering |
| Plugin authoring language | TS/JS for UI and editor extensions; Rust or WASM for privileged work | TS/JS everywhere; Node in main or utility process | Rust native plugins, or WASM; smallest author pool |
| Plugin sandboxing | Best if designed: WASM/Worker tiers plus capability scopes; in-realm editor plugins are trusted | Good OS primitives (sandbox, contextIsolation, utilityProcess), but renderer plugins share the DOM | Strongest if WASM/WASI; native Rust plugins are fully trusted |
| Community size | Large Rust ecosystem, huge JS/CM6 ecosystem; Tauri 2 is mature | Largest desktop JS ecosystem; Chromium and Node | Smallest of the three for GUI apps and editor plugins |
| Installer size | Small; uses the OS webview; this spike's frontend is 612 KB JS (210 KB gzip) before Rust | Large; the Electron 45.3.0 Linux x64 zip alone is 123.8 MB | Smallest; no browser engine |
| Memory | Lower disk; RAM depends on WebKitGTK/WebView2; measure, do not assume | Higher baseline; bundles Chromium and Node | Lowest idle; GPU and text stack still cost |
| Rendering consistency | Varies by OS webview: WebKitGTK on Linux, WKWebView on macOS, WebView2 on Windows | One Chromium version everywhere | One renderer, but you own text shaping and fonts |
| Mobile | Tauri 2 supports iOS and Android; CM6 has known mobile quirks | No official mobile support | No mobile (iced targets desktop and web; GPUI is desktop) |
| Security | Rust core, capability system, CSP, isolation pattern; system-webview patching is the OS's job | Chromium sandbox and context isolation; you ship Chromium updates; larger attack surface | Smallest web attack surface; native plugins are trusted code; WASM needed for third parties |
| Porting effort from current modules | Medium-high: Rust core, TS shell, editor rebuild | Medium: TS shell and editor; Node core rewrite | High: editor and plugin model from scratch |
| Fit with an Obsidian slipbox | Direct: same CM6 editor model; vault format is native | Direct: same | Indirect: would reimplement the editor and plugin model |

### Editor maturity

CodeMirror 6 is the strongest reason to choose (a) or (b). Its extension model
is exactly the shape the Omawrite editor seam needs:

- `StateField` and `ViewPlugin` for state and imperative view components.
- `Decoration.mark`, `Decoration.replace`, and `Decoration.widget`, including
  block widgets, for hiding markers, drawing checkboxes, and replacing an
  image line or a table with a rendered block.
- `EditorView.atomicRanges` for embeds and wikilinks that should behave as one
  unit when the caret moves.
- A Lezer Markdown parser with a GFM `Table` extension and a `TaskList`
  extension already available; the `markdownLanguage` export covers GFM plus
  subscript, superscript, and emoji.

Obsidian's own developer documentation says Obsidian uses CodeMirror 6 to power
its Markdown editor and exposes CM6 extensions to plugins. That is the strongest
available signal that the target live-preview behavior is achievable in CM6.

What CM6 does **not** give you is a turnkey Obsidian live preview. The table
widget, task checkbox widget, image block, wiki-link autocomplete, and
caret-aware marker rules are application code. The prior art exists:
`atomic-editor` (MIT, ~160 GitHub stars as of 2026-11-11) demonstrates inline
live preview, WYSIWYG tables, wiki links, and task lists on CM6. It is a
reference, not a dependency to adopt blindly.

The current `MarkdownHighlighter` is 820 lines of hand-written regex and block
state. A CM6 port can let Lezer handle fences, headings, lists, and GFM tables,
but Obsidian syntax (`[[...]]`, `![[...]]`, aliases, headings, block IDs) still
needs a custom inline parser. The 139 existing tests are the specification for
that parser; they should be ported before the UI.

For (c), iced 0.14's `text_editor` is a multi-line text input with syntax
highlighting. It does not expose a CodeMirror-style decoration/widget layer for
inline live preview or embedded table widgets; those would be custom rendering
and hit-testing. iced added IME support in 0.14, but accessibility, text
layout, and the editing surface remain your problem. GPUI is Zed's
GPU-accelerated framework, explicitly pre-1.0 with breaking changes between
versions; Zed's editor is not packaged as a general-purpose reusable editor.
Embedding a webview inside a Rust GUI would recover CM6, but then the stack is
Tauri-or-Electron tradeoffs with an extra native layer, not a pure-Rust win.

### Plugin authoring and sandboxing

This is the part where the naive answer ("use Tauri's plugin system") is wrong.
Tauri plugins are Cargo crates plus optional npm API bindings, compiled into the
app. They are not runtime-installable user plugins. "Lots of plugins" therefore
means Omawrite must build its own plugin host in every option.

- **Tauri + TS:** plugin authors write TypeScript for the shell and editor
  extensions; privileged work is Rust or WASM. You get the largest author pool
  and the CM6 extension API. The cost is that you must build the manifest,
  permission, install, and isolation layers yourself.
- **Electron + TS:** plugin authors write TS/JS everywhere. Electron gives
  mature primitives: renderer sandbox on by default since Electron 20,
  `contextIsolation` on by default since 12, preload scripts with
  `contextBridge`, and `utilityProcess` for Node-side work. But an
  Obsidian-style plugin that runs in the renderer shares the DOM with the app;
  a plugin in the main process has full Node. Neither is a sandbox by default.
  You still have to design the capability API.
- **Pure Rust:** native Rust plugins are fast and fully trusted; any untrusted
  plugin must be WASM. Wasmtime/WASI gives capability-based filesystem access
  and a real sandbox, but plugin authors must write Rust, AssemblyScript, or
  compile TS to a WASM component. The UI has no DOM to extend; plugins can
  contribute data and commands, not arbitrary editor decorations, unless you
  build a declarative rendering protocol.

The central technical constraint for CM6: editor extensions must live in the
same JavaScript realm as the editor. `@codemirror/state` uses object identity
for fields and facets; two copies of the package silently break the editor.
A plugin cannot hand a real CM6 `Extension` object across a Worker or iframe
boundary. So there are only two honest designs:

1. **Sandboxed plugins** run in a Worker or WASM, cannot touch the DOM, and
   contribute through declarative rules or message-passing. Safe, but not
   arbitrary CM6 extensions.
2. **Editor plugins** run in the renderer realm and return real CM6
   extensions. Powerful, Obsidian-like, and **not a security sandbox**. Treat
   them as installed software and gate them behind a trust prompt and, later, a
   signed registry.

The recommendation is to support both tiers and make the difference explicit in
the install UI. Do not claim editor plugins are sandboxed.

### Community size

Adoption signals on 2026-11-11 (rough, but measured):

| Signal | Value |
| --- | --- |
| npm downloads, last month: `@codemirror/view` | 80.5M |
| npm downloads, last month: `electron` | 42.8M |
| npm downloads, last month: `@tauri-apps/api` | 15.3M |
| GitHub stars: `electron/electron` | 123.9k |
| GitHub stars: `tauri-apps/tauri` | 112.8k |
| GitHub stars: `iced-rs/iced` | 32.0k |
| GitHub stars: `zed-industries/zed` | 94.7k |
| Obsidian community plugins listed | 10,249 |

The Obsidian number matters more than the raw npm numbers: it is the size of
the plugin culture Omawrite would be adjacent to. Obsidian plugins are not
directly compatible (they assume Obsidian's API and, on desktop, Node/Electron),
but their authors already know CM6 and the vault model. That is the pool a
TypeScript plugin API can reach.

### Installer size and memory

- Tauri's own documentation says a minimal Tauri app can be under 600 KB
  because it uses the system webview. This spike's frontend bundle is 612 KB
  JavaScript (210 KB gzip), and its release build measured a 4.7 MB binary and
  a 1.92 MiB `.deb` on WebKitGTK. A real app will be larger, but still far
  below Electron.
- Electron 45.3.0's Linux x64 release zip is 123.8 MB. An installed app is
  larger. Every Electron app ships its own Chromium and Node.
- A pure-Rust GUI has the smallest installer and usually the lowest idle RAM,
  but GPU drivers, font stacks, and accessibility bridges still cost.
- RAM comparisons are workload-dependent. The honest statement is: Tauri
  usually wins on disk and baseline memory; Electron is more predictable
  because every user runs the same Chromium; pure Rust is lowest only if you
  do not reimplement a browser badly.

### Rendering consistency

Tauri 2 uses WebView2 (Chromium) on Windows, WKWebView on macOS, and
WebKitGTK on Linux. This is the main technical risk for a Linux-first app:
Omarchy users get WebKitGTK, not Chromium. Font rasterization, contenteditable
selection, IME, and CSS details can differ from Obsidian's bundled Chromium.
The Tauri docs list webview versions per platform and warn that Linux
WebKitGTK versions vary by distribution.

Electron removes that variable by shipping one Chromium everywhere. That is
the strongest argument for the fallback, especially for a writing app where
caret behavior and text rendering are the product.

Tauri 3 (alpha as of 2026-11-11) adds a CEF runtime that ships Chromium with
the app, giving consistent rendering on Windows, macOS, and Linux. The docs are
explicit about the costs: hundreds of megabytes larger bundles, more memory,
desktop only, and a first build that downloads about 1 GB of CEF. Tauri tells
production apps to stay on 2.x. Treat Tauri 3 + CEF as the escape hatch if
WebKitGTK blocks the default path, not as the starting point.

### Mobile

Tauri 2 supports iOS and Android. Electron officially supports macOS, Windows,
and Linux only. iced targets desktop and the web; GPUI is desktop. Omawrite is
an Omarchy desktop app, so mobile should not decide this. If mobile becomes a
goal, Tauri is the only option here with a credible story, and even then CM6's
mobile behavior needs its own test pass; there are known iOS tap/selection
issues and ongoing Android/iOS fixes in the CM6 changelog.

### Security

- **Tauri:** Rust core for file and vault operations, a capability system that
  constrains frontend access to commands, CSP, and an optional isolation
  pattern that intercepts IPC in a sandboxed iframe. The docs are explicit
  that capabilities do not protect against malicious Rust code, overly broad
  scopes, or unpatched system-webview vulnerabilities. The system webview is
  patched by the OS, not by the app.
- **Electron:** Chromium's process sandbox, `contextIsolation`, and
  `utilityProcess` are solid primitives. The Electron docs themselves say
  Electron cannot be as secure as Chromium because it lacks Chromium's
  dedicated security resources, disables some Chrome security features, and
  depends on app vendors to ship updates. You own the Chromium update cadence.
- **Pure Rust:** the smallest web attack surface, because there is no web
  engine. But native plugins are trusted code, and Markdown/HTML rendering can
  still introduce injection bugs if you render untrusted HTML. WASM plugins
  with WASI capabilities are the strongest third-party model.

For Omawrite, the dominant threat is a malicious plugin reading or exfiltrating
a private vault. Capability-scoped file access and no network by default matter
more than the shell's CVE count. That is true in all three options; the plugin
host design is the real security work.

### Porting effort from the current modules

Estimates assume one experienced engineer, desktop parity, existing behavior
preserved, and the 139-test corpus ported. They exclude the plugin host.

| Current module | Target | Effort | Risk |
| --- | --- | --- | --- |
| CLI and startup (`cli.cpp`, `main.cpp`) | Rust `clap`/manual parsing, Tauri setup | 1-2 weeks | Low |
| File lifecycle: open/save/save-as, autosave, recovery, external change | Rust commands, `notify`, atomic writes, lock file | 4-6 weeks | Medium-high; preserve the untitled/CRLF/non-UTF-8 guarantees |
| Frontmatter and properties panel | Rust parser/writer plus TS panel | 4-6 weeks | High; comments, key order, unknown keys, quoted values |
| Note index, suggestions, backlinks, link tree | Rust index plus TS tree/panel | 6-9 weeks | High; Obsidian bare-name resolution and `up:` semantics |
| Markdown editor and live preview | CM6 + Lezer + custom Obsidian inline parser | 8-12 weeks | Highest; this is the product |
| Images: paste, attachment folder, embeds, viewer | Rust attachment logic plus CM6 block widgets | 3-5 weeks | Medium; webview clipboard and asset protocol |
| Shell: tabs, history, quick open, panels, dialogs, search | TS components | 8-12 weeks | Medium; many behaviors, mostly mechanical |
| Settings, shortcuts, theme, fonts, text scale | Rust settings plus TS UI | 3-5 weeks | Medium; Omarchy theme and D-Bus integration |
| Print and copy-as-HTML | Webview print and clipboard APIs | 2-3 weeks | Medium; print fidelity differs from Qt |
| Tests | Rust unit/integration plus Vitest/Playwright | 5-8 weeks | High; port pure functions first, keep a fixture vault |

Rough total: **44-68 engineer-weeks, or 10-16 engineer-months**, for desktop
parity without plugins. Two engineers can overlap the Rust core and the editor
port for a 6-9 month calendar estimate. The plugin host adds roughly 8-12
engineer-weeks for v1 (manifest, permissions, install/disable, command and
editor-extension registries, sandboxed worker/WASM runtime), plus ongoing
review and registry work.

The estimate is dominated by the editor, not by Tauri or Electron. Choosing a
shell does not remove that work.

## Recommended architecture

```text
┌──────────────────────────────────────────────────────────────┐
│ TypeScript shell (Tauri webview)                             │
│  tabs · history · quick open · command palette · dialogs     │
│  properties panel · settings · plugin manager                │
│  CodeMirror 6 editor · live preview · plugin host API        │
└───────────────────────────┬──────────────────────────────────┘
                            │ typed invoke/events (DTOs)
┌───────────────────────────▼──────────────────────────────────┐
│ Rust core (Tauri process)                                    │
│  vault root + path safety · note index · search · backlinks  │
│  note read/write · autosave · recovery · file watching       │
│  attachments · theme/system integration · plugin supervisor  │
└──────────────────────────────────────────────────────────────┘
```

### What belongs in Rust

- **Vault:** canonical root, root discovery by nearest `.obsidian` ancestor,
  path normalization, symlink/escape rejection, folder listing, attachment
  directory resolution.
- **Note index and search:** note paths, titles, tags, `up:` parents, wikilink
  targets, backlink candidates, incremental updates from the file watcher.
  A persisted index (SQLite FTS5 or Tantivy) is worth it once the vault is
  larger than a few thousand notes; start with an in-memory index and a
  rebuild-on-start path.
- **Note operations:** read/write with encoding and line-ending preservation,
  atomic write (temp file + rename), autosave scheduling, recovery drafts and
  lock file, dirty comparison, external-change detection, frontmatter
  parse/write that preserves comments and key order.
- **File watching:** `notify` (or the stable 8.x line; the current crates.io
  head is 9.0.0-rc.8, so pin a stable release) with debouncing and a
  content-hash check, because editors and sync clients produce bursts.
- **Images and attachments:** clipboard image bytes, vault/attachment folder
  choice, image dimensions, safe image file names, asset protocol root.
- **System integration:** Omarchy theme files, D-Bus dark mode and text scale,
  fonts, window geometry, settings persistence.
- **Plugin supervisor:** manifest validation, capability checks, path scopes,
  network proxy, plugin storage, WASM/Worker lifecycle and limits.

### What belongs in TypeScript

- **Shell:** window chrome, tabs, back/forward history, quick open, command
  palette, dialogs, footer, status, settings and shortcut editors.
- **Editor:** CM6 state, Lezer Markdown parsing, live-preview decorations,
  table widget, task checkbox widget, image block widget, wiki-link
  autocomplete, find/replace UI, keybindings, undo grouping, scroll behavior.
- **Properties panel:** renders Rust-provided property rows, sends surgical
  edits back through Rust.
- **Plugin host API and UI:** command registry, editor-extension registry,
  settings schema rendering, permission consent, enable/disable.
- **No direct filesystem or network access.** The webview talks to Rust through
  typed commands and events, and Rust enforces every path and network scope.

### Editor seam mapping

| Current mechanism | Rewrite mechanism |
| --- | --- |
| `QSyntaxHighlighter` regex passes | Lezer Markdown parse plus CM6 mark/replace decorations |
| Caret-aware marker hiding by `setCaret` | Decoration field recomputed from `state.selection` |
| Task `[ ]` drawn over hidden text via `positionToRectangle` | `WidgetType` replacing the marker; click dispatches one change |
| Image line drawn as an overlay with reserved block margin | Block `WidgetType` replacing the image line; dimensions from Rust |
| Table raw text | Block widget replacing the table; raw text returns when the caret is inside |
| `QTextBlockFormat` hanging indents and line height | CSS line decorations (`text-indent`, `padding-inline-start`) and editor theme |
| `MarkdownHighlighter::findMatches` diacritic folding | Custom search predicate/port; CM6 search supports case, regexp, whole word, and a custom `test`, but not diacritic folding |
| Tabs and file history in `Main.qml` | TS state; the Rust core only owns the open buffer and file I/O |
| Print and copy-as-HTML | `window.print()` and the async clipboard HTML APIs; verify against Qt output |

The existing tests are the porting checklist. Start with the pure statics:
`countWords`, `normalizedLinkUrl`, `suggestedFileName`, `tomlValue`,
`inlineMarkup`, `linkUrlAt`, `wikiLinkTargetAt`, `taskMarker`, `imageEmbed`,
`codeSpans`, `findMatches`. Those can be ported to TS and tested with Vitest
before any window exists.

## Plugin interface sketch

This is an interface sketch, not implementation. The goal is to make the
security boundary explicit before writing code.

### Two plugin tiers

| Tier | Runtime | Can do | Cannot do | Trust |
| --- | --- | --- | --- | --- |
| Sandboxed | Worker or WASM/WASI | Commands, settings, storage, vault API through host, declarative editor rules and fence renderers | Touch the DOM, return a real CM6 extension, read arbitrary files, open sockets | Untrusted; safe by default |
| Editor | Renderer JS realm | Return CM6 extensions, add decorations/widgets, use the editor view, register commands | Bypass host capabilities for files/network unless granted | Trusted; install prompt and review |

The default should be sandboxed. Editor plugins are the "power user" tier, the
way Obsidian plugins are powerful but not sandboxed.

### Manifest

`plugin.json` sketch:

```json
{
  "id": "com.example.daily-notes",
  "name": "Daily notes",
  "version": "1.2.0",
  "apiVersion": 1,
  "engines": { "omawrite": ">=0.8.0 <1.0.0" },
  "description": "Open or create today's note.",
  "author": "Example",
  "license": "MIT",
  "runtime": "sandboxed",
  "entry": "dist/plugin.js",
  "activation": ["onStartup", "onCommand:daily-notes.openToday"],
  "permissions": [
    "commands:register",
    "editor:decorate",
    "vault:read:notes/**",
    "vault:write:notes/daily/**",
    "vault:write:attachments/**",
    "settings:own",
    "storage:own"
  ],
  "contributes": {
    "commands": [
      {
        "id": "daily-notes.openToday",
        "title": "Open today's note",
        "category": "Daily notes",
        "keybindings": ["Mod+Shift+D"]
      }
    ],
    "settings": {
      "type": "object",
      "properties": {
        "folder": { "type": "string", "default": "daily" },
        "template": { "type": "string", "default": "" }
      }
    }
  }
}
```

The `runtime` field selects the tier. `entry` is a bundled ES module for the
Worker/editor tier, or a `.wasm` component for the WASM tier. A plugin must not
bundle its own `@codemirror/state` or `@codemirror/view`; the host injects its
copies, because duplicate CM6 packages break identity checks.

### Commands

```ts
interface PluginCommand {
  id: string;                 // must start with the plugin id
  title: string;
  category?: string;
  keybindings?: string[];     // contributions, not global grabs
  run(context: CommandContext): Promise<CommandResult | void>;
}

interface CommandContext {
  note: { path: string; title: string } | null;
  selection: { from: number; to: number; text: string };
  vault: { name: string };    // no absolute path unless granted
}

interface CommandResult {
  changes?: Array<{ from: number; to: number; insert: string }>;
  openNote?: string;          // vault-relative path
  message?: string;
}
```

All edits returned by a command are applied as one transaction with
`userEvent: "plugin:<id>"`, so one undo reverts the plugin's edit. Commands are
listed in the palette and can be bound by the user; the manifest's keybindings
are defaults, not global shortcuts.

### Editor contributions

Sandboxed tier:

```ts
ctx.editor.registerMarkRule({
  regex: /#[\w-]+/g,
  className: "plugin-tag",
});

ctx.editor.registerFenceRenderer({
  language: "mermaid",
  render: async (source) => ({ html: await renderMermaid(source) }),
}); // The host sanitizes the HTML or renders it in a sandboxed iframe.

ctx.editor.registerCompletionSource({
  trigger: "[[",
  provide: async (query) => suggestNotes(query),
});
```

Editor tier:

```ts
export function activate(ctx: PluginContext) {
  return {
    extensions: [myDecorationField, myKeymap],
    dispose() { /* remove listeners and timers */ },
  };
}
```

The host mounts editor extensions through a `Compartment`, so enable/disable
reconfigures the live editor without restarting the app. A plugin that throws
during activation is disabled and reported; a plugin that throws in a command
is caught per invocation.

### Capability-scoped file access

Permissions are strings with a namespace and a scope. The host is the only
component that touches the filesystem.

| Permission | Meaning |
| --- | --- |
| `vault:read` | Read any note or attachment in the vault |
| `vault:read:notes/**` | Read notes under a vault-relative glob |
| `vault:write:notes/**` | Write notes under a glob; atomic writes only |
| `vault:write:attachments/**` | Write attachments under a glob |
| `vault:create` / `vault:rename` / `vault:delete` | Destructive operations, separate consent |
| `network:https://api.example.com` | Host-mediated HTTPS requests to one origin |
| `clipboard:read` / `clipboard:write` | Host-mediated clipboard access |
| `editor:decorate` | Contribute editor decorations/rules |
| `editor:command` | Register commands and keybindings |
| `settings:own` / `storage:own` | Own settings schema and namespaced storage |
| `commands:execute:<plugin>.<command>` | Call another plugin's command |

Rules:

- All paths are vault-relative. The host normalizes, canonicalizes, verifies
  the result is under the vault root (or an explicitly granted attachment
  root), and rejects `..` and symlink escapes.
- No absolute paths cross the API. A plugin never learns the user's home path.
- Writes go through a temp file and atomic rename; destructive operations keep
  a backup for a short window.
- Network is opt-in per origin, proxied by Rust, with request/response size
  limits and no ambient cookies or credentials.
- Clipboard is host-mediated so the webview's CSP can stay closed.
- Plugin storage is a namespaced key-value store with a size cap, not a path.

### Versioning

- `apiVersion` is an integer major. The host supports a range (start with
  `1`), and rejects an unknown major at install time.
- `engines.omawrite` is semver for the app version.
- Additive capabilities are feature-detected (`ctx.features.has(...)`), not
  inferred from the app version.
- Breaking API changes bump `apiVersion`; keep the previous major working for
  at least two minor releases.
- The manifest is validated against a JSON Schema before install; unknown
  fields are rejected, not ignored, so typos do not silently drop permissions.

### Install, enable, disable

- Install from a folder or zip; validate id (reverse-DNS), version, API
  version, manifest schema, entry path, and optional signature/checksum.
- Show a consent sheet with each permission in plain language. The user can
  deny optional permissions; the plugin sees the granted set and must degrade.
- Copy to `~/.local/share/omawrite/plugins/<id>/<version>/` with a `current`
  pointer; keep the previous version for rollback.
- Registry state in `plugins.json`: enabled, granted permissions, source,
  version, crash count, last error.
- Enable/disable reconfigures the editor `Compartment` and starts/stops the
  Worker/WASM instance. No app restart.
- Safe mode (`omawrite --safe-mode` or holding Shift at startup) disables all
  community plugins; core features still work.
- A plugin that crashes three times in a row is quarantined and reported.
  Sandboxed plugins get a timeout and a memory cap; the UI stays alive.
- Uninstall removes code; plugin data is kept by default and can be deleted
  explicitly.
- Updates are manual in v1. A later registry adds signatures, review, and
  version ranges. No silent auto-update of code that can read the vault.

## Runnable skeleton

`spikes/tauri-codemirror/` is a minimal Tauri 2 app that proves the seam:

- Rust command `vault_root` returns a canonical vault root; the TypeScript
  header displays it.
- CodeMirror 6 in GFM Markdown mode renders a table block widget when the caret
  is outside the table and reveals the raw Markdown when the caret is inside.
- `npm run build`, `cargo test`, and a real Tauri window were run on WebKitGTK
  2.54.2 on 2026-11-11. The README has the exact commands and the limits of
  what was verified.

It is not wired into `omawrite.pro` or `tests/tests.pro`.

## Porting plan

1. **Bake-off (2 weeks).** Build the CM6 editor surface on WebKitGTK and
   Chromium. Port `inlineMarkup`, `taskMarker`, `imageEmbed`, and `codeSpans`
   plus their tests. Test IME (fcitx/ibus), selection, scroll, font rendering,
   a 10,000-line note, and a table-heavy note. Gate the default stack on this.
2. **Rust core (4-6 weeks).** Vault root, path safety, read/write, atomic save,
   autosave/recovery, file watching, frontmatter parser/writer, in-memory note
   index. Port the pure `Backend` tests.
3. **Editor parity (8-12 weeks).** Live preview, task checkboxes, image blocks,
   tables, wiki autocomplete, search/replace, typography, undo grouping.
4. **Shell parity (8-12 weeks).** Tabs, history, quick open, tree, backlinks,
   properties panel, settings, shortcuts, dialogs, print.
5. **Plugin host (8-12 weeks).** Manifest, permissions, install/disable,
   command registry, sandboxed runtime, then the trusted editor tier.
6. **Packaging (1-2 weeks).** Arch package, desktop file, icons, `--update`,
   CLI flags, release pipeline.

The order matters: the editor bake-off can kill the Tauri default before the
team spends months on the Rust core. If WebKitGTK fails, move the same
TypeScript editor package to Electron and keep the Rust core as a sidecar or
port it to Node.

## What is still unknown

- **CM6 on WebKitGTK is the big one.** IME, selection, caret placement,
  contenteditable quirks, font rasterization, and scroll behavior need a real
  Omarchy test, not a Chromium test. The spike proves the table seam in
  WebKitGTK, not the full editor.
- **Table editing, not just rendering.** The spike replaces a table with a
  read-only widget. Editable cells, column resizing, wide-table scrolling, and
  keyboard navigation are additional work.
- **Vault scale.** The current index is rebuilt in memory and `backlinks()`
  rereads candidate notes. A Rust index will be faster, but the right threshold
  for SQLite/Tantivy and the incremental update strategy need measurements on a
  real vault.
- **Exact Obsidian link semantics.** Bare-name resolution, aliases, headings,
  block IDs, URL-encoded Markdown links, and `up:` cycles are subtle. The
  current code is the spec; a fixture vault is mandatory.
- **Plugin demand and compatibility.** Obsidian plugins rely on Obsidian's API
  and Node/Electron. Omawrite cannot run them unchanged. The realistic promise
  is "CM6 extensions and a TypeScript API", not "Obsidian plugin compatible".
- **Memory and bundle size for a parity build.** The spike's 612 KB frontend
  and 179 MB debug binary are not a release measurement.
- **Print fidelity.** Browser print layout will not match Qt's `QPrinter`
  exactly; decide whether that matters.
- **Tauri 3 timing.** CEF solves the rendering-consistency problem, but it is
  alpha. Do not block v1 on it; keep the runtime swap as a planned migration.

## Sources checked

Versions and claims were checked on 2026-11-11.

- Tauri releases: <https://v2.tauri.app/release/> (2.12.x line; crates.io
  resolves `tauri` 2.12.5, `tauri-cli` 2.12.2, npm `@tauri-apps/api` 2.12.4).
- Tauri 3 alpha and CEF: <https://v3.tauri.app/start/migrate/from-tauri-2/>,
  <https://v3.tauri.app/develop/cef/>. Latest alpha tag `tauri-v3.0.0-alpha.6`,
  2026-11-06.
- Tauri webview versions: <https://v2.tauri.app/reference/webview-versions/>.
- Tauri capabilities: <https://v2.tauri.app/security/capabilities/>.
- Tauri isolation pattern:
  <https://v2.tauri.app/concept/inter-process-communication/isolation/>.
- Tauri plugin model: <https://v2.tauri.app/develop/plugins/>.
- Electron releases: <https://releases.electronjs.org/> (45.3.0 stable,
  2026-11-10). Linux x64 zip size from the GitHub release asset.
- Electron platform support:
  <https://github.com/electron/electron/blob/v45.3.0/README.md>.
- Electron process model and sandbox:
  <https://www.electronjs.org/docs/latest/tutorial/process-model>,
  <https://www.electronjs.org/docs/latest/tutorial/sandbox>,
  <https://www.electronjs.org/docs/latest/tutorial/context-isolation>.
- CodeMirror system guide and decoration example:
  <https://codemirror.net/docs/guide/>,
  <https://codemirror.net/examples/decoration/>.
- CodeMirror search reference: <https://codemirror.net/docs/ref/#search>.
- `@lezer/markdown` GFM `Table` and `TaskList`:
  <https://github.com/lezer-parser/markdown> (repository archived and moved to
  <https://code.haverbeke.berlin/lezer/markdown>; npm `@lezer/markdown` 1.8.1
  published 2026-10-19).
- Obsidian editor extensions (CM6):
  <https://docs.obsidian.md/Plugins/Editor/Editor+extensions>.
- Obsidian plugin submission requirements (Node/Electron desktop-only):
  <https://docs.obsidian.md/community-directory/submission-requirements-for-plugins>.
- Obsidian community plugin count: 10,249 entries in
  <https://github.com/obsidianmd/obsidian-releases/blob/master/community-plugins.json>
  on 2026-11-11; the community site search result listed 10,246 at the same
  time, which is close enough to confirm the order of magnitude.
- iced 0.14: <https://docs.rs/iced/0.14.0/iced/widget/text_editor/index.html>,
  <https://iced.rs/>. Published 2026-10-27.
- GPUI: <https://github.com/zed-industries/zed/tree/main/crates/gpui>
  (README: pre-1.0, breaking changes; crates.io `gpui` 0.2.2).
- Wasmtime/WASI capability model:
  <https://docs.wasmtime.dev/security.html>.
- Atomic Editor prior art: <https://github.com/kenforthewin/atomic-editor>
  (MIT, inline live preview/tables/wiki links on CM6).
- npm download and GitHub star counts: npm registry download API and GitHub
  REST API, 2026-11-11.
