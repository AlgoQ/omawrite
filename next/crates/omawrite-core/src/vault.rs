use std::fs;
use std::path::{Component, Path, PathBuf};

use serde::Serialize;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum VaultKind {
    Obsidian,
    Plain,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct VaultInfo {
    pub root: String,
    pub name: String,
    pub kind: VaultKind,
    pub attachment_folder: String,
}

#[derive(Clone, Debug)]
pub struct Vault {
    root: PathBuf,
    kind: VaultKind,
    attachment_setting: Option<String>,
}

#[derive(Debug, thiserror::Error)]
pub enum VaultError {
    #[error("cannot read {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("not a directory: {0}")]
    NotADirectory(PathBuf),
}

impl Vault {
    /// Find the vault around a note or folder.
    ///
    /// The nearest ancestor with a `.obsidian` directory wins. A plain folder
    /// of notes is still a vault, rooted at the folder itself, which keeps the
    /// app useful outside Obsidian.
    pub fn discover(path: &Path) -> Result<Self, VaultError> {
        let start = if path.is_file() {
            path.parent().unwrap_or(path)
        } else {
            path
        };
        let start = fs::canonicalize(start).unwrap_or_else(|_| start.to_path_buf());
        if !start.is_dir() {
            return Err(VaultError::NotADirectory(start));
        }

        let mut root = start.clone();
        let mut kind = VaultKind::Plain;
        for ancestor in start.ancestors() {
            if ancestor.join(".obsidian").is_dir() {
                root = ancestor.to_path_buf();
                kind = VaultKind::Obsidian;
                break;
            }
        }

        let attachment_setting = read_attachment_setting(&root);
        Ok(Self {
            root,
            kind,
            attachment_setting,
        })
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    pub fn kind(&self) -> VaultKind {
        self.kind
    }

    pub fn info(&self) -> VaultInfo {
        VaultInfo {
            root: self.root.to_string_lossy().into_owned(),
            name: self
                .root
                .file_name()
                .map(|name| name.to_string_lossy().into_owned())
                .unwrap_or_else(|| self.root.to_string_lossy().into_owned()),
            kind: self.kind,
            attachment_folder: self
                .attachment_dir(&self.root)
                .to_string_lossy()
                .into_owned(),
        }
    }

    pub fn contains(&self, path: &Path) -> bool {
        let path = self.absolute(path);
        path.starts_with(&self.root)
    }

    pub fn relative(&self, path: &Path) -> Option<PathBuf> {
        let path = self.absolute(path);
        path.strip_prefix(&self.root).ok().map(Path::to_path_buf)
    }

    pub fn relative_string(&self, path: &Path) -> Option<String> {
        self.relative(path)
            .map(|relative| relative.to_string_lossy().replace('\\', "/"))
    }

    pub fn absolute(&self, path: &Path) -> PathBuf {
        if path.is_absolute() {
            path.to_path_buf()
        } else {
            self.root.join(path)
        }
    }

    pub fn is_note(path: &Path) -> bool {
        matches!(
            path.extension()
                .and_then(|extension| extension.to_str())
                .map(str::to_ascii_lowercase)
                .as_deref(),
            Some("md") | Some("markdown")
        )
    }

    pub fn is_excluded_tree_path(path: &Path) -> bool {
        path.components().any(|component| match component {
            Component::Normal(name) => {
                let name = name.to_string_lossy();
                name == "drawings" || name.starts_with('.')
            }
            _ => false,
        })
    }

    pub fn walk_notes(&self) -> Vec<PathBuf> {
        let mut notes = Vec::new();
        let walker = walkdir::WalkDir::new(&self.root)
            .follow_links(false)
            .into_iter()
            .filter_entry(|entry| {
                if entry.depth() == 0 {
                    return true;
                }
                let name = entry.file_name().to_string_lossy();
                if entry.file_type().is_dir() {
                    !matches!(
                        name.as_ref(),
                        ".obsidian" | ".git" | "node_modules" | "drawings"
                    ) && !name.starts_with('.')
                } else {
                    Self::is_note(Path::new(&*name)) || !name.starts_with('.')
                }
            });
        for entry in walker.flatten() {
            if entry.file_type().is_file() && Self::is_note(entry.path()) {
                notes.push(entry.into_path());
            }
        }
        notes.sort_by(|a, b| {
            a.file_name()
                .unwrap_or_default()
                .to_string_lossy()
                .to_lowercase()
                .cmp(
                    &b.file_name()
                        .unwrap_or_default()
                        .to_string_lossy()
                        .to_lowercase(),
                )
                .then_with(|| a.cmp(b))
        });
        notes
    }

    /// Resolve an Obsidian wikilink target.
    ///
    /// The order matches Obsidian and the current Omawrite: a note beside the
    /// linking note, then a note at the vault root, then a bare name anywhere
    /// in the vault when exactly one note has it. Ambiguity is not guessed at.
    pub fn resolve_note(
        &self,
        target: &str,
        source_dir: &Path,
        known_notes: &[String],
    ) -> Option<String> {
        let mut name = target.trim().to_string();
        if name.is_empty() {
            return None;
        }
        if let Some((before, _)) = name.split_once('#') {
            name = before.trim().to_string();
        }
        if name.len() > 9 && name.to_ascii_lowercase().ends_with(".markdown") {
            name.truncate(name.len() - 9);
        } else if name.len() > 3 && name.to_ascii_lowercase().ends_with(".md") {
            name.truncate(name.len() - 3);
        }
        name = name.trim().to_string();
        if name.is_empty() || name.contains("..") || Path::new(&name).is_absolute() {
            return None;
        }

        let source = self.absolute(source_dir);
        for suffix in [".md", ".markdown"] {
            let beside = source.join(format!("{name}{suffix}"));
            if beside.is_file() {
                if let Some(relative) = self.relative_string(&beside) {
                    return Some(relative);
                }
            }
        }
        for suffix in [".md", ".markdown"] {
            let at_root = self.root.join(format!("{name}{suffix}"));
            if at_root.is_file() {
                if let Some(relative) = self.relative_string(&at_root) {
                    return Some(relative);
                }
            }
        }

        let wanted = Path::new(&name)
            .file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or(name);
        let mut found: Option<&String> = None;
        for note in known_notes {
            let stem = Path::new(note)
                .file_stem()
                .map(|stem| stem.to_string_lossy().into_owned())
                .unwrap_or_default();
            if !stem.eq_ignore_ascii_case(&wanted) {
                continue;
            }
            if let Some(existing) = found {
                if existing != note {
                    return None;
                }
            } else {
                found = Some(note);
            }
        }
        found.cloned()
    }

    pub fn attachment_dir(&self, note: &Path) -> PathBuf {
        match self.kind {
            VaultKind::Obsidian => {
                let setting = self
                    .attachment_setting
                    .as_deref()
                    .map(str::trim)
                    .filter(|setting| !setting.is_empty())
                    .unwrap_or("/");
                if setting == "/" || setting == "." {
                    return self.root.clone();
                }
                let setting = setting.trim_start_matches("./");
                self.root.join(setting)
            }
            VaultKind::Plain => {
                let note = self.absolute(note);
                note.parent().unwrap_or(&self.root).join("_attachments")
            }
        }
    }

    /// Find the file behind an image embed.
    ///
    /// An Obsidian embed is a bare name looked for beside the note, in the
    /// attachment folder, at the vault root, and then anywhere in the vault.
    /// A Markdown path is relative to the note, with percent escapes decoded.
    pub fn resolve_attachment(&self, target: &str, note: &Path, wiki: bool) -> Option<PathBuf> {
        let mut target = target.trim().trim_start_matches('!').trim().to_string();
        if let Some((before, _)) = target.split_once('|') {
            target = before.to_string();
        }
        if let Some((before, _)) = target.split_once('#') {
            target = before.to_string();
        }
        if target.is_empty() {
            return None;
        }
        let target_path = Path::new(&target);
        if target_path.is_absolute() && target_path.is_file() {
            return Some(
                target_path
                    .canonicalize()
                    .unwrap_or_else(|_| target_path.to_path_buf()),
            );
        }

        let note = self.absolute(note);
        let note_dir = note.parent().unwrap_or(&self.root);
        if wiki {
            let name = target_path
                .file_name()
                .unwrap_or_else(|| target_path.as_os_str());
            for candidate in [
                note_dir.join(name),
                self.attachment_dir(&note).join(name),
                self.root.join(name),
                note_dir.join(&target),
                self.root.join(&target),
            ] {
                if candidate.is_file() {
                    return Some(candidate.canonicalize().unwrap_or(candidate));
                }
            }
            let wanted = name.to_string_lossy().to_lowercase();
            let mut found: Option<PathBuf> = None;
            for entry in walkdir::WalkDir::new(&self.root)
                .follow_links(false)
                .into_iter()
                .filter_entry(|entry| {
                    if entry.depth() == 0 {
                        return true;
                    }
                    let name = entry.file_name().to_string_lossy();
                    !matches!(
                        name.as_ref(),
                        ".obsidian" | ".git" | "node_modules" | "target"
                    )
                })
                .flatten()
            {
                if !entry.file_type().is_file() {
                    continue;
                }
                let name = entry.file_name().to_string_lossy().to_lowercase();
                if name != wanted {
                    continue;
                }
                if found.is_some() {
                    return None;
                }
                let path = entry.into_path();
                found = Some(path.canonicalize().unwrap_or(path));
            }
            return found;
        }

        let decoded = percent_decode(&target);
        let relative = note_dir.join(&decoded);
        if relative.is_file() {
            return Some(relative.canonicalize().unwrap_or(relative));
        }
        let at_root = self.root.join(&decoded);
        if at_root.is_file() {
            return Some(at_root.canonicalize().unwrap_or(at_root));
        }
        None
    }

    pub fn list_folder(&self, relative: &Path) -> Vec<FolderEntry> {
        let folder = self.absolute(relative);
        let mut entries = Vec::new();
        let Ok(read) = fs::read_dir(&folder) else {
            return entries;
        };
        for entry in read.flatten() {
            let path = entry.path();
            let name = entry.file_name().to_string_lossy().into_owned();
            if name.starts_with('.') || name == "node_modules" {
                continue;
            }
            let is_dir = path.is_dir();
            if !is_dir && !Self::is_note(&path) {
                continue;
            }
            let relative = self
                .relative_string(&path)
                .unwrap_or_else(|| path.to_string_lossy().into_owned());
            entries.push(FolderEntry {
                name,
                path: relative,
                is_dir,
            });
        }
        entries.sort_by(|a, b| {
            b.is_dir
                .cmp(&a.is_dir)
                .then_with(|| a.name.to_lowercase().cmp(&b.name.to_lowercase()))
        });
        entries
    }
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FolderEntry {
    pub name: String,
    pub path: String,
    pub is_dir: bool,
}

fn percent_decode(value: &str) -> String {
    let bytes = value.as_bytes();
    let mut output = Vec::with_capacity(bytes.len());
    let mut index = 0;
    while index < bytes.len() {
        if bytes[index] == b'%' && index + 2 < bytes.len() {
            let pair = std::str::from_utf8(&bytes[index + 1..index + 3]).ok();
            if let Some(pair) = pair {
                if let Ok(byte) = u8::from_str_radix(pair, 16) {
                    output.push(byte);
                    index += 3;
                    continue;
                }
            }
        }
        output.push(bytes[index]);
        index += 1;
    }
    String::from_utf8_lossy(&output).into_owned()
}

fn read_attachment_setting(root: &Path) -> Option<String> {
    let text = fs::read_to_string(root.join(".obsidian/app.json")).ok()?;
    let value: serde_json::Value = serde_json::from_str(&text).ok()?;
    value
        .get("attachmentFolderPath")
        .and_then(|value| value.as_str())
        .map(str::to_string)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn vault() -> (tempfile::TempDir, PathBuf) {
        let directory = tempfile::tempdir().unwrap();
        let root = directory.path().to_path_buf();
        fs::create_dir_all(root.join(".obsidian")).unwrap();
        fs::create_dir_all(root.join("notes")).unwrap();
        fs::write(root.join("Root.md"), "# Root\n").unwrap();
        fs::write(root.join("notes/Beside.md"), "# Beside\n").unwrap();
        fs::write(root.join("notes/Unique.md"), "# Unique\n").unwrap();
        (directory, root)
    }

    #[test]
    fn finds_the_nearest_obsidian_ancestor() {
        let (_directory, root) = vault();
        let discovered = Vault::discover(&root.join("notes/Beside.md")).unwrap();
        assert_eq!(discovered.root(), root.as_path());
        assert_eq!(discovered.kind(), VaultKind::Obsidian);
    }

    #[test]
    fn resolves_beside_then_root_then_unique_name() {
        let (_directory, root) = vault();
        let notes = vec![
            "Root.md".to_string(),
            "notes/Beside.md".to_string(),
            "notes/Unique.md".to_string(),
        ];
        let discovered = Vault::discover(&root).unwrap();
        assert_eq!(
            discovered.resolve_note("Beside", &root.join("notes"), &notes),
            Some("notes/Beside.md".to_string())
        );
        assert_eq!(
            discovered.resolve_note("Root", &root.join("notes"), &notes),
            Some("Root.md".to_string())
        );
        assert_eq!(
            discovered.resolve_note("Unique", &root, &notes),
            Some("notes/Unique.md".to_string())
        );
    }

    #[test]
    fn refuses_ambiguous_bare_names() {
        let directory = tempfile::tempdir().unwrap();
        let root = directory.path();
        fs::write(root.join("One.md"), "").unwrap();
        fs::write(root.join("Two.md"), "").unwrap();
        let discovered = Vault::discover(root).unwrap();
        let notes = vec!["One.md".to_string(), "Two.md".to_string()];
        assert_eq!(
            discovered.resolve_note("One", root, &notes).as_deref(),
            Some("One.md")
        );
        assert_eq!(discovered.resolve_note("Missing", root, &notes), None);
    }

    #[test]
    fn resolves_attachments_beside_the_note_and_in_the_vault() {
        let (_directory, root) = vault();
        fs::create_dir_all(root.join("_attachments")).unwrap();
        fs::write(root.join("notes/local.png"), b"png").unwrap();
        fs::write(root.join("_attachments/shared.png"), b"png").unwrap();
        fs::write(root.join("elsewhere.png"), b"png").unwrap();
        let discovered = Vault::discover(&root).unwrap();
        let note = root.join("notes/Beside.md");

        assert_eq!(
            discovered.resolve_attachment("local.png", &note, true),
            Some(root.join("notes/local.png"))
        );
        assert_eq!(
            discovered.resolve_attachment("_attachments/shared.png", &note, true),
            Some(root.join("_attachments/shared.png"))
        );
        assert_eq!(
            discovered.resolve_attachment("elsewhere.png", &note, true),
            Some(root.join("elsewhere.png"))
        );
        assert_eq!(
            discovered.resolve_attachment("../elsewhere.png", &note, false),
            Some(root.join("elsewhere.png"))
        );
    }

    #[test]
    fn reads_obsidian_attachment_setting() {
        let (_directory, root) = vault();
        fs::write(
            root.join(".obsidian/app.json"),
            r#"{"attachmentFolderPath":"assets/images"}"#,
        )
        .unwrap();
        let discovered = Vault::discover(&root).unwrap();
        assert_eq!(
            discovered.attachment_dir(&root.join("Root.md")),
            root.join("assets/images")
        );
    }
}
