use std::ops::Range;

#[derive(Clone, Debug)]
pub struct WikiLink {
    /// Byte range of the whole token, including a leading `!` for an embed.
    pub range: Range<usize>,
    pub target: String,
    pub heading: Option<String>,
    pub alias: Option<String>,
    pub embed: bool,
}

#[derive(Clone, Debug)]
pub struct MarkdownLink {
    pub range: Range<usize>,
    pub url_range: Range<usize>,
    pub url: String,
    pub embed: bool,
    pub line: usize,
}

/// Find `[[wikilinks]]` outside fenced code and inline code spans.
pub fn wikilinks(text: &str) -> Vec<WikiLink> {
    let mut links = Vec::new();
    let mut offset = 0usize;
    let mut fence: Option<(char, usize)> = None;

    for line in text.split_inclusive('\n') {
        let line_start = offset;
        offset += line.len();
        let content = line.trim_end_matches(['\r', '\n']);
        let trimmed = content.trim_start();
        let fence_run = leading_run(trimmed, '`').max(leading_run(trimmed, '~'));
        if fence_run >= 3 {
            let marker = trimmed.chars().next().unwrap();
            match fence {
                Some((open, length)) if open == marker && fence_run >= length => fence = None,
                None => fence = Some((marker, fence_run)),
                _ => {}
            }
            continue;
        }
        if fence.is_some() {
            continue;
        }

        let bytes = content.as_bytes();
        let mut index = 0usize;
        let mut code_run = 0usize;
        while index + 1 < bytes.len() {
            let character = bytes[index] as char;
            if character == '`' {
                let run = leading_run(&content[index..], '`');
                if code_run == 0 {
                    code_run = run;
                } else if code_run == run {
                    code_run = 0;
                }
                index += run;
                continue;
            }
            if code_run > 0 {
                index += 1;
                continue;
            }
            if character != '[' || bytes[index + 1] != b'[' {
                index += 1;
                continue;
            }

            let opener = index;
            let mut close = index + 2;
            while close + 1 < bytes.len() {
                if bytes[close] == b']' && bytes[close + 1] == b']' {
                    break;
                }
                close += 1;
            }
            if close + 1 >= bytes.len() {
                index += 2;
                continue;
            }

            let inner = &content[opener + 2..close];
            if inner.contains('\n') {
                index = close + 2;
                continue;
            }
            let embed = opener > 0 && content.as_bytes()[opener - 1] == b'!';
            let start = if embed {
                line_start + opener - 1
            } else {
                line_start + opener
            };
            let end = line_start + close + 2;
            let (before_alias, alias) = match inner.split_once('|') {
                Some((before, alias)) => (before, Some(alias.trim().to_string())),
                None => (inner, None),
            };
            let (target, heading) = match before_alias.split_once('#') {
                Some((target, heading)) => (target.trim(), Some(heading.trim().to_string())),
                None => (before_alias.trim(), None),
            };
            if !target.is_empty() {
                links.push(WikiLink {
                    range: start..end,
                    target: target.to_string(),
                    heading: heading.filter(|heading| !heading.is_empty()),
                    alias: alias.filter(|alias| !alias.is_empty()),
                    embed,
                });
            }
            index = close + 2;
        }
    }
    links
}

pub fn rewrite_wikilinks(
    text: &str,
    mut rewrite: impl FnMut(&str) -> Option<String>,
) -> (String, usize) {
    let links = wikilinks(text);
    if links.is_empty() {
        return (text.to_string(), 0);
    }
    let mut output = String::with_capacity(text.len());
    let mut cursor = 0usize;
    let mut changed = 0usize;
    for link in links {
        output.push_str(&text[cursor..link.range.start]);
        match rewrite(&link.target) {
            Some(target) => {
                changed += 1;
                if link.embed {
                    output.push('!');
                }
                output.push_str("[[");
                output.push_str(&target);
                if let Some(heading) = &link.heading {
                    output.push('#');
                    output.push_str(heading);
                }
                if let Some(alias) = &link.alias {
                    output.push('|');
                    output.push_str(alias);
                }
                output.push_str("]]");
            }
            None => output.push_str(&text[link.range.clone()]),
        }
        cursor = link.range.end;
    }
    output.push_str(&text[cursor..]);
    (output, changed)
}

/// Find Markdown links and images outside fenced code and inline code. This is
/// used only for path rewrites; rendering is CodeMirror's job.
pub fn markdown_links(text: &str) -> Vec<MarkdownLink> {
    let mut links = Vec::new();
    let mut offset = 0usize;
    let mut fence: Option<(char, usize)> = None;

    for line in text.split_inclusive('\n') {
        let line_start = offset;
        offset += line.len();
        let content = line.trim_end_matches(['\r', '\n']);
        let trimmed = content.trim_start();
        let fence_run = leading_run(trimmed, '`').max(leading_run(trimmed, '~'));
        if fence_run >= 3 {
            let marker = trimmed.chars().next().unwrap();
            match fence {
                Some((open, length)) if open == marker && fence_run >= length => fence = None,
                None => fence = Some((marker, fence_run)),
                _ => {}
            }
            continue;
        }
        if fence.is_some() {
            continue;
        }

        let bytes = content.as_bytes();
        let mut index = 0usize;
        let mut code_run = 0usize;
        while index < bytes.len() {
            let character = bytes[index] as char;
            if character == '`' {
                let run = leading_run(&content[index..], '`');
                if code_run == 0 {
                    code_run = run;
                } else if code_run == run {
                    code_run = 0;
                }
                index += run;
                continue;
            }
            if code_run > 0 || character != '[' {
                index += 1;
                continue;
            }

            let embed = index > 0 && bytes[index - 1] == b'!';
            let Some(close_bracket) = find_closing_bracket(content, index + 1) else {
                index += 1;
                continue;
            };
            if close_bracket + 1 >= bytes.len() || bytes[close_bracket + 1] != b'(' {
                index += 1;
                continue;
            }
            let url_start = close_bracket + 2;
            let Some(close_paren) = content[url_start..].find(')').map(|at| url_start + at) else {
                index += 1;
                continue;
            };
            let raw = &content[url_start..close_paren];
            let url = raw.split_whitespace().next().unwrap_or("").trim();
            if url.is_empty() {
                index = close_paren + 1;
                continue;
            }
            let start = if embed {
                line_start + index - 1
            } else {
                line_start + index
            };
            links.push(MarkdownLink {
                range: start..line_start + close_paren + 1,
                url_range: line_start + url_start..line_start + url_start + url.len(),
                url: url.to_string(),
                embed,
                line: text[..start].bytes().filter(|byte| *byte == b'\n').count() + 1,
            });
            index = close_paren + 1;
        }
    }
    links
}

pub fn rewrite_markdown_links(
    text: &str,
    mut rewrite: impl FnMut(&str, bool) -> Option<String>,
) -> (String, usize) {
    let links = markdown_links(text);
    if links.is_empty() {
        return (text.to_string(), 0);
    }
    let mut output = String::with_capacity(text.len());
    let mut cursor = 0usize;
    let mut changed = 0usize;
    for link in links {
        output.push_str(&text[cursor..link.url_range.start]);
        match rewrite(&link.url, link.embed) {
            Some(url) => {
                changed += 1;
                output.push_str(&url);
            }
            None => output.push_str(&link.url),
        }
        cursor = link.url_range.end;
    }
    output.push_str(&text[cursor..]);
    (output, changed)
}

pub fn line_of(text: &str, offset: usize) -> usize {
    text[..offset.min(text.len())]
        .bytes()
        .filter(|byte| *byte == b'\n')
        .count()
        + 1
}

pub fn snippet_at(text: &str, offset: usize, limit: usize) -> String {
    let line_start = text[..offset.min(text.len())]
        .rfind('\n')
        .map(|index| index + 1)
        .unwrap_or(0);
    let line_end = text[offset.min(text.len())..]
        .find('\n')
        .map(|index| offset + index)
        .unwrap_or(text.len());
    let line = text[line_start..line_end].trim();
    if line.chars().count() <= limit {
        return line.to_string();
    }
    let mut snippet: String = line.chars().take(limit).collect();
    snippet.push('…');
    snippet
}

fn leading_run(text: &str, marker: char) -> usize {
    text.chars()
        .take_while(|character| *character == marker)
        .count()
}

fn find_closing_bracket(text: &str, start: usize) -> Option<usize> {
    let mut depth = 1i32;
    for (offset, character) in text[start..].char_indices() {
        match character {
            '[' => depth += 1,
            ']' => {
                depth -= 1;
                if depth == 0 {
                    return Some(start + offset);
                }
            }
            _ => {}
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_links_aliases_headings_and_embeds() {
        let text = "See [[Note]] and [[Other#Head|shown]] and ![[image.png]].\n";
        let links = wikilinks(text);
        assert_eq!(links.len(), 3);
        assert_eq!(links[0].target, "Note");
        assert_eq!(links[1].target, "Other");
        assert_eq!(links[1].heading.as_deref(), Some("Head"));
        assert_eq!(links[1].alias.as_deref(), Some("shown"));
        assert!(links[2].embed);
        assert_eq!(&text[links[2].range.clone()], "![[image.png]]");
    }

    #[test]
    fn skips_fences_and_inline_code() {
        let text = "```\n[[no]]\n```\n`[[no]]` [[yes]]\n";
        let links = wikilinks(text);
        assert_eq!(links.len(), 1);
        assert_eq!(links[0].target, "yes");
    }

    #[test]
    fn rewrites_targets_and_keeps_headings_and_aliases() {
        let text = "[[Old#Head|shown]] and ![[Old]] and [[Other]]";
        let (rewritten, changed) =
            rewrite_wikilinks(text, |target| (target == "Old").then(|| "New".to_string()));
        assert_eq!(changed, 2);
        assert_eq!(rewritten, "[[New#Head|shown]] and ![[New]] and [[Other]]");
    }

    #[test]
    fn finds_markdown_links_and_images() {
        let text = "[note](notes/One.md) and ![shot](_attachments/a.png)\n";
        let links = markdown_links(text);
        assert_eq!(links.len(), 2);
        assert_eq!(links[0].url, "notes/One.md");
        assert!(!links[0].embed);
        assert!(links[1].embed);
    }

    #[test]
    fn ignores_links_in_code() {
        let text = "`[x](no.md)`\n```\n[y](no.md)\n```\n[z](yes.md)\n";
        let links = markdown_links(text);
        assert_eq!(links.len(), 1);
        assert_eq!(links[0].url, "yes.md");
    }
}
