export interface Command {
  id: string;
  title: string;
  category: "File" | "Edit" | "View" | "Navigate" | "Note" | "App";
  keys: string;
  run(): void;
}

export interface CommandHost {
  runCommand(id: string): void;
}

let host: CommandHost | null = null;

export function setCommandHost(next: CommandHost): void {
  host = next;
}

export const commands: Command[] = [
  {
    id: "palette",
    title: "Command palette",
    category: "App",
    keys: "Mod+p",
    run: () => host?.runCommand("palette"),
  },
  {
    id: "palette-files",
    title: "Search notes",
    category: "Navigate",
    keys: "Mod+Shift+o",
    run: () => host?.runCommand("palette-files"),
  },
  {
    id: "new-note",
    title: "New note",
    category: "File",
    keys: "Mod+n",
    run: () => host?.runCommand("new-note"),
  },
  {
    id: "save",
    title: "Save note",
    category: "File",
    keys: "Mod+s",
    run: () => host?.runCommand("save"),
  },
  {
    id: "save-as",
    title: "Save note as…",
    category: "File",
    keys: "Mod+Shift+s",
    run: () => host?.runCommand("save-as"),
  },
  {
    id: "open-file",
    title: "Open a note anywhere…",
    category: "File",
    keys: "Mod+o",
    run: () => host?.runCommand("open-file"),
  },
  {
    id: "open-vault",
    title: "Open slipbox…",
    category: "File",
    keys: "",
    run: () => host?.runCommand("open-vault"),
  },
  {
    id: "close-tab",
    title: "Close tab",
    category: "File",
    keys: "Mod+w",
    run: () => host?.runCommand("close-tab"),
  },
  {
    id: "print",
    title: "Print note",
    category: "File",
    keys: "Mod+Shift+p",
    run: () => host?.runCommand("print"),
  },
  {
    id: "toggle-sidebar",
    title: "Toggle file tree",
    category: "View",
    keys: "Mod+b",
    run: () => host?.runCommand("toggle-sidebar"),
  },
  {
    id: "close-sidebar",
    title: "Close file tree",
    category: "View",
    keys: "Mod+Alt+ArrowLeft",
    run: () => host?.runCommand("close-sidebar"),
  },
  {
    id: "close-backlinks",
    title: "Close backlinks",
    category: "View",
    keys: "Mod+Alt+ArrowRight",
    run: () => host?.runCommand("close-backlinks"),
  },
  {
    id: "toggle-backlinks",
    title: "Toggle backlinks",
    category: "View",
    keys: "Mod+Shift+b",
    run: () => host?.runCommand("toggle-backlinks"),
  },
  {
    id: "settings",
    title: "Open settings",
    category: "App",
    keys: "Mod+,",
    run: () => host?.runCommand("settings"),
  },
  {
    id: "zoom-in",
    title: "Increase text size",
    category: "View",
    keys: "Mod+=",
    run: () => host?.runCommand("zoom-in"),
  },
  {
    id: "zoom-out",
    title: "Decrease text size",
    category: "View",
    keys: "Mod+-",
    run: () => host?.runCommand("zoom-out"),
  },
  {
    id: "zoom-reset",
    title: "Reset text size",
    category: "View",
    keys: "Mod+0",
    run: () => host?.runCommand("zoom-reset"),
  },
  {
    id: "bold",
    title: "Bold",
    category: "Edit",
    keys: "Mod+Alt+b",
    run: () => host?.runCommand("bold"),
  },
  {
    id: "italic",
    title: "Italic",
    category: "Edit",
    keys: "Mod+i",
    run: () => host?.runCommand("italic"),
  },
  {
    id: "code",
    title: "Inline code",
    category: "Edit",
    keys: "Mod+e",
    run: () => host?.runCommand("code"),
  },
  {
    id: "wiki-link",
    title: "Insert wikilink",
    category: "Edit",
    keys: "Mod+Shift+k",
    run: () => host?.runCommand("wiki-link"),
  },
  {
    id: "markdown-link",
    title: "Insert Markdown link",
    category: "Edit",
    keys: "Mod+k",
    run: () => host?.runCommand("markdown-link"),
  },
  {
    id: "move-to-slipbox",
    title: "Move current note to slipbox",
    category: "Note",
    keys: "",
    run: () => host?.runCommand("move-to-slipbox"),
  },
  {
    id: "copy-to-slipbox",
    title: "Copy current note to slipbox",
    category: "Note",
    keys: "",
    run: () => host?.runCommand("copy-to-slipbox"),
  },
  {
    id: "move-to-folder",
    title: "Move current note to folder…",
    category: "Note",
    keys: "",
    run: () => host?.runCommand("move-to-folder"),
  },
  {
    id: "copy-to-folder",
    title: "Copy current note to folder…",
    category: "Note",
    keys: "",
    run: () => host?.runCommand("copy-to-folder"),
  },
  {
    id: "history-back",
    title: "Go back",
    category: "Navigate",
    keys: "Alt+ArrowLeft",
    run: () => host?.runCommand("history-back"),
  },
  {
    id: "history-forward",
    title: "Go forward",
    category: "Navigate",
    keys: "Alt+ArrowRight",
    run: () => host?.runCommand("history-forward"),
  },
  {
    id: "next-tab",
    title: "Next tab",
    category: "Navigate",
    keys: "Mod+PageDown",
    run: () => host?.runCommand("next-tab"),
  },
  {
    id: "previous-tab",
    title: "Previous tab",
    category: "Navigate",
    keys: "Mod+PageUp",
    run: () => host?.runCommand("previous-tab"),
  },
  {
    id: "refresh-index",
    title: "Refresh note index",
    category: "App",
    keys: "",
    run: () => host?.runCommand("refresh-index"),
  },
];

export function commandById(id: string): Command | undefined {
  return commands.find((command) => command.id === id);
}

export function matchingCommands(query: string): Command[] {
  const needle = query.trim().toLowerCase();
  if (!needle) return commands;
  return commands.filter((command) => {
    const haystack = `${command.title} ${command.category} ${command.keys}`.toLowerCase();
    return needle.split(/\s+/).every((part) => haystack.includes(part));
  });
}
