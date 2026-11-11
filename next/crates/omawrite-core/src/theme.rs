use std::fs;
use std::path::PathBuf;

use serde::Serialize;

/// The colours the shell paints with.
///
/// Omarchy keeps one `colors.toml` per theme, and the current theme is a
/// symlink. Reading that file means Omawrite next follows the desktop theme
/// instead of shipping a second palette.
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Theme {
    pub mode: String,
    pub background: String,
    pub foreground: String,
    pub accent: String,
    pub selection: String,
    pub selection_foreground: String,
    pub code_background: String,
    pub muted: String,
    pub border: String,
    pub panel: String,
}

impl Theme {
    pub fn load() -> Self {
        let path = theme_directory().join("colors.toml");
        let Ok(text) = fs::read_to_string(path) else {
            return Self::fallback(true);
        };

        let mut values = std::collections::HashMap::new();
        for line in text.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let Some((key, value)) = line.split_once('=') else {
                continue;
            };
            let key = key.trim();
            let value = value.trim();
            let value = value
                .strip_prefix('"')
                .and_then(|value| value.strip_suffix('"'))
                .or_else(|| {
                    value
                        .strip_prefix('\'')
                        .and_then(|value| value.strip_suffix('\''))
                })
                .unwrap_or(value);
            values.insert(key.to_string(), value.to_string());
        }

        let background = values
            .get("background")
            .cloned()
            .unwrap_or_else(|| "#101010".to_string());
        let foreground = values
            .get("foreground")
            .cloned()
            .unwrap_or_else(|| "#eeeeee".to_string());
        let dark = match values.get("mode").map(String::as_str) {
            Some("dark") => true,
            Some("light") => false,
            _ => luminance(&background)
                .map(|value| value < 0.5)
                .unwrap_or(true),
        };
        let fallback = Self::fallback(dark);
        let accent = values.get("accent").cloned().unwrap_or(fallback.accent);
        let selection = values
            .get("selection_background")
            .or_else(|| values.get("selection"))
            .cloned()
            .unwrap_or(fallback.selection);
        let selection_foreground = values
            .get("selection_foreground")
            .or_else(|| values.get("bright_foreground"))
            .cloned()
            .unwrap_or_else(|| foreground.clone());
        let code_background = values
            .get("lighter_background")
            .filter(|value| *value != &background)
            .cloned()
            .unwrap_or_else(|| {
                blend(&background, &foreground, 0.06).unwrap_or(fallback.code_background)
            });
        let muted = values
            .get("muted")
            .cloned()
            .unwrap_or_else(|| blend(&foreground, &background, 0.35).unwrap_or(fallback.muted));
        let border = blend(&background, &foreground, 0.14).unwrap_or(fallback.border);
        let panel = values
            .get("lighter_background")
            .cloned()
            .unwrap_or_else(|| blend(&background, &foreground, 0.04).unwrap_or(fallback.panel));

        Self {
            mode: if dark { "dark" } else { "light" }.to_string(),
            background,
            foreground,
            accent,
            selection,
            selection_foreground,
            code_background,
            muted,
            border,
            panel,
        }
    }

    pub fn fallback(dark: bool) -> Self {
        if dark {
            Self {
                mode: "dark".to_string(),
                background: "#101010".to_string(),
                foreground: "#eeeeee".to_string(),
                accent: "#5584aa".to_string(),
                selection: "#186a9a".to_string(),
                selection_foreground: "#eeeeee".to_string(),
                code_background: "#1b1b1b".to_string(),
                muted: "#909191".to_string(),
                border: "#2e2e2e".to_string(),
                panel: "#181818".to_string(),
            }
        } else {
            Self {
                mode: "light".to_string(),
                background: "#ffffff".to_string(),
                foreground: "#222324".to_string(),
                accent: "#2077b2".to_string(),
                selection: "#2077b2".to_string(),
                selection_foreground: "#ffffff".to_string(),
                code_background: "#f1f1ef".to_string(),
                muted: "#aeb1b5".to_string(),
                border: "#d9d8c8".to_string(),
                panel: "#f7f7f4".to_string(),
            }
        }
    }

    pub fn is_dark(&self) -> bool {
        self.mode == "dark"
    }
}

fn theme_directory() -> PathBuf {
    if let Some(home) = std::env::var_os("HOME") {
        return PathBuf::from(home).join(".local/state/omarchy/current/theme");
    }
    PathBuf::from(".omarchy/current/theme")
}

fn luminance(hex: &str) -> Option<f64> {
    let (red, green, blue) = parse_hex(hex)?;
    Some(0.299 * red + 0.587 * green + 0.114 * blue)
}

fn blend(a: &str, b: &str, amount: f64) -> Option<String> {
    let (ar, ag, ab) = parse_hex(a)?;
    let (br, bg, bb) = parse_hex(b)?;
    let mix = |a: f64, b: f64| (a + (b - a) * amount).clamp(0.0, 1.0);
    Some(format!(
        "#{:02x}{:02x}{:02x}",
        (mix(ar, br) * 255.0).round() as u8,
        (mix(ag, bg) * 255.0).round() as u8,
        (mix(ab, bb) * 255.0).round() as u8
    ))
}

fn parse_hex(hex: &str) -> Option<(f64, f64, f64)> {
    let hex = hex.trim().trim_start_matches('#');
    if hex.len() != 6 && hex.len() != 8 {
        return None;
    }
    let red = u8::from_str_radix(&hex[0..2], 16).ok()? as f64 / 255.0;
    let green = u8::from_str_radix(&hex[2..4], 16).ok()? as f64 / 255.0;
    let blue = u8::from_str_radix(&hex[4..6], 16).ok()? as f64 / 255.0;
    Some((red, green, blue))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_hex_and_blends() {
        assert!(parse_hex("#282828").is_some());
        assert!(blend("#000000", "#ffffff", 0.5).unwrap().starts_with('#'));
    }

    #[test]
    fn fallback_has_both_modes() {
        assert!(Theme::fallback(true).is_dark());
        assert!(!Theme::fallback(false).is_dark());
    }
}
