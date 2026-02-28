use tauri::{AppHandle, State, Emitter};
use tauri_plugin_shell::process::CommandChild;
use tauri_plugin_shell::ShellExt;
use std::sync::{Arc, Mutex};
use std::fs;
use std::path::PathBuf;

#[derive(Default)]
struct BackendState {
    child: Arc<Mutex<Option<CommandChild>>>,
}

#[tauri::command]
async fn start_backend(app: AppHandle, state: State<'_, BackendState>) -> Result<(), String> {
    let mut child_guard = state.child.lock().unwrap();
    if child_guard.is_some() {
        return Err("Backend already running".into());
    }

    println!("Current Working Directory: {:?}", std::env::current_dir().unwrap_or_default());

    let app_handle = app.clone();
    let app_data_dir = app_handle.path().app_data_dir().map_err(|e| e.to_string())?;
    
    // In dev, we might still want to use the local folder if requested, 
    // but the backend now bootstraps itself in AppData by default.
    // We will force WORKSPACE_DIR to AppData for production readiness.
    fs::create_dir_all(&app_data_dir).map_err(|e| e.to_string())?;

    let working_dir = if cfg!(debug_assertions) {
        PathBuf::from("../miniclaw")
    } else {
        app_data_dir.clone()
    };

    let binary_name = if cfg!(windows) { "miniclaw.exe" } else { "miniclaw" };
    // Assuming binary is in the 'bin' folder relative to current exe in dev, 
    // or bundled as a sidecar in production.
    let binary_path = if cfg!(debug_assertions) {
        PathBuf::from("../miniclaw/bin").join(binary_name)
    } else {
        // In production, we'd use sidecar, but the user is currently using a custom bin path
        PathBuf::from("../miniclaw/bin").join(binary_name) 
    };

    println!("Starting backend from: {:?}", binary_path);
    println!("Workspace directory (AppData): {:?}", app_data_dir);

    // Use command instead of sidecar to run from the specific bin folder
    let cmd = app.shell().command(binary_path.to_string_lossy().to_string())
        .env("WORKSPACE_DIR", app_data_dir.to_string_lossy().to_string());
    
    let (mut rx, child) = cmd.spawn()
        .map_err(|e| format!("Failed to spawn backend: {}. Path: {:?}", e, binary_path))?;

    // Listen to stdout/stderr and emit to frontend
    let app_handle = app.clone();
    tauri::async_runtime::spawn(async move {
        use tauri_plugin_shell::process::CommandEvent;
        while let Some(event) = rx.recv().await {
            match event {
                CommandEvent::Stdout(line) => {
                    let log = String::from_utf8_lossy(&line).to_string();
                    println!("[BACKEND] {}", log.trim());
                    let _ = app_handle.emit("backend-log", log);
                }
                CommandEvent::Stderr(line) => {
                    let log = String::from_utf8_lossy(&line).to_string();
                    println!("[BACKEND ERROR] {}", log.trim());
                    let _ = app_handle.emit("backend-log", format!("[ERROR] {}", log));
                }
                CommandEvent::Terminated(payload) => {
                    println!("[SYSTEM] Process terminated with code {:?}. Payload: {:?}", payload.code, payload.signal);
                    let _ = app_handle.emit("backend-log", format!("[SYSTEM] Process terminated with code {:?}", payload.code));
                    break;
                }
                _ => {}
            }
        }
    });

    *child_guard = Some(child);
    Ok(())
}

#[tauri::command]
async fn stop_backend(state: State<'_, BackendState>) -> Result<(), String> {
    let child = {
        let mut child_guard = state.child.lock().unwrap();
        child_guard.take()
    };

    if let Some(child) = child {
        // Try graceful shutdown via API
        let client = reqwest::Client::new();
        let _ = client.post("http://localhost:9000/api/shutdown")
            .timeout(std::time::Duration::from_secs(2))
            .send()
            .await;

        // Give it a moment to shutdown gracefully
        tokio::time::sleep(std::time::Duration::from_millis(500)).await;

        // Fallback: kill the process if it's still there
        let _ = child.kill();
    }
    Ok(())
}

#[tauri::command]
fn get_config_path(app: AppHandle) -> PathBuf {
    let app_data_dir = app.path().app_data_dir().unwrap_or_else(|_| PathBuf::from("."));
    app_data_dir.join("config").join("config.yaml")
}

#[tauri::command]
async fn read_config(app: AppHandle) -> Result<String, String> {
    let path = get_config_path(app);
    println!("Reading config from: {:?}", path);
    fs::read_to_string(path).map_err(|e| e.to_string())
}

#[tauri::command]
async fn save_config(app: AppHandle, content: String) -> Result<(), String> {
    let path = get_config_path(app);
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    fs::write(path, content).map_err(|e| e.to_string())
}

#[tauri::command]
fn get_backend_status(state: State<'_, BackendState>) -> bool {
    state.child.lock().unwrap().is_some()
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .manage(BackendState::default())
        .plugin(tauri_plugin_shell::init())
        .plugin(tauri_plugin_fs::init())
        .plugin(tauri_plugin_opener::init())
        .invoke_handler(tauri::generate_handler![
            start_backend,
            stop_backend,
            get_backend_status,
            read_config,
            save_config
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
