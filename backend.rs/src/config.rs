use std::path::{Path, PathBuf};
use std::fs;
use serde::Deserialize;
use std::env;

#[derive(Debug, Deserialize, Clone)]
pub struct Config {
    #[serde(skip)]
    pub actual_config_path: String,
    // Add other config fields as needed, mirrored from C++ or config.yaml
    pub server: Option<ServerConfig>,
    pub memory: Option<MemoryConfig>,
    pub skills: Option<SkillsConfig>,
}

#[derive(Debug, Deserialize, Clone)]
pub struct ServerConfig {
    pub port: Option<u16>,
    pub threads: Option<usize>,
}

#[derive(Debug, Deserialize, Clone)]
pub struct MemoryConfig {
    pub workspace: Option<String>,
}

#[derive(Debug, Deserialize, Clone)]
pub struct SkillsConfig {
    pub path: Option<String>,
}

impl Config {
    pub fn load() -> Self {
        let default_path = "config/config.yaml";
        let mut p = PathBuf::from(default_path);
        let mut found = false;

        // 1. Check relative to CWD or EXE
        if p.exists() {
            found = true;
        } else {
            let exe_dir = env::current_exe().map(|p| p.parent().unwrap().to_path_buf()).unwrap_or_else(|_| PathBuf::from("."));
            if exe_dir.join(default_path).exists() {
                p = exe_dir.join(default_path);
                found = true;
            } else if PathBuf::from("..").join(default_path).exists() {
                p = PathBuf::from("..").join(default_path);
                found = true;
            }
        }

        // 2. Check system config directory
        if !found {
            let system_config = PathBuf::from(Self::get_default_workspace()).join("config.yaml");
            if system_config.exists() {
                p = system_config;
                found = true;
            }
        }

        // 3. Try to bootstrap from template if not found
        if !found {
            Self::ensure_config_exists();
            let system_config = PathBuf::from(Self::get_default_workspace()).join("config.yaml");
            if system_config.exists() {
                p = system_config;
                found = true;
            }
        }

        if !found {
            tracing::warn!("Config file not found. Using defaults.");
            return Config {
                actual_config_path: "".to_string(),
                server: None,
                memory: None,
                skills: None,
            };
        }

        let content = fs::read_to_string(&p).unwrap_or_default();
        let mut config: Config = serde_yaml::from_str(&content).unwrap_or_else(|e| {
            tracing::error!("Failed to parse config: {}", e);
            Config {
                actual_config_path: p.to_string_lossy().to_string(),
                server: None,
                memory: None,
                skills: None,
            }
        });
        config.actual_config_path = fs::canonicalize(&p).unwrap_or(p).to_string_lossy().to_string();
        config
    }

    pub fn get_default_workspace() -> String {
        let home = if cfg!(windows) {
            env::var("USERPROFILE").or_else(|_| env::var("HOME")).ok()
        } else {
            env::var("HOME").ok()
        };

        if let Some(h) = home {
            PathBuf::from(h).join(".miniclaw").to_string_lossy().to_string()
        } else {
            ".miniclaw".to_string()
        }
    }

    pub fn memory_workspace(&self) -> String {
        if let Ok(ws) = env::var("WORKSPACE_DIR") {
            return fs::canonicalize(ws).unwrap_or_else(|_| PathBuf::from(".")).to_string_lossy().to_string();
        }

        if let Some(ref mem) = self.memory {
            if let Some(ref ws) = mem.workspace {
                let p = PathBuf::from(ws);
                if p.is_relative() {
                    let config_dir = Path::new(&self.actual_config_path).parent().unwrap_or(Path::new("."));
                    return fs::canonicalize(config_dir.join(p)).unwrap_or_else(|_| PathBuf::from(".")).to_string_lossy().to_string();
                }
                return fs::canonicalize(p).unwrap_or_else(|_| PathBuf::from(".")).to_string_lossy().to_string();
            }
        }

        fs::canonicalize(Self::get_default_workspace()).unwrap_or_else(|_| PathBuf::from(Self::get_default_workspace())).to_string_lossy().to_string()
    }

    pub fn skills_path(&self) -> String {
        let p = self.skills.as_ref().and_then(|s| s.path.as_ref()).map(|s| s.as_str()).unwrap_or("skills");
        let path = PathBuf::from(p);
        if path.is_relative() {
            PathBuf::from(self.memory_workspace()).join(path).to_string_lossy().to_string()
        } else {
            p.to_string()
        }
    }

    pub fn ensure_config_exists() {
        let workspace = PathBuf::from(Self::get_default_workspace());
        let target_config = workspace.join("config.yaml");

        if !workspace.exists() {
            let _ = fs::create_dir_all(&workspace);
        }

        // Try to find templates
        let exe_dir = env::current_exe().map(|p| p.parent().unwrap().to_path_buf()).unwrap_or_else(|_| PathBuf::from("."));
        let template_dirs = vec![
            exe_dir.join("config"),
            exe_dir.join("../config"),
            exe_dir.join("../../frontend/miniclaw/config"),
            PathBuf::from("config"),
            PathBuf::from("../config"),
        ];

        if !target_config.exists() {
            for d in &template_dirs {
                let src = d.join("config.yaml");
                if src.exists() {
                    if let Err(e) = fs::copy(&src, &target_config) {
                        tracing::error!("Failed to copy template config: {}", e);
                    } else {
                        tracing::info!("Initial config copied from {} to {}", src.display(), target_config.display());
                        break;
                    }
                }
            }
        }

        // Copy bootstrap files
        let bootstrap_files = vec!["AGENTS.md", "SOUL.md", "USER.md", "TOOLS.md", "IDENTITY.md"];
        for fname in bootstrap_files {
            let target_file = workspace.join(fname);
            if !target_file.exists() {
                for d in &template_dirs {
                    let src_candidates = vec![d.join(fname), d.join("..").join(fname)];
                    let mut copied = false;
                    for src in src_candidates {
                        if src.exists() {
                            if let Err(e) = fs::copy(&src, &target_file) {
                                tracing::error!("Failed to copy bootstrap file {}: {}", fname, e);
                            } else {
                                tracing::info!("Bootstrap file copied from {} to {}", src.display(), target_file.display());
                                copied = true;
                                break;
                            }
                        }
                    }
                    if copied { break; }
                }
            }
        }
    }
}
