use std::fs;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use base64::engine::general_purpose::STANDARD as BASE64;
use base64::Engine as _;
use notify::{RecommendedWatcher, RecursiveMode, Watcher};
use omawrite_core::{
    note_ops, Frontmatter, NoteIndex, Property, Settings, Theme, TreeNode, Vault, VaultInfo,
};
use serde::{Deserialize, Serialize};
use serde_json::json;
use tauri::{AppHandle, Emitter, State};

use crate::state::{AppState, Inner};

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NoteDoc {
    pub path: String,
    pub absolute_path: String,
    pub name: String,
    pub title: String,
    pub frontmatter: String,
    pub body: String,
    pub properties: Vec<Property>,
    pub tags: Vec<String>,
    pub ups: Vec<String>,
    pub aliases: Vec<String>,
    pub in_vault: bool,
    pub modified: u64,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveResult {
    pub success: bool,
    pub modified: u64,
    pub conflict: bool,
    pub disk_text: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Attachment {
    pub mime: String,
    pub data: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct EmbedResult {
    pub markdown: String,
    pub path: String,
}

#[tauri::command]
pub fn open_vault(
    app: AppHandle,
    state: State<'_, AppState>,
    path: Option<String>,
) -> Result<VaultInfo, String> {
    let mut inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    let candidate = match path {
        Some(path) if !path.trim().is_empty() => PathBuf::from(path),
        _ => default_vault_candidate(&inner),
    };
    let vault = Vault::discover(&candidate).map_err(|error| error.to_string())?;
    let info = vault.info();
    let index = NoteIndex::build(&vault);

    inner.settings.last_vault = Some(info.root.clone());
    let _ = inner.settings.save();
    inner.watcher = watch_vault(&app, Path::new(&info.root));
    inner.vault = Some(vault);
    inner.index = Some(index);

    let _ = app.emit("vault-opened", info.clone());
    Ok(info)
}

#[tauri::command]
pub fn vault_info(state: State<'_, AppState>) -> Result<Option<VaultInfo>, String> {
    let inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    Ok(inner.vault.as_ref().map(Vault::info))
}

#[tauri::command]
pub fn tree(state: State<'_, AppState>) -> Result<Vec<TreeNode>, String> {
    let inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    let vault = inner.vault.as_ref().ok_or("Open a vault first.")?;
    let index = inner.index.as_ref().ok_or("The note index is not ready.")?;
    Ok(index.tree(vault))
}

#[tauri::command]
pub fn refresh_index(app: AppHandle, state: State<'_, AppState>) -> Result<usize, String> {
    let mut inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    let vault = inner.vault.as_ref().ok_or("Open a vault first.")?.clone();
    let index = NoteIndex::build(&vault);
    let count = index.notes().len();
    inner.index = Some(index);
    let _ = app.emit("index-changed", count);
    Ok(count)
}

#[tauri::command]
pub fn read_note(state: State<'_, AppState>, path: String) -> Result<NoteDoc, String> {
    let inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    let vault = inner.vault.as_ref().ok_or("Open a vault first.")?;
    let absolute = note_path(vault, &path);
    note_doc(vault, &absolute)
}

#[tauri::command]
pub fn save_note(
    state: State<'_, AppState>,
    path: String,
    frontmatter: String,
    body: String,
    expected_modified: Option<u64>,
    force: Option<bool>,
) -> Result<SaveResult, String> {
    let mut inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    let vault = inner.vault.as_ref().ok_or("Open a vault first.")?.clone();
    let absolute = note_path(&vault, &path);
    if let Some(parent) = absolute.parent() {
        if !parent.is_dir() {
            fs::create_dir_all(parent)
                .map_err(|error| format!("Cannot create {}: {error}", parent.display()))?;
        }
    }

    let current = modified_millis(&absolute).unwrap_or(0);
    if absolute.exists() && !force.unwrap_or(false) {
        if let Some(expected) = expected_modified {
            if current != 0 && current != expected {
                let disk_text = fs::read_to_string(&absolute).ok();
                return Ok(SaveResult {
                    success: false,
                    modified: current,
                    conflict: true,
                    disk_text,
                });
            }
        }
    }

    let mut full = frontmatter;
    if !full.is_empty() && !full.ends_with('\n') {
        full.push('\n');
    }
    full.push_str(&body);
    write_atomic(&absolute, full.as_bytes())
        .map_err(|error| format!("Cannot write {}: {error}", absolute.display()))?;
    let modified = modified_millis(&absolute).unwrap_or(0);

    if let Some(relative) = vault.relative_string(&absolute) {
        if let Some(index) = &mut inner.index {
            index.update_note(&vault, &relative, &full);
        }
    }

    Ok(SaveResult {
        success: true,
        modified,
        conflict: false,
        disk_text: None,
    })
}

#[tauri::command]
pub fn create_note(
    state: State<'_, AppState>,
    folder: Option<String>,
    name: Option<String>,
) -> Result<NoteDoc, String> {
    let mut inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    let vault = inner.vault.as_ref().ok_or("Open a vault first.")?.clone();
    let folder = folder
        .map(|folder| note_path(&vault, &folder))
        .unwrap_or_else(|| vault.root().to_path_buf());
    fs::create_dir_all(&folder)
        .map_err(|error| format!("Cannot create {}: {error}", folder.display()))?;

    let mut stem = name.unwrap_or_else(|| "Untitled".to_string());
    stem = stem
        .trim()
        .trim_end_matches(".md")
        .trim_end_matches(".markdown")
        .to_string();
    if stem.is_empty() {
        stem = "Untitled".to_string();
    }
    stem = stem.replace('/', "-").replace('\\', "-");
    let mut file_name = format!("{stem}.md");
    let mut counter = 1;
    while folder.join(&file_name).exists() {
        file_name = format!("{stem} {counter}.md");
        counter += 1;
    }
    let absolute = folder.join(file_name);
    write_atomic(&absolute, b"")
        .map_err(|error| format!("Cannot create {}: {error}", absolute.display()))?;
    let doc = note_doc(&vault, &absolute)?;
    if let Some(relative) = vault.relative_string(&absolute) {
        if let Some(index) = &mut inner.index {
            index.update_note(&vault, &relative, "");
        }
    }
    Ok(doc)
}

#[tauri::command]
pub fn move_note(
    app: AppHandle,
    state: State<'_, AppState>,
    path: String,
    folder: String,
    copy: bool,
) -> Result<note_ops::OperationResult, String> {
    let mut inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    let vault = inner.vault.as_ref().ok_or("Open a vault first.")?.clone();
    let index = inner.index.as_ref().ok_or("The note index is not ready.")?;
    let source = note_path(&vault, &path);
    let destination = note_path(&vault, &folder);
    let result = note_ops::move_note(&vault, index, &source, &destination, copy);
    if result.success {
        inner.index = Some(NoteIndex::build(&vault));
        let _ = app.emit(
            "note-moved",
            json!({
                "from": path,
                "to": result.new_path.clone().unwrap_or_else(|| result.new_absolute_path.clone()),
                "copy": copy,
            }),
        );
        let _ = app.emit("vault-changed", ());
    }
    Ok(result)
}

#[tauri::command]
pub fn suggest(
    state: State<'_, AppState>,
    query: String,
) -> Result<Vec<omawrite_core::Suggestion>, String> {
    let inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    let index = inner.index.as_ref().ok_or("The note index is not ready.")?;
    Ok(index.suggestions(&query))
}

#[tauri::command]
pub fn backlinks(
    state: State<'_, AppState>,
    path: String,
) -> Result<Vec<omawrite_core::Backlink>, String> {
    let inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    let index = inner.index.as_ref().ok_or("The note index is not ready.")?;
    Ok(index.backlinks(&path))
}

#[tauri::command]
pub fn search(
    state: State<'_, AppState>,
    query: String,
) -> Result<Vec<omawrite_core::SearchHit>, String> {
    let inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    let index = inner.index.as_ref().ok_or("The note index is not ready.")?;
    Ok(index.search(&query))
}

#[tauri::command]
pub fn resolve_note(
    state: State<'_, AppState>,
    target: String,
    source: String,
) -> Result<Option<String>, String> {
    let inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    let vault = inner.vault.as_ref().ok_or("Open a vault first.")?;
    let index = inner.index.as_ref().ok_or("The note index is not ready.")?;
    let source = note_path(vault, &source);
    let source_dir = source.parent().unwrap_or(vault.root());
    Ok(vault.resolve_note(&target, source_dir, &index.paths()))
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FrontmatterEditResult {
    pub frontmatter: String,
    pub properties: Vec<Property>,
    pub tags: Vec<String>,
    pub ups: Vec<String>,
    pub aliases: Vec<String>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(tag = "kind", rename_all = "camelCase")]
pub enum FrontmatterEdit {
    Set {
        key: String,
        values: Vec<String>,
    },
    Remove {
        key: String,
    },
    AddTag {
        tag: String,
    },
    RemoveTag {
        tag: String,
    },
    AddUp {
        target: String,
    },
    RemoveUp {
        target: String,
    },
    SetValue {
        key: String,
        index: usize,
        value: String,
    },
    AddValue {
        key: String,
        value: String,
    },
    RemoveValue {
        key: String,
        index: usize,
    },
}

#[tauri::command]
pub fn edit_frontmatter(
    frontmatter: String,
    edit: FrontmatterEdit,
) -> Result<FrontmatterEditResult, String> {
    let mut frontmatter = Frontmatter::parse(&frontmatter);
    match edit {
        FrontmatterEdit::Set { key, values } => frontmatter.set_values(&key, &values),
        FrontmatterEdit::Remove { key } => frontmatter.remove_key(&key),
        FrontmatterEdit::AddTag { tag } => frontmatter.add_tag(&tag),
        FrontmatterEdit::RemoveTag { tag } => frontmatter.remove_tag(&tag),
        FrontmatterEdit::AddUp { target } => frontmatter.add_up(&target),
        FrontmatterEdit::RemoveUp { target } => frontmatter.remove_up(&target),
        FrontmatterEdit::SetValue { key, index, value } => {
            frontmatter.set_item(&key, index, &value)
        }
        FrontmatterEdit::AddValue { key, value } => frontmatter.add_value(&key, &value),
        FrontmatterEdit::RemoveValue { key, index } => frontmatter.remove_value(&key, index),
    }
    Ok(FrontmatterEditResult {
        frontmatter: frontmatter.raw().to_string(),
        properties: frontmatter.properties(),
        tags: frontmatter.values("tags"),
        ups: frontmatter.values("up"),
        aliases: frontmatter.values("aliases"),
    })
}

#[tauri::command]
pub fn resolve_attachment(
    state: State<'_, AppState>,
    target: String,
    note: String,
    wiki: bool,
) -> Result<Option<String>, String> {
    let inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    let vault = inner.vault.as_ref().ok_or("Open a vault first.")?;
    let note = note_path(vault, &note);
    Ok(vault
        .resolve_attachment(&target, &note, wiki)
        .map(|path| path.to_string_lossy().into_owned()))
}

#[tauri::command]
pub fn list_folder(
    state: State<'_, AppState>,
    relative: Option<String>,
) -> Result<Vec<omawrite_core::vault::FolderEntry>, String> {
    let inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    let vault = inner.vault.as_ref().ok_or("Open a vault first.")?;
    Ok(vault.list_folder(Path::new(relative.as_deref().unwrap_or(""))))
}

#[tauri::command]
pub fn read_attachment(state: State<'_, AppState>, path: String) -> Result<Attachment, String> {
    let inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    let vault = inner.vault.as_ref().ok_or("Open a vault first.")?;
    let absolute = note_path(vault, &path);
    let bytes = fs::read(&absolute)
        .map_err(|error| format!("Cannot read {}: {error}", absolute.display()))?;
    Ok(Attachment {
        mime: mime_for(&absolute).to_string(),
        data: BASE64.encode(bytes),
    })
}

#[tauri::command]
pub fn paste_image(
    state: State<'_, AppState>,
    note: String,
    data: String,
    extension: Option<String>,
) -> Result<EmbedResult, String> {
    let inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    let vault = inner.vault.as_ref().ok_or("Open a vault first.")?;
    let note = note_path(vault, &note);
    let folder = vault.attachment_dir(&note);
    fs::create_dir_all(&folder)
        .map_err(|error| format!("Cannot create {}: {error}", folder.display()))?;
    let bytes = BASE64
        .decode(data)
        .map_err(|error| format!("The pasted image is not valid base64: {error}"))?;
    let extension = extension.unwrap_or_else(|| "png".to_string());
    let stamp = std::time::SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_secs())
        .unwrap_or(0);
    let mut name = format!("Pasted image {stamp}.{extension}");
    let mut counter = 1;
    while folder.join(&name).exists() {
        name = format!("Pasted image {stamp} {counter}.{extension}");
        counter += 1;
    }
    let absolute = folder.join(&name);
    fs::write(&absolute, bytes)
        .map_err(|error| format!("Cannot write {}: {error}", absolute.display()))?;

    let markdown = match vault.kind() {
        omawrite_core::VaultKind::Obsidian => format!("![[{name}]]"),
        omawrite_core::VaultKind::Plain => {
            let note_dir = note.parent().unwrap_or(vault.root());
            let relative = absolute
                .strip_prefix(note_dir)
                .map(|path| path.to_string_lossy().replace('\\', "/"))
                .unwrap_or_else(|_| name.clone());
            format!("![]({relative})")
        }
    };
    Ok(EmbedResult {
        markdown,
        path: absolute.to_string_lossy().into_owned(),
    })
}

#[tauri::command]
pub fn settings_get(state: State<'_, AppState>) -> Result<Settings, String> {
    let inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    Ok(inner.settings.clone())
}

#[tauri::command]
pub fn settings_set(
    state: State<'_, AppState>,
    mut settings: Settings,
) -> Result<Settings, String> {
    settings.scroll_speed = settings.scroll_speed.clamp(0.5, 4.0);
    settings.font_size = settings.font_size.clamp(10.0, 40.0);
    if !matches!(settings.theme.as_str(), "auto" | "dark" | "light") {
        settings.theme = "auto".to_string();
    }
    settings.save()?;
    let mut inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    inner.settings = settings.clone();
    Ok(settings)
}

#[tauri::command]
pub fn theme(state: State<'_, AppState>) -> Result<Theme, String> {
    let inner = state
        .inner
        .lock()
        .map_err(|_| "The vault lock is poisoned.")?;
    Ok(match inner.settings.theme.as_str() {
        "dark" => Theme::fallback(true),
        "light" => Theme::fallback(false),
        _ => Theme::load(),
    })
}

#[tauri::command]
pub fn initial_note() -> Result<Option<String>, String> {
    if let Some(path) = std::env::var_os("OMAWRITE_NOTE") {
        let path = PathBuf::from(path);
        if path.is_file() {
            let canonical = path.canonicalize().unwrap_or_else(|_| path.clone());
            return Ok(Some(canonical.to_string_lossy().into_owned()));
        }
    }
    for argument in std::env::args().skip(1) {
        if argument.starts_with('-') {
            continue;
        }
        let path = PathBuf::from(&argument);
        if path.is_file()
            && matches!(
                path.extension().and_then(|extension| extension.to_str()),
                Some("md") | Some("markdown")
            )
        {
            let canonical = path.canonicalize().unwrap_or_else(|_| path.clone());
            return Ok(Some(canonical.to_string_lossy().into_owned()));
        }
    }
    Ok(None)
}

fn note_doc(vault: &Vault, absolute: &Path) -> Result<NoteDoc, String> {
    let bytes = fs::read(absolute)
        .map_err(|error| format!("Cannot read {}: {error}", absolute.display()))?;
    let text = String::from_utf8(bytes)
        .map_err(|_| format!("{} is not valid UTF-8.", absolute.display()))?;
    let frontmatter = Frontmatter::parse(&text);
    let relative = vault.relative_string(absolute);
    let name = absolute
        .file_stem()
        .map(|stem| stem.to_string_lossy().into_owned())
        .unwrap_or_else(|| "Untitled".to_string());
    let title = frontmatter.value("title").unwrap_or_else(|| name.clone());
    let mut tags = frontmatter.values("tags");
    tags.extend(frontmatter.values("tag"));
    let mut aliases = frontmatter.values("aliases");
    aliases.extend(frontmatter.values("alias"));
    Ok(NoteDoc {
        path: relative
            .clone()
            .unwrap_or_else(|| absolute.to_string_lossy().into_owned()),
        absolute_path: absolute.to_string_lossy().into_owned(),
        name,
        title,
        frontmatter: frontmatter.raw().to_string(),
        body: frontmatter.body.clone(),
        properties: frontmatter.properties(),
        tags,
        ups: frontmatter.values("up"),
        aliases,
        in_vault: relative.is_some(),
        modified: modified_millis(absolute).unwrap_or(0),
    })
}

fn note_path(vault: &Vault, path: &str) -> PathBuf {
    let path = Path::new(path);
    if path.is_absolute() {
        path.to_path_buf()
    } else {
        vault.absolute(path)
    }
}

fn modified_millis(path: &Path) -> Option<u64> {
    fs::metadata(path)
        .and_then(|metadata| metadata.modified())
        .ok()
        .and_then(|time| time.duration_since(UNIX_EPOCH).ok())
        .map(|duration| duration.as_millis() as u64)
}

fn write_atomic(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    let parent = path.parent().unwrap_or_else(|| Path::new("."));
    let name = path
        .file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_else(|| "note".to_string());
    let temporary = parent.join(format!(".{name}.{}.tmp", std::process::id()));
    fs::write(&temporary, bytes)?;
    match fs::rename(&temporary, path) {
        Ok(()) => Ok(()),
        Err(error) => {
            let _ = fs::remove_file(&temporary);
            Err(error)
        }
    }
}

fn mime_for(path: &Path) -> &'static str {
    match path
        .extension()
        .and_then(|extension| extension.to_str())
        .map(str::to_ascii_lowercase)
        .as_deref()
    {
        Some("png") => "image/png",
        Some("jpg") | Some("jpeg") => "image/jpeg",
        Some("gif") => "image/gif",
        Some("webp") => "image/webp",
        Some("svg") => "image/svg+xml",
        Some("avif") => "image/avif",
        Some("bmp") => "image/bmp",
        Some("ico") => "image/x-icon",
        _ => "application/octet-stream",
    }
}

fn default_vault_candidate(inner: &Inner) -> PathBuf {
    if let Some(path) = std::env::var_os("OMAWRITE_VAULT_ROOT") {
        return PathBuf::from(path);
    }
    if let Some(last) = &inner.settings.last_vault {
        let path = PathBuf::from(last);
        if path.exists() {
            return path;
        }
    }
    if let Some(home) = std::env::var_os("HOME") {
        let slipbox = PathBuf::from(home).join("Documents/slipbox");
        if slipbox.is_dir() {
            return slipbox;
        }
    }
    std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."))
}

fn watch_vault(app: &AppHandle, root: &Path) -> Option<RecommendedWatcher> {
    let app = app.clone();
    let mut watcher = notify::recommended_watcher(move |result: notify::Result<notify::Event>| {
        let Ok(event) = result else {
            return;
        };
        if event.paths.iter().any(|path| !ignored_by_watcher(path)) {
            let _ = app.emit("vault-changed", ());
        }
    })
    .ok()?;
    watcher.watch(root, RecursiveMode::Recursive).ok()?;
    Some(watcher)
}

fn ignored_by_watcher(path: &Path) -> bool {
    path.components().any(|component| {
        let name = component.as_os_str().to_string_lossy();
        matches!(
            name.as_ref(),
            ".obsidian" | ".git" | "node_modules" | "target"
        ) || name.ends_with(".tmp")
    })
}
