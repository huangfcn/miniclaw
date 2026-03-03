use std::process::Command;
use std::fs;
use std::path::Path;
use anyhow::{Result, anyhow};
use async_trait::async_trait;
use serde_json::json;
use crate::agent::SubagentManager;
use std::sync::Arc;

mod gmail;
pub use gmail::GmailTool;

#[async_trait]
pub trait Tool: Send + Sync {
    fn name(&self) -> &str;
    fn description(&self) -> &str;
    fn schema(&self) -> serde_json::Value;
    async fn execute(&self, args: &serde_json::Value) -> Result<String>;
}

pub struct TerminalTool;

#[async_trait]
impl Tool for TerminalTool {
    fn name(&self) -> &str { "exec" }
    fn description(&self) -> &str { "Execute a shell command and return its output. Use for running scripts, curl, grep, etc." }
    fn schema(&self) -> serde_json::Value {
        json!({
            "type": "function",
            "function": {
                "name": "exec",
                "description": "Execute a shell command and return its output. Use for running scripts, curl, grep, etc.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "command": {
                            "type": "string",
                            "description": "The shell command to execute"
                        }
                    },
                    "required": ["command"]
                }
            }
        })
    }
    async fn execute(&self, args: &serde_json::Value) -> Result<String> {
        let command = args.get("command")
            .and_then(|v| v.as_str())
            .ok_or_else(|| anyhow!("Missing 'command' argument"))?;

        let output = if cfg!(target_os = "windows") {
            Command::new("cmd").arg("/C").arg(command).output()?
        } else {
            Command::new("sh").arg("-c").arg(command).output()?
        };

        let stdout = String::from_utf8_lossy(&output.stdout).to_string();
        let stderr = String::from_utf8_lossy(&output.stderr).to_string();

        if output.status.success() {
            let res = if stdout.is_empty() { "(no output)".to_string() } else { stdout };
            Ok(res)
        } else {
            Ok(format!("Error (exit {}):\n{}{}", output.status.code().unwrap_or(-1), stdout, stderr))
        }
    }
}

pub struct ReadFileTool;

#[async_trait]
impl Tool for ReadFileTool {
    fn name(&self) -> &str { "read_file" }
    fn description(&self) -> &str { "Read the contents of a file." }
    fn schema(&self) -> serde_json::Value {
        json!({
            "type": "function",
            "function": {
                "name": "read_file",
                "description": "Read the contents of a file.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "path": {
                            "type": "string",
                            "description": "Absolute or relative path to the file"
                        }
                    },
                    "required": ["path"]
                }
            }
        })
    }
    async fn execute(&self, args: &serde_json::Value) -> Result<String> {
        let path = args.get("path")
            .and_then(|v| v.as_str())
            .ok_or_else(|| anyhow!("Missing 'path' argument"))?;
        
        fs::read_to_string(path).map_err(|e| anyhow!("Failed to read file: {}", e))
    }
}

pub struct WriteFileTool;

#[async_trait]
impl Tool for WriteFileTool {
    fn name(&self) -> &str { "write_file" }
    fn description(&self) -> &str { "Write content to a file (creates parent directories if needed)." }
    fn schema(&self) -> serde_json::Value {
        json!({
            "type": "function",
            "function": {
                "name": "write_file",
                "description": "Write content to a file (creates parent directories if needed).",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "path": {
                            "type": "string",
                            "description": "Path to write to"
                        },
                        "content": {
                            "type": "string",
                            "description": "Content to write"
                        }
                    },
                    "required": ["path", "content"]
                }
            }
        })
    }
    async fn execute(&self, args: &serde_json::Value) -> Result<String> {
        let path = args.get("path")
            .and_then(|v| v.as_str())
            .ok_or_else(|| anyhow!("Missing 'path' argument"))?;
        let content = args.get("content")
            .and_then(|v| v.as_str())
            .ok_or_else(|| anyhow!("Missing 'content' argument"))?;

        if let Some(parent) = Path::new(path).parent() {
            if !parent.as_os_str().is_empty() {
                fs::create_dir_all(parent)?;
            }
        }

        fs::write(path, content).map_err(|e| anyhow!("Failed to write file: {}", e))?;
        Ok(format!("File written successfully: {}", path))
    }
}

pub struct EditFileTool;

#[async_trait]
impl Tool for EditFileTool {
    fn name(&self) -> &str { "edit_file" }
    fn description(&self) -> &str { "Replace specific text in a file." }
    fn schema(&self) -> serde_json::Value {
        json!({
            "type": "function",
            "function": {
                "name": "edit_file",
                "description": "Replace specific text in a file.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "path": {
                            "type": "string",
                            "description": "Path to the file"
                        },
                        "old_text": {
                            "type": "string",
                            "description": "Text to replace"
                        },
                        "new_text": {
                            "type": "string",
                            "description": "Replacement text"
                        }
                    },
                    "required": ["path", "old_text", "new_text"]
                }
            }
        })
    }
    async fn execute(&self, args: &serde_json::Value) -> Result<String> {
        let path = args.get("path")
            .and_then(|v| v.as_str())
            .ok_or_else(|| anyhow!("Missing 'path' argument"))?;
        let old_text = args.get("old_text")
            .and_then(|v| v.as_str())
            .ok_or_else(|| anyhow!("Missing 'old_text' argument"))?;
        let new_text = args.get("new_text")
            .and_then(|v| v.as_str())
            .ok_or_else(|| anyhow!("Missing 'new_text' argument"))?;

        let content = fs::read_to_string(path)?;
        if !content.contains(old_text) {
            return Err(anyhow!("Error: old_text not found in file"));
        }
        
        let new_content = content.replace(old_text, new_text);
        fs::write(path, new_content)?;
        Ok(format!("File edited successfully: {}", path))
    }
}

pub struct ListDirTool;

#[async_trait]
impl Tool for ListDirTool {
    fn name(&self) -> &str { "list_dir" }
    fn description(&self) -> &str { "List the contents of a directory." }
    fn schema(&self) -> serde_json::Value {
        json!({
            "type": "function",
            "function": {
                "name": "list_dir",
                "description": "List the contents of a directory.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "path": {
                            "type": "string",
                            "description": "Path to the directory"
                        }
                    },
                    "required": ["path"]
                }
            }
        })
    }
    async fn execute(&self, args: &serde_json::Value) -> Result<String> {
        let path = args.get("path")
            .and_then(|v| v.as_str())
            .ok_or_else(|| anyhow!("Missing 'path' argument"))?;

        let mut entries = vec![];
        let p = Path::new(path);
        
        if p.is_dir() {
            for entry in fs::read_dir(p)? {
                let entry = entry?;
                let file_name = entry.file_name().into_string().unwrap_or_default();
                if entry.path().is_dir() {
                    entries.push(format!("{}/", file_name));
                } else {
                    entries.push(file_name);
                }
            }
        } else {
            return Err(anyhow!("Error: Path is not a directory or does not exist"));
        }

        if entries.is_empty() {
            Ok("(empty directory)".to_string())
        } else {
            Ok(entries.join("\n"))
        }
    }
}

pub struct WebSearchTool {
    pub api_key: String,
}

#[async_trait]
impl Tool for WebSearchTool {
    fn name(&self) -> &str { "web_search" }
    fn description(&self) -> &str { "Search the web using Brave Search API" }
    fn schema(&self) -> serde_json::Value {
        json!({
            "type": "function",
            "function": {
                "name": "web_search",
                "description": "Search the web using Brave Search API",
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
        })
    }
    async fn execute(&self, args: &serde_json::Value) -> Result<String> {
        let input = args.get("query")
            .and_then(|v| v.as_str())
            .ok_or_else(|| anyhow!("Missing 'query' argument"))?;

        if self.api_key.is_empty() {
            return Err(anyhow!("BRAVE_API_KEY not configured"));
        }

        let client = reqwest::Client::new();
        let url = format!("https://api.search.brave.com/res/v1/web/search?q={}", urlencoding::encode(input));
        
        let res = client.get(&url)
            .header("X-Subscription-Token", &self.api_key)
            .send()
            .await?;

        if !res.status().is_success() {
            return Err(anyhow!("Search failed: Status {}", res.status()));
        }

        let j: serde_json::Value = res.json().await?;
        let mut output = format!("Search results for: {}\n\n", input);
        if let Some(results) = j["web"]["results"].as_array() {
            for (i, item) in results.iter().take(5).enumerate() {
                output.push_str(&format!("{}. {}\n   URL: {}\n   {}\n\n", 
                    i + 1,
                    item["title"].as_str().unwrap_or(""),
                    item["url"].as_str().unwrap_or(""),
                    item["description"].as_str().unwrap_or("")
                ));
            }
        }
        Ok(output)
    }
}

pub struct WebFetchTool;

#[async_trait]
impl Tool for WebFetchTool {
    fn name(&self) -> &str { "web_fetch" }
    fn description(&self) -> &str { "Fetch a URL and extract text content" }
    fn schema(&self) -> serde_json::Value {
        json!({
            "type": "function",
            "function": {
                "name": "web_fetch",
                "description": "Fetch a URL and extract text content",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "url": {
                            "type": "string",
                            "description": "The URL to fetch"
                        }
                    },
                    "required": ["url"]
                }
            }
        })
    }
    async fn execute(&self, args: &serde_json::Value) -> Result<String> {
        let input = args.get("url")
            .and_then(|v| v.as_str())
            .ok_or_else(|| anyhow!("Missing 'url' argument"))?;

        let client = reqwest::Client::new();
        let res = client.get(input).send().await?;
        
        let status = res.status();
        if !status.is_success() {
            return Err(anyhow!("Fetch failed: Status {}", status));
        }

        let html = res.text().await?;
        let text = strip_html(&html);
        let truncated = if text.len() > 2000 { &text[..2000] } else { &text };
        
        Ok(format!("URL: {}\nContent (first 2000 chars):\n\n{}", input, truncated))
    }
}

pub struct SpawnTool {
    pub manager: Arc<SubagentManager>,
    pub session_id: String,
}

#[async_trait]
impl Tool for SpawnTool {
    fn name(&self) -> &str { "spawn" }
    fn description(&self) -> &str { "Spawn a subagent to handle a complex task in the background." }
    fn schema(&self) -> serde_json::Value {
        json!({
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
        })
    }
    async fn execute(&self, args: &serde_json::Value) -> Result<String> {
        let task = args.get("task")
            .and_then(|v| v.as_str())
            .ok_or_else(|| anyhow!("Missing 'task' argument"))?;
        let label = args.get("label").and_then(|v| v.as_str()).map(|s| s.to_string());

        Ok(self.manager.spawn(task.to_string(), label, self.session_id.clone()))
    }
}

fn strip_html(html: &str) -> String {
    use regex::Regex;
    let re_script = Regex::new(r"(?i)<script[\s\S]*?</script>").unwrap();
    let re_style = Regex::new(r"(?i)<style[\s\S]*?</style>").unwrap();
    let re_tags = Regex::new(r"<[^>]+>").unwrap();
    let re_spaces = Regex::new(r"[ \t]+").unwrap();
    let re_newlines = Regex::new(r"\n{2,}").unwrap();

    let s = re_script.replace_all(html, "");
    let s = re_style.replace_all(&s, "");
    let s = re_tags.replace_all(&s, " ");
    let s = re_spaces.replace_all(&s, " ");
    let s = re_newlines.replace_all(&s, "\n\n");
    s.to_string()
}
