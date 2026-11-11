export type PropertyKind =
  | "tags"
  | "links"
  | "list"
  | "checkbox"
  | "number"
  | "date"
  | "text";

export interface Property {
  key: string;
  kind: PropertyKind;
  values: string[];
  display: string;
}

export interface VaultInfo {
  root: string;
  name: string;
  kind: "obsidian" | "plain";
  attachmentFolder: string;
}

export interface NoteDoc {
  path: string;
  absolutePath: string;
  name: string;
  title: string;
  frontmatter: string;
  body: string;
  properties: Property[];
  tags: string[];
  ups: string[];
  aliases: string[];
  inVault: boolean;
  modified: number;
}

export interface TreeNode {
  name: string;
  path: string;
  tags: string[];
  childCount: number;
  children: TreeNode[];
}

export interface Suggestion {
  name: string;
  path: string;
  insert: string;
  score: number;
}

export interface Backlink {
  path: string;
  name: string;
  line: number;
  snippet: string;
}

export interface SearchHit {
  path: string;
  name: string;
  line: number;
  snippet: string;
  score: number;
}

export interface SaveResult {
  success: boolean;
  modified: number;
  conflict: boolean;
  diskText: string | null;
}

export interface OperationResult {
  success: boolean;
  error: string | null;
  newPath: string | null;
  newAbsolutePath: string;
  changedLinks: number;
  changedFiles: string[];
  warnings: string[];
}

export interface Settings {
  scrollSpeed: number;
  fontSize: number;
  theme: "auto" | "dark" | "light";
  sidebarOpen: boolean;
  backlinksOpen: boolean;
  keymap: Record<string, string>;
  lastVault: string | null;
}

export interface Theme {
  mode: "dark" | "light";
  background: string;
  foreground: string;
  accent: string;
  selection: string;
  selectionForeground: string;
  codeBackground: string;
  muted: string;
  border: string;
  panel: string;
}

export interface Attachment {
  mime: string;
  data: string;
}

export interface EmbedResult {
  markdown: string;
  path: string;
}

export interface FolderEntry {
  name: string;
  path: string;
  isDir: boolean;
}

export type FrontmatterEdit =
  | { kind: "set"; key: string; values: string[] }
  | { kind: "remove"; key: string }
  | { kind: "addTag"; tag: string }
  | { kind: "removeTag"; tag: string }
  | { kind: "addUp"; target: string }
  | { kind: "removeUp"; target: string }
  | { kind: "setValue"; key: string; index: number; value: string }
  | { kind: "addValue"; key: string; value: string }
  | { kind: "removeValue"; key: string; index: number };

export interface FrontmatterEditResult {
  frontmatter: string;
  properties: Property[];
  tags: string[];
  ups: string[];
  aliases: string[];
}
