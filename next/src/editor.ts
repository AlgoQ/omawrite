import {
  autocompletion,
  type CompletionContext,
  type CompletionResult,
} from "@codemirror/autocomplete";
import {
  defaultKeymap,
  history,
  historyKeymap,
  indentWithTab,
} from "@codemirror/commands";
import { bracketMatching } from "@codemirror/language";
import {
  insertNewlineContinueMarkup,
  markdown,
  markdownLanguage,
} from "@codemirror/lang-markdown";
import { searchKeymap } from "@codemirror/search";
import { EditorState, type Extension } from "@codemirror/state";
import {
  drawSelection,
  dropCursor,
  EditorView,
  highlightSpecialChars,
  keymap,
} from "@codemirror/view";
import { api } from "./api";
import { app, type Tab } from "./state";
import { imageEmbeds } from "./live-preview/images";
import { liveMarkers } from "./live-preview/markers";
import { tableWidget } from "./live-preview/table";
import { taskCheckboxes } from "./live-preview/tasks";
import { wikiLinks } from "./live-preview/wiki";

export interface EditorHooks {
  contentChanged(tab: Tab, body: string): void;
  selectionChanged(tab: Tab, state: EditorState): void;
  pasteImage(tab: Tab, file: File): void;
  save(tab: Tab): void;
  saveAs(tab: Tab): void;
  palette(mode?: "all" | "files"): void;
  toggleSidebar(): void;
  toggleBacklinks(): void;
  print(): void;
  openFile(): void;
  closeTab(tab: Tab): void;
}

let hooks: EditorHooks | null = null;
let view: EditorView | null = null;

export function setEditorHooks(next: EditorHooks): void {
  hooks = next;
}

export function mountEditor(): EditorView {
  const parent = document.getElementById("editor");
  if (!parent) throw new Error("The editor mount point is missing.");
  view = new EditorView({
    state: EditorState.create({ doc: "" }),
    parent,
  });
  view.scrollDOM.addEventListener(
    "wheel",
    (event) => {
      const speed = app.settings?.scrollSpeed ?? 2;
      if (speed === 1 || event.ctrlKey || event.shiftKey) return;
      if (event.deltaY === 0) return;
      event.preventDefault();
      const delta =
        event.deltaMode === WheelEvent.DOM_DELTA_LINE
          ? event.deltaY * 20
          : event.deltaY;
      view!.scrollDOM.scrollTop += delta * speed;
    },
    { passive: false },
  );
  return view;
}

export function showTab(tab: Tab): void {
  if (!view) return;
  const current = app.tabs.find((candidate) => candidate.path === app.activePath);
  if (current && current !== tab) {
    current.editorState = view.state;
    current.selection = view.state.selection.main;
    current.scrollTop = view.scrollDOM.scrollTop;
  }
  view.setState(tab.editorState ?? createState(tab));
  tab.editorState = view.state;
  const editor = document.getElementById("editor");
  const empty = document.getElementById("empty-state");
  if (editor) editor.hidden = false;
  if (empty) empty.hidden = true;
  view.scrollDOM.scrollTop = tab.scrollTop;
  view.focus();
}

export function showEmptyState(): void {
  const editor = document.getElementById("editor");
  const empty = document.getElementById("empty-state");
  if (editor) editor.hidden = true;
  if (empty) empty.hidden = false;
}

export function currentView(): EditorView | null {
  return view;
}

export function resetTabState(tab: Tab): void {
  const fresh = createState(tab);
  tab.editorState = fresh;
  if (app.activePath === tab.path && view) {
    view.setState(fresh);
  }
}

export function insertText(text: string): void {
  if (!view) return;
  const selection = view.state.selection.main;
  view.dispatch({
    changes: { from: selection.from, to: selection.to, insert: text },
    selection: { anchor: selection.from + text.length },
  });
  view.focus();
}

export function wrapSelection(marker: string): void {
  if (!view) return;
  const selection = view.state.selection.main;
  const selected = view.state.sliceDoc(selection.from, selection.to);
  const before = view.state.sliceDoc(
    Math.max(0, selection.from - marker.length),
    selection.from,
  );
  const after = view.state.sliceDoc(
    selection.to,
    Math.min(view.state.doc.length, selection.to + marker.length),
  );
  if (before === marker && after === marker) {
    view.dispatch({
      changes: [
        { from: selection.from - marker.length, to: selection.from },
        { from: selection.to, to: selection.to + marker.length },
      ],
      selection: {
        anchor: selection.from - marker.length,
        head: selection.to - marker.length,
      },
    });
  } else if (
    selected.startsWith(marker) &&
    selected.endsWith(marker) &&
    selected.length > marker.length * 2
  ) {
    const inner = selected.slice(marker.length, -marker.length);
    view.dispatch({
      changes: { from: selection.from, to: selection.to, insert: inner },
      selection: { anchor: selection.from, head: selection.from + inner.length },
    });
  } else {
    const inserted = `${marker}${selected}${marker}`;
    view.dispatch({
      changes: { from: selection.from, to: selection.to, insert: inserted },
      selection: {
        anchor: selection.from + marker.length,
        head: selection.from + marker.length + selected.length,
      },
    });
  }
  view.focus();
}

export function insertWikiLink(): void {
  if (!view) return;
  const selection = view.state.selection.main;
  const selected = view.state.sliceDoc(selection.from, selection.to);
  const inserted = selected ? `[[${selected}]]` : "[[]]";
  view.dispatch({
    changes: { from: selection.from, to: selection.to, insert: inserted },
    selection: selected
      ? { anchor: selection.from + inserted.length }
      : { anchor: selection.from + 2 },
  });
  view.focus();
}

export function insertMarkdownLink(): void {
  if (!view) return;
  const selection = view.state.selection.main;
  const selected = view.state.sliceDoc(selection.from, selection.to) || "text";
  const inserted = `[${selected}]()`;
  view.dispatch({
    changes: { from: selection.from, to: selection.to, insert: inserted },
    selection: { anchor: selection.from + inserted.length - 1 },
  });
  view.focus();
}

export function applyFontSize(size: number): void {
  document.documentElement.style.setProperty("--editor-font-size", `${size}px`);
}

function createState(tab: Tab): EditorState {
  const notePath = tab.path;
  const extensions: Extension[] = [
    history(),
    drawSelection(),
    dropCursor(),
    highlightSpecialChars(),
    bracketMatching(),
    markdown({ base: markdownLanguage }),
    EditorState.tabSize.of(4),
    EditorView.lineWrapping,
    EditorView.contentAttributes.of({
      spellcheck: "true",
      autocorrect: "off",
      autocapitalize: "off",
    }),
    EditorView.theme({
      "&": {
        height: "100%",
        color: "var(--fg)",
        backgroundColor: "transparent",
        fontSize: "var(--editor-font-size)",
      },
      ".cm-scroller": {
        fontFamily: '"IBM Plex Mono", "JetBrains Mono", monospace',
        lineHeight: "1.7",
        overflow: "auto",
      },
      ".cm-content": {
        maxWidth: "65ch",
        margin: "0 auto",
        padding: "28px 24px 140px",
        caretColor: "var(--accent)",
      },
      ".cm-line": { padding: "0" },
      ".cm-cursor, .cm-dropCursor": {
        borderLeftColor: "var(--accent)",
        borderLeftWidth: "2px",
      },
      "&.cm-focused .cm-selectionBackground, .cm-selectionBackground": {
        backgroundColor: "var(--selection) !important",
      },
      ".cm-selectionMatch": { backgroundColor: "var(--selection)" },
      ".cm-activeLine": { backgroundColor: "transparent" },
      ".cm-tooltip": {
        backgroundColor: "var(--panel)",
        border: "1px solid var(--border)",
        color: "var(--fg)",
      },
      ".cm-tooltip-autocomplete ul li[aria-selected]": {
        backgroundColor: "var(--selection)",
        color: "var(--selection-fg)",
      },
      ".cm-panels": {
        backgroundColor: "var(--panel)",
        color: "var(--fg)",
      },
      ".cm-searchMatch": {
        backgroundColor: "var(--accent)",
        color: "var(--selection-fg)",
      },
    }),
    keymap.of([
      { key: "Enter", run: (target) => insertNewlineContinueMarkup(target) },
      ...historyKeymap,
      ...searchKeymap,
      ...defaultKeymap,
      indentWithTab,
    ]),
    autocompletion({
      override: [wikiCompletion],
      activateOnTyping: true,
      maxRenderedOptions: 40,
    }),
    EditorView.updateListener.of((update) => {
      if (!view) return;
      if (update.docChanged) {
        tab.doc.body = update.state.doc.toString();
        tab.editorState = update.state;
        hooks?.contentChanged(tab, tab.doc.body);
      }
      if (update.selectionSet) {
        tab.selection = update.state.selection.main;
        hooks?.selectionChanged(tab, update.state);
      }
    }),
    EditorView.domEventHandlers({
      paste: (event) => {
        if (!event.clipboardData) return false;
        const file = imageFromClipboard(event.clipboardData);
        if (!file) return false;
        event.preventDefault();
        hooks?.pasteImage(tab, file);
        return true;
      },
    }),
    liveMarkers(),
    tableWidget(),
    taskCheckboxes(),
    wikiLinks(),
    imageEmbeds(notePath),
  ];
  return EditorState.create({ doc: tab.doc.body, extensions });
}

async function wikiCompletion(context: CompletionContext): Promise<CompletionResult | null> {
  const match = context.matchBefore(/\[\[([^\]\n]*)$/);
  if (!match) return null;
  if (match.from === match.to && !context.explicit) return null;
  const query = match.text.slice(2);
  try {
    const suggestions = await api.suggest(query);
    return {
      from: match.from + 2,
      options: suggestions.map((suggestion) => ({
        label: suggestion.name,
        detail: suggestion.path,
        apply: `${suggestion.insert}]]`,
        type: "text",
      })),
      filter: false,
    };
  } catch {
    return null;
  }
}

function imageFromClipboard(data: DataTransfer): File | null {
  for (const item of Array.from(data.items)) {
    if (item.kind !== "file" || !item.type.startsWith("image/")) continue;
    const file = item.getAsFile();
    if (file) return file;
  }
  return null;
}
