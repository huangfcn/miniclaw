use serde::{Deserialize, Serialize};
use std::sync::Arc;
use tokio::sync::mpsc;
use anyhow::{Result, anyhow};
use crate::config::Config;
use crate::tools::{Tool, TerminalTool, ReadFileTool, WriteFileTool, WebSearchTool, WebFetchTool, ListDirTool, EditFileTool, GmailTool};
use crate::memory::{SessionManager, MemoryStore};
use chrono::Utc;

pub mod skills;
pub mod subagent;

pub use skills::SkillsLoader;
pub use subagent::SubagentManager;
use std::sync::Mutex;

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct Message {
    pub role: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub content: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tool_calls: Option<serde_json::Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tool_call_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
}

#[derive(Debug, Serialize, Clone)]
pub struct AgentEvent {
    pub r#type: String,
    pub content: String,
}
pub struct Agent {
    api_key: String,
    api_base: String,
    model: String,
    #[allow(dead_code)]
    workspace: String,
    tools: std::collections::HashMap<String, Arc<dyn Tool>>,
    pub sessions: SessionManager,
    memory: MemoryStore,
    skills: SkillsLoader,
    subagents: Mutex<Option<Arc<SubagentManager>>>,
    config: Config,
}

#[derive(Default)]
struct ToolCallAccum {
    #[allow(dead_code)]
    index: i64,
    id: String,
    name: String,
    arguments: String,
}

impl Agent {
    pub fn new(workspace: String, config: Config) -> Self {
        let mut tools: std::collections::HashMap<String, Arc<dyn Tool>> = std::collections::HashMap::new();
        tools.insert("exec".to_string(), Arc::new(TerminalTool));
        tools.insert("read_file".to_string(), Arc::new(ReadFileTool));
        tools.insert("write_file".to_string(), Arc::new(WriteFileTool));
        tools.insert("edit_file".to_string(), Arc::new(EditFileTool));
        tools.insert("list_dir".to_string(), Arc::new(ListDirTool));
        tools.insert("web_search".to_string(), Arc::new(WebSearchTool {
            api_key: std::env::var("BRAVE_API_KEY").unwrap_or_default()
        }));
        tools.insert("web_fetch".to_string(), Arc::new(WebFetchTool));
        tools.insert("gmail".to_string(), Arc::new(GmailTool::new(&workspace)));
       
        Self {
            api_key: std::env::var("OPENAI_API_KEY").unwrap_or_default(),
            api_base: std::env::var("OPENAI_API_BASE").unwrap_or_else(|_| "https://api.openai.com/v1".to_string()),
            model: std::env::var("OPENAI_MODEL").unwrap_or_else(|_| "gpt-4-turbo".to_string()),
            sessions: SessionManager::new(&workspace),
            memory: MemoryStore::new(&workspace, 1536),
            skills: SkillsLoader::new(&workspace),
            subagents: Mutex::new(None),
            workspace,
            tools,
            config,
        }
    }

    pub fn set_subagents(&self, manager: Arc<SubagentManager>) {
        let mut sub = self.subagents.lock().unwrap();
        *sub = Some(manager);
    }

    pub async fn run(
        &self,
        message: String,
        session_id: String,
        tx: mpsc::Sender<AgentEvent>,
        channel: Option<String>,
        chat_id: Option<String>,
    ) -> Result<()> {
        let mut session = self.sessions.get_or_create(&session_id).await;
        
        // Log user message to session memory index L1
        self.memory.index_session_message(&session_id, "user", &message);

        let mut history = vec![
            Message {
                role: "system".to_string(),
                content: Some(self.build_system_prompt(channel.as_deref(), chat_id.as_deref())),
                tool_calls: None,
                tool_call_id: None,
                name: None,
            },
        ];

        // add session history window
        let memory_window = 20;
        let mut recent_msgs = session.messages.clone();
        if recent_msgs.len() > memory_window {
            recent_msgs.drain(0..(recent_msgs.len() - memory_window));
        }
        history.extend(recent_msgs);

        history.push(Message {
            role: "user".to_string(),
            content: Some(message.clone()),
            tool_calls: None,
            tool_call_id: None,
            name: None,
        });

        let mut iteration = 0;
        let max_iterations = 15;

        while iteration < max_iterations {
            iteration += 1;

            let (response_content, tool_calls): (String, Vec<serde_json::Value>) = self.call_llm(&history, tx.clone()).await?;
            let has_tools = !tool_calls.is_empty();

            history.push(Message {
                role: "assistant".to_string(),
                content: if response_content.is_empty() { None } else { Some(response_content.clone()) },
                tool_calls: if tool_calls.is_empty() { None } else { Some(serde_json::Value::Array(tool_calls.clone())) },
                tool_call_id: None,
                name: None,
            });

            if !has_tools {
                self.memory.index_session_message(&session_id, "assistant", &response_content);
                session.add_message("user", &message);
                session.add_message("assistant", &response_content);
                
                // Maybe distill L1 -> L2
                self.maybe_distill(&mut session).await;

                // Maybe consolidate L2 -> L3
                self.maybe_consolidate(&mut session).await;

                self.sessions.save(&session).await;

                tx.send(AgentEvent {
                    r#type: "done".to_string(),
                    content: "".to_string(),
                }).await?;
                break;
            }

            for tc in tool_calls {
                let id = tc["id"].as_str().unwrap_or("").to_string();
                let name = tc["function"]["name"].as_str().unwrap_or("").to_string();
                let arguments_str = tc["function"]["arguments"].as_str().unwrap_or("{}");

                tx.send(AgentEvent {
                    r#type: "tool_start".to_string(),
                    content: format!("{}: {}", name, arguments_str),
                }).await?;

                let tool_output = if name == "memory_search" {
                    let args_val: serde_json::Value = serde_json::from_str(arguments_str).unwrap_or_else(|_| serde_json::json!({}));
                    if let Some(query) = args_val.get("query").and_then(|q| q.as_str()) {
                        let results = self.memory.search(query, &[]);
                        let mut ss = format!("Search Results for \"{}\":\n", query);
                        for r in results.iter().take(5) {
                            let trunc_text = if r.text.len() > 200 { &r.text[..200] } else { &r.text };
                            ss.push_str(&format!("- [{}] {}: {} (Score: {})\n", r.source, r.path, trunc_text, r.score));
                        }
                        if results.is_empty() {
                            ss.push_str("No memory results found.");
                        }
                        ss
                    } else {
                        "Error: Missing query argument".to_string()
                    }
                } else if name == "spawn" {
                    let args_val: serde_json::Value = serde_json::from_str(arguments_str).unwrap_or_else(|_| serde_json::json!({}));
                    if let Some(mgr) = self.subagents.lock().unwrap().as_ref() {
                        let task = args_val.get("task").and_then(|t| t.as_str()).unwrap_or("");
                        let label = args_val.get("label").and_then(|l| l.as_str()).map(|s| s.to_string());
                        mgr.spawn(task.to_string(), label, session_id.clone())
                    } else {
                        "Error: Subagent manager not initialized".to_string()
                    }
                } else if let Some(tool) = self.tools.get(&name) {
                    let args_val: serde_json::Value = serde_json::from_str(arguments_str).unwrap_or_else(|_| serde_json::json!({}));
                    match tool.execute(&args_val).await {
                        Ok(res) => res,
                        Err(e) => format!("Error executing tool: {}", e),
                    }
                } else {
                    format!("Error: Tool '{}' not found", name)
                };

                tx.send(AgentEvent {
                    r#type: "tool_end".to_string(),
                    content: tool_output.clone(),
                }).await?;

                history.push(Message {
                    role: "tool".to_string(),
                    content: Some(tool_output),
                    tool_calls: None,
                    tool_call_id: Some(id),
                    name: Some(name),
                });
            }
        }

        Ok(())
    }

    fn build_system_prompt(&self, channel: Option<&str>, chat_id: Option<&str>) -> String {
        let mut parts = Vec::new();

        // 1. Identity Header
        parts.push(self.get_identity_header());

        // 2. Bootstrap Files
        let bootstrap = self.load_bootstrap_files();
        if !bootstrap.is_empty() {
            parts.push(bootstrap);
        }

        // 3. Memory Context
        let mem_ctx = self.memory.get_memory_context();
        if !mem_ctx.is_empty() {
            parts.push(format!("# Memory\n\n{}", mem_ctx));
        }

        // 4. Always Skills
        let always_skills = self.skills.load_always_skills();
        if !always_skills.is_empty() {
            parts.push(format!("# Active Skills\n\n{}", always_skills));
        }

        // 5. Skills Summary
        let skills_summary = self.skills.build_skills_summary();
        if !skills_summary.is_empty() {
            parts.push(format!(
                "# Available Skills\n\nThe following skills extend your capabilities. To use a skill, read its SKILL.md file with `read_file`.\n\n{}",
                skills_summary
            ));
        }

        let mut prompt = parts.join("\n\n---\n\n");

        if let (Some(ch), Some(id)) = (channel, chat_id) {
            prompt.push_str(&format!("\n\n## Current Session\nChannel: {}\nChat ID: {}", ch, id));
        }

        prompt
    }

    fn get_identity_header(&self) -> String {
        let now = Utc::now();
        let time_str = now.format("%Y-%m-%d %H:%M (%A)").to_string();
        let ws_path = std::path::Path::new(&self.workspace).canonicalize()
            .unwrap_or_else(|_| std::path::PathBuf::from(&self.workspace));
        
        let default_identity = format!(
            "# miniclaw 🦞\n\nYou are miniclaw, a high-performance personal AI assistant.\n\n## Current Time\n{}\n\n## Workspace\nYour workspace is at: {}\n- Long-term memory: memory/MEMORY.md\n- History log: memory/HISTORY.md\n- Skills: skills/\n\nAlways be helpful, accurate, and concise.\nWhen remembering something important, write to memory/MEMORY.md\nTo recall past events, use exec to grep memory/HISTORY.md",
            time_str,
            ws_path.to_string_lossy()
        );

        self.config.load_prompt("role", &default_identity)
            .replace("{{TIME}}", &time_str)
            .replace("{{WORKSPACE}}", &ws_path.to_string_lossy())
    }

    fn load_bootstrap_files(&self) -> String {
        let files = ["AGENTS.md", "SOUL.md", "USER.md", "TOOLS.md", "IDENTITY.md"];
        let mut parts = Vec::new();

        for fname in files {
            let p = std::path::Path::new(&self.workspace).join(fname);
            if p.exists() {
                if let Ok(content) = std::fs::read_to_string(p) {
                    parts.push(format!("## {}\n\n{}", fname, content));
                }
            }
        }

        parts.join("\n\n")
    }

    async fn call_llm(
        &self,
        messages: &[Message],
        tx: mpsc::Sender<AgentEvent>,
    ) -> Result<(String, Vec<serde_json::Value>)> {
        let client = reqwest::Client::new();
        let url = format!("{}/chat/completions", self.api_base);

        let mut tools_schema: Vec<serde_json::Value> = self.tools.values().map(|t| t.schema()).collect();
        
        // Add spawn tool dynamically if available
        if self.subagents.lock().unwrap().is_some() {
            // let _session_id = "TODO_NEED_SESSION_ID".to_string(); 
            // We'll just define the interface here. 
            // The execution logic in run() already handles the real manager.
            tools_schema.push(serde_json::json!({
                "type": "function",
                "function": {
                    "name": "spawn",
                    "description": "Spawn a subagent to handle a complex task in the background.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "task": {
                                "type": "string",
                                "description": "Full description of the task for the subagent"
                            },
                            "label": {
                                "type": "string",
                                "description": "Optional short label for the task"
                            }
                        },
                        "required": ["task"]
                    }
                }
            }));
        }
        
        // Add memory_search dynamically
        tools_schema.push(serde_json::json!({
            "type": "function",
            "function": {
                "name": "memory_search",
                "description": "Search through the agent's multi-tiered memory (sessions, daily logs, curated facts)",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "query": {
                            "type": "string",
                            "description": "The search query"
                        }
                    },
                    "required": ["query"]
                }
            }
        }));

        let mut payload_map = serde_json::Map::new();
        payload_map.insert("model".to_string(), serde_json::json!(self.model));
        payload_map.insert("messages".to_string(), serde_json::json!(messages));
        payload_map.insert("stream".to_string(), serde_json::json!(true));

        if !tools_schema.is_empty() {
            payload_map.insert("tools".to_string(), serde_json::json!(tools_schema));
            payload_map.insert("tool_choice".to_string(), serde_json::json!("auto"));
        }

        let payload = serde_json::Value::Object(payload_map);

        let res = client
            .post(&url)
            .bearer_auth(&self.api_key)
            .json(&payload)
            .send()
            .await?;

        if !res.status().is_success() {
            let status = res.status();
            let err_text = res.text().await?;
            return Err(anyhow!("LLM Error: {} - {}", status, err_text));
        }

        let mut full_content = String::new();
        let mut accumulated_tool_calls: Vec<ToolCallAccum> = Vec::new();

        let mut stream = res.bytes_stream();
        use futures::StreamExt;
        let mut buffer = String::new();

        while let Some(item) = stream.next().await {
            let chunk = item?;
            buffer.push_str(&String::from_utf8_lossy(&chunk));

            while let Some(pos) = buffer.find('\n') {
                let line = buffer[..pos].trim().to_string();
                buffer.drain(..pos + 1);

                if line.starts_with("data: ") {
                    let json_str = &line[6..];
                    if json_str == "[DONE]" {
                        continue;
                    }

                    if let Ok(j) = serde_json::from_str::<serde_json::Value>(json_str) {
                        if let Some(choices) = j.get("choices").and_then(|c| c.as_array()) {
                            if choices.is_empty() {
                                continue;
                            }
                            let delta = &choices[0]["delta"];
                            
                            if let Some(content) = delta.get("content").and_then(|c| c.as_str()) {
                                full_content.push_str(content);
                                let _ = tx.send(AgentEvent {
                                    r#type: "token".to_string(),
                                    content: content.to_string(),
                                }).await;
                            }

                            if let Some(tool_calls_chunk) = delta.get("tool_calls").and_then(|t| t.as_array()) {
                                for tc in tool_calls_chunk {
                                    let idx = tc.get("index").and_then(|i| i.as_i64()).unwrap_or(0) as usize;
                                    while accumulated_tool_calls.len() <= idx {
                                        accumulated_tool_calls.push(ToolCallAccum { 
                                            index: accumulated_tool_calls.len() as i64, 
                                            ..Default::default() 
                                        });
                                    }
                                    let accum = &mut accumulated_tool_calls[idx];
                                    
                                    if let Some(id) = tc.get("id").and_then(|i| i.as_str()) {
                                        accum.id = id.to_string();
                                    }
                                    if let Some(func) = tc.get("function") {
                                        if let Some(name) = func.get("name").and_then(|n| n.as_str()) {
                                            accum.name = name.to_string();
                                        }
                                        if let Some(args) = func.get("arguments").and_then(|a| a.as_str()) {
                                            accum.arguments.push_str(args);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        let mut final_tool_calls = Vec::new();
        for accum in accumulated_tool_calls {
            let id = if accum.id.is_empty() { 
                format!("call_{}", rand::random::<u32>())
            } else { 
                accum.id 
            };
            
            final_tool_calls.push(serde_json::json!({
                "id": id,
                "type": "function",
                "function": {
                    "name": accum.name,
                    "arguments": accum.arguments,
                }
            }));
        }

        Ok((full_content, final_tool_calls))
    }

    async fn maybe_distill(&self, session: &mut crate::memory::Session) {
        let token_threshold = 2000;
        let msg_threshold = 20;
        
        let current_tokens = session.estimate_tokens();
        let tokens_since_distill = current_tokens.saturating_sub(session.last_distilled_token_count);
        let msgs_since_distill = session.messages.len().saturating_sub(session.last_consolidated);

        if tokens_since_distill > token_threshold || msgs_since_distill > msg_threshold {
            let _ = self.distill_l1_to_l2(session).await;
        }
    }

    async fn distill_l1_to_l2(&self, session: &mut crate::memory::Session) -> Result<()> {
        let start = session.last_consolidated;
        let end = session.messages.len();
        if start >= end { return Ok(()); }

        let mut conv = String::new();
        for i in start..end {
            let m = &session.messages[i];
            conv.push_str(&format!("[{}]: {}\n", m.role, m.content.as_deref().unwrap_or("")));
        }

        let prompt = format!(
            "Summarize the following conversation segment into a durable daily log entry. Focus on key facts, decisions, and outcomes.\n\n## Conversation Segment\n{}",
            conv
        );

        let msgs = vec![
            Message {
                role: "system".to_string(),
                content: Some("You are a memory distillation agent. Your goal is to compress raw session logs into meaningful daily summaries.".to_string()),
                tool_calls: None,
                tool_call_id: None,
                name: None,
            },
            Message {
                role: "user".to_string(),
                content: Some(prompt),
                tool_calls: None,
                tool_call_id: None,
                name: None,
            }
        ];

        // dummy tx for distillation (we don't want to stream these tokens to the user)
        let (tx, _) = mpsc::channel(100);
        let (summary, _) = self.call_llm(&msgs, tx).await?;

        if !summary.is_empty() {
            self.memory.append_daily_log(&summary);
            session.last_consolidated = end;
            session.last_distilled_token_count = session.estimate_tokens();
        }

        Ok(())
    }

    async fn maybe_consolidate(&self, session: &mut crate::memory::Session) {
        let today = Utc::now().format("%Y-%m-%d").to_string();
        if session.last_consolidation_date != today {
            let _ = self.consolidate_memory(session).await;
        }
    }

    async fn consolidate_memory(&self, session: &mut crate::memory::Session) -> Result<()> {
        let today = Utc::now().format("%Y-%m-%d").to_string();
        
        // Get last 3 daily logs
        let mut daily_logs = Vec::new();
        for i in 1..=3 {
            let date = (Utc::now() - chrono::Duration::days(i)).format("%Y-%m-%d").to_string();
            if let Some(log) = self.memory.get_daily_log(&date) {
                daily_logs.push(format!("### Log ({})\n{}", date, log));
            }
        }

        if daily_logs.is_empty() { 
            session.last_consolidation_date = today;
            return Ok(()); 
        }

        let current_mem = self.memory.read_long_term();
        let prompt = format!(
            "You are a memory consolidation agent. Update the existing long-term memory with new information from recent daily logs. Keep it concise and organized.\n\n## Existing Long-term Memory\n{}\n\n## Recent Daily Logs\n{}\n\nOutput the UPDATED long-term memory in Markdown format.",
            current_mem,
            daily_logs.join("\n\n")
        );

        let msgs = vec![
            Message {
                role: "system".to_string(),
                content: Some("You are a memory consolidation agent.".to_string()),
                tool_calls: None,
                tool_call_id: None,
                name: None,
            },
            Message {
                role: "user".to_string(),
                content: Some(prompt),
                tool_calls: None,
                tool_call_id: None,
                name: None,
            }
        ];

        let (tx, _) = mpsc::channel(10);
        let (updated_mem, _) = self.call_llm(&msgs, tx).await?;

        if !updated_mem.is_empty() {
            self.memory.write_long_term(&updated_mem);
            session.last_consolidation_date = today;
        }

        Ok(())
    }
}
