use miniclaw_rust::tools::{Tool, GmailTool};
use miniclaw_rust::config::Config;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let args: Vec<String> = std::env::args().collect();
    let config = Config::load();
    let workspace = config.memory_workspace();
    println!("Workspace: {}", workspace);

    let gmail = GmailTool::new(&workspace);
    
    // Check credentials
    let res: String = gmail.execute(&serde_json::json!({"action": "check_creds"})).await?;
    println!("Check Creds: {}", res);

    if res.contains("Error") {
        return Ok(());
    }

    // Try to trigger auth or exchange code
    let auth_args = if args.len() > 1 {
        serde_json::json!({"action": "auth", "auth_code": &args[1]})
    } else {
        serde_json::json!({"action": "auth"})
    };

    let res: String = gmail.execute(&auth_args).await?;
    println!("Auth Status: {}", res);

    // If authenticated, try to list and read
    if res.contains("Successfully") || res.contains("Already") || res.contains("found") {
        println!("\n--- Testing Email Listing ---");
        let list_res: String = gmail.execute(&serde_json::json!({"action": "list", "query": "is:unread"})).await?;
        
        let v: serde_json::Value = serde_json::from_str(&list_res).unwrap_or(serde_json::json!({}));
        if let Some(messages) = v["messages"].as_array() {
            println!("Found {} unread messages.", messages.len());
            if !messages.is_empty() {
                let id = messages[0]["id"].as_str().unwrap();
                println!("\n--- Testing Email Fetch (ID: {}) ---", id);
                let get_res: String = gmail.execute(&serde_json::json!({"action": "get", "message_id": id})).await?;
                
                // Truncate for display
                let display_len = std::cmp::min(1000, get_res.len());
                println!("Message Content Preview:\n{}", &get_res[..display_len]);
                if get_res.len() > 1000 { println!("..."); }
                
                println!("\n✅ Success! The Gmail skill is fully integrated and ready.");
                println!("The agent can now read these details, summarize them, and suggest replies.");
            } else {
                println!("\nNo unread emails found to test fetching.");
            }
        } else {
            println!("No messages found or error listing: {}", list_res);
        }
    } else if args.len() <= 1 && res.contains("visit this URL") {
        println!("\nPlease visit the URL above to authorize, then run:");
        println!("cargo run --bin gmail-verify <YOUR_CODE>");
    }

    Ok(())
}
