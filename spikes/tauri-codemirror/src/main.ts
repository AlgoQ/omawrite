import { basicSetup } from "codemirror";
import { markdown, markdownLanguage } from "@codemirror/lang-markdown";
import { EditorView } from "@codemirror/view";
import { invoke } from "@tauri-apps/api/core";
import { tableWidgetExtension } from "./table-widget";
import "./styles.css";

const initialDocument = `# Omawrite rewrite spike

This is a real CodeMirror 6 editor with GFM Markdown parsing. It is the seam the
rewrite has to prove: plain Markdown stays the source of truth, while the view
renders Markdown in place.

| Piece | Current Qt app | This spike |
| --- | --- | --- |
| Editor | QTextEdit + QSyntaxHighlighter | CodeMirror 6 |
| Markdown parse | hand-written regex passes | Lezer Markdown |
| Tables | raw text | block widget stub |
| Shell | QML | TypeScript + DOM |

Put the caret inside the table above. The widget is replaced by the raw table
Markdown so it can be edited, then the table renders again when the caret leaves.

- [ ] Task checkboxes are still plain Markdown in this spike.
- [x] The editor seam is real, not a screenshot.

## What is deliberately missing

No file watching, no vault index, no frontmatter panel, no plugin host. Those
are the Rust and TypeScript modules described in docs/rewrite-spike.md.
`;

const editorParent = document.querySelector<HTMLDivElement>("#editor");
const status = document.querySelector<HTMLSpanElement>("#status");
const vaultRoot = document.querySelector<HTMLElement>("#vault-root");

if (!editorParent || !status || !vaultRoot) {
  throw new Error("spike markup is missing a mount point");
}

// TypeScript does not carry the guard's narrowing into the async closure.
const vaultRootLabel = vaultRoot;

const view = new EditorView({
  doc: initialDocument,
  extensions: [
    basicSetup,
    markdown({ base: markdownLanguage }),
    tableWidgetExtension(),
    EditorView.theme({
      "&": { height: "100%" },
      ".cm-scroller": {
        fontFamily: '"IBM Plex Mono", "JetBrains Mono", monospace',
        lineHeight: "1.7",
      },
      ".cm-content": { padding: "28px 0 80px" },
      ".cm-line": { padding: "0 8px" },
    }),
  ],
  parent: editorParent,
});

status.textContent = `${view.state.doc.lines} lines loaded`;

async function loadVaultRoot(): Promise<void> {
  try {
    const root = await invoke<string>("vault_root");
    vaultRootLabel.textContent = root;
    vaultRootLabel.title = root;
  } catch (error) {
    // Opening http://localhost:1420 directly in a browser has no Tauri IPC.
    vaultRootLabel.textContent = "not running under Tauri";
    vaultRootLabel.title = String(error);
  }
}

void loadVaultRoot();
