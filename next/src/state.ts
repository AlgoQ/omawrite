import type { EditorState } from "@codemirror/state";
import type { NoteDoc, Settings, Theme, TreeNode, VaultInfo } from "./types";

export interface Tab {
  path: string;
  doc: NoteDoc;
  editorState: EditorState | null;
  dirty: boolean;
  saving: boolean;
  saveTimer: number | null;
  lastSavedText: string;
  lastSavedFrontmatter: string;
  selection: { anchor: number; head: number } | null;
  scrollTop: number;
  conflict: boolean;
}

export type PaletteMode = "all" | "files";

export const app = {
  vault: null as VaultInfo | null,
  tree: [] as TreeNode[],
  tabs: [] as Tab[],
  activePath: null as string | null,
  history: [] as string[],
  historyIndex: -1,
  settings: null as Settings | null,
  theme: null as Theme | null,
  sidebarOpen: false,
  backlinksOpen: false,
  paletteOpen: false,
  paletteMode: "all" as PaletteMode,
  closedFolds: new Set<string>(),
  treeFilter: "",
  settingsDraft: null as Settings | null,
  contextTarget: null as string | null,
  contextKind: "tree" as "tree" | "tab",
  indexRefreshTimer: 0,
};

export function activeTab(): Tab | null {
  return app.tabs.find((tab) => tab.path === app.activePath) ?? null;
}

export function findTab(path: string): Tab | null {
  return app.tabs.find((tab) => tab.path === path) ?? null;
}

export function pushHistory(path: string): void {
  if (app.historyIndex >= 0 && app.history[app.historyIndex] === path) return;
  app.history = app.history.slice(0, app.historyIndex + 1);
  app.history.push(path);
  app.historyIndex = app.history.length - 1;
}

export function stepHistory(direction: -1 | 1): string | null {
  const next = app.historyIndex + direction;
  if (next < 0 || next >= app.history.length) return null;
  app.historyIndex = next;
  return app.history[next];
}

export function anyDirty(): boolean {
  return app.tabs.some((tab) => tab.dirty);
}

export function loadFolds(): void {
  try {
    const raw = window.localStorage.getItem("omawrite.folds");
    if (!raw) return;
    const paths = JSON.parse(raw) as string[];
    app.closedFolds = new Set(paths);
  } catch {
    app.closedFolds = new Set();
  }
}

export function saveFolds(): void {
  try {
    window.localStorage.setItem(
      "omawrite.folds",
      JSON.stringify([...app.closedFolds]),
    );
  } catch {
    // A full or unavailable localStorage must not take the editor down.
  }
}
