import { clear, el } from "./dom";
import { app, type Tab } from "./state";

export interface TabsHost {
  activate(path: string): void;
  close(path: string): void;
  showContextMenu(path: string, x: number, y: number): void;
}

let host: TabsHost | null = null;

export function setTabsHost(next: TabsHost): void {
  host = next;
}

export function renderTabs(): void {
  const bar = document.getElementById("tabs");
  if (!bar) return;
  clear(bar);
  for (const tab of app.tabs) {
    bar.appendChild(renderTab(tab));
  }
  const active = bar.querySelector<HTMLElement>(".tab.active");
  active?.scrollIntoView({ block: "nearest", inline: "nearest" });
}

function renderTab(tab: Tab): HTMLElement {
  const button = el("button", "tab");
  if (tab.path === app.activePath) button.classList.add("active");
  if (tab.dirty) button.classList.add("dirty");
  button.title = tab.doc.path;
  button.dataset.path = tab.path;

  button.appendChild(el("span", "tab-title", tab.doc.name));
  if (tab.dirty) {
    button.appendChild(el("span", "tab-dirty", "●"));
  }
  const close = el("button", "tab-close", "×");
  close.title = "Close tab";
  close.addEventListener("click", (event) => {
    event.stopPropagation();
    host?.close(tab.path);
  });
  button.appendChild(close);

  button.addEventListener("click", () => host?.activate(tab.path));
  button.addEventListener("auxclick", (event) => {
    if (event.button === 1) {
      event.preventDefault();
      host?.close(tab.path);
    }
  });
  button.addEventListener("contextmenu", (event) => {
    event.preventDefault();
    event.stopPropagation();
    host?.showContextMenu(tab.path, event.clientX, event.clientY);
  });
  return button;
}
