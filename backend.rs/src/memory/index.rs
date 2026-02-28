use serde::{Deserialize, Serialize};
use std::fs::{self, OpenOptions};
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use chrono::{DateTime, Utc};
use std::collections::HashMap;

use tantivy::schema::*;
use tantivy::collector::TopDocs;
use tantivy::query::QueryParser;
use tantivy::TantivyDocument;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SearchResult {
    pub id: String,
    pub path: String,
    pub start_line: usize,
    pub end_line: usize,
    pub text: String,
    pub score: f32,
    pub source: String,
}

#[derive(Clone, Serialize, Deserialize)]
struct VectorData {
    id: String,
    embedding: Vec<f32>,
}

pub struct MemoryIndex {
    index_dir: PathBuf,
    dimension: usize,
    vectors: Vec<VectorData>,
    tantivy_index: tantivy::Index,
    schema: Schema,
}

impl MemoryIndex {
    pub fn new(workspace: &str, dimension: usize) -> Self {
        let dir = Path::new(workspace).join("index");
        fs::create_dir_all(&dir).unwrap_or_default();
        
        let mut vectors = Vec::new();
        let vec_path = dir.join("vectors.jsonl");
        if vec_path.exists() {
            if let Ok(file) = fs::File::open(&vec_path) {
                let reader = BufReader::new(file);
                for line in reader.lines().flatten() {
                    if let Ok(vd) = serde_json::from_str::<VectorData>(&line) {
                        vectors.push(vd);
                    }
                }
            }
        }

        let mut schema_builder = Schema::builder();
        schema_builder.add_text_field("id", STRING | STORED);
        schema_builder.add_text_field("path", STRING | STORED);
        schema_builder.add_u64_field("start_line", STORED);
        schema_builder.add_u64_field("end_line", STORED);
        schema_builder.add_text_field("text", TEXT | STORED);
        schema_builder.add_text_field("source", STRING | STORED);
        schema_builder.add_text_field("timestamp", STRING | STORED);
        let schema = schema_builder.build();

        let tantivy_dir = dir.join("tantivy");
        fs::create_dir_all(&tantivy_dir).unwrap_or_default();
        
        let tantivy_index = tantivy::Index::open_or_create(
            tantivy::directory::MmapDirectory::open(&tantivy_dir).unwrap(),
            schema.clone()
        ).unwrap();

        Self {
            index_dir: dir,
            dimension,
            vectors,
            tantivy_index,
            schema,
        }
    }

    pub fn add_document(
        &mut self,
        id: String,
        path: String,
        start_line: usize,
        end_line: usize,
        text: String,
        embedding: Vec<f32>,
        source: String,
    ) {
        // 1. Vector
        let mut emb = embedding;
        if emb.len() != self.dimension {
            emb.resize(self.dimension, 0.0);
        }
        let norm: f32 = emb.iter().map(|v| v * v).sum::<f32>().sqrt();
        if norm > 1e-6 {
            for v in &mut emb {
                *v /= norm;
            }
        }
        
        let vd = VectorData { id: id.clone(), embedding: emb };
        self.vectors.push(vd.clone());
        
        let vec_path = self.index_dir.join("vectors.jsonl");
        if let Ok(mut file) = OpenOptions::new().create(true).append(true).open(&vec_path) {
            let _ = writeln!(file, "{}", serde_json::to_string(&vd).unwrap());
        }

        // 2. Tantivy
        let mut index_writer = self.tantivy_index.writer(50_000_000).unwrap();
        let id_field = self.schema.get_field("id").unwrap();
        let path_field = self.schema.get_field("path").unwrap();
        let start_line_field = self.schema.get_field("start_line").unwrap();
        let end_line_field = self.schema.get_field("end_line").unwrap();
        let text_field = self.schema.get_field("text").unwrap();
        let source_field = self.schema.get_field("source").unwrap();
        let timestamp_field = self.schema.get_field("timestamp").unwrap();

        let timestamp = Utc::now().to_rfc3339();
        
        let mut doc = TantivyDocument::default();
        doc.add_text(id_field, &id);
        doc.add_text(path_field, &path);
        doc.add_u64(start_line_field, start_line as u64);
        doc.add_u64(end_line_field, end_line as u64);
        doc.add_text(text_field, &text);
        doc.add_text(source_field, &source);
        doc.add_text(timestamp_field, &timestamp);

        index_writer.add_document(doc).unwrap();
        index_writer.commit().unwrap();
    }

    pub fn search(
        &mut self,
        query: &str,
        query_embedding: &[f32],
        top_k: usize,
    ) -> Vec<SearchResult> {
        let mut vector_scores = HashMap::new();
        let mut text_scores = HashMap::new();
        let mut metadata = HashMap::new();
        
        let candidate_k = top_k * 4;
        
        // 1. Vector search (exact inner product)
        let mut q_emb = query_embedding.to_vec();
        if q_emb.len() == self.dimension {
            let norm: f32 = q_emb.iter().map(|v| v * v).sum::<f32>().sqrt();
            if norm > 1e-6 {
                for v in &mut q_emb {
                    *v /= norm;
                }
            }
            
            let mut scored_vectors: Vec<(String, f32)> = self.vectors.iter().map(|vd| {
                let dot: f32 = vd.embedding.iter().zip(q_emb.iter()).map(|(a, b)| a * b).sum();
                (vd.id.clone(), dot)
            }).collect();
            
            scored_vectors.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap_or(std::cmp::Ordering::Equal));
            for (id, score) in scored_vectors.into_iter().take(candidate_k) {
                vector_scores.insert(id, score);
            }
        }

        // 2. Tantivy keyword search
        let reader = self.tantivy_index.reader().unwrap();
        let searcher = reader.searcher();
        let text_field = self.schema.get_field("text").unwrap();
        
        if !query.is_empty() {
            let query_parser = QueryParser::for_index(&self.tantivy_index, vec![text_field]);
            if let Ok(q) = query_parser.parse_query(query) {
                if let Ok(top_docs) = searcher.search(&q, &TopDocs::with_limit(candidate_k)) {
                    for (i, (_score, doc_address)) in top_docs.iter().enumerate() {
                        if let Ok(doc) = searcher.doc::<TantivyDocument>(*doc_address) {
                            let id_field = self.schema.get_field("id").unwrap();
                            if let Some(id_val) = doc.get_first(id_field) {
                                let id_str = id_val.as_str().unwrap_or("").to_string();
                                
                                // Reciprocal rank scoring for text matches
                                let text_score = 1.0 / (1.0 + i as f32);
                                text_scores.insert(id_str.clone(), text_score);

                                let path_field = self.schema.get_field("path").unwrap();
                                let start_line_field = self.schema.get_field("start_line").unwrap();
                                let end_line_field = self.schema.get_field("end_line").unwrap();
                                let source_field = self.schema.get_field("source").unwrap();
                                //let _timestamp_field = self.schema.get_field("timestamp").unwrap();

                                let path = doc.get_first(path_field).and_then(|v| v.as_str()).unwrap_or("").to_string();
                                let start_line = doc.get_first(start_line_field).and_then(|v| v.as_u64()).unwrap_or(0) as usize;
                                let end_line = doc.get_first(end_line_field).and_then(|v| v.as_u64()).unwrap_or(0) as usize;
                                let text = doc.get_first(text_field).and_then(|v| v.as_str()).unwrap_or("").to_string();
                                let source = doc.get_first(source_field).and_then(|v| v.as_str()).unwrap_or("").to_string();

                                metadata.insert(id_str.clone(), SearchResult {
                                    id: id_str.clone(),
                                    path,
                                    start_line,
                                    end_line,
                                    text,
                                    score: 0.0,
                                    source,
                                });
                            }
                        }
                    }
                }
            }
        }

        // We need to fetch metadata for IDs found only via vector search
        let id_field = self.schema.get_field("id").unwrap();
        for id in vector_scores.keys() {
            if !metadata.contains_key(id) {
                let term = tantivy::Term::from_field_text(id_field, id);
                let q = tantivy::query::TermQuery::new(term, tantivy::schema::IndexRecordOption::Basic);
                if let Ok(top_docs) = searcher.search(&q, &TopDocs::with_limit(1)) {
                    if let Some((_, doc_addr)) = top_docs.first() {
                        if let Ok(doc) = searcher.doc::<TantivyDocument>(*doc_addr) {
                            let path_field = self.schema.get_field("path").unwrap();
                            let start_line_field = self.schema.get_field("start_line").unwrap();
                            let end_line_field = self.schema.get_field("end_line").unwrap();
                            let source_field = self.schema.get_field("source").unwrap();
                            
                            let path = doc.get_first(path_field).and_then(|v| v.as_str()).unwrap_or("").to_string();
                            let start_line = doc.get_first(start_line_field).and_then(|v| v.as_u64()).unwrap_or(0) as usize;
                            let end_line = doc.get_first(end_line_field).and_then(|v| v.as_u64()).unwrap_or(0) as usize;
                            let text = doc.get_first(text_field).and_then(|v| v.as_str()).unwrap_or("").to_string();
                            let source = doc.get_first(source_field).and_then(|v| v.as_str()).unwrap_or("").to_string();
                            
                            metadata.insert(id.clone(), SearchResult {
                                id: id.clone(),
                                path,
                                start_line,
                                end_line,
                                text,
                                score: 0.0,
                                source,
                            });
                        }
                    }
                }
            }
        }

        let vec_weight = 0.7;
        let text_weight = 0.3;
        let mut results = Vec::new();

        for (id, mut meta) in metadata {
            let v = vector_scores.get(&id).copied().unwrap_or(0.0);
            let t = text_scores.get(&id).copied().unwrap_or(0.0);
            
            let mut final_score = (vec_weight * v) + (text_weight * t);
            
            // Apply temporal decay if it's a daily log (memory)
            if meta.source == "memory" {
                let re = regex::Regex::new(r"(\d{4})-(\d{2})-(\d{2})").unwrap();
                if let Some(caps) = re.captures(&meta.path) {
                    if let Ok(ts) = DateTime::parse_from_rfc3339(&format!("{}-{}-{}T00:00:00Z", &caps[1], &caps[2], &caps[3])) {
                        let age_days = (Utc::now() - ts.with_timezone(&Utc)).num_days() as f32;
                        if age_days > 0.0 {
                            let lambda = (2.0_f32).ln() / 30.0;
                            final_score *= (-lambda * age_days).exp();
                        }
                    }
                }
            }

            meta.score = final_score;
            results.push(meta);
        }

        results.sort_by(|a, b| b.score.partial_cmp(&a.score).unwrap_or(std::cmp::Ordering::Equal));
        if results.len() > top_k {
            results.truncate(top_k);
        }
        
        results
    }
}
