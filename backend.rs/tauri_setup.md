# Tauri Sidecar Setup Guide

This guide explains how to integrate the Rust backend as a sidecar in a Tauri application.

## 1. Binary Preparation

Tauri expects sidecar binaries in `src-tauri/binaries/`. You must rename the binary to include the target triple.

- **Windows**: `miniclaw-rust-x86_64-pc-windows-msvc.exe`
- **macOS**: `miniclaw-rust-x86_64-apple-darwin` (or `aarch64-...`)
- **Linux**: `miniclaw-rust-x86_64-unknown-linux-gnu`

## 2. tauri.conf.json Configuration

Add the sidecar to your `bundle` section:

```json
"bundle": {
  "externalBin": [
    "binaries/miniclaw-rust"
  ]
}
```

## 3. Frontend Integration (Rust)

In your `src-tauri/src/main.rs`, resolve the standard AppData directory and pass it to the sidecar via the `WORKSPACE_DIR` environment variable.

```rust
use tauri::Manager;
use std::fs;

fn main() {
    tauri::Builder::default()
        .setup(|app| {
            let app_handle = app.handle();
            let app_data_dir = app_handle.path().app_data_dir().expect("failed to get app data dir");
            
            // 1. Create the directory if it doesn't exist
            fs::create_dir_all(&app_data_dir)?;

            // 2. Start the sidecar
            let (mut rx, mut _child) = tauri::process::Command::new_sidecar("miniclaw-rust")
                .expect("failed to setup sidecar")
                .env("WORKSPACE_DIR", app_data_dir.to_string_lossy())
                .spawn()
                .expect("failed to spawn sidecar");

            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
```

## 4. Why this works?

- **Persistence**: By using `app_data_dir()`, the backend stores memory, sessions, and logs in the official OS location (e.g., `%LOCALAPPDATA%` on Windows).
- **Permissions**: Tauri's installation directory is often read-only. This setup ensures all writes happen in a user-writable location.
- **Portability**: The `directories` crate in the backend automatically falls back to these same standard paths if `WORKSPACE_DIR` is not provided, making it safe to run manually.
