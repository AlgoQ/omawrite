use std::fs;
use std::io;
use std::path::{Component, Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::Serialize;

use crate::frontmatter::Frontmatter;
use crate::index::NoteIndex;
use crate::links;
use crate::vault::Vault;

#[derive(Clone, Debug, Serialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct OperationResult {
    pub success: bool,
    pub error: Option<String>,
    pub new_path: Option<String>,
    pub new_absolute_path: String,
    pub changed_links: usize,
    pub changed_files: Vec<String>,
    pub warnings: Vec<String>,
}

pub fn move_note(
    vault: &Vault,
    index: &NoteIndex,
    source: &Path,
    destination_folder: &Path,
    copy: bool,
) -> OperationResult {
    match move_note_inner(vault, index, source, destination_folder, copy) {
        Ok(result) => result,
        Err(error) => OperationResult {
            success: false,
            error: Some(error.to_string()),
            ..Default::default()
        },
    }
}

fn move_note_inner(
    vault: &Vault,
    index: &NoteIndex,
    source: &Path,
    destination_folder: &Path,
    copy: bool,
) -> Result<OperationResult, String> {
    let source_absolute = if source.is_absolute() {
        source.to_path_buf()
    } else {
        vault.root().join(source)
    };
    if !source_absolute.is_file() {
        return Err(format!("No note at {}", source_absolute.display()));
    }
    if !Vault::is_note(&source_absolute) {
        return Err("Only Markdown notes can be moved or copied.".to_string());
    }

    let folder_absolute = if destination_folder.is_absolute() {
        destination_folder.to_path_buf()
    } else {
        vault.root().join(destination_folder)
    };
    if !folder_absolute.is_dir() {
        return Err(format!("No folder at {}", folder_absolute.display()));
    }

    let file_name = source_absolute
        .file_name()
        .ok_or_else(|| "The note has no file name.".to_string())?
        .to_string_lossy()
        .into_owned();
    let new_absolute = folder_absolute.join(free_name(&folder_absolute, &file_name));
    if same_path(&source_absolute, &new_absolute) {
        return Err("The note is already in that folder.".to_string());
    }

    let source_relative = vault.relative_string(&source_absolute);
    let new_relative = vault.relative_string(&new_absolute);
    let source_dir = source_absolute.parent().unwrap_or(vault.root());
    let new_dir = new_absolute.parent().unwrap_or(vault.root());

    let bytes = fs::read(&source_absolute)
        .map_err(|error| format!("Cannot read {}: {error}", source_absolute.display()))?;
    let text = match String::from_utf8(bytes.clone()) {
        Ok(text) => Some(text),
        Err(_) => None,
    };

    let mut result = OperationResult {
        success: true,
        new_path: new_relative.clone(),
        new_absolute_path: new_absolute.to_string_lossy().into_owned(),
        ..Default::default()
    };
    if text.is_none() {
        result
            .warnings
            .push("The note is not valid UTF-8, so its links were left as they are.".to_string());
    }

    // Inbound references change only on a move. A copy leaves the original
    // name in place, so links that pointed at it stay correct.
    if !copy {
        if let (Some(source_relative), Some(new_relative)) = (&source_relative, &new_relative) {
            let known = index.paths();
            for note in index.notes() {
                if note.meta.path == *source_relative {
                    continue;
                }
                let note_absolute = vault.root().join(&note.meta.path);
                let Ok(note_bytes) = fs::read(&note_absolute) else {
                    continue;
                };
                let Ok(note_text) = String::from_utf8(note_bytes) else {
                    continue;
                };
                let mut frontmatter = Frontmatter::parse(&note_text);
                let note_dir = note_absolute.parent().unwrap_or(vault.root());
                let mut changed = 0usize;

                let (body, wiki_changed) = links::rewrite_wikilinks(&frontmatter.body, |target| {
                    let Some(resolved) = vault.resolve_note(target, note_dir, &known) else {
                        return None;
                    };
                    if resolved != *source_relative {
                        return None;
                    }
                    Some(best_target(index, new_relative))
                });
                if wiki_changed > 0 {
                    frontmatter.body = body;
                    changed += wiki_changed;
                }

                let mut ups = frontmatter.values("up");
                let mut ups_changed = 0usize;
                for up in &mut ups {
                    let Some(resolved) = vault.resolve_note(up, note_dir, &known) else {
                        continue;
                    };
                    if resolved == *source_relative {
                        *up = best_target(index, new_relative);
                        ups_changed += 1;
                    }
                }
                if ups_changed > 0 {
                    frontmatter.set_values("up", &ups);
                    changed += ups_changed;
                }

                let (body, markdown_changed) =
                    links::rewrite_markdown_links(&frontmatter.body, |url, _embed| {
                        if is_external(url) || url.starts_with('#') {
                            return None;
                        }
                        let (path_part, suffix) = split_url_suffix(url);
                        if path_part.is_empty() {
                            return None;
                        }
                        let target = if let Some(absolute) = path_part.strip_prefix('/') {
                            vault.root().join(absolute)
                        } else {
                            note_dir.join(path_part)
                        };
                        if !same_path(&target, &source_absolute) {
                            return None;
                        }
                        let relative = relative_path(note_dir, &new_absolute);
                        (relative != url).then(|| encode_path(&relative) + suffix)
                    });
                if markdown_changed > 0 {
                    frontmatter.body = body;
                    changed += markdown_changed;
                }

                if changed > 0 {
                    write_atomic(&note_absolute, frontmatter.full_text().as_bytes()).map_err(
                        |error| format!("Cannot write {}: {error}", note_absolute.display()),
                    )?;
                    result.changed_links += changed;
                    result.changed_files.push(note.meta.path.clone());
                }
            }
        }
    }

    if copy {
        fs::copy(&source_absolute, &new_absolute).map_err(|error| {
            format!(
                "Cannot copy {} to {}: {error}",
                source_absolute.display(),
                new_absolute.display()
            )
        })?;
    } else {
        move_file(&source_absolute, &new_absolute).map_err(|error| {
            format!(
                "Cannot move {} to {}: {error}",
                source_absolute.display(),
                new_absolute.display()
            )
        })?;
    }

    if let Some(text) = text {
        if source_relative.is_some() {
            let (rewritten, changed) = rewrite_outbound(
                vault,
                &text,
                source_dir,
                new_dir,
                &source_absolute,
                &new_absolute,
            );
            if changed > 0 {
                write_atomic(&new_absolute, rewritten.as_bytes())
                    .map_err(|error| format!("Cannot write {}: {error}", new_absolute.display()))?;
                result.changed_links += changed;
                if let Some(path) = &new_relative {
                    result.changed_files.push(path.clone());
                }
            }
        } else {
            result.warnings.push(
                "The note came from outside the slipbox, so relative links were left as they are."
                    .to_string(),
            );
        }
    }

    Ok(result)
}

fn rewrite_outbound(
    vault: &Vault,
    text: &str,
    old_dir: &Path,
    new_dir: &Path,
    old_absolute: &Path,
    new_absolute: &Path,
) -> (String, usize) {
    links::rewrite_markdown_links(text, |url, _embed| {
        if is_external(url) || url.starts_with('#') {
            return None;
        }
        let (path_part, suffix) = split_url_suffix(url);
        if path_part.is_empty() {
            return None;
        }
        let mut target = if let Some(absolute) = path_part.strip_prefix('/') {
            vault.root().join(absolute)
        } else {
            old_dir.join(path_part)
        };
        target = normalize(&target);
        if same_path(&target, old_absolute) {
            target = new_absolute.to_path_buf();
        } else if !target.exists() {
            return None;
        }
        let relative = relative_path(new_dir, &target);
        if relative.is_empty() {
            return None;
        }
        let rewritten = encode_path(&relative) + suffix;
        (rewritten != url).then_some(rewritten)
    })
}

fn best_target(index: &NoteIndex, target: &str) -> String {
    let path = Path::new(target);
    let stem = path
        .file_stem()
        .map(|stem| stem.to_string_lossy().into_owned())
        .unwrap_or_else(|| target.to_string());
    let count = index
        .notes()
        .iter()
        .filter(|note| note.meta.name.eq_ignore_ascii_case(&stem))
        .count();
    if count <= 1 {
        stem
    } else {
        target
            .trim_end_matches(".markdown")
            .trim_end_matches(".md")
            .replace('\\', "/")
    }
}

fn free_name(folder: &Path, wanted: &str) -> String {
    if !folder.join(wanted).exists() {
        return wanted.to_string();
    }
    let path = Path::new(wanted);
    let stem = path
        .file_stem()
        .map(|stem| stem.to_string_lossy().into_owned())
        .unwrap_or_else(|| wanted.to_string());
    let extension = path
        .extension()
        .map(|extension| format!(".{}", extension.to_string_lossy()))
        .unwrap_or_default();
    for index in 1.. {
        let candidate = format!("{stem} {index}{extension}");
        if !folder.join(&candidate).exists() {
            return candidate;
        }
    }
    unreachable!()
}

fn move_file(from: &Path, to: &Path) -> io::Result<()> {
    match fs::rename(from, to) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == io::ErrorKind::CrossesDevices => {
            fs::copy(from, to)?;
            fs::remove_file(from)
        }
        Err(error) => Err(error),
    }
}

fn write_atomic(path: &Path, bytes: &[u8]) -> io::Result<()> {
    let parent = path.parent().unwrap_or_else(|| Path::new("."));
    let serial = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_nanos())
        .unwrap_or(0);
    let name = path
        .file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_else(|| "note".to_string());
    let temporary = parent.join(format!(".{name}.{}.{serial}.tmp", std::process::id()));
    fs::write(&temporary, bytes)?;
    match fs::rename(&temporary, path) {
        Ok(()) => Ok(()),
        Err(error) => {
            let _ = fs::remove_file(&temporary);
            Err(error)
        }
    }
}

fn same_path(a: &Path, b: &Path) -> bool {
    normalize(a) == normalize(b)
}

fn normalize(path: &Path) -> PathBuf {
    let mut output = PathBuf::new();
    for component in path.components() {
        match component {
            Component::CurDir => {}
            Component::ParentDir => {
                if !output.pop() {
                    output.push(component);
                }
            }
            _ => output.push(component),
        }
    }
    output
}

fn relative_path(from: &Path, to: &Path) -> String {
    let from = normalize(from);
    let to = normalize(to);
    let from_components: Vec<Component> = from.components().collect();
    let to_components: Vec<Component> = to.components().collect();
    let mut common = 0usize;
    while common < from_components.len()
        && common < to_components.len()
        && from_components[common] == to_components[common]
    {
        common += 1;
    }
    let mut components: Vec<String> = Vec::new();
    for _ in common..from_components.len() {
        components.push("..".to_string());
    }
    for component in &to_components[common..] {
        components.push(component.as_os_str().to_string_lossy().into_owned());
    }
    components.join("/")
}

fn split_url_suffix(url: &str) -> (&str, &str) {
    let query = url.find('?');
    let anchor = url.find('#');
    let split = match (query, anchor) {
        (Some(a), Some(b)) => a.min(b),
        (Some(a), None) | (None, Some(a)) => a,
        (None, None) => url.len(),
    };
    url.split_at(split)
}

fn is_external(url: &str) -> bool {
    url.starts_with("http://")
        || url.starts_with("https://")
        || url.starts_with("mailto:")
        || url.starts_with("data:")
        || url.starts_with("obsidian:")
        || url.contains("://")
}

fn encode_path(path: &str) -> String {
    let mut encoded = String::with_capacity(path.len());
    for character in path.chars() {
        match character {
            ' ' => encoded.push_str("%20"),
            '(' => encoded.push_str("%28"),
            ')' => encoded.push_str("%29"),
            '#' => encoded.push_str("%23"),
            '?' => encoded.push_str("%3F"),
            _ => encoded.push(character),
        }
    }
    encoded
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::index::NoteIndex;
    use std::fs;

    fn vault() -> (tempfile::TempDir, Vault, NoteIndex) {
        let directory = tempfile::tempdir().unwrap();
        let root = directory.path();
        fs::create_dir_all(root.join(".obsidian")).unwrap();
        fs::create_dir_all(root.join("notes")).unwrap();
        fs::create_dir_all(root.join("archive")).unwrap();
        fs::write(
            root.join("notes/Target.md"),
            "---\ntags: [one]\n---\n# Target\n",
        )
        .unwrap();
        fs::write(
            root.join("notes/Source.md"),
            "---\nup:\n  - \"[[Target]]\"\n---\nSee [[Target]] and [file](Target.md).\n",
        )
        .unwrap();
        fs::write(root.join("Other.md"), "Links to [[Target]].\n").unwrap();
        let vault = Vault::discover(root).unwrap();
        let index = NoteIndex::build(&vault);
        (directory, vault, index)
    }

    #[test]
    fn moves_a_note_and_rewrites_links_on_both_sides() {
        let (_directory, vault, index) = vault();
        let result = move_note(
            &vault,
            &index,
            Path::new("notes/Target.md"),
            Path::new("archive"),
            false,
        );
        assert!(result.success, "{:?}", result.error);
        assert_eq!(result.new_path.as_deref(), Some("archive/Target.md"));
        assert!(vault.root().join("archive/Target.md").exists());
        assert!(!vault.root().join("notes/Target.md").exists());

        let other = fs::read_to_string(vault.root().join("Other.md")).unwrap();
        assert!(other.contains("[[Target]]"));
        let source = fs::read_to_string(vault.root().join("notes/Source.md")).unwrap();
        assert!(source.contains("[[Target]]"));
        assert!(source.contains("[file](../archive/Target.md)"));
    }

    #[test]
    fn copy_leaves_the_original_and_duplicates_its_links() {
        let (_directory, vault, index) = vault();
        let result = move_note(
            &vault,
            &index,
            Path::new("notes/Target.md"),
            Path::new("archive"),
            true,
        );
        assert!(result.success, "{:?}", result.error);
        assert!(vault.root().join("notes/Target.md").exists());
        assert!(vault.root().join("archive/Target.md").exists());
        let other = fs::read_to_string(vault.root().join("Other.md")).unwrap();
        assert!(other.contains("[[Target]]"));
    }

    #[test]
    fn picks_a_free_name_on_collision() {
        let (_directory, vault, index) = vault();
        fs::write(vault.root().join("archive/Target.md"), "old\n").unwrap();
        let result = move_note(
            &vault,
            &index,
            Path::new("notes/Target.md"),
            Path::new("archive"),
            true,
        );
        assert!(result.success, "{:?}", result.error);
        assert_eq!(result.new_path.as_deref(), Some("archive/Target 1.md"));
    }

    #[test]
    fn moves_a_note_from_outside_the_vault_into_it() {
        let (_directory, vault, index) = vault();
        let external_directory = tempfile::tempdir().unwrap();
        let external = external_directory.path().join("Outside.md");
        fs::write(
            &external,
            "---\ntags: [outside]\n---\nA note from elsewhere.\n",
        )
        .unwrap();

        let result = move_note(&vault, &index, &external, vault.root(), false);
        assert!(result.success, "{:?}", result.error);
        assert_eq!(result.new_path.as_deref(), Some("Outside.md"));
        assert!(!external.exists());
        assert!(vault.root().join("Outside.md").exists());
        assert!(result
            .warnings
            .iter()
            .any(|warning| warning.contains("outside the slipbox")));
    }

    #[test]
    fn copies_a_note_from_outside_the_vault_into_it() {
        let (_directory, vault, index) = vault();
        let external_directory = tempfile::tempdir().unwrap();
        let external = external_directory.path().join("Outside.md");
        fs::write(&external, "A note from elsewhere.\n").unwrap();

        let result = move_note(&vault, &index, &external, vault.root(), true);
        assert!(result.success, "{:?}", result.error);
        assert!(external.exists());
        assert!(vault.root().join("Outside.md").exists());
    }

    #[test]
    fn rewrites_relative_markdown_links_in_the_moved_note() {
        let (_directory, vault, _index) = vault();
        // A link to a note that stays behind should keep working from the new
        // folder, so it gains a `../notes/` prefix.
        fs::write(
            vault.root().join("notes/Source.md"),
            "[sibling](Target.md)\n",
        )
        .unwrap();
        let index = NoteIndex::build(&vault);
        let result = move_note(
            &vault,
            &index,
            Path::new("notes/Source.md"),
            Path::new("archive"),
            true,
        );
        assert!(result.success, "{:?}", result.error);
        let moved = fs::read_to_string(vault.root().join("archive/Source.md")).unwrap();
        assert_eq!(moved, "[sibling](../notes/Target.md)\n");
    }
}
