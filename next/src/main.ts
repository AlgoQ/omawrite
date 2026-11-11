import "./styles.css";

import { listen } from "@tauri-apps/api/event";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { open, save as saveFileDialog } from "@tauri-apps/plugin-dialog";
import { openUrl, revealItemInDir } from "@tauri-apps/plugin-opener";
import { api } from "./api";
import { renderBacklinks, setBacklinksHost } from "./backlinks";
import { setCommandHost } from "./commands";
import {
  anyDirty,
  app,
  activeTab,
  findTab,
  loadFolds,
  pushHistory,
  stepHistory,
  type Tab,
} from "./state";
import {
  applyFontSize,
  currentView,
  insertMarkdownLink,
  insertText,
  insertWikiLink,
  mountEditor,
  resetTabState,
  setEditorHooks,
  showEmptyState,
  showTab,
  wrapSelection,
} from "./editor";
import {
  closestAction,
  dirname,
  el,
  escapeHtml,
  formatWords,
  showToast,
  wordCount,
} from "./dom";
import { installKeymap } from "./keymap";
import {
  closePalette,
  openPalette,
  paletteInput,
  paletteKeydown,
  setPaletteHost,
} from "./palette";
import { renderProperties, setPropertiesHost } from "./properties";
import {
  closeSettings,
  isSettingsOpen,
  openSettings,
  resetSettings,
  saveSettings,
} from "./settings";
import { renderTabs, setTabsHost } from "./tabs";
import { renderTree, setTreeHost } from "./tree";
import type { NoteDoc, Theme } from "./types";

let cursorLine = 1;
let cursorColumn = 1;
let treeRefreshTimer = 0;
let settingsTimer = 0;
let lastNoteTimer = 0;

void boot();

async function boot(): Promise<void> {
  loadFolds();
  setCommandHost({ runCommand });
  setTreeHost({
    openNote: (path) => void openNote(path),
    showContextMenu: (path, x, y) => showContextMenu(path, x, y, "tree"),
  });
  setTabsHost({
    activate: activateTab,
    close: (path) => void closeTab(path),
    showContextMenu: (path, x, y) => showContextMenu(path, x, y, "tab"),
  });
  setPaletteHost({ openNote: (path) => void openNote(path) });
  setPropertiesHost({
    openNote: (path) => void openNote(path),
    changed: touchTab,
  });
  setBacklinksHost({ openNote: (path) => void openNote(path) });

  mountEditor();
  setEditorHooks({
    contentChanged: touchTab,
    selectionChanged: (tab, state) => {
      const line = state.doc.lineAt(state.selection.main.head);
      cursorLine = line.number;
      cursorColumn = state.selection.main.head - line.from + 1;
      updateStatus(tab);
    },
    pasteImage: (tab, file) => void pasteImage(tab, file),
    save: (tab) => void saveTab(tab),
    saveAs: (tab) => void saveAs(tab),
    palette: (mode = "all") => openPalette(mode),
    toggleSidebar,
    toggleBacklinks,
    print: () => printNote(),
    openFile: () => void openFileDialog(),
    closeTab: (tab) => void closeTab(tab.path),
  });

  installGlobalHandlers();
  installKeymap();

  try {
    const [settings, theme] = await Promise.all([api.settingsGet(), api.theme()]);
    app.settings = settings;
    app.sidebarOpen = settings.sidebarOpen;
    app.backlinksOpen = settings.backlinksOpen;
    applyTheme(theme);
    applyFontSize(settings.fontSize);
    updateSidebar();
    updateBacklinksPanel();
  } catch (error) {
    console.error(error);
    showToast("Run Omawrite through Tauri: npm run tauri:dev");
  }

  try {
    await openVault(null);
  } catch (error) {
    showToast(String(error));
  }

  await installTauriEvents();
  await restoreSession();
}

async function openVault(path: string | null): Promise<void> {
  const info = await api.openVault(path);
  app.vault = info;
  app.tree = await api.tree();
  if (!app.tabs.length) {
    app.sidebarOpen = true;
    updateSidebar();
    showEmptyState();
  }
  renderTree();
  const name = document.getElementById("vault-name");
  if (name) name.textContent = info.name;
  showToast(`Opened ${info.name}`);
}

async function openVaultDialog(): Promise<void> {
  const selected = await open({
    directory: true,
    multiple: false,
    defaultPath: app.vault?.root,
  });
  if (typeof selected !== "string") return;
  try {
    await openVault(selected);
    await refreshTree();
  } catch (error) {
    showToast(String(error));
  }
}

async function openNote(path: string, fromHistory = false): Promise<void> {
  try {
    let tab = findTab(path);
    if (!tab) {
      const doc = await api.readNote(path);
      tab = makeTab(doc);
      app.tabs.push(tab);
    }
    saveEditorState();
    if (app.activePath !== tab.path) {
      app.activePath = tab.path;
      if (!fromHistory) pushHistory(tab.path);
    } else if (!fromHistory) {
      pushHistory(tab.path);
    }
    showTab(tab);
    renderAll();
    if (app.backlinksOpen) void renderBacklinks();
    persistLastNote(tab.path);
  } catch (error) {
    showToast(String(error));
  }
}

function activateTab(path: string): void {
  const tab = findTab(path);
  if (!tab || app.activePath === path) return;
  saveEditorState();
  app.activePath = path;
  pushHistory(path);
  showTab(tab);
  renderAll();
  if (app.backlinksOpen) void renderBacklinks();
  persistLastNote(path);
}

async function closeTab(path: string): Promise<void> {
  const tab = findTab(path);
  if (!tab) return;
  if (tab.dirty) {
    const close = window.confirm(`Close ${tab.doc.name} without saving?`);
    if (!close) return;
  }
  if (tab.saveTimer !== null) window.clearTimeout(tab.saveTimer);
  const index = app.tabs.findIndex((candidate) => candidate.path === path);
  app.tabs.splice(index, 1);
  if (app.activePath === path) {
    const next = app.tabs[Math.min(index, app.tabs.length - 1)] ?? null;
    if (next) {
      app.activePath = next.path;
      showTab(next);
    } else {
      app.activePath = null;
      showEmptyState();
    }
  }
  renderAll();
  if (app.backlinksOpen) void renderBacklinks();
}

async function saveTab(tab: Tab, force = false): Promise<void> {
  if (tab.saving) return;
  if (!tab.dirty && !force) return;
  tab.saving = true;
  try {
    const result = await api.saveNote(
      tab.path,
      tab.doc.frontmatter,
      tab.doc.body,
      tab.doc.modified,
      force,
    );
    if (result.conflict) {
      tab.saving = false;
      const overwrite = window.confirm(
        "This note changed on disk. Overwrite it with your version?",
      );
      if (overwrite) {
        await saveTab(tab, true);
        return;
      }
      const reload = window.confirm(
        "Reload the version on disk? Your unsaved changes will be lost.",
      );
      if (reload) {
        const doc = await api.readNote(tab.path);
        tab.doc = doc;
        tab.lastSavedText = doc.body;
        tab.lastSavedFrontmatter = doc.frontmatter;
        tab.dirty = false;
        tab.conflict = false;
        resetTabState(tab);
        renderAll();
      }
      return;
    }
    if (!result.success) {
      showToast("The note could not be saved.");
      return;
    }
    tab.doc.modified = result.modified;
    tab.lastSavedText = tab.doc.body;
    tab.lastSavedFrontmatter = tab.doc.frontmatter;
    tab.dirty = false;
    tab.conflict = false;
    renderTabs();
    updateStatus(tab);
    updateTitle();
    scheduleTreeRefresh();
  } catch (error) {
    showToast(String(error));
  } finally {
    tab.saving = false;
  }
}

async function saveAs(tab: Tab): Promise<void> {
  const selected = await saveFileDialog({
    defaultPath: tab.doc.absolutePath,
    filters: [{ name: "Markdown", extensions: ["md", "markdown"] }],
  });
  if (!selected) return;
  const target =
    /\.(md|markdown)$/i.test(selected) ? selected : `${selected}.md`;
  try {
    const result = await api.saveNote(
      target,
      tab.doc.frontmatter,
      tab.doc.body,
      null,
      true,
    );
    if (!result.success) {
      showToast("The note could not be saved.");
      return;
    }
    const doc = await api.readNote(target);
    const previous = tab.path;
    tab.path = doc.path;
    tab.doc = doc;
    tab.lastSavedText = doc.body;
    tab.lastSavedFrontmatter = doc.frontmatter;
    tab.dirty = false;
    tab.conflict = false;
    if (app.activePath === previous) app.activePath = doc.path;
    app.history = app.history.map((path) => (path === previous ? doc.path : path));
    resetTabState(tab);
    await refreshTree();
    renderAll();
    persistLastNote(doc.path);
  } catch (error) {
    showToast(String(error));
  }
}

async function newNote(): Promise<void> {
  if (!app.vault) {
    showToast("Open a slipbox first.");
    return;
  }
  try {
    const doc = await api.createNote(null, null);
    await refreshTree();
    await openNote(doc.path);
    currentView()?.focus();
  } catch (error) {
    showToast(String(error));
  }
}

async function openFileDialog(): Promise<void> {
  const selected = await open({
    multiple: false,
    directory: false,
    filters: [{ name: "Markdown", extensions: ["md", "markdown"] }],
  });
  if (typeof selected === "string") await openNote(selected);
}

function touchTab(tab: Tab): void {
  tab.dirty =
    tab.doc.body !== tab.lastSavedText ||
    tab.doc.frontmatter !== tab.lastSavedFrontmatter;
  renderTabs();
  updateStatus(tab);
  updateTitle();
  if (tab.saveTimer !== null) window.clearTimeout(tab.saveTimer);
  if (tab.dirty) {
    tab.saveTimer = window.setTimeout(() => {
      tab.saveTimer = null;
      void saveTab(tab);
    }, 900);
  } else {
    tab.saveTimer = null;
  }
}

function saveEditorState(): void {
  const view = currentView();
  const tab = app.tabs.find((candidate) => candidate.path === app.activePath);
  if (!view || !tab) return;
  tab.editorState = view.state;
  tab.selection = view.state.selection.main;
  tab.scrollTop = view.scrollDOM.scrollTop;
}

function makeTab(doc: NoteDoc): Tab {
  return {
    path: doc.path,
    doc,
    editorState: null,
    dirty: false,
    saving: false,
    saveTimer: null,
    lastSavedText: doc.body,
    lastSavedFrontmatter: doc.frontmatter,
    selection: null,
    scrollTop: 0,
    conflict: false,
  };
}

async function refreshTree(): Promise<void> {
  if (!app.vault) return;
  try {
    app.tree = await api.tree();
    renderTree();
  } catch (error) {
    showToast(String(error));
  }
}

function scheduleTreeRefresh(): void {
  if (treeRefreshTimer) window.clearTimeout(treeRefreshTimer);
  treeRefreshTimer = window.setTimeout(() => {
    treeRefreshTimer = 0;
    void refreshTree();
    if (app.backlinksOpen) void renderBacklinks();
  }, 260);
}

function toggleSidebar(): void {
  setSidebar(!app.sidebarOpen);
}

function setSidebar(value: boolean): void {
  app.sidebarOpen = value;
  if (app.settings) {
    app.settings.sidebarOpen = value;
    persistSettings();
  }
  updateSidebar();
  if (value) renderTree();
}

function updateSidebar(): void {
  const sidebar = document.getElementById("sidebar");
  if (sidebar) sidebar.hidden = !app.sidebarOpen;
}

function toggleBacklinks(): void {
  setBacklinks(!app.backlinksOpen);
}

function setBacklinks(value: boolean): void {
  app.backlinksOpen = value;
  if (app.settings) {
    app.settings.backlinksOpen = value;
    persistSettings();
  }
  updateBacklinksPanel();
  if (value) void renderBacklinks();
}

function updateBacklinksPanel(): void {
  const panel = document.getElementById("backlinks");
  if (panel) panel.hidden = !app.backlinksOpen;
}

function updateExternalBanner(): void {
  const tab = activeTab();
  const banner = document.getElementById("external-banner");
  if (banner) banner.hidden = !(tab && !tab.doc.inVault);
}

function renderAll(): void {
  renderTabs();
  renderTree();
  renderProperties();
  updateExternalBanner();
  updateStatus();
  updateTitle();
  updateSidebar();
  updateBacklinksPanel();
}

function updateStatus(tab = activeTab()): void {
  const left = document.getElementById("status-left");
  const right = document.getElementById("status-right");
  if (!left || !right) return;
  if (!tab) {
    left.textContent = "No note open";
    right.textContent = "Ctrl+P to search";
    return;
  }
  const words = formatWords(wordCount(tab.doc.body));
  const state = tab.dirty ? " · unsaved" : " · saved";
  const location = tab.doc.inVault ? "" : " · outside slipbox";
  left.textContent = `${tab.doc.path}${state} · ${words}${location}`;
  right.textContent = `Ln ${cursorLine}, Col ${cursorColumn}`;
}

function printNote(): void {
  const tab = activeTab();
  if (!tab) return;
  let page = document.getElementById("print-view");
  if (!page) {
    page = document.createElement("div");
    page.id = "print-view";
    document.body.appendChild(page);
  }
  page.innerHTML = `<h1>${escapeHtml(tab.doc.name)}</h1><pre>${escapeHtml(
    tab.doc.body,
  )}</pre>`;
  window.print();
}

function updateTitle(): void {
  const tab = activeTab();
  document.title = `${tab?.dirty ? "* " : ""}${tab?.doc.name ?? "Omawrite"} - Omawrite`;
}

function applyTheme(theme: Theme): void {
  app.theme = theme;
  const root = document.documentElement;
  root.dataset.theme = theme.mode;
  root.style.colorScheme = theme.mode;
  root.style.setProperty("--bg", theme.background);
  root.style.setProperty("--fg", theme.foreground);
  root.style.setProperty("--muted", theme.muted);
  root.style.setProperty("--accent", theme.accent);
  root.style.setProperty("--selection", theme.selection);
  root.style.setProperty("--selection-fg", theme.selectionForeground);
  root.style.setProperty("--code-bg", theme.codeBackground);
  root.style.setProperty("--border", theme.border);
  root.style.setProperty("--panel", theme.panel);
}

async function loadTheme(): Promise<void> {
  try {
    applyTheme(await api.theme());
  } catch (error) {
    showToast(String(error));
  }
}

function persistSettings(): void {
  if (settingsTimer) window.clearTimeout(settingsTimer);
  settingsTimer = window.setTimeout(async () => {
    if (!app.settings) return;
    try {
      app.settings = await api.settingsSet(app.settings);
    } catch (error) {
      showToast(String(error));
    }
  }, 300);
}

function persistLastNote(path: string): void {
  if (lastNoteTimer) window.clearTimeout(lastNoteTimer);
  lastNoteTimer = window.setTimeout(() => {
    try {
      window.localStorage.setItem("omawrite.lastNote", path);
    } catch {
      // Ignore a full localStorage.
    }
  }, 150);
}

async function restoreSession(): Promise<void> {
  const initial = await api.initialNote().catch(() => null);
  if (initial) {
    await openNote(initial);
    return;
  }
  let last: string | null = null;
  try {
    last = window.localStorage.getItem("omawrite.lastNote");
  } catch {
    last = null;
  }
  if (last && findTab(last) === null) {
    try {
      await openNote(last);
    } catch {
      // A deleted or moved last note is not an error the user must dismiss.
    }
  }
}

function runCommand(id: string): void {
  const tab = activeTab();
  switch (id) {
    case "palette":
      openPalette("all");
      break;
    case "palette-files":
      openPalette("files");
      break;
    case "new-note":
      void newNote();
      break;
    case "save":
      if (tab) void saveTab(tab);
      break;
    case "save-as":
      if (tab) void saveAs(tab);
      break;
    case "open-file":
      void openFileDialog();
      break;
    case "open-vault":
      void openVaultDialog();
      break;
    case "close-tab":
      if (tab) void closeTab(tab.path);
      break;
    case "print":
      printNote();
      break;
    case "toggle-sidebar":
      toggleSidebar();
      break;
    case "toggle-backlinks":
      toggleBacklinks();
      break;
    case "close-sidebar":
      setSidebar(false);
      break;
    case "close-backlinks":
      setBacklinks(false);
      break;
    case "settings":
      openSettings();
      break;
    case "zoom-in":
      changeFontSize(1);
      break;
    case "zoom-out":
      changeFontSize(-1);
      break;
    case "zoom-reset":
      setFontSize(20);
      break;
    case "bold":
      wrapSelection("**");
      break;
    case "italic":
      wrapSelection("*");
      break;
    case "code":
      wrapSelection("`");
      break;
    case "wiki-link":
      insertWikiLink();
      break;
    case "markdown-link":
      insertMarkdownLink();
      break;
    case "move-to-slipbox":
      void moveActiveToSlipbox(false);
      break;
    case "copy-to-slipbox":
      void moveActiveToSlipbox(true);
      break;
    case "move-to-folder":
      void moveActiveToFolder(false);
      break;
    case "copy-to-folder":
      void moveActiveToFolder(true);
      break;
    case "history-back":
      void stepHistoryAndOpen(-1);
      break;
    case "history-forward":
      void stepHistoryAndOpen(1);
      break;
    case "next-tab":
      cycleTab(1);
      break;
    case "previous-tab":
      cycleTab(-1);
      break;
    case "refresh-index":
      void refreshIndex();
      break;
    default:
      showToast(`Unknown action: ${id}`);
  }
}

function changeFontSize(delta: number): void {
  if (!app.settings) return;
  setFontSize(app.settings.fontSize + delta);
}

function setFontSize(size: number): void {
  if (!app.settings) return;
  app.settings.fontSize = Math.min(32, Math.max(12, size));
  applyFontSize(app.settings.fontSize);
  persistSettings();
}

async function stepHistoryAndOpen(direction: -1 | 1): Promise<void> {
  const path = stepHistory(direction);
  if (!path) return;
  const tab = findTab(path);
  if (tab) {
    saveEditorState();
    app.activePath = path;
    showTab(tab);
    renderAll();
    if (app.backlinksOpen) void renderBacklinks();
  } else {
    await openNote(path, true);
  }
}

function cycleTab(direction: 1 | -1): void {
  if (app.tabs.length < 2) return;
  const index = app.tabs.findIndex((tab) => tab.path === app.activePath);
  const next = (index + direction + app.tabs.length) % app.tabs.length;
  activateTab(app.tabs[next].path);
}

async function refreshIndex(): Promise<void> {
  try {
    const count = await api.refreshIndex();
    await refreshTree();
    if (app.backlinksOpen) void renderBacklinks();
    showToast(`Indexed ${count} notes`);
  } catch (error) {
    showToast(String(error));
  }
}

async function moveActiveToSlipbox(copy: boolean): Promise<void> {
  const tab = activeTab();
  if (!tab || !app.vault) return;
  await moveTo(tab.path, app.vault.root, copy);
}

async function moveActiveToFolder(copy: boolean): Promise<void> {
  const tab = activeTab();
  if (!tab || !app.vault) return;
  const selected = await open({
    directory: true,
    multiple: false,
    defaultPath: app.vault.root,
  });
  if (typeof selected !== "string") return;
  await moveTo(tab.path, selected, copy);
}

async function moveTo(path: string, folder: string, copy: boolean): Promise<void> {
  try {
    const result = await api.moveNote(path, folder, copy);
    if (!result.success) {
      showToast(result.error ?? "The note could not be moved.");
      return;
    }
    if (!copy && result.newPath) {
      updateTabAfterMove(path, result.newPath, result.newAbsolutePath);
    }
    await refreshTree();
    renderAll();
    if (app.backlinksOpen) void renderBacklinks();
    const changed = result.changedLinks
      ? ` · ${result.changedLinks} links rewritten`
      : "";
    const copied = copy ? "Copied" : "Moved";
    showToast(`${copied} ${result.newPath ?? result.newAbsolutePath}${changed}`);
    if (result.warnings.length) showToast(result.warnings[0]);
  } catch (error) {
    showToast(String(error));
  }
}

function updateTabAfterMove(
  from: string,
  to: string,
  absolute: string,
): void {
  const tab = findTab(from);
  if (!tab || tab.path === to) return;
  tab.path = to;
  tab.doc.path = to;
  tab.doc.absolutePath = absolute;
  tab.doc.inVault = !to.startsWith("/");
  if (app.activePath === from) app.activePath = to;
  app.history = app.history.map((path) => (path === from ? to : path));
  resetTabState(tab);
  persistLastNote(to);
}

async function pasteImage(tab: Tab, file: File): Promise<void> {
  try {
    const buffer = await file.arrayBuffer();
    const base64 = arrayBufferToBase64(buffer);
    const extension =
      file.type === "image/jpeg" ? "jpg" : file.type.split("/")[1] || "png";
    const result = await api.pasteImage(tab.path, base64, extension);
    insertText(result.markdown);
    showToast("Pasted image");
  } catch (error) {
    showToast(String(error));
  }
}

function arrayBufferToBase64(buffer: ArrayBuffer): string {
  const bytes = new Uint8Array(buffer);
  const chunk = 0x8000;
  let binary = "";
  for (let index = 0; index < bytes.length; index += chunk) {
    binary += String.fromCharCode(...bytes.subarray(index, index + chunk));
  }
  return window.btoa(binary);
}

function showContextMenu(
  path: string,
  x: number,
  y: number,
  kind: "tree" | "tab",
): void {
  hideContextMenu();
  const menu = document.getElementById("context-menu");
  if (!menu) return;
  app.contextTarget = path;
  app.contextKind = kind;
  menu.hidden = false;

  const add = (label: string, action: () => void, danger = false) => {
    const item = el("button", "context-item", label);
    if (danger) item.classList.add("danger");
    item.addEventListener("click", () => {
      hideContextMenu();
      action();
    });
    menu.appendChild(item);
  };
  add("Open", () => void openNote(path));
  if (kind === "tree") {
    add("New note in this folder", () => void newNoteInFolder(dirname(path)));
  }
  add("Move to slipbox", () => void moveTo(path, app.vault?.root ?? "", false));
  add("Copy to slipbox", () => void moveTo(path, app.vault?.root ?? "", true));
  add("Move to folder…", () => void moveToFolderDialog(path, false));
  add("Copy to folder…", () => void moveToFolderDialog(path, true));
  if (kind === "tab") {
    add("Close tab", () => void closeTab(path), true);
  }
  add("Reveal in file manager", () => {
    const tab = findTab(path);
    const absolute = tab?.doc.absolutePath ??
      (path.startsWith("/") ? path : `${app.vault?.root ?? ""}/${path}`);
    void revealItemInDir(absolute).catch((error: unknown) => showToast(String(error)));
  });

  menu.style.left = `${x}px`;
  menu.style.top = `${y}px`;
  requestAnimationFrame(() => {
    const rect = menu.getBoundingClientRect();
    if (rect.right > window.innerWidth - 8) {
      menu.style.left = `${Math.max(8, window.innerWidth - rect.width - 8)}px`;
    }
    if (rect.bottom > window.innerHeight - 8) {
      menu.style.top = `${Math.max(8, window.innerHeight - rect.height - 8)}px`;
    }
  });
}

function hideContextMenu(): void {
  const menu = document.getElementById("context-menu");
  if (menu) menu.hidden = true;
  app.contextTarget = null;
}

async function moveToFolderDialog(path: string, copy: boolean): Promise<void> {
  const selected = await open({
    directory: true,
    multiple: false,
    defaultPath: app.vault?.root,
  });
  if (typeof selected !== "string") return;
  await moveTo(path, selected, copy);
}

async function newNoteInFolder(folder: string): Promise<void> {
  if (!app.vault) return;
  try {
    const doc = await api.createNote(folder || null, null);
    await refreshTree();
    await openNote(doc.path);
  } catch (error) {
    showToast(String(error));
  }
}

function handleAction(action: string): void {
  switch (action) {
    case "new-note":
      void newNote();
      break;
    case "refresh-index":
      void refreshIndex();
      break;
    case "close-sidebar":
      setSidebar(false);
      break;
    case "open-vault":
      void openVaultDialog();
      break;
    case "open-file":
      void openFileDialog();
      break;
    case "toggle-sidebar":
      toggleSidebar();
      break;
    case "history-back":
      void stepHistoryAndOpen(-1);
      break;
    case "history-forward":
      void stepHistoryAndOpen(1);
      break;
    case "toggle-backlinks":
      toggleBacklinks();
      break;
    case "close-backlinks":
      setBacklinks(false);
      break;
    case "palette":
      openPalette("all");
      break;
    case "settings":
      openSettings();
      break;
    case "close-settings":
      closeSettings();
      break;
    case "save-settings":
      void saveSettings();
      break;
    case "reset-settings":
      resetSettings();
      break;
    case "move-to-slipbox":
      void moveActiveToSlipbox(false);
      break;
    case "copy-to-slipbox":
      void moveActiveToSlipbox(true);
      break;
    default:
      if (action.startsWith("zoom")) runCommand(action);
  }
}

function installGlobalHandlers(): void {
  document.addEventListener("click", (event) => {
    const action = closestAction(event.target);
    if (action) {
      event.preventDefault();
      handleAction(action);
      return;
    }
    if (!(event.target instanceof Element)) return;
    const anchor = event.target.closest<HTMLAnchorElement>("a.cm-link");
    if (anchor?.dataset.href) {
      event.preventDefault();
      void openMarkdownTarget(anchor.dataset.href);
      return;
    }
    hideContextMenu();
  });

  const filter = document.getElementById("tree-filter");
  filter?.addEventListener("input", () => {
    app.treeFilter = (filter as HTMLInputElement).value;
    renderTree();
  });

  const palette = document.getElementById("palette");
  palette?.addEventListener("click", (event) => {
    if (event.target === palette) closePalette();
  });
  const paletteInputElement = document.getElementById(
    "palette-input",
  ) as HTMLInputElement | null;
  paletteInputElement?.addEventListener("input", () => {
    paletteInput(paletteInputElement.value);
  });
  paletteInputElement?.addEventListener("keydown", (event) => {
    event.stopPropagation();
    paletteKeydown(event);
  });

  const settings = document.getElementById("settings");
  settings?.addEventListener("click", (event) => {
    if (event.target === settings) closeSettings();
  });
  settings?.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      event.preventDefault();
      closeSettings();
    }
  });

  window.addEventListener("keydown", (event) => {
    if (event.key !== "Escape") return;
    if (app.paletteOpen) {
      closePalette();
      return;
    }
    if (isSettingsOpen()) {
      closeSettings();
      return;
    }
    hideContextMenu();
  });

  window.addEventListener("omawrite:open-wiki", (event) => {
    const target = (event as CustomEvent<string>).detail;
    void openWiki(target);
  });

  window.addEventListener("omawrite:settings-changed", () => {
    void loadTheme();
    if (app.settings) applyFontSize(app.settings.fontSize);
  });
}

async function openWiki(target: string): Promise<void> {
  const tab = activeTab();
  if (!tab) return;
  try {
    const resolved = await api.resolveNote(target, tab.path);
    if (!resolved) {
      showToast(`No note named ${target}`);
      return;
    }
    await openNote(resolved);
  } catch (error) {
    showToast(String(error));
  }
}

async function openMarkdownTarget(url: string): Promise<void> {
  if (/^(https?:|mailto:)/.test(url)) {
    try {
      await openUrl(url);
    } catch (error) {
      showToast(String(error));
    }
    return;
  }
  if (url.startsWith("#")) return;
  const tab = activeTab();
  if (!tab) return;
  const clean = decodeURIComponent(url.split("#")[0].split("?")[0]);
  if (!/\.(md|markdown)$/i.test(clean)) return;
  const absolute = resolveRelative(tab.doc.absolutePath, clean);
  await openNote(absolute);
}

function resolveRelative(baseFile: string, relative: string): string {
  if (relative.startsWith("/")) return relative;
  const base = baseFile.slice(0, baseFile.lastIndexOf("/") + 1);
  const parts = `${base}${relative}`.split("/");
  const output: string[] = [];
  for (const part of parts) {
    if (!part || part === ".") continue;
    if (part === "..") output.pop();
    else output.push(part);
  }
  return `/${output.join("/")}`;
}

async function installTauriEvents(): Promise<void> {
  try {
    await listen("vault-changed", () => scheduleTreeRefresh());
    await listen("vault-opened", () => scheduleTreeRefresh());
    await listen("note-moved", () => scheduleTreeRefresh());
    await getCurrentWindow().onCloseRequested((event) => {
      if (!anyDirty()) return;
      if (!window.confirm("You have unsaved changes. Close Omawrite anyway?")) {
        event.preventDefault();
      }
    });
  } catch {
    // Running in a plain browser has no Tauri event bridge.
  }
}
