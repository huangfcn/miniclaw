use serde::{Deserialize, Serialize};
use std::sync::Arc;
use tokio::sync::mpsc;
use anyhow::{Result, anyhow};
use regex::Regex;
use crate::tools::{Tool, TerminalTool, ReadFileTool, WriteFileTool, WebSearchTool, WebFetchTool};

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct Message {
    pub role: String,
    pub content: String,
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
    _workspace: String,
    tools: std::collections::HashMap<String, Arc<dyn Tool>>,
}

impl Agent {
    pub fn new() -> Self {
        let mut tools: std::collections::HashMap<String, Arc<dyn Tool>> = std::collections::HashMap::new();
        tools.insert("terminal".to_string(), Arc::new(TerminalTool));
        tools.insert("read_file".to_string(), Arc::new(ReadFileTool));
        tools.insert("write_file".to_string(), Arc::new(WriteFileTool));
        tools.insert("web_search".to_string(), Arc::new(WebSearchTool {
            api_key: std::env::var("BRAVE_API_KEY").unwrap_or_default()
        }));
        tools.insert("web_fetch".to_string(), Arc::new(WebFetchTool));

        Self {
            api_key: std::env::var("OPENAI_API_KEY").unwrap_or_default(),
            api_base: std::env::var("OPENAI_API_BASE").unwrap_or_else(|_| "https://api.openai.com/v1".to_string()),
            model: std::env::var("OPENAI_MODEL").unwrap_or_else(|_| "gpt-4-turbo".to_string()),
            _workspace: std::env::var("WORKSPACE_DIR").unwrap_or_else(|_| ".".to_string()),
            tools,
        }
    }

    pub async fn run(
        &self,
        message: String,
        _session_id: String,
        tx: mpsc::Sender<AgentEvent>,
    ) -> Result<()> {
        let mut history = vec![
            Message {
                role: "system".to_string(),
                content: self.build_system_prompt(),
            },
            Message {
                role: "user".to_string(),
                content: message,
            },
        ];

        let mut iteration = 0;
        let max_iterations = 10;

        while iteration < max_iterations {
            iteration += 1;

            let response = self.call_llm(&history, tx.clone()).await?;
            history.push(Message {
                role: "assistant".to_string(),
                content: response.clone(),
            });

            let re = Regex::new(r#"<tool name="([^"]+)">([\s\S]*?)</tool>"#).unwrap();
            if let Some(caps) = re.captures(&response) {
                let tool_name = caps.get(1).unwrap().as_str();
                let tool_input = caps.get(2).unwrap().as_str().trim();

                tx.send(AgentEvent {
                    r#type: "tool_start".to_string(),
                    content: format!("{}: {}", tool_name, tool_input),
                }).await?;

                let tool_output = if let Some(tool) = self.tools.get(tool_name) {
                    match tool.execute(tool_input).await {
                        Ok(res) => res,
                        Err(e) => format!("Error executing tool: {}", e),
                    }
                } else {
                    format!("Error: Tool '{}' not found", tool_name)
                };

                tx.send(AgentEvent {
                    r#type: "tool_end".to_string(),
                    content: tool_output.clone(),
                }).await?;

                history.push(Message {
                    role: "user".to_string(),
                    content: format!("<observation>\n{}\n</observation>", tool_output),
                });
            } else {
                tx.send(AgentEvent {
                    r#type: "done".to_string(),
                    content: "".to_string(),
                }).await?;
                break;
            }
        }

        Ok(())
    }

    fn build_system_prompt(&self) -> String {
        let mut prompt = "You are a powerful AI agent. Use the following tools when necessary:\n\n".to_string();
        for tool in self.tools.values() {
            prompt.push_str(&format!("- {}: {}\n", tool.name(), tool.description()));
        }
        prompt.push_str("\nTo use a tool, use this format:\n<tool name=\"tool_name\">\ninput\n</tool>\n");
        prompt.push_str("\nAlways reason step by step using Thought: before acting.\n");
        prompt
    }

    async fn call_llm(
        &self,
        messages: &[Message],
        tx: mpsc::Sender<AgentEvent>,
    ) -> Result<String> {
        let client = reqwest::Client::new();
        let url = format!("{}/chat/completions", self.api_base);

        let payload = serde_json::json!({
            "model": self.model,
            "messages": messages,
            "stream": true,
        });

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
                        if let Some(content) = j["choices"][0]["delta"]["content"].as_str() {
                            full_content.push_str(content);
                            let _ = tx.send(AgentEvent {
                                r#type: "token".to_string(),
                                content: content.to_string(),
                            }).await;
                        }
                    }
                }
            }
        }

        Ok(full_content)
    }
}
