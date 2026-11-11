use std::collections::HashMap;
use std::fs;
use std::path::PathBuf;

use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase")]
pub struct Settings {
    pub scroll_speed: f64,
    pub font_size: f64,
    /// `auto`, `dark` or `light`.
    pub theme: String,
    pub sidebar_open: bool,
    pub backlinks_open: bool,
    /// Action id to key sequence. Missing actions keep their default.
    pub keymap: HashMap<String, String>,
    pub last_vault: Option<String>,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            scroll_speed: 2.0,
            font_size: 20.0,
            theme: "auto".to_string(),
            sidebar_open: true,
            backlinks_open: false,
            keymap: HashMap::new(),
            last_vault: None,
        }
    }
}

impl Settings {
    pub fn path() -> PathBuf {
        config_directory().join("settings.json")
    }

    pub fn load() -> Self {
        let path = Self::path();
        let Ok(text) = fs::read_to_string(path) else {
            return Self::default();
        };
        serde_json::from_str(&text).unwrap_or_default()
    }

    pub fn save(&self) -> Result<(), String> {
        let path = Self::path();
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)
                .map_err(|error| format!("Cannot create {}: {error}", parent.display()))?;
        }
        let text = serde_json::to_string_pretty(self)
            .map_err(|error| format!("Cannot serialize settings: {error}"))?;
        fs::write(&path, text).map_err(|error| format!("Cannot write {}: {error}", path.display()))
    }
}

fn config_directory() -> PathBuf {
    if let Some(path) = std::env::var_os("XDG_CONFIG_HOME") {
        return PathBuf::from(path).join("omawrite-next");
    }
    if let Some(home) = std::env::var_os("HOME") {
        return PathBuf::from(home).join(".config/omawrite-next");
    }
    PathBuf::from(".omawrite-next")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_to_double_scroll_speed() {
        assert_eq!(Settings::default().scroll_speed, 2.0);
        assert_eq!(Settings::default().theme, "auto");
    }
}
