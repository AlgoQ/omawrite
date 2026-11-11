mod commands;
mod state;

use omawrite_core::Settings;
use state::AppState;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let settings = Settings::load();
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .manage(AppState::new(settings))
        .invoke_handler(tauri::generate_handler![
            commands::open_vault,
            commands::vault_info,
            commands::tree,
            commands::refresh_index,
            commands::read_note,
            commands::save_note,
            commands::create_note,
            commands::move_note,
            commands::suggest,
            commands::backlinks,
            commands::search,
            commands::resolve_note,
            commands::list_folder,
            commands::read_attachment,
            commands::resolve_attachment,
            commands::edit_frontmatter,
            commands::paste_image,
            commands::settings_get,
            commands::settings_set,
            commands::theme,
            commands::initial_note,
        ])
        .run(tauri::generate_context!())
        .expect("error while running Omawrite");
}
