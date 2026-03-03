use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::fs;
use anyhow::{Result, anyhow};
use async_trait::async_trait;
use serde_json::json;
use crate::tools::Tool;

#[derive(Debug, Serialize, Deserialize, Clone)]
struct GmailCredentials {
    #[serde(alias = "installed")]
    web: WebConfig,
}

#[derive(Debug, Serialize, Deserialize, Clone)]
struct WebConfig {
    client_id: String,
    project_id: String,
    auth_uri: String,
    token_uri: String,
    auth_provider_x509_cert_url: String,
    client_secret: String,
    #[serde(default)]
    redirect_uris: Vec<String>,
}

#[derive(Debug, Serialize, Deserialize, Clone)]
struct GmailToken {
    access_token: String,
    refresh_token: Option<String>,
    expires_in: Option<u64>,
    token_type: String,
    #[allow(dead_code)]
    expires_at: Option<chrono::DateTime<chrono::Utc>>,
}

pub struct GmailTool {
    pub workspace: String,
}

impl GmailTool {
    pub fn new(workspace: &str) -> Self {
        Self {
            workspace: workspace.to_string(),
        }
    }

    fn get_gmail_dir(&self) -> PathBuf {
        Path::new(&self.workspace).join("skills").join("gmail")
    }

    fn load_credentials(&self) -> Result<GmailCredentials> {
        let dir = self.get_gmail_dir();
        // The user mentioned credentials_huangfcn.json, but let's look for any *.json that has "web" or "installed"
        let candidates = vec!["credentials_huangfcn.json", "credentials.json"];
        for c in candidates {
            let p = dir.join(c);
            if p.exists() {
                let content = fs::read_to_string(&p)?;
                if let Ok(creds) = serde_json::from_str::<GmailCredentials>(&content) {
                    return Ok(creds);
                }
            }
        }
        Err(anyhow!("No valid Gmail credentials.json found in skills/gmail/"))
    }

    fn load_token(&self) -> Result<GmailToken> {
        let p = self.get_gmail_dir().join("token.json");
        let content = fs::read_to_string(p)?;
        let token: GmailToken = serde_json::from_str(&content)?;
        Ok(token)
    }

    fn save_token(&self, token: &GmailToken) -> Result<()> {
        let p = self.get_gmail_dir().join("token.json");
        let content = serde_json::json!(token).to_string();
        fs::write(p, content)?;
        Ok(())
    }

    async fn refresh_token(&self, creds: &GmailCredentials, token: &mut GmailToken) -> Result<()> {
        let refresh_token = token.refresh_token.as_ref().ok_or_else(|| anyhow!("No refresh token available"))?;
        
        let client = reqwest::Client::new();
        let params = [
            ("client_id", creds.web.client_id.as_str()),
            ("client_secret", creds.web.client_secret.as_str()),
            ("refresh_token", refresh_token.as_str()),
            ("grant_type", "refresh_token"),
        ];

        let res = client.post(&creds.web.token_uri)
            .form(&params)
            .send()
            .await?;

        if !res.status().is_success() {
            return Err(anyhow!("Failed to refresh token: {}", res.status()));
        }

        let new_token_data: serde_json::Value = res.json().await?;
        if let Some(at) = new_token_data["access_token"].as_str() {
            token.access_token = at.to_string();
        }
        if let Some(exp) = new_token_data["expires_in"].as_u64() {
            token.expires_in = Some(exp);
        }
        
        self.save_token(token)?;
        Ok(())
    }
}

#[async_trait]
impl Tool for GmailTool {
    fn name(&self) -> &str { "gmail" }
    fn description(&self) -> &str { "Interacts with Gmail API to read, list, and search emails." }
    fn schema(&self) -> serde_json::Value {
        json!({
            "type": "function",
            "function": {
                "name": "gmail",
                "description": "Interacts with Gmail API to read, list, and search emails.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "action": {
                            "type": "string",
                            "enum": ["list", "get", "auth", "check_creds"],
                            "description": "Action to perform"
                        },
                        "query": {
                            "type": "string",
                            "description": "Search query for 'list' action (e.g., 'is:unread')"
                        },
                        "message_id": {
                            "type": "string",
                            "description": "Message ID for 'get' action"
                        },
                        "auth_code": {
                            "type": "string",
                            "description": "The authorization code from Google (for 'auth' action)"
                        }
                    },
                    "required": ["action"]
                }
            }
        })
    }

    async fn execute(&self, args: &serde_json::Value) -> Result<String> {
        let action = args.get("action").and_then(|v| v.as_str()).ok_or_else(|| anyhow!("Missing action"))?;
        
        match action {
            "check_creds" => {
                match self.load_credentials() {
                    Ok(_) => Ok("Gmail credentials found and valid.".to_string()),
                    Err(e) => Ok(format!("Error: {}", e)),
                }
            },
            "auth" => {
                let creds = self.load_credentials()?;
                if let Some(code) = args.get("auth_code").and_then(|v| v.as_str()) {
                    // Exchange code
                    let client = reqwest::Client::new();
                    let redirect_uri = if !creds.web.redirect_uris.is_empty() {
                        &creds.web.redirect_uris[0]
                    } else {
                        "http://localhost"
                    };

                    let params = [
                        ("client_id", creds.web.client_id.as_str()),
                        ("client_secret", creds.web.client_secret.as_str()),
                        ("code", code),
                        ("grant_type", "authorization_code"),
                        ("redirect_uri", redirect_uri),
                    ];

                    let res = client.post(&creds.web.token_uri)
                        .form(&params)
                        .send()
                        .await?;

                    let status = res.status();
                    if !status.is_success() {
                        let err_text = res.text().await?;
                        return Ok(format!("Failed to exchange code: {}\n{}", status, err_text));
                    }

                    let token: GmailToken = res.json().await?;
                    self.save_token(&token)?;
                    Ok("Successfully authenticated! tokens saved.".to_string())
                } else {
                    // Check if already authenticated
                    if self.load_token().is_ok() {
                        return Ok("Already authenticated.".to_string());
                    }

                    // Generate URL
                    let scopes = "https://www.googleapis.com/auth/gmail.readonly";
                    let redirect_uri = if !creds.web.redirect_uris.is_empty() {
                        &creds.web.redirect_uris[0]
                    } else {
                        "http://localhost"
                    };
                    let url = format!(
                        "{}?client_id={}&redirect_uri={}&response_type=code&scope={}&access_type=offline&prompt=consent",
                        creds.web.auth_uri,
                        creds.web.client_id,
                        urlencoding::encode(redirect_uri),
                        urlencoding::encode(scopes)
                    );
                    Ok(format!("Please visit this URL to authorize Gmail access, then call gmail(action='auth', auth_code='...') with the code you received:\n\n{}", url))
                }
            },
            "list" => {
                let creds = self.load_credentials()?;
                let mut token = self.load_token().map_err(|_| anyhow!("Not authenticated. Use action='auth' first."))?;
                
                // TODO: Check expiration and refresh
                
                let client = reqwest::Client::new();
                let query = args.get("query").and_then(|v| v.as_str()).unwrap_or("is:unread");
                
                let res = client.get("https://gmail.googleapis.com/gmail/v1/users/me/messages")
                    .query(&[("q", query), ("maxResults", "10")])
                    .bearer_auth(&token.access_token)
                    .send()
                    .await?;

                if res.status().as_u16() == 401 {
                    self.refresh_token(&creds, &mut token).await?;
                    // Retry once
                    let res2 = client.get("https://gmail.googleapis.com/gmail/v1/users/me/messages")
                        .query(&[("q", query), ("maxResults", "10")])
                        .bearer_auth(&token.access_token)
                        .send()
                        .await?;
                    
                    if !res2.status().is_success() {
                        return Ok(format!("Failed to list messages (after refresh): {}", res2.status()));
                    }
                    
                    let j: serde_json::Value = res2.json().await?;
                    return Ok(serde_json::to_string_pretty(&j)?);
                }

                if !res.status().is_success() {
                    return Ok(format!("Failed to list messages: {}", res.status()));
                }

                let j: serde_json::Value = res.json().await?;
                Ok(serde_json::to_string_pretty(&j)?)
            },
            "get" => {
                let id = args.get("message_id").and_then(|v| v.as_str()).ok_or_else(|| anyhow!("Missing message_id"))?;
                let creds = self.load_credentials()?;
                let mut token = self.load_token()?;

                let client = reqwest::Client::new();
                let url = format!("https://gmail.googleapis.com/gmail/v1/users/me/messages/{}", id);
                
                let res = client.get(&url)
                    .bearer_auth(&token.access_token)
                    .send()
                    .await?;

                if res.status().as_u16() == 401 {
                    self.refresh_token(&creds, &mut token).await?;
                    let res2 = client.get(&url)
                        .bearer_auth(&token.access_token)
                        .send()
                        .await?;
                    if !res2.status().is_success() {
                         return Ok(format!("Failed to get message (after refresh): {}", res2.status()));
                    }
                    let j: serde_json::Value = res2.json().await?;
                    return Ok(serde_json::to_string_pretty(&j)?);
                }

                if !res.status().is_success() {
                    return Ok(format!("Failed to get message: {}", res.status()));
                }

                let j: serde_json::Value = res.json().await?;
                Ok(serde_json::to_string_pretty(&j)?)
            }
            _ => Err(anyhow!("Unknown action: {}", action)),
        }
    }
}
