use tauri::{AppHandle, State, Emitter, Manager};
use tauri_plugin_shell::process::CommandChild;
use tauri_plugin_shell::ShellExt;
use std::sync::{Arc, Mutex};
use std::fs;
use std::path::PathBuf;

#[derive(Default)]
struct BackendState {
    child: Arc<Mutex<Option<CommandChild>>>,
}

fn get_miniclaw_path() -> PathBuf {
    #[cfg(windows)]
    {
        if let Ok(profile) = std::env::var("USERPROFILE") {
            return PathBuf::from(profile).join(".miniclaw");
        }
    }
    if let Ok(home) = std::env::var("HOME") {
        return PathBuf::from(home).join(".miniclaw");
    }
    PathBuf::from(".miniclaw")
}

#[tauri::command]
async fn start_backend(app: AppHandle, state: State<'_, BackendState>) -> Result<(), String> {
    let mut child_guard = state.child.lock().unwrap();
    if child_guard.is_some() {
        return Err("Backend already running".into());
    }

    println!("Current Working Directory: {:?}", std::env::current_dir().unwrap_or_default());

    let miniclaw_path = get_miniclaw_path();
    
    // Ensure the system local folder exists
    fs::create_dir_all(&miniclaw_path).map_err(|e| e.to_string())?;

    // Use sidecar API. Tauri automatically handles the target triple postfix.
    let resource_dir = app.path().resource_dir().map_err(|e| e.to_string())?;
    let cmd = app.shell().sidecar("miniclaw")
        .map_err(|e| format!("Failed to create sidecar command: {}", e))?
        .env("RESOURCES_DIR", resource_dir.to_string_lossy().to_string());
    
    println!("Starting sidecar: miniclaw");
    println!("Resource directory: {:?}", resource_dir);
    println!("Workspace directory: {:?}", miniclaw_path);

    let (mut rx, child) = cmd.spawn()
        .map_err(|e| format!("Failed to spawn sidecar backend: {}", e))?;

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
fn get_config_path() -> PathBuf {
    get_miniclaw_path().join("config.yaml")
}

#[tauri::command]
async fn read_config() -> Result<String, String> {
    let path = get_config_path();
    println!("Reading config from: {:?}", path);
    fs::read_to_string(path).map_err(|e| e.to_string())
}

#[tauri::command]
async fn save_config(content: String) -> Result<(), String> {
    let path = get_config_path();
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
    let app = tauri::Builder::default()
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
        .build(tauri::generate_context!())
        .expect("error while building tauri application");

    app.run(|handle, event| {
        if let tauri::RunEvent::Exit = event {
            let state = handle.state::<BackendState>();
            let mut child_guard = state.child.lock().unwrap();
            if let Some(child) = child_guard.take() {
                println!("[SYSTEM] Clean up sidecar on exit...");
                let _ = child.kill();
            }
        }
    });
}
