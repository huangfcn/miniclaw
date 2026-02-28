use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::fs::{self, OpenOptions};
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use tokio::sync::Mutex;
use chrono::Utc;
use std::collections::HashMap;

use crate::agent::Message;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Session {
    #[serde(skip)]
    pub key: String,
    pub messages: Vec<Message>,
    pub created_at: String,
    pub updated_at: String,
    pub metadata: Value,
    pub last_consolidated: usize,
    pub last_consolidation_date: String,
    pub last_distilled_token_count: usize,
}

impl Default for Session {
    fn default() -> Self {
        Self {
            key: String::new(),
            messages: Vec::new(),
            created_at: String::new(),
            updated_at: String::new(),
            metadata: serde_json::json!({}),
            last_consolidated: 0,
            last_consolidation_date: String::new(),
            last_distilled_token_count: 0,
        }
    }
}

impl Session {
    pub fn add_message(&mut self, role: &str, content: &str) {
        self.messages.push(Message {
            role: role.to_string(),
            content: Some(content.to_string()),
            tool_calls: None,
            tool_call_id: None,
            name: None,
        });
        self.updated_at = Utc::now().to_rfc3339();
    }

    pub fn estimate_tokens(&self) -> usize {
        let mut tokens = 0;
        for msg in &self.messages {
            let content_len = msg.content.as_ref().map(|c| c.len()).unwrap_or(0);
            tokens += (msg.role.len() + content_len) / 4 + 1;
            
            if let Some(ref tc) = msg.tool_calls {
                tokens += tc.to_string().len() / 4;
            }
        }
        tokens
    }
}

#[derive(Clone)]
pub struct SessionManager {
    sessions_dir: PathBuf,
    cache: Arc<Mutex<HashMap<String, Session>>>,
}

impl SessionManager {
    pub fn new(workspace: &str) -> Self {
        let dir = Path::new(workspace).join("sessions");
        fs::create_dir_all(&dir).unwrap_or_default();
        Self {
            sessions_dir: dir,
            cache: Arc::new(Mutex::new(HashMap::new())),
        }
    }

    fn get_session_path(&self, key: &str) -> PathBuf {
        let safe_key = key.replace(":", "_").replace("/", "_");
        self.sessions_dir.join(format!("{}.jsonl", safe_key))
    }

    pub async fn get_or_create(&self, key: &str) -> Session {
        let mut cache = self.cache.lock().await;
        if let Some(sess) = cache.get(key) {
            return sess.clone();
        }

        let mut session = Session {
            key: key.to_string(),
            ..Default::default()
        };

        let path = self.get_session_path(key);
        if !path.exists() {
            session.created_at = Utc::now().to_rfc3339();
            session.updated_at = session.created_at.clone();
        } else {
            if let Ok(file) = fs::File::open(&path) {
                let reader = BufReader::new(file);
                for line in reader.lines().flatten() {
                    if line.is_empty() { continue; }
                    if let Ok(data) = serde_json::from_str::<Value>(&line) {
                        if data.get("_type").and_then(|v| v.as_str()) == Some("metadata") {
                            if let Some(meta) = data.get("metadata") { session.metadata = meta.clone(); }
                            if let Some(created) = data.get("created_at").and_then(|v| v.as_str()) {
                                session.created_at = created.to_string();
                            }
                            if let Some(updated) = data.get("updated_at").and_then(|v| v.as_str()) {
                                session.updated_at = updated.to_string();
                            }
                            if let Some(lc) = data.get("last_consolidated").and_then(|v| v.as_u64()) {
                                session.last_consolidated = lc as usize;
                            }
                            if let Some(lcd) = data.get("last_consolidation_date").and_then(|v| v.as_str()) {
                                session.last_consolidation_date = lcd.to_string();
                            }
                            if let Some(ltc) = data.get("last_distilled_token_count").and_then(|v| v.as_u64()) {
                                session.last_distilled_token_count = ltc as usize;
                            }
                        } else {
                            if let Ok(msg) = serde_json::from_value::<Message>(data.clone()) {
                                session.messages.push(msg);
                            }
                        }
                    }
                }
            }
        }

        cache.insert(key.to_string(), session.clone());
        session
    }

    pub async fn save(&self, session: &Session) {
        let mut cache = self.cache.lock().await;
        cache.insert(session.key.clone(), session.clone());

        let path = self.get_session_path(&session.key);
        if let Ok(mut file) = OpenOptions::new().write(true).create(true).truncate(true).open(&path) {
            let meta_obj = serde_json::json!({
                "_type": "metadata",
                "created_at": session.created_at,
                "updated_at": session.updated_at,
                "metadata": session.metadata,
                "last_consolidated": session.last_consolidated,
                "last_consolidation_date": session.last_consolidation_date,
                "last_distilled_token_count": session.last_distilled_token_count,
            });
            let _ = writeln!(file, "{}", serde_json::to_string(&meta_obj).unwrap());

            for msg in &session.messages {
                if let Ok(msg_str) = serde_json::to_string(msg) {
                    let _ = writeln!(file, "{}", msg_str);
                }
            }
        }
    }
}
