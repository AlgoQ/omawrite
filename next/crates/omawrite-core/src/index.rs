use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use serde::Serialize;

use crate::frontmatter::Frontmatter;
use crate::links;
use crate::vault::Vault;

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NoteMeta {
    pub path: String,
    pub name: String,
    pub title: String,
    pub tags: Vec<String>,
    pub ups: Vec<String>,
    pub aliases: Vec<String>,
    pub modified: u64,
}

#[derive(Clone, Debug)]
pub struct Note {
    pub meta: NoteMeta,
    pub body: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TreeNode {
    pub name: String,
    pub path: String,
    pub tags: Vec<String>,
    pub child_count: usize,
    pub children: Vec<TreeNode>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Suggestion {
    pub name: String,
    pub path: String,
    pub insert: String,
    pub score: usize,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Backlink {
    pub path: String,
    pub name: String,
    pub line: usize,
    pub snippet: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SearchHit {
    pub path: String,
    pub name: String,
    pub line: usize,
    pub snippet: String,
    pub score: usize,
}

#[derive(Clone, Debug)]
pub struct NoteIndex {
    root: PathBuf,
    notes: Vec<Note>,
    by_path: HashMap<String, usize>,
    backlinks: HashMap<String, Vec<Backlink>>,
}

impl NoteIndex {
    pub fn build(vault: &Vault) -> Self {
        let mut notes = Vec::new();
        for absolute in vault.walk_notes() {
            let Some(relative) = vault.relative_string(&absolute) else {
                continue;
            };
            let Ok(bytes) = fs::read(&absolute) else {
                continue;
            };
            let text = String::from_utf8_lossy(&bytes);
            let frontmatter = Frontmatter::parse(&text);
            let name = Path::new(&relative)
                .file_stem()
                .map(|stem| stem.to_string_lossy().into_owned())
                .unwrap_or_else(|| relative.clone());
            let mut tags = frontmatter.values("tags");
            tags.extend(frontmatter.values("tag"));
            dedupe_ci(&mut tags);
            let mut aliases = frontmatter.values("aliases");
            aliases.extend(frontmatter.values("alias"));
            dedupe_ci(&mut aliases);
            let modified = fs::metadata(&absolute)
                .and_then(|metadata| metadata.modified())
                .ok()
                .and_then(|time| time.duration_since(UNIX_EPOCH).ok())
                .map(|duration| duration.as_millis() as u64)
                .unwrap_or(0);
            notes.push(Note {
                meta: NoteMeta {
                    path: relative.clone(),
                    name: name.clone(),
                    title: frontmatter.value("title").unwrap_or(name),
                    tags,
                    ups: frontmatter.values("up"),
                    aliases,
                    modified,
                },
                body: frontmatter.body.clone(),
            });
        }

        notes.sort_by(|a, b| a.meta.path.cmp(&b.meta.path));
        let by_path: HashMap<String, usize> = notes
            .iter()
            .enumerate()
            .map(|(index, note)| (note.meta.path.clone(), index))
            .collect();
        let backlinks = build_backlinks(vault, &notes);

        Self {
            root: vault.root().to_path_buf(),
            notes,
            by_path,
            backlinks,
        }
    }

    /// Update one note after a save, without re-walking the vault. Backlinks
    /// are rebuilt from the warm bodies already in memory.
    pub fn update_note(&mut self, vault: &Vault, path: &str, text: &str) {
        let frontmatter = Frontmatter::parse(text);
        let name = Path::new(path)
            .file_stem()
            .map(|stem| stem.to_string_lossy().into_owned())
            .unwrap_or_else(|| path.to_string());
        let mut tags = frontmatter.values("tags");
        tags.extend(frontmatter.values("tag"));
        dedupe_ci(&mut tags);
        let mut aliases = frontmatter.values("aliases");
        aliases.extend(frontmatter.values("alias"));
        dedupe_ci(&mut aliases);
        let modified = fs::metadata(vault.absolute(Path::new(path)))
            .and_then(|metadata| metadata.modified())
            .ok()
            .and_then(|time| time.duration_since(UNIX_EPOCH).ok())
            .map(|duration| duration.as_millis() as u64)
            .unwrap_or(0);
        let note = Note {
            meta: NoteMeta {
                path: path.to_string(),
                name: name.clone(),
                title: frontmatter.value("title").unwrap_or(name),
                tags,
                ups: frontmatter.values("up"),
                aliases,
                modified,
            },
            body: frontmatter.body.clone(),
        };

        if let Some(index) = self.by_path.get(path).copied() {
            self.notes[index] = note;
        } else {
            self.notes.push(note);
            self.notes.sort_by(|a, b| a.meta.path.cmp(&b.meta.path));
            self.by_path = self
                .notes
                .iter()
                .enumerate()
                .map(|(index, note)| (note.meta.path.clone(), index))
                .collect();
        }
        self.backlinks = build_backlinks(vault, &self.notes);
    }

    pub fn remove_note(&mut self, vault: &Vault, path: &str) {
        self.notes.retain(|note| note.meta.path != path);
        self.by_path = self
            .notes
            .iter()
            .enumerate()
            .map(|(index, note)| (note.meta.path.clone(), index))
            .collect();
        self.backlinks = build_backlinks(vault, &self.notes);
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    pub fn notes(&self) -> &[Note] {
        &self.notes
    }

    pub fn paths(&self) -> Vec<String> {
        self.notes
            .iter()
            .map(|note| note.meta.path.clone())
            .collect()
    }

    pub fn meta(&self, path: &str) -> Option<&NoteMeta> {
        self.by_path
            .get(path)
            .and_then(|index| self.notes.get(*index))
            .map(|note| &note.meta)
    }

    pub fn tree(&self, vault: &Vault) -> Vec<TreeNode> {
        let visible: Vec<&Note> = self
            .notes
            .iter()
            .filter(|note| !Vault::is_excluded_tree_path(Path::new(&note.meta.path)))
            .collect();
        let visible_paths: Vec<String> =
            visible.iter().map(|note| note.meta.path.clone()).collect();
        let by_path: HashMap<String, &Note> = visible
            .iter()
            .map(|note| (note.meta.path.clone(), *note))
            .collect();

        let mut children: HashMap<String, Vec<String>> = HashMap::new();
        let mut has_parent: HashSet<String> = HashSet::new();
        for note in &visible {
            let source_dir = vault
                .root()
                .join(Path::new(&note.meta.path))
                .parent()
                .map(Path::to_path_buf)
                .unwrap_or_else(|| vault.root().to_path_buf());
            for up in &note.meta.ups {
                let Some(parent) = vault.resolve_note(up, &source_dir, &visible_paths) else {
                    continue;
                };
                if parent == note.meta.path || !by_path.contains_key(&parent) {
                    continue;
                }
                let entry = children.entry(parent).or_default();
                if !entry.contains(&note.meta.path) {
                    entry.push(note.meta.path.clone());
                }
                has_parent.insert(note.meta.path.clone());
            }
        }
        for entries in children.values_mut() {
            entries.sort_by(|a, b| {
                let a_name = Path::new(a)
                    .file_stem()
                    .unwrap_or_default()
                    .to_string_lossy();
                let b_name = Path::new(b)
                    .file_stem()
                    .unwrap_or_default()
                    .to_string_lossy();
                a_name
                    .to_lowercase()
                    .cmp(&b_name.to_lowercase())
                    .then_with(|| a.cmp(b))
            });
        }

        let mut roots: Vec<String> = visible
            .iter()
            .map(|note| note.meta.path.clone())
            .filter(|path| !has_parent.contains(path))
            .collect();
        roots.sort_by(|a, b| {
            let a_name = by_path
                .get(a)
                .map(|note| note.meta.name.as_str())
                .unwrap_or(a);
            let b_name = by_path
                .get(b)
                .map(|note| note.meta.name.as_str())
                .unwrap_or(b);
            a_name
                .to_lowercase()
                .cmp(&b_name.to_lowercase())
                .then_with(|| a.cmp(b))
        });

        let mut placed = HashSet::new();
        let mut tree = Vec::new();
        for root in &roots {
            if placed.contains(root) {
                continue;
            }
            let node = build_node(root, &mut HashSet::new(), &by_path, &children, &mut placed);
            tree.push(node);
        }
        // A pure cycle has no roots. Lift its members so no note disappears.
        for note in &visible {
            if placed.contains(&note.meta.path) {
                continue;
            }
            let node = build_node(
                &note.meta.path,
                &mut HashSet::new(),
                &by_path,
                &children,
                &mut placed,
            );
            tree.push(node);
        }
        tree
    }

    pub fn suggestions(&self, query: &str) -> Vec<Suggestion> {
        let needle = query.trim().to_lowercase();
        let mut name_counts: HashMap<String, usize> = HashMap::new();
        for note in &self.notes {
            *name_counts
                .entry(note.meta.name.to_lowercase())
                .or_default() += 1;
        }
        let mut suggestions = Vec::new();
        for note in &self.notes {
            let name = note.meta.name.to_lowercase();
            let path = note.meta.path.to_lowercase();
            let name_at = if needle.is_empty() {
                Some(0)
            } else {
                name.find(&needle)
            };
            let path_at = if needle.is_empty() {
                Some(0)
            } else {
                path.find(&needle)
            };
            if name_at.is_none() && path_at.is_none() {
                continue;
            }
            let score = match (name_at, path_at) {
                (Some(at), _) => at,
                (None, Some(at)) => 1000 + at,
                _ => 2000,
            };
            let duplicate = name_counts.get(&name).copied().unwrap_or(0) > 1;
            let insert = if duplicate && !note.meta.path.contains('/') {
                note.meta.name.clone()
            } else if duplicate {
                note.meta
                    .path
                    .trim_end_matches(".md")
                    .trim_end_matches(".markdown")
                    .to_string()
            } else {
                note.meta.name.clone()
            };
            suggestions.push(Suggestion {
                name: note.meta.name.clone(),
                path: note.meta.path.clone(),
                insert,
                score,
            });
        }
        suggestions.sort_by(|a, b| {
            a.score
                .cmp(&b.score)
                .then_with(|| a.name.to_lowercase().cmp(&b.name.to_lowercase()))
                .then_with(|| a.path.cmp(&b.path))
        });
        suggestions.truncate(50);
        suggestions
    }

    pub fn backlinks(&self, path: &str) -> Vec<Backlink> {
        self.backlinks.get(path).cloned().unwrap_or_default()
    }

    pub fn search(&self, query: &str) -> Vec<SearchHit> {
        let needle = query.trim().to_lowercase();
        if needle.is_empty() {
            return Vec::new();
        }
        let mut hits = Vec::new();
        for note in &self.notes {
            let name = note.meta.name.to_lowercase();
            let path = note.meta.path.to_lowercase();
            if let Some(at) = name.find(&needle) {
                hits.push(SearchHit {
                    path: note.meta.path.clone(),
                    name: note.meta.name.clone(),
                    line: 1,
                    snippet: note.meta.title.clone(),
                    score: at,
                });
                continue;
            }
            if let Some(at) = path.find(&needle) {
                hits.push(SearchHit {
                    path: note.meta.path.clone(),
                    name: note.meta.name.clone(),
                    line: 1,
                    snippet: note.meta.path.clone(),
                    score: 100 + at,
                });
                continue;
            }
            if let Some((line, snippet)) = first_body_match(&note.body, &needle) {
                hits.push(SearchHit {
                    path: note.meta.path.clone(),
                    name: note.meta.name.clone(),
                    line,
                    snippet,
                    score: 1000 + line,
                });
            }
        }
        hits.sort_by(|a, b| a.score.cmp(&b.score).then_with(|| a.name.cmp(&b.name)));
        hits.truncate(100);
        hits
    }
}

fn build_backlinks(vault: &Vault, notes: &[Note]) -> HashMap<String, Vec<Backlink>> {
    let paths: Vec<String> = notes.iter().map(|note| note.meta.path.clone()).collect();
    let mut backlinks: HashMap<String, Vec<Backlink>> = HashMap::new();
    for note in notes {
        let source_path = Path::new(&note.meta.path);
        let source_dir = vault
            .root()
            .join(source_path)
            .parent()
            .map(Path::to_path_buf)
            .unwrap_or_else(|| vault.root().to_path_buf());
        let mut seen = HashSet::new();
        for link in links::wikilinks(&note.body) {
            let Some(target) = vault.resolve_note(&link.target, &source_dir, &paths) else {
                continue;
            };
            if target == note.meta.path || !seen.insert(target.clone()) {
                continue;
            }
            let offset = link.range.start;
            backlinks.entry(target).or_default().push(Backlink {
                path: note.meta.path.clone(),
                name: note.meta.name.clone(),
                line: links::line_of(&note.body, offset),
                snippet: links::snippet_at(&note.body, offset, 120),
            });
        }
        for up in &note.meta.ups {
            let Some(target) = vault.resolve_note(up, &source_dir, &paths) else {
                continue;
            };
            if target == note.meta.path || !seen.insert(target.clone()) {
                continue;
            }
            backlinks.entry(target).or_default().push(Backlink {
                path: note.meta.path.clone(),
                name: note.meta.name.clone(),
                line: 1,
                snippet: format!("up: [[{up}]]"),
            });
        }
    }
    for entries in backlinks.values_mut() {
        entries.sort_by(|a, b| {
            a.name
                .to_lowercase()
                .cmp(&b.name.to_lowercase())
                .then_with(|| a.path.cmp(&b.path))
        });
    }
    backlinks
}

fn build_node(
    path: &str,
    ancestors: &mut HashSet<String>,
    by_path: &HashMap<String, &Note>,
    children: &HashMap<String, Vec<String>>,
    placed: &mut HashSet<String>,
) -> TreeNode {
    placed.insert(path.to_string());
    ancestors.insert(path.to_string());
    let mut nodes = Vec::new();
    if let Some(child_paths) = children.get(path) {
        for child in child_paths {
            if ancestors.contains(child) {
                continue;
            }
            nodes.push(build_node(child, ancestors, by_path, children, placed));
        }
    }
    ancestors.remove(path);
    let note = by_path.get(path);
    TreeNode {
        name: note.map(|note| note.meta.name.clone()).unwrap_or_else(|| {
            Path::new(path)
                .file_stem()
                .map(|stem| stem.to_string_lossy().into_owned())
                .unwrap_or_else(|| path.to_string())
        }),
        path: path.to_string(),
        tags: note.map(|note| note.meta.tags.clone()).unwrap_or_default(),
        child_count: nodes.len(),
        children: nodes,
    }
}

fn dedupe_ci(values: &mut Vec<String>) {
    let mut seen = HashSet::new();
    values.retain(|value| seen.insert(value.to_lowercase()));
}

fn first_body_match(body: &str, needle: &str) -> Option<(usize, String)> {
    for (index, line) in body.lines().enumerate() {
        if line.to_lowercase().contains(needle) {
            let trimmed = line.trim();
            let snippet = if trimmed.chars().count() > 140 {
                let mut snippet: String = trimmed.chars().take(140).collect();
                snippet.push('…');
                snippet
            } else {
                trimmed.to_string()
            };
            return Some((index + 1, snippet));
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn vault() -> (tempfile::TempDir, Vault) {
        let directory = tempfile::tempdir().unwrap();
        let root = directory.path();
        fs::create_dir_all(root.join(".obsidian")).unwrap();
        fs::create_dir_all(root.join("notes")).unwrap();
        fs::write(
            root.join("Parent.md"),
            "---\ntags:\n  - root\n---\n# Parent\n",
        )
        .unwrap();
        fs::write(
            root.join("notes/Child.md"),
            "---\nup:\n  - \"[[Parent]]\"\n---\nLinks to [[Parent]].\n",
        )
        .unwrap();
        fs::write(root.join("Orphan.md"), "---\ntags: [loose]\n---\n").unwrap();
        let vault = Vault::discover(root).unwrap();
        (directory, vault)
    }

    #[test]
    fn indexes_notes_and_frontmatter() {
        let (_directory, vault) = vault();
        let index = NoteIndex::build(&vault);
        assert_eq!(index.notes().len(), 3);
        let child = index.meta("notes/Child.md").unwrap();
        assert_eq!(child.ups, vec!["Parent"]);
        let parent = index.meta("Parent.md").unwrap();
        assert_eq!(parent.tags, vec!["root"]);
    }

    #[test]
    fn builds_the_up_tree_and_leaves_orphans_at_the_root() {
        let (_directory, vault) = vault();
        let index = NoteIndex::build(&vault);
        let tree = index.tree(&vault);
        assert_eq!(tree.len(), 2);
        let parent = tree.iter().find(|node| node.name == "Parent").unwrap();
        assert_eq!(parent.child_count, 1);
        assert_eq!(parent.children[0].name, "Child");
        assert!(tree.iter().any(|node| node.name == "Orphan"));
    }

    #[test]
    fn finds_backlinks_for_wikilinks_and_up() {
        let (_directory, vault) = vault();
        let index = NoteIndex::build(&vault);
        let backlinks = index.backlinks("Parent.md");
        assert_eq!(backlinks.len(), 1);
        assert_eq!(backlinks[0].path, "notes/Child.md");
    }

    #[test]
    fn suggests_by_name_and_disambiguates_duplicates() {
        let (_directory, vault) = vault();
        let index = NoteIndex::build(&vault);
        let suggestions = index.suggestions("chi");
        assert_eq!(suggestions[0].name, "Child");
        assert_eq!(suggestions[0].path, "notes/Child.md");
    }

    #[test]
    fn searches_note_bodies() {
        let (_directory, vault) = vault();
        let index = NoteIndex::build(&vault);
        let hits = index.search("links to");
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0].path, "notes/Child.md");
        assert_eq!(hits[0].line, 1);
    }
}
