import { api } from "./api";
import { clear, el, showToast } from "./dom";
import { activeTab } from "./state";

export interface BacklinksHost {
  openNote(path: string): void;
}

let host: BacklinksHost | null = null;

export function setBacklinksHost(next: BacklinksHost): void {
  host = next;
}

export async function renderBacklinks(): Promise<void> {
  const list = document.getElementById("backlink-list");
  if (!list) return;
  clear(list);
  const tab = activeTab();
  if (!tab || !tab.doc.inVault) {
    list.appendChild(el("p", "backlink-empty", "No backlinks."));
    return;
  }
  try {
    const backlinks = await api.backlinks(tab.path);
    if (!backlinks.length) {
      list.appendChild(el("p", "backlink-empty", "No backlinks yet."));
      return;
    }
    for (const backlink of backlinks) {
      const item = el("button", "backlink");
      item.appendChild(el("span", "backlink-name", backlink.name));
      const snippet = el("span", "backlink-snippet", backlink.snippet);
      snippet.title = `Line ${backlink.line}`;
      item.appendChild(snippet);
      item.addEventListener("click", () => host?.openNote(backlink.path));
      list.appendChild(item);
    }
  } catch (error) {
    showToast(String(error));
  }
}
