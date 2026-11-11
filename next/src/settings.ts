import { api } from "./api";
import { commands } from "./commands";
import { clear, el, formatKeySequence, showToast } from "./dom";
import { eventToSequence } from "./keymap";
import { app } from "./state";
import type { Settings } from "./types";

let captureId: string | null = null;
let captureListener: ((event: KeyboardEvent) => void) | null = null;

export function openSettings(): void {
  const overlay = document.getElementById("settings");
  if (!overlay || !app.settings) return;
  app.settingsDraft = structuredClone(app.settings);
  overlay.hidden = false;
  renderSettings();
}

export function closeSettings(): void {
  const overlay = document.getElementById("settings");
  if (overlay) overlay.hidden = true;
  app.settingsDraft = null;
  if (app.settings) {
    document.documentElement.style.setProperty(
      "--editor-font-size",
      `${app.settings.fontSize}px`,
    );
  }
  stopCapture();
}

export function isSettingsOpen(): boolean {
  return app.settingsDraft !== null;
}

export function renderSettings(): void {
  const draft = app.settingsDraft;
  if (!draft) return;

  const scroll = document.getElementById("setting-scroll") as HTMLInputElement | null;
  const scrollValue = document.getElementById("setting-scroll-value");
  if (scroll) {
    scroll.value = String(draft.scrollSpeed);
    if (scrollValue) scrollValue.textContent = `${draft.scrollSpeed}×`;
  }
  const font = document.getElementById("setting-font") as HTMLInputElement | null;
  const fontValue = document.getElementById("setting-font-value");
  if (font) {
    font.value = String(draft.fontSize);
    if (fontValue) fontValue.textContent = `${draft.fontSize}px`;
  }
  const theme = document.getElementById("setting-theme") as HTMLSelectElement | null;
  if (theme) theme.value = draft.theme;

  const list = document.getElementById("keybinding-list");
  if (!list) return;
  clear(list);
  for (const command of commands) {
    const row = el("div", "keybinding-row");
    row.appendChild(el("span", "keybinding-title", command.title));
    const binding = draft.keymap[command.id] ?? command.keys;
    const button = el(
      "button",
      "keybinding-key",
      captureId === command.id ? "Press keys…" : formatKeySequence(binding),
    );
    button.addEventListener("click", () => startCapture(command.id));
    row.appendChild(button);
    const reset = el("button", "keybinding-reset", "↺");
    reset.title = "Use the default shortcut";
    reset.addEventListener("click", () => {
      delete draft.keymap[command.id];
      stopCapture();
      renderSettings();
    });
    row.appendChild(reset);
    list.appendChild(row);
  }

  if (scroll) {
    scroll.oninput = () => {
      draft.scrollSpeed = Number(scroll.value);
      if (scrollValue) scrollValue.textContent = `${draft.scrollSpeed}×`;
    };
  }
  if (font) {
    font.oninput = () => {
      draft.fontSize = Number(font.value);
      if (fontValue) fontValue.textContent = `${draft.fontSize}px`;
      document.documentElement.style.setProperty(
        "--editor-font-size",
        `${draft.fontSize}px`,
      );
    };
  }
  if (theme) {
    theme.onchange = () => {
      draft.theme = theme.value as Settings["theme"];
    };
  }
}

function startCapture(id: string): void {
  stopCapture();
  captureId = id;
  renderSettings();
  captureListener = (event: KeyboardEvent) => {
    event.preventDefault();
    event.stopPropagation();
    if (event.key === "Escape") {
      stopCapture();
      renderSettings();
      return;
    }
    if (event.key === "Backspace" || event.key === "Delete") {
      if (app.settingsDraft) app.settingsDraft.keymap[id] = "";
      stopCapture();
      renderSettings();
      return;
    }
    const sequence = eventToSequence(event);
    if (!sequence) return;
    if (app.settingsDraft) app.settingsDraft.keymap[id] = sequence;
    stopCapture();
    renderSettings();
  };
  window.addEventListener("keydown", captureListener, true);
}

function stopCapture(): void {
  if (captureListener) {
    window.removeEventListener("keydown", captureListener, true);
    captureListener = null;
  }
  captureId = null;
}

export async function saveSettings(): Promise<void> {
  const draft = app.settingsDraft;
  if (!draft) return;
  try {
    const saved = await api.settingsSet(draft);
    app.settings = saved;
    app.settingsDraft = null;
    document.documentElement.style.setProperty(
      "--editor-font-size",
      `${saved.fontSize}px`,
    );
    closeSettings();
    window.dispatchEvent(new CustomEvent("omawrite:settings-changed"));
  } catch (error) {
    showToast(String(error));
  }
}

export function resetSettings(): void {
  if (!app.settingsDraft) return;
  app.settingsDraft.scrollSpeed = 2;
  app.settingsDraft.fontSize = 20;
  app.settingsDraft.theme = "auto";
  app.settingsDraft.keymap = {};
  document.documentElement.style.setProperty("--editor-font-size", "20px");
  renderSettings();
}
