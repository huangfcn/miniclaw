//! Mobile embedded engine (Android / iOS)
//!
//! On desktop, miniclaw runs as a sidecar process and the frontend talks to
//! it over HTTP on localhost:9000. Tauri mobile cannot spawn sidecars, so on
//! Android/iOS we link `libminiclaw_core` (the C++ engine behind a C ABI, see
//! backend/src/mobile/agent_api.h) directly into the app and drive it from
//! Rust.
//!
//! Events flow back to the webview as Tauri events named "agent-event":
//!   { "type": "status" | "token" | "tool_start" | "tool_end" | "error" | "done",
//!     "content": "..." }

// On desktop builds this module is compiled but unused (dead_code); on
// Android/iOS every item is reachable through the Tauri command handlers.
#![allow(dead_code)]

use std::ffi::{c_char, c_void, CStr, CString};
use std::sync::{Arc, Mutex};

use serde_json::json;
use tauri::{AppHandle, Emitter, Manager, State};

type McEnginePtr = *mut c_void;

/// Wrapper giving a raw FFI pointer Send + Sync. Sound here: the pointer is
/// written once during init and never mutated afterwards; the C++ engine
/// manages its own internal synchronization.
#[derive(Clone, Copy)]
struct RawPtr(McEnginePtr);
unsafe impl Send for RawPtr {}
unsafe impl Sync for RawPtr {}

// ── FFI (mirrors backend/src/mobile/agent_api.h) ────────────────────────────

#[link(name = "miniclaw_core")]
extern "C" {
    fn mc_engine_create(
        workspace: *const c_char,
        config_path: *const c_char,
    ) -> McEnginePtr;
    fn mc_engine_destroy(engine: McEnginePtr);
    fn mc_send_message(
        engine: McEnginePtr,
        session_id: *const c_char,
        message: *const c_char,
        cb: Option<
            unsafe extern "C" fn(
                user_data: *mut c_void,
                event_type: *const c_char,
                content: *const c_char,
            ),
        >,
        user_data: *mut c_void,
    );
    fn mc_get_config(engine: McEnginePtr) -> *mut c_char;
    fn mc_set_config(engine: McEnginePtr, yaml: *const c_char) -> i32;
    fn mc_set_string(
        engine: McEnginePtr,
        section: *const c_char,
        key: *const c_char,
        value: *const c_char,
    ) -> i32;
    fn mc_is_running(engine: McEnginePtr) -> i32;
    fn mc_last_error() -> *const c_char;
    fn mc_free_string(s: *mut c_char);
}

// ── Event callback plumbing ─────────────────────────────────────────────────

/// Passed as `user_data` to the C++ engine. Leaked for the process lifetime —
/// there is exactly one engine per app process on mobile.
struct CallbackData {
    app: Arc<AppHandle>,
}

unsafe extern "C" fn on_engine_event(
    user_data: *mut c_void,
    event_type: *const c_char,
    content: *const c_char,
) {
    if user_data.is_null() || event_type.is_null() || content.is_null() {
        return;
    }
    let data = &*(user_data as *const CallbackData);
    let t = CStr::from_ptr(event_type).to_string_lossy().to_string();
    let c = CStr::from_ptr(content).to_string_lossy().to_string();
    // Emitter is available on all platforms in Tauri v2.
    let _ = data.app.emit("agent-event", json!({ "type": t, "content": c }));
}

// ── Managed state ───────────────────────────────────────────────────────────

pub struct MobileBackend {
    engine: Mutex<RawPtr>,
    /// Raw pointer to the leaked CallbackData handed to the C++ engine.
    cb_ptr: Mutex<RawPtr>,
}

impl MobileBackend {
    pub fn new() -> Self {
        Self {
            engine: Mutex::new(RawPtr(std::ptr::null_mut())),
            cb_ptr: Mutex::new(RawPtr(std::ptr::null_mut())),
        }
    }

    /// Create the C++ engine rooted at the app's private data directory.
    pub fn init(&self, app: &AppHandle) -> Result<(), String> {
        let mut guard = self.engine.lock().unwrap();
        if !guard.0.is_null() {
            return Ok(()); // already initialized
        }

        let data_dir = app
            .path()
            .app_data_dir()
            .map_err(|e| format!("cannot resolve app data dir: {}", e))?;
        std::fs::create_dir_all(&data_dir)
            .map_err(|e| format!("cannot create app data dir: {}", e))?;

        let workspace = data_dir.join("workspace");
        std::fs::create_dir_all(&workspace)
            .map_err(|e| format!("cannot create workspace dir: {}", e))?;

        let ws_c = CString::new(workspace.to_string_lossy().as_ref())
            .map_err(|_| "workspace path contains NUL")?;
        let cfg_c = CString::new(data_dir.join("config.yaml").to_string_lossy().as_ref())
            .map_err(|_| "config path contains NUL")?;

        let engine = unsafe { mc_engine_create(ws_c.as_ptr(), cfg_c.as_ptr()) };
        if engine.is_null() {
            return Err(last_error());
        }

        // Keep the AppHandle alive for the callback. Intentionally leaked:
        // one engine per process, torn down at app exit.
        let cb_data = Box::new(CallbackData {
            app: Arc::new(app.clone()),
        });
        *self.cb_ptr.lock().unwrap() = RawPtr(Box::into_raw(cb_data) as *mut c_void);

        *guard = RawPtr(engine);
        Ok(())
    }

    fn engine_or_err(&self) -> Result<McEnginePtr, String> {
        let guard = self.engine.lock().unwrap();
        if guard.0.is_null() {
            Err("engine not initialized".into())
        } else {
            Ok(guard.0)
        }
    }
}

impl Drop for MobileBackend {
    fn drop(&mut self) {
        let engine = self.engine.lock().unwrap();
        if !engine.0.is_null() {
            unsafe { mc_engine_destroy(engine.0) };
        }
    }
}

fn last_error() -> String {
    unsafe {
        let p = mc_last_error();
        if p.is_null() {
            "unknown engine error".into()
        } else {
            CStr::from_ptr(p).to_string_lossy().to_string()
        }
    }
}

// ── Tauri commands (mobile only) ────────────────────────────────────────────

#[tauri::command]
fn mobile_send_message(
    state: State<'_, MobileBackend>,
    session_id: String,
    message: String,
) -> Result<(), String> {
    let engine = state.engine_or_err()?;
    let cb = *state.cb_ptr.lock().unwrap();
    if cb.0.is_null() {
        return Err("engine not initialized".into());
    }
    let sid = CString::new(session_id).map_err(|_| "invalid session id")?;
    let msg = CString::new(message).map_err(|_| "invalid message")?;

    // The C++ side invokes the callback on fiber/worker threads; Tauri's
    // emit is thread-safe. mc_send_message returns immediately — the turn
    // runs in the background and streams "agent-event" events.
    unsafe {
        mc_send_message(
            engine,
            sid.as_ptr(),
            msg.as_ptr(),
            Some(on_engine_event),
            cb.0,
        );
    }
    Ok(())
}

#[tauri::command]
fn mobile_get_config(state: State<'_, MobileBackend>) -> Result<String, String> {
    let engine = state.engine_or_err()?;
    unsafe {
        let p = mc_get_config(engine);
        if p.is_null() {
            return Err("no config available".into());
        }
        let s = CStr::from_ptr(p).to_string_lossy().to_string();
        mc_free_string(p);
        Ok(s)
    }
}

#[tauri::command]
fn mobile_save_config(state: State<'_, MobileBackend>, content: String) -> Result<(), String> {
    let engine = state.engine_or_err()?;
    let yaml = CString::new(content).map_err(|_| "invalid YAML")?;
    let rc = unsafe { mc_set_config(engine, yaml.as_ptr()) };
    if rc == 0 {
        Ok(())
    } else {
        Err(last_error())
    }
}

#[tauri::command]
fn mobile_set_string(
    state: State<'_, MobileBackend>,
    section: String,
    key: String,
    value: String,
) -> Result<(), String> {
    let engine = state.engine_or_err()?;
    let s = CString::new(section).map_err(|_| "invalid section")?;
    let k = CString::new(key).map_err(|_| "invalid key")?;
    let v = CString::new(value).map_err(|_| "invalid value")?;
    let rc = unsafe { mc_set_string(engine, s.as_ptr(), k.as_ptr(), v.as_ptr()) };
    if rc == 0 {
        Ok(())
    } else {
        Err(last_error())
    }
}

#[tauri::command]
fn mobile_status(state: State<'_, MobileBackend>) -> Result<bool, String> {
    let engine = state.engine_or_err()?;
    Ok(unsafe { mc_is_running(engine) == 1 })
}
