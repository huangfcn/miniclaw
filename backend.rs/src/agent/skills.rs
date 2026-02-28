use std::path::{Path, PathBuf};
use std::fs;
use std::collections::HashMap;
use regex::Regex;

#[derive(Debug, Clone)]
pub struct SkillInfo {
    pub name: String,
    pub path: PathBuf,
    pub description: String,
    pub always_load: bool,
}

pub struct SkillsLoader {
    skills_dir: PathBuf,
}

impl SkillsLoader {
    pub fn new(workspace: &str) -> Self {
        let dir = Path::new(workspace).join("skills");
        if !dir.exists() {
            let _ = fs::create_dir_all(&dir);
        }
        Self {
            skills_dir: dir,
        }
    }

    pub fn list_skills(&self) -> Vec<SkillInfo> {
        let mut result = Vec::new();
        if !self.skills_dir.exists() {
            return result;
        }

        if let Ok(entries) = fs::read_dir(&self.skills_dir) {
            for entry in entries.flatten() {
                let path = entry.path();
                if path.is_dir() {
                    let skill_md = path.join("SKILL.md");
                    if skill_md.exists() {
                        let name = path.file_name().unwrap().to_string_lossy().into_owned();
                        let content = fs::read_to_string(&skill_md).unwrap_or_default();
                        let meta = self.parse_frontmatter(&content);
                        
                        let description = meta.get("description").cloned().unwrap_or_else(|| name.clone());
                        let always_load = meta.get("always").map(|v| v == "true" || v == "\"true\"").unwrap_or(false);

                        result.push(SkillInfo {
                            name,
                            path: fs::canonicalize(&path).unwrap_or(path),
                            description,
                            always_load,
                        });
                    }
                }
            }
        }
        result
    }

    pub fn build_skills_summary(&self) -> String {
        let skills = self.list_skills();
        if skills.is_empty() {
            return String::new();
        }

        let mut summary = String::from("<skills>\n");
        for s in skills {
            summary.push_str("  <skill>\n");
            summary.push_str(&format!("    <name>{}</name>\n", self.escape_xml(&s.name)));
            summary.push_str(&format!("    <description>{}</description>\n", self.escape_xml(&s.description)));
            summary.push_str(&format!("    <location>{}</location>\n", self.escape_xml(&s.path.to_string_lossy())));
            summary.push_str("  </skill>\n");
        }
        summary.push_str("</skills>");
        summary
    }

    pub fn load_always_skills(&self) -> String {
        let mut result = String::new();
        let skills_path_str = self.skills_dir.to_string_lossy().to_string();

        for s in self.list_skills() {
            if s.always_load {
                if let Ok(content) = fs::read_to_string(&s.path) {
                    let stripped = self.strip_frontmatter(&content);
                    // Replace {{skills_path}} with absolute path to skills directory
                    let resolved = stripped.replace("{{skills_path}}", &skills_path_str);
                    result.push_str(&format!("### Skill: {}\n\n{}\n\n---\n\n", s.name, resolved));
                }
            }
        }
        result
    }

    fn parse_frontmatter(&self, content: &str) -> HashMap<String, String> {
        let mut meta = HashMap::new();
        if !content.starts_with("---") {
            return meta;
        }

        let re = Regex::new(r"(?s)^---\n(.*?)\n---").unwrap();
        if let Some(caps) = re.captures(content) {
            let body = caps.get(1).unwrap().as_str();
            for line in body.lines() {
                if let Some((key, val)) = line.split_once(':') {
                    let k = key.trim().to_string();
                    let mut v = val.trim().to_string();
                    if (v.starts_with('"') && v.ends_with('"')) || (v.starts_with('\'') && v.ends_with('\'')) {
                        v = v[1..v.len()-1].to_string();
                    }
                    meta.insert(k, v);
                }
            }
        }
        meta
    }

    fn strip_frontmatter(&self, content: &str) -> String {
        let re = Regex::new(r"(?s)^---\n.*?\n---\n").unwrap();
        re.replace(content, "").to_string()
    }

    fn escape_xml(&self, s: &str) -> String {
        s.replace('&', "&amp;")
         .replace('<', "&lt;")
         .replace('>', "&gt;")
    }
}
