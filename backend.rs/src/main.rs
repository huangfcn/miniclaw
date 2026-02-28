mod agent;
mod tools;
mod memory;

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
use std::path::Path;
use tower_http::cors::CorsLayer;

use crate::agent::{Agent, AgentEvent};

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
    let workspace = std::env::var("WORKSPACE_DIR").unwrap_or_else(|_| ".".to_string());
    let workspace_path = Path::new(&workspace).canonicalize().unwrap_or_else(|_| workspace.clone().into());
    
    // Configure logging
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
    tracing::info!("Workspace: {}", workspace_path.display());

    let agent = Arc::new(Agent::new());
    let subagent_mgr = Arc::new(crate::agent::SubagentManager::new(Arc::clone(&agent)));
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
    axum::serve(listener, app).await.unwrap();
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
