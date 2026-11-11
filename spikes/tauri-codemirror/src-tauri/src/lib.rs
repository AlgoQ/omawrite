use std::path::PathBuf;

/// Return the absolute vault root the shell should index.
///
/// The spike reads `OMAWRITE_VAULT_ROOT`, falling back to the process working
/// directory. The path is canonicalized so the frontend never receives `..`
/// segments or a relative path. A production host would resolve this from the
/// open vault and keep the canonical root in Rust state instead of trusting a
/// frontend argument.
#[tauri::command]
fn vault_root() -> Result<String, String> {
    let candidate = std::env::var_os("OMAWRITE_VAULT_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|| std::env::current_dir().unwrap_or_else(|_| PathBuf::from(".")));

    let root = std::fs::canonicalize(&candidate)
        .map_err(|error| format!("cannot resolve vault root {}: {error}", candidate.display()))?;

    if !root.is_dir() {
        return Err(format!("vault root is not a directory: {}", root.display()));
    }

    #[cfg(debug_assertions)]
    eprintln!("vault_root command resolved {}", root.display());

    Ok(root.to_string_lossy().into_owned())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![vault_root])
        .run(tauri::generate_context!())
        .expect("error while running the Omawrite spike");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vault_root_resolves_to_an_absolute_directory() {
        let directory = std::env::temp_dir().join("omawrite-spike-vault-root-test");
        std::fs::create_dir_all(&directory).expect("create test vault");
        std::env::set_var("OMAWRITE_VAULT_ROOT", &directory);

        let root = vault_root().expect("vault_root should resolve");
        let canonical = std::fs::canonicalize(&directory).expect("canonical test vault");

        assert_eq!(root, canonical.to_string_lossy());
        assert!(std::path::Path::new(&root).is_absolute());

        std::env::remove_var("OMAWRITE_VAULT_ROOT");
    }
}
