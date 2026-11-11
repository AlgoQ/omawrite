export function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  className?: string,
  text?: string,
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

export function clear(node: Element): void {
  while (node.firstChild) node.removeChild(node.firstChild);
}

export function escapeHtml(value: string): string {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

export function debounce<T extends (...args: never[]) => void>(
  callback: T,
  delay: number,
): (...args: Parameters<T>) => void {
  let timer: number | undefined;
  return (...args: Parameters<T>) => {
    if (timer !== undefined) window.clearTimeout(timer);
    timer = window.setTimeout(() => callback(...args), delay);
  };
}

export function basename(path: string): string {
  const normalized = path.replaceAll("\\", "/");
  return normalized.slice(normalized.lastIndexOf("/") + 1);
}

export function dirname(path: string): string {
  const normalized = path.replaceAll("\\", "/");
  const index = normalized.lastIndexOf("/");
  return index < 0 ? "" : normalized.slice(0, index);
}

export function stem(path: string): string {
  const name = basename(path);
  return name.replace(/\.(md|markdown)$/i, "");
}

export function formatWords(count: number): string {
  return `${count} ${count === 1 ? "word" : "words"}`;
}

export function wordCount(text: string): number {
  const trimmed = text.trim();
  if (!trimmed) return 0;
  return trimmed.split(/\s+/u).filter(Boolean).length;
}

export function isMac(): boolean {
  return navigator.platform.toLowerCase().includes("mac");
}

export function modKey(event: KeyboardEvent): boolean {
  return isMac() ? event.metaKey : event.ctrlKey;
}

export function closestAction(target: EventTarget | null): string | null {
  if (!(target instanceof Element)) return null;
  const element = target.closest<HTMLElement>("[data-action]");
  return element?.dataset.action ?? null;
}

export function showToast(message: string): void {
  const toast = document.getElementById("toast");
  if (!toast) return;
  toast.textContent = message;
  toast.hidden = false;
  toast.classList.add("toast-visible");
  window.clearTimeout(Number(toast.dataset.timer ?? 0));
  const timer = window.setTimeout(() => {
    toast.classList.remove("toast-visible");
    toast.hidden = true;
  }, 2600);
  toast.dataset.timer = String(timer);
}

export function formatKeySequence(sequence: string): string {
  if (!sequence) return "—";
  return sequence
    .replaceAll("Mod", isMac() ? "⌘" : "Ctrl")
    .replaceAll("Shift", "⇧")
    .replaceAll("Alt", "⌥")
    .replaceAll("+", " ");
}
