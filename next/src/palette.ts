import { api } from "./api";
import { matchingCommands, type Command } from "./commands";
import { clear, el, formatKeySequence, showToast } from "./dom";
import { app, type PaletteMode } from "./state";
import type { NoteDoc, Suggestion } from "./types";

type PaletteItem =
  | { kind: "command"; command: Command }
  | { kind: "file"; suggestion: Suggestion };

export interface PaletteHost {
  openNote(path: string): void;
}

let host: PaletteHost | null = null;
let items: PaletteItem[] = [];
let selected = 0;
let mode: PaletteMode = "all";
let previewToken = 0;
let searchToken = 0;
let previewTimer: number | null = null;

export function setPaletteHost(next: PaletteHost): void {
  host = next;
}

export function openPalette(nextMode: PaletteMode = "all"): void {
  const overlay = document.getElementById("palette");
  const input = document.getElementById("palette-input") as HTMLInputElement | null;
  if (!overlay || !input) return;
  mode = nextMode;
  app.paletteOpen = true;
  app.paletteMode = nextMode;
  overlay.hidden = false;
  input.value = "";
  const badge = document.getElementById("palette-mode");
  if (badge) badge.textContent = nextMode === "files" ? "FILES" : "ALL";
  input.placeholder =
    nextMode === "files"
      ? "Find a note…"
      : "Find a note or run a command…";
  selected = 0;
  input.focus();
  void refresh("");
}

export function closePalette(): void {
  const overlay = document.getElementById("palette");
  if (overlay) overlay.hidden = true;
  app.paletteOpen = false;
  const preview = document.getElementById("palette-preview");
  if (preview) preview.hidden = true;
}

export function isPaletteOpen(): boolean {
  return app.paletteOpen;
}

async function refresh(query: string): Promise<void> {
  const token = ++searchToken;
  const commands: PaletteItem[] =
    mode === "files"
      ? []
      : matchingCommands(query).map((command) => ({ kind: "command", command }));
  let files: PaletteItem[] = [];
  try {
    if (app.vault) {
      const suggestions = await api.suggest(query);
      files = suggestions.map((suggestion) => ({ kind: "file", suggestion }));
    }
  } catch (error) {
    showToast(String(error));
  }
  if (token !== searchToken) return;
  items = [...commands, ...files].slice(0, 80);
  if (selected >= items.length) selected = Math.max(0, items.length - 1);
  renderList();
  schedulePreview();
}

function renderList(): void {
  const list = document.getElementById("palette-list");
  if (!list) return;
  clear(list);
  if (!items.length) {
    list.appendChild(el("p", "palette-empty", "No matches."));
    return;
  }
  items.forEach((item, index) => {
    const row = el("button", "palette-row");
    if (index === selected) row.classList.add("selected");
    row.dataset.index = String(index);
    const badge = el(
      "span",
      "palette-badge",
      item.kind === "command" ? "CMD" : "FILE",
    );
    row.appendChild(badge);
    const text = el("span", "palette-row-text");
    if (item.kind === "command") {
      text.appendChild(el("span", "palette-title", item.command.title));
      text.appendChild(el("span", "palette-subtitle", item.command.category));
    } else {
      text.appendChild(el("span", "palette-title", item.suggestion.name));
      text.appendChild(el("span", "palette-subtitle", item.suggestion.path));
    }
    row.appendChild(text);
    if (item.kind === "command" && item.command.keys) {
      row.appendChild(el("span", "palette-keys", formatKeySequence(item.command.keys)));
    } else if (item.kind === "file") {
      row.appendChild(el("span", "palette-keys", "↵"));
    }
    row.addEventListener("click", () => {
      selected = index;
      activate();
    });
    row.addEventListener("mouseenter", () => {
      if (selected === index) return;
      selected = index;
      renderList();
      schedulePreview();
    });
    list.appendChild(row);
  });
  list.querySelector(".palette-row.selected")?.scrollIntoView({ block: "nearest" });
}

function moveSelection(delta: number): void {
  if (!items.length) return;
  selected = Math.max(0, Math.min(items.length - 1, selected + delta));
  renderList();
  schedulePreview();
}

function activate(): void {
  const item = items[selected];
  if (!item) return;
  closePalette();
  if (item.kind === "command") {
    item.command.run();
  } else {
    host?.openNote(item.suggestion.path);
  }
}

function schedulePreview(): void {
  if (previewTimer !== null) window.clearTimeout(previewTimer);
  previewTimer = window.setTimeout(() => {
    void renderPreview();
  }, 110);
}

async function renderPreview(): Promise<void> {
  const preview = document.getElementById("palette-preview");
  if (!preview) return;
  const item = items[selected];
  if (!item) {
    preview.hidden = true;
    return;
  }
  const token = ++previewToken;
  preview.hidden = false;
  clear(preview);

  if (item.kind === "command") {
    preview.appendChild(el("h3", "preview-title", item.command.title));
    preview.appendChild(el("p", "preview-meta", item.command.category));
    if (item.command.keys) {
      preview.appendChild(
        el("p", "preview-meta", formatKeySequence(item.command.keys)),
      );
    }
    preview.appendChild(
      el(
        "p",
        "preview-body",
        "Runs the same handler as the keyboard shortcut and the Settings panel.",
      ),
    );
    return;
  }

  preview.appendChild(el("h3", "preview-title", item.suggestion.name));
  preview.appendChild(el("p", "preview-meta", item.suggestion.path));
  try {
    const doc = await api.readNote(item.suggestion.path);
    if (token !== previewToken) return;
    renderPreviewDoc(preview, doc);
  } catch (error) {
    preview.appendChild(el("p", "preview-body", String(error)));
  }
}

function renderPreviewDoc(preview: HTMLElement, doc: NoteDoc): void {
  if (doc.tags.length) {
    const tags = el("div", "preview-tags");
    for (const tag of doc.tags) tags.appendChild(el("span", "preview-tag", `#${tag}`));
    preview.appendChild(tags);
  }
  const body = doc.body.trim();
  if (!body) {
    preview.appendChild(el("p", "preview-body", "Empty note."));
    return;
  }
  const lines = body.split("\n").slice(0, 14).join("\n");
  preview.appendChild(el("pre", "preview-body", lines));
}

export function paletteKeydown(event: KeyboardEvent): void {
  if (!app.paletteOpen) return;
  if (event.key === "Escape") {
    event.preventDefault();
    closePalette();
    return;
  }
  if (event.key === "ArrowDown") {
    event.preventDefault();
    moveSelection(1);
    return;
  }
  if (event.key === "ArrowUp") {
    event.preventDefault();
    moveSelection(-1);
    return;
  }
  if (event.key === "PageDown") {
    event.preventDefault();
    moveSelection(8);
    return;
  }
  if (event.key === "PageUp") {
    event.preventDefault();
    moveSelection(-8);
    return;
  }
  if (event.key === "Enter") {
    event.preventDefault();
    activate();
  }
}

export function paletteInput(value: string): void {
  selected = 0;
  void refresh(value);
}
