use std::sync::Mutex;

use notify::RecommendedWatcher;
use omawrite_core::{NoteIndex, Settings, Vault};

pub struct AppState {
    pub inner: Mutex<Inner>,
}

pub struct Inner {
    pub vault: Option<Vault>,
    pub index: Option<NoteIndex>,
    pub settings: Settings,
    pub watcher: Option<RecommendedWatcher>,
}

impl AppState {
    pub fn new(settings: Settings) -> Self {
        Self {
            inner: Mutex::new(Inner {
                vault: None,
                index: None,
                settings,
                watcher: None,
            }),
        }
    }
}
