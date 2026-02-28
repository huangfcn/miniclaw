use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use chrono::Utc;
use std::sync::{Arc, Mutex};

use super::index::{MemoryIndex, SearchResult};

#[derive(Clone)]
pub struct MemoryStore {
    #[allow(dead_code)]
    workspace_dir: PathBuf,
    memory_dir: PathBuf,
    memory_file: PathBuf,
    index: Arc<Mutex<MemoryIndex>>,
}

impl MemoryStore {
    pub fn new(workspace: &str, dimension: usize) -> Self {
        let w_path = Path::new(workspace);
        let m_dir = w_path.join("memory");
        fs::create_dir_all(&m_dir).unwrap_or_default();
        
        Self {
            workspace_dir: w_path.to_path_buf(),
            memory_dir: m_dir.clone(),
            memory_file: m_dir.join("MEMORY.md"),
            index: Arc::new(Mutex::new(MemoryIndex::new(workspace, dimension))),
        }
    }

    pub fn read_long_term(&self) -> String {
        fs::read_to_string(&self.memory_file).unwrap_or_default()
    }

    pub fn get_daily_log(&self, date: &str) -> Option<String> {
        let path = self.memory_dir.join(format!("{}.md", date));
        fs::read_to_string(path).ok()
    }

    pub fn write_long_term(&self, content: &str) {
        let _ = fs::write(&self.memory_file, content);
        let mut idx = self.index.lock().unwrap();
        idx.add_document(
            "L3_MEMORY".to_string(),
            self.memory_file.to_string_lossy().into_owned(),
            0,
            0,
            content.to_string(),
            vec![],
            "long-term".to_string()
        );
    }

    pub fn append_daily_log(&self, content: &str) {
        let date_str = Utc::now().format("%Y-%m-%d").to_string();
        let path = self.memory_dir.join(format!("{}.md", date_str));
        
        if let Ok(mut file) = OpenOptions::new().create(true).append(true).open(&path) {
            let _ = writeln!(file, "{}\n", content);
        }
        
        let id = format!("L2_{}_{}", date_str, Utc::now().timestamp());
        let mut idx = self.index.lock().unwrap();
        idx.add_document(
            id,
            path.to_string_lossy().into_owned(),
            0,
            0,
            content.to_string(),
            vec![],
            "memory".to_string()
        );
    }

    pub fn index_session_message(&self, session_id: &str, role: &str, content: &str) {
        let id = format!("L1_{}_{}", session_id, Utc::now().timestamp());
        let mut idx = self.index.lock().unwrap();
        idx.add_document(
            id,
            format!("session:{}", session_id),
            0,
            0,
            format!("[{}] {}", role, content),
            vec![],
            "sessions".to_string()
        );
    }

    pub fn search(&self, query: &str, embedding: &[f32]) -> Vec<SearchResult> {
        let mut idx = self.index.lock().unwrap();
        idx.search(query, embedding, 10)
    }

    pub fn get_memory_context(&self) -> String {
        let mut context = String::new();
        let lt = self.read_long_term();
        if !lt.is_empty() {
            context.push_str("## Long-term Memory (Curated Facts)\n\n");
            context.push_str(&lt);
            context.push_str("\n\n");
        }

        let y_date = (Utc::now() - chrono::Duration::days(1)).format("%Y-%m-%d").to_string();
        let y_path = self.memory_dir.join(format!("{}.md", y_date));
        if let Ok(y_log) = fs::read_to_string(y_path) {
            context.push_str(&format!("## Daily Log ({})\n\n", y_date));
            context.push_str(&y_log);
            context.push_str("\n\n");
        }

        let t_date = Utc::now().format("%Y-%m-%d").to_string();
        let t_path = self.memory_dir.join(format!("{}.md", t_date));
        if let Ok(t_log) = fs::read_to_string(t_path) {
            context.push_str(&format!("## Daily Log ({} - Today)\n\n", t_date));
            context.push_str(&t_log);
            context.push_str("\n\n");
        }

        context
    }
}
