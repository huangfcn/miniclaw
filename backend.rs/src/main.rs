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
    tracing_subscriber::registry()
        .with(tracing_subscriber::EnvFilter::new(
            std::env::var("RUST_LOG").unwrap_or_else(|_| "info".into()),
        ))
        .with(tracing_subscriber::fmt::layer())
        .init();

    let state = Arc::new(AppState {
        agent: Arc::new(Agent::new()),
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
        if let Err(e) = agent.run(message, session_id, tx).await {
            tracing::error!("Agent run error: {}", e);
        }
    });

    let stream = ReceiverStream::new(rx).map(|ev| {
        Event::default().json_data(ev)
    });

    Sse::new(stream)
}
