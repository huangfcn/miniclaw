use tokio::sync::mpsc;
use std::sync::Arc;
use crate::agent::{Agent, AgentEvent};
use chrono::Utc;

pub struct SubagentManager {
    agent: Arc<Agent>,
}

impl SubagentManager {
    pub fn new(agent: Arc<Agent>) -> Self {
        Self { agent }
    }

    pub fn spawn(&self, task: String, label: Option<String>, session_id: String) -> String {
        let task_id = format!("sub_{}", Utc::now().timestamp_millis() % 1000000);
        let display_label = label.unwrap_or_else(|| {
            if task.len() > 30 { format!("{}...", &task[..30]) } else { task.clone() }
        });

        let agent = Arc::clone(&self.agent);
        let sub_task_id = task_id.clone();
        let sub_display_label = display_label.clone();
        let sub_session_id = session_id.clone();

        tokio::spawn(async move {
            let (tx, mut rx) = mpsc::channel(100);
            
            // In a real subagent we might want to log its progress to the same session or a sub-session
            // For now, we'll just run it and announce completion.
            if let Err(e) = agent.run(task.clone(), format!("subagent:{}", sub_task_id), tx).await {
                eprintln!("Subagent [{}] error: {}", sub_task_id, e);
            }

            // After completion, announce back to the parent session
            let mut final_result = String::new();
            while let Some(ev) = rx.recv().await {
                if ev.r#type == "token" {
                    final_result.push_str(&ev.content);
                }
            }

            if final_result.is_empty() {
                final_result = "Task completed but no final response was generated.".to_string();
            }

            let announcement = format!(
                "[Subagent '{}' completed]\n\nTask: {}\n\nResult:\n{}",
                sub_display_label, task, final_result
            );

            // Append to original session's history
            agent.sessions.get_or_create(&sub_session_id).await.add_message("system", &announcement);
            // In a more complex system, we'd trigger a push or notification here.
        });

        format!("Subagent [{}] started (id: {}). I'll notify you when it completes.", display_label, task_id)
    }
}
