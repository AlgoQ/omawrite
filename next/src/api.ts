import { invoke } from "@tauri-apps/api/core";
import type {
  Attachment,
  Backlink,
  EmbedResult,
  FolderEntry,
  FrontmatterEdit,
  FrontmatterEditResult,
  NoteDoc,
  OperationResult,
  SaveResult,
  SearchHit,
  Settings,
  Suggestion,
  Theme,
  TreeNode,
  VaultInfo,
} from "./types";

export const api = {
  openVault: (path?: string | null) =>
    invoke<VaultInfo>("open_vault", { path: path ?? null }),
  vaultInfo: () => invoke<VaultInfo | null>("vault_info"),
  tree: () => invoke<TreeNode[]>("tree"),
  refreshIndex: () => invoke<number>("refresh_index"),
  readNote: (path: string) => invoke<NoteDoc>("read_note", { path }),
  saveNote: (
    path: string,
    frontmatter: string,
    body: string,
    expectedModified: number | null,
    force = false,
  ) =>
    invoke<SaveResult>("save_note", {
      path,
      frontmatter,
      body,
      expectedModified,
      force,
    }),
  createNote: (folder?: string | null, name?: string | null) =>
    invoke<NoteDoc>("create_note", { folder: folder ?? null, name: name ?? null }),
  moveNote: (path: string, folder: string, copy: boolean) =>
    invoke<OperationResult>("move_note", { path, folder, copy }),
  suggest: (query: string) => invoke<Suggestion[]>("suggest", { query }),
  backlinks: (path: string) => invoke<Backlink[]>("backlinks", { path }),
  search: (query: string) => invoke<SearchHit[]>("search", { query }),
  resolveNote: (target: string, source: string) =>
    invoke<string | null>("resolve_note", { target, source }),
  resolveAttachment: (target: string, note: string, wiki: boolean) =>
    invoke<string | null>("resolve_attachment", { target, note, wiki }),
  listFolder: (relative?: string | null) =>
    invoke<FolderEntry[]>("list_folder", { relative: relative ?? null }),
  readAttachment: (path: string) => invoke<Attachment>("read_attachment", { path }),
  pasteImage: (note: string, data: string, extension?: string) =>
    invoke<EmbedResult>("paste_image", {
      note,
      data,
      extension: extension ?? null,
    }),
  editFrontmatter: (frontmatter: string, edit: FrontmatterEdit) =>
    invoke<FrontmatterEditResult>("edit_frontmatter", { frontmatter, edit }),
  settingsGet: () => invoke<Settings>("settings_get"),
  settingsSet: (settings: Settings) => invoke<Settings>("settings_set", { settings }),
  theme: () => invoke<Theme>("theme"),
  initialNote: () => invoke<string | null>("initial_note"),
};

export type { Settings };
