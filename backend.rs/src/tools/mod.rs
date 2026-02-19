use std::process::Command;
use std::fs;
use anyhow::{Result, anyhow};
use async_trait::async_trait;

#[async_trait]
pub trait Tool: Send + Sync {
    fn name(&self) -> &str;
    fn description(&self) -> &str;
    async fn execute(&self, input: &str) -> Result<String>;
}

pub struct TerminalTool;

#[async_trait]
impl Tool for TerminalTool {
    fn name(&self) -> &str { "terminal" }
    fn description(&self) -> &str { "Execute a shell command" }
    async fn execute(&self, input: &str) -> Result<String> {
        let output = Command::new("sh")
            .arg("-c")
            .arg(input)
            .output()?;

        let stdout = String::from_utf8_lossy(&output.stdout).to_string();
        let stderr = String::from_utf8_lossy(&output.stderr).to_string();

        if output.status.success() {
            Ok(stdout)
        } else {
            Ok(format!("Error (exit {}):\n{}{}", output.status.code().unwrap_or(-1), stdout, stderr))
        }
    }
}

pub struct ReadFileTool;

#[async_trait]
impl Tool for ReadFileTool {
    fn name(&self) -> &str { "read_file" }
    fn description(&self) -> &str { "Read a file from disk" }
    async fn execute(&self, input: &str) -> Result<String> {
        let path = input.trim();
        fs::read_to_string(path).map_err(|e| anyhow!("Failed to read file: {}", e))
    }
}

pub struct WriteFileTool;

#[async_trait]
impl Tool for WriteFileTool {
    fn name(&self) -> &str { "write_file" }
    fn description(&self) -> &str { "Write a file to disk. Input format: <path>\n<content>" }
    async fn execute(&self, input: &str) -> Result<String> {
        let lines: Vec<&str> = input.lines().collect();
        if lines.is_empty() {
            return Err(anyhow!("Invalid input for write_file"));
        }
        let path = lines[0].trim();
        let content = lines[1..].join("\n");
        fs::write(path, content).map_err(|e| anyhow!("Failed to write file: {}", e))?;
        Ok(format!("File written successfully: {}", path))
    }
}

pub struct WebSearchTool {
    pub api_key: String,
}

#[async_trait]
impl Tool for WebSearchTool {
    fn name(&self) -> &str { "web_search" }
    fn description(&self) -> &str { "Search the web using Brave Search API" }
    async fn execute(&self, input: &str) -> Result<String> {
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
    async fn execute(&self, input: &str) -> Result<String> {
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
