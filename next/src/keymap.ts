import { commands } from "./commands";
import { modKey } from "./dom";
import { app } from "./state";

let enabled = true;

export function setKeymapEnabled(value: boolean): void {
  enabled = value;
}

export function sequenceForCommand(id: string): string {
  return app.settings?.keymap[id] ?? commands.find((command) => command.id === id)?.keys ?? "";
}

export function eventToSequence(event: KeyboardEvent): string | null {
  if (["Control", "Meta", "Shift", "Alt"].includes(event.key)) return null;
  if (event.key === "Escape") return "Escape";
  const modifiers: string[] = [];
  const isMod = modKey(event);
  if (isMod) modifiers.push("Mod");
  if (event.shiftKey) modifiers.push("Shift");
  if (event.altKey) modifiers.push("Alt");

  let key = event.key;
  if (key === " ") key = "Space";
  else if (key.length === 1) key = key.toLowerCase();
  else if (key.startsWith("Arrow")) {
    // ArrowLeft stays ArrowLeft; this branch documents the intent.
  }
  if (!key) return null;
  return [...modifiers, key].join("+");
}

export function installKeymap(): void {
  window.addEventListener("keydown", (event) => {
    if (!enabled) return;
    if (event.defaultPrevented) return;
    const sequence = eventToSequence(event);
    if (!sequence) return;

    for (const command of commands) {
      const binding = sequenceForCommand(command.id);
      if (!binding) continue;
      if (binding.toLowerCase() !== sequence.toLowerCase()) continue;
      // Let the palette and settings fields handle their own keys.
      if (isTextInput(event.target) && !binding.startsWith("Mod")) return;
      event.preventDefault();
      event.stopPropagation();
      command.run();
      return;
    }
  });
}

function isTextInput(target: EventTarget | null): boolean {
  if (!(target instanceof HTMLElement)) return false;
  return (
    target.tagName === "INPUT" ||
    target.tagName === "TEXTAREA" ||
    target.isContentEditable
  );
}
