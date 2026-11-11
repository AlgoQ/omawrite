use serde::Serialize;

/// The value shape the properties panel draws. The Rust side decides the kind
/// so the TypeScript panel never has to re-parse YAML.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum PropertyKind {
    Tags,
    Links,
    List,
    Checkbox,
    Number,
    Date,
    Text,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Property {
    pub key: String,
    pub kind: PropertyKind,
    pub values: Vec<String>,
    pub display: String,
}

#[derive(Clone, Debug)]
struct Entry {
    key: String,
    values: Vec<String>,
    is_list: bool,
    is_mapping: bool,
    /// Line range inside `content`, end exclusive.
    start: usize,
    end: usize,
}

/// A note split into YAML frontmatter and body.
///
/// The body is what the editor holds. The frontmatter is kept as lines so a
/// property edit rewrites only the key it touches and leaves comments, key
/// order and keys the panel does not understand alone.
#[derive(Clone, Debug, Default)]
pub struct Frontmatter {
    raw: String,
    pub body: String,
    entries: Vec<Entry>,
    content: Vec<String>,
    opening: String,
    closing: String,
    line_ending: String,
    present: bool,
}

impl Frontmatter {
    pub fn parse(text: &str) -> Self {
        let line_ending = if text.contains("\r\n") { "\r\n" } else { "\n" }.to_string();
        let raw_lines: Vec<&str> = text.split_inclusive('\n').collect();

        let first = raw_lines.first().map(|line| content_of(line)).unwrap_or("");
        if first.trim() != "---" {
            return Self {
                raw: String::new(),
                body: text.to_string(),
                entries: Vec::new(),
                content: Vec::new(),
                opening: String::new(),
                closing: String::new(),
                line_ending,
                present: false,
            };
        }

        let mut closing_index = None;
        for (index, line) in raw_lines.iter().enumerate().skip(1) {
            let trimmed = content_of(line).trim();
            if trimmed == "---" || trimmed == "..." {
                closing_index = Some(index);
                break;
            }
        }

        let Some(closing_index) = closing_index else {
            return Self {
                raw: String::new(),
                body: text.to_string(),
                entries: Vec::new(),
                content: Vec::new(),
                opening: String::new(),
                closing: String::new(),
                line_ending,
                present: false,
            };
        };

        let raw_end: usize = raw_lines[..=closing_index]
            .iter()
            .map(|line| line.len())
            .sum();
        let raw = text[..raw_end].to_string();
        let body = text[raw_end..].to_string();
        let content: Vec<String> = raw_lines[1..closing_index]
            .iter()
            .map(|line| content_of(line).to_string())
            .collect();
        let opening = content_of(raw_lines[0]).to_string();
        let closing = content_of(raw_lines[closing_index]).to_string();

        let mut frontmatter = Self {
            raw,
            body,
            entries: Vec::new(),
            content,
            opening,
            closing,
            line_ending,
            present: true,
        };
        frontmatter.reparse();
        frontmatter
    }

    pub fn raw(&self) -> &str {
        &self.raw
    }

    pub fn full_text(&self) -> String {
        let mut text = self.raw.clone();
        text.push_str(&self.body);
        text
    }

    pub fn is_empty(&self) -> bool {
        !self.present
    }

    pub fn properties(&self) -> Vec<Property> {
        let mut rows = Vec::new();
        for entry in &self.entries {
            if entry.is_mapping {
                continue;
            }
            let kind = property_kind(&entry.key, &entry.values, entry.is_list);
            let display = match kind {
                PropertyKind::Checkbox => entry.values.first().cloned().unwrap_or_default(),
                _ => entry.values.join(", "),
            };
            rows.push(Property {
                key: entry.key.clone(),
                kind,
                values: entry.values.clone(),
                display,
            });
        }
        rows
    }

    pub fn values(&self, key: &str) -> Vec<String> {
        self.entry(key)
            .map(|entry| entry.values.clone())
            .unwrap_or_default()
    }

    pub fn value(&self, key: &str) -> Option<String> {
        self.values(key).into_iter().next()
    }

    pub fn set_scalar(&mut self, key: &str, value: &str) {
        let block = if value.is_empty() {
            vec![format!("{key}:")]
        } else {
            vec![format!("{key}: {value}")]
        };
        self.replace_entry(key, block);
    }

    pub fn set_values(&mut self, key: &str, values: &[String]) {
        let block = self.block_for(key, values);
        self.replace_entry(key, block);
    }

    pub fn remove_key(&mut self, key: &str) {
        self.ensure_present();
        if let Some(entry) = self.entry(key) {
            let (start, end) = (entry.start, entry.end);
            self.content.drain(start..end);
            self.rebuild();
        }
    }

    pub fn set_item(&mut self, key: &str, index: usize, value: &str) {
        let mut values = self.values(key);
        if index >= values.len() {
            return;
        }
        values[index] = clean_for_key(key, value);
        self.set_values(key, &values);
    }

    pub fn add_value(&mut self, key: &str, value: &str) {
        let mut values = self.values(key);
        let value = clean_for_key(key, value);
        if !values.iter().any(|existing| existing == &value) {
            values.push(value);
        }
        self.set_values(key, &values);
    }

    pub fn remove_value(&mut self, key: &str, index: usize) {
        let mut values = self.values(key);
        if index < values.len() {
            values.remove(index);
        }
        self.set_values(key, &values);
    }

    pub fn add_tag(&mut self, tag: &str) {
        let tag = tag.trim().trim_start_matches('#').to_string();
        if tag.is_empty() {
            return;
        }
        let mut tags = self.values("tags");
        if tags.is_empty() && self.entry("tags").is_none() {
            tags = self.values("tag");
        }
        if !tags
            .iter()
            .any(|existing| existing.eq_ignore_ascii_case(&tag))
        {
            tags.push(tag);
        }
        self.set_values("tags", &tags);
        if self.entry("tag").is_some() {
            self.remove_key("tag");
        }
    }

    pub fn remove_tag(&mut self, tag: &str) {
        let tags: Vec<String> = self
            .values("tags")
            .into_iter()
            .filter(|existing| !existing.eq_ignore_ascii_case(tag))
            .collect();
        self.set_values("tags", &tags);
    }

    pub fn add_up(&mut self, target: &str) {
        let mut ups = self.values("up");
        let target = clean_link(target);
        if target.is_empty() || ups.iter().any(|existing| existing == &target) {
            return;
        }
        ups.push(target);
        self.set_values("up", &ups);
    }

    pub fn remove_up(&mut self, target: &str) {
        let target = clean_link(target);
        let ups: Vec<String> = self
            .values("up")
            .into_iter()
            .filter(|existing| existing != &target)
            .collect();
        self.set_values("up", &ups);
    }

    fn entry(&self, key: &str) -> Option<&Entry> {
        self.entries
            .iter()
            .find(|entry| entry.key.eq_ignore_ascii_case(key))
    }

    fn block_for(&self, key: &str, values: &[String]) -> Vec<String> {
        let needs_links = key.eq_ignore_ascii_case("up");
        let is_list_key = matches!(
            key.to_ascii_lowercase().as_str(),
            "tags" | "tag" | "up" | "aliases" | "alias" | "authors" | "projects"
        );

        if values.is_empty() {
            return vec![format!("{key}: []")];
        }
        if values.len() == 1 && !is_list_key && !needs_links {
            return vec![format!("{key}: {}", values[0])];
        }

        let mut block = vec![format!("{key}:")];
        for value in values {
            if needs_links {
                block.push(format!("  - \"[[{value}]]\""));
            } else {
                block.push(format!("  - {value}"));
            }
        }
        block
    }

    fn replace_entry(&mut self, key: &str, block: Vec<String>) {
        self.ensure_present();
        if let Some(entry) = self.entry(key) {
            let (start, end) = (entry.start, entry.end);
            self.content.splice(start..end, block);
        } else {
            self.content.extend(block);
        }
        self.rebuild();
    }

    fn ensure_present(&mut self) {
        if self.present {
            return;
        }
        self.present = true;
        self.opening = "---".to_string();
        self.closing = "---".to_string();
        self.content.clear();
        self.entries.clear();
        self.rebuild();
    }

    fn rebuild(&mut self) {
        let ending = &self.line_ending;
        let mut raw = String::new();
        if self.present {
            raw.push_str(&self.opening);
            raw.push_str(ending);
            for line in &self.content {
                raw.push_str(line);
                raw.push_str(ending);
            }
            raw.push_str(&self.closing);
            raw.push_str(ending);
        }
        self.raw = raw;
        self.reparse();
    }

    fn reparse(&mut self) {
        self.entries = parse_entries(&self.content);
    }
}

fn content_of(line: &str) -> &str {
    line.strip_suffix('\n')
        .unwrap_or(line)
        .strip_suffix('\r')
        .unwrap_or_else(|| line.strip_suffix('\n').unwrap_or(line))
}

fn parse_entries(content: &[String]) -> Vec<Entry> {
    let mut entries = Vec::new();
    let mut index = 0;
    while index < content.len() {
        let line = &content[index];
        let Some((key, rest)) = split_key(line) else {
            index += 1;
            continue;
        };

        let start = index;
        let mut end = index + 1;
        while end < content.len() {
            let candidate = &content[end];
            if is_top_level_key(candidate) {
                break;
            }
            end += 1;
        }

        let block = &content[index + 1..end];
        let (mut values, is_list, is_mapping) = parse_values(&rest, block);
        for value in &mut values {
            *value = clean_for_key(&key, value);
        }
        values.retain(|value| !value.is_empty());

        entries.push(Entry {
            key,
            values,
            is_list,
            is_mapping,
            start,
            end,
        });
        index = end;
    }
    entries
}

fn split_key(line: &str) -> Option<(String, String)> {
    if line.starts_with(' ') || line.starts_with('\t') {
        return None;
    }
    let trimmed = line.trim_end();
    if trimmed.is_empty() || trimmed.starts_with('#') || trimmed.starts_with('-') {
        return None;
    }
    let (key, rest) = trimmed.split_once(':')?;
    let key = key.trim();
    if key.is_empty()
        || key.starts_with('-')
        || key.starts_with('#')
        || key.contains('[')
        || key.contains(']')
        || key
            .chars()
            .any(|character| !(character.is_alphanumeric() || " _-.".contains(character)))
    {
        return None;
    }
    let rest = rest.trim_start();
    if rest.starts_with("//") {
        return None;
    }
    Some((key.to_string(), rest.to_string()))
}

fn is_top_level_key(line: &str) -> bool {
    if line.is_empty() {
        return false;
    }
    if line.starts_with(' ') || line.starts_with('\t') {
        return false;
    }
    split_key(line).is_some()
}

fn parse_values(rest: &str, block: &[String]) -> (Vec<String>, bool, bool) {
    let rest = rest.trim();
    if !rest.is_empty() {
        if rest.starts_with("[[") && rest.ends_with("]]") && !rest.contains(',') {
            return (vec![unquote(rest)], false, false);
        }
        if rest.starts_with('[') && rest.ends_with(']') {
            return (split_inline_list(&rest[1..rest.len() - 1]), true, false);
        }
        if rest.starts_with('{') && rest.ends_with('}') {
            return (Vec::new(), false, true);
        }
        return (vec![unquote(rest)], false, false);
    }

    let mut values = Vec::new();
    let mut is_mapping = false;
    let mut saw_list = false;
    for line in block {
        let trimmed = line.trim_start();
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }
        if let Some(item) = trimmed.strip_prefix('-') {
            saw_list = true;
            let item = item.trim_start();
            if !item.is_empty() {
                values.push(unquote(item));
            }
        } else if trimmed.contains(':') {
            is_mapping = true;
        }
    }
    (values, saw_list, is_mapping)
}

fn split_inline_list(inner: &str) -> Vec<String> {
    let mut values = Vec::new();
    let mut current = String::new();
    let mut quote: Option<char> = None;
    let mut depth = 0i32;
    for character in inner.chars() {
        match character {
            '"' | '\'' => {
                if quote == Some(character) {
                    quote = None;
                } else if quote.is_none() {
                    quote = Some(character);
                }
                current.push(character);
            }
            '[' => {
                if quote.is_none() {
                    depth += 1;
                }
                current.push(character);
            }
            ']' => {
                if quote.is_none() {
                    depth -= 1;
                }
                current.push(character);
            }
            ',' if quote.is_none() && depth == 0 => {
                let value = unquote(current.trim());
                if !value.is_empty() {
                    values.push(value);
                }
                current.clear();
            }
            _ => current.push(character),
        }
    }
    let value = unquote(current.trim());
    if !value.is_empty() {
        values.push(value);
    }
    values
}

fn unquote(value: &str) -> String {
    let value = value.trim();
    if value.len() >= 2 {
        let first = value.chars().next().unwrap();
        let last = value.chars().last().unwrap();
        if (first == '"' && last == '"') || (first == '\'' && last == '\'') {
            return value[1..value.len() - 1].trim().to_string();
        }
    }
    value.to_string()
}

fn clean_for_key(key: &str, value: &str) -> String {
    let value = unquote(value);
    if key.eq_ignore_ascii_case("tags") || key.eq_ignore_ascii_case("tag") {
        return value.trim_start_matches('#').trim().to_string();
    }
    if key.eq_ignore_ascii_case("up") {
        return clean_link(&value);
    }
    value
}

pub fn clean_link(value: &str) -> String {
    let mut value = unquote(value);
    value = value.trim_start_matches('!').trim().to_string();
    if let Some(inner) = value.strip_prefix("[[") {
        if let Some(inner) = inner.strip_suffix("]]") {
            value = inner.to_string();
        }
    }
    if let Some((before, _)) = value.split_once('|') {
        value = before.to_string();
    }
    if let Some((before, _)) = value.split_once('#') {
        value = before.to_string();
    }
    value.trim().to_string()
}

fn property_kind(key: &str, values: &[String], is_list: bool) -> PropertyKind {
    if key.eq_ignore_ascii_case("tags") || key.eq_ignore_ascii_case("tag") {
        return PropertyKind::Tags;
    }
    if key.eq_ignore_ascii_case("up") {
        return PropertyKind::Links;
    }
    if is_list {
        return PropertyKind::List;
    }
    if values.len() == 1 {
        let value = &values[0];
        if value == "true" || value == "false" {
            return PropertyKind::Checkbox;
        }
        if value.parse::<f64>().is_ok() {
            return PropertyKind::Number;
        }
        if is_date(value) {
            return PropertyKind::Date;
        }
    }
    PropertyKind::Text
}

fn is_date(value: &str) -> bool {
    let bytes = value.as_bytes();
    bytes.len() == 10
        && bytes[4] == b'-'
        && bytes[7] == b'-'
        && bytes[..4].iter().all(u8::is_ascii_digit)
        && bytes[5..7].iter().all(u8::is_ascii_digit)
        && bytes[8..].iter().all(u8::is_ascii_digit)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn splits_frontmatter_and_body() {
        let note =
            Frontmatter::parse("---\ntags:\n  - one\nup:\n  - \"[[Parent]]\"\n---\n# Body\n");
        assert_eq!(note.body, "# Body\n");
        assert_eq!(note.values("tags"), vec!["one"]);
        assert_eq!(note.values("up"), vec!["Parent"]);
        assert_eq!(note.properties().len(), 2);
    }

    #[test]
    fn leaves_a_note_without_frontmatter_alone() {
        let note = Frontmatter::parse("# Body\n");
        assert!(note.is_empty());
        assert_eq!(note.raw(), "");
        assert_eq!(note.body, "# Body\n");
        assert_eq!(note.full_text(), "# Body\n");
    }

    #[test]
    fn edits_one_key_without_touching_the_rest() {
        let mut note = Frontmatter::parse("---\n# keep me\nfoo: bar\ntags:\n  - one\n---\nbody\n");
        note.add_tag("two");
        assert_eq!(note.values("tags"), vec!["one", "two"]);
        assert!(note.raw().contains("# keep me"));
        assert!(note.raw().contains("foo: bar"));
        assert_eq!(note.body, "body\n");
    }

    #[test]
    fn adds_frontmatter_to_a_plain_note() {
        let mut note = Frontmatter::parse("body\n");
        note.add_tag("new");
        assert_eq!(note.values("tags"), vec!["new"]);
        assert!(note.full_text().starts_with("---\ntags:"));
        assert!(note.full_text().ends_with("body\n"));
    }

    #[test]
    fn writes_up_as_links() {
        let mut note = Frontmatter::parse("---\ntitle: Test\n---\n");
        note.add_up("Parent");
        assert_eq!(note.values("up"), vec!["Parent"]);
        assert!(note.raw().contains("  - \"[[Parent]]\""));
    }

    #[test]
    fn recognizes_property_kinds() {
        let note = Frontmatter::parse(
            "---\ntags: [a, b]\nup: \"[[Parent]]\"\ncount: 3\nok: true\nday: 2026-11-11\nfree: text\n---\n",
        );
        let rows = note.properties();
        let kind = |key: &str| rows.iter().find(|row| row.key == key).unwrap().kind;
        assert_eq!(kind("tags"), PropertyKind::Tags);
        assert_eq!(kind("up"), PropertyKind::Links);
        assert_eq!(kind("count"), PropertyKind::Number);
        assert_eq!(kind("ok"), PropertyKind::Checkbox);
        assert_eq!(kind("day"), PropertyKind::Date);
        assert_eq!(kind("free"), PropertyKind::Text);
    }

    #[test]
    fn keeps_crlf_line_endings() {
        let mut note = Frontmatter::parse("---\r\ntags:\r\n  - one\r\n---\r\nbody\r\n");
        note.add_tag("two");
        assert!(note.raw().contains("\r\n"));
        assert_eq!(note.body, "body\r\n");
    }
}
