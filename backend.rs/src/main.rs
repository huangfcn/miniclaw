// Modules are now in lib.rs

use miniclaw_rust::config::Config;

use axum::{
    extract::State,
    response::{sse::{Event, Sse}, IntoResponse},
    routing::{get, post},
    Json, Router,
};
use serde::Deserialize;
use std::sync::Arc;
use tokio::sync::mpsc;
use tokio_stream::wrappers::ReceiverStream;
use tokio_stream::StreamExt;
use tracing_subscriber::prelude::*;
use tracing_appender::non_blocking;
use std::path::{Path, PathBuf};
use directories::ProjectDirs;
use tower_http::cors::CorsLayer;

use miniclaw_rust::agent::{Agent, AgentEvent};

#[derive(Deserialize)]
struct ChatRequest {
    message: String,
    session_id: String,
}

struct AppState {
    agent: Arc<Agent>,
}

#[tokio::main]
async fn main() {
    let config = Config::load();
    let workspace_path = PathBuf::from(config.memory_workspace());
    
    // Configure logging early
    let file_appender = tracing_appender::rolling::never(&workspace_path, "backend.log");
    let (non_blocking, _guard) = non_blocking(file_appender);

    tracing_subscriber::registry()
        .with(tracing_subscriber::EnvFilter::new(
            std::env::var("RUST_LOG").unwrap_or_else(|_| "info".into()),
        ))
        .with(tracing_subscriber::fmt::layer()) // Console
        .with(tracing_subscriber::fmt::layer().with_writer(non_blocking)) // File
        .init();

    tracing::info!("Starting miniclaw Backend (Rust)");
    tracing::info!("Config file: {}", config.actual_config_path);
    tracing::info!("Memory workspace: {}", workspace_path.display());
    tracing::info!("Skills path: {}", config.skills_path());
    tracing::info!("Prompts path: {}", workspace_path.join("prompts").display());

    // Log bootstrap files
    for f in &["AGENTS.md", "SOUL.md", "USER.md", "TOOLS.md", "IDENTITY.md"] {
        let p = workspace_path.join(f);
        if p.exists() {
            tracing::info!("Using bootstrap file: {}", p.display());
        } else {
            tracing::warn!("Bootstrap file not found: {}", p.display());
        }
    }

    bootstrap_workspace(&workspace_path);
    
    let agent = Arc::new(Agent::new(workspace_path.to_string_lossy().to_string(), config.clone()));
    let subagent_mgr = Arc::new(miniclaw_rust::agent::SubagentManager::new(Arc::clone(&agent)));
    agent.set_subagents(subagent_mgr);

    let state = Arc::new(AppState {
        agent,
    });

    let app = Router::new()
        .route("/api/health", get(|| async { "OK" }))
        .route("/api/chat", post(chat_handler))
        .layer(CorsLayer::permissive())
        .with_state(state);

    let addr = std::net::SocketAddr::from(([0, 0, 0, 0], 8081));
    tracing::info!("Starting miniclaw Rust Backend on {}", addr);
    
    let listener = tokio::net::TcpListener::bind(addr).await.unwrap();
    axum::serve(listener, app)
        .with_graceful_shutdown(async move {
            let mut last_ctrl_c = None;
            loop {
                tokio::signal::ctrl_c().await.expect("failed to listen for event");
                let now = tokio::time::Instant::now();
                if let Some(last) = last_ctrl_c {
                    if now.duration_since(last).as_secs_f32() <= 1.0 {
                        tracing::info!("\nReceived second Ctrl-C within 1s, initiating graceful shutdown...");
                        break;
                    }
                }
                last_ctrl_c = Some(now);
                tracing::info!("\nPress Ctrl-C again within 1s to gracefully exit.");
            }
        })
        .await
        .unwrap();
}

// Removed resolve_workspace_path as it's now handled by Config

fn bootstrap_workspace(path: &Path) {
    let _ = std::fs::create_dir_all(path);
    let _ = std::fs::create_dir_all(path.join("memory"));
    let _ = std::fs::create_dir_all(path.join("sessions"));
    let _ = std::fs::create_dir_all(path.join("skills"));
}

async fn chat_handler(
    State(state): State<Arc<AppState>>,
    Json(payload): Json<ChatRequest>,
) -> impl IntoResponse {
    let (tx, rx) = mpsc::channel::<AgentEvent>(100);
    
    let agent = Arc::clone(&state.agent);
    let message = payload.message;
    let session_id = payload.session_id;

    tokio::spawn(async move {
        if let Err(e) = agent.run(message, session_id, tx, None, None).await {
            tracing::error!("Agent run error: {}", e);
        }
    });

    let stream = ReceiverStream::new(rx).map(|ev| {
        Event::default().json_data(ev)
    });

    Sse::new(stream)
}
