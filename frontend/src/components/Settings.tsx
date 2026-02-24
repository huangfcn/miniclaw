import { useState, useEffect } from "react";
import { invoke } from "@tauri-apps/api/core";
import { Save, RefreshCw, AlertCircle, CheckCircle2 } from "lucide-react";

const Settings = () => {
  const [config, setConfig] = useState("");
  const [isLoading, setIsLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [saveStatus, setSaveStatus] = useState<"idle" | "saving" | "saved" | "error">("idle");

  const loadConfig = async () => {
    setIsLoading(true);
    setError(null);
    try {
      const data = await invoke("read_config");
      setConfig(data as string);
    } catch (err) {
      console.error(err);
      setError("Failed to load config.yaml. Make sure the backend directory exists.");
    } finally {
      setIsLoading(false);
    }
  };

  const handleSave = async () => {
    setSaveStatus("saving");
    try {
      await invoke("save_config", { content: config });
      setSaveStatus("saved");
      setTimeout(() => setSaveStatus("idle"), 2000);
    } catch (err) {
      console.error(err);
      setSaveStatus("error");
    }
  };

  useEffect(() => {
    loadConfig();
  }, []);

  return (
    <div className="flex-1 flex flex-col p-8 bg-[#0b0b0b] space-y-8 max-w-5xl mx-auto w-full">
      <div className="flex items-center justify-between border-b border-gray-800 pb-6">
        <div className="space-y-1">
          <h2 className="text-3xl font-bold tracking-tight text-white flex items-center space-x-3">
            <span className="bg-indigo-600/10 text-indigo-400 p-2 rounded-xl border border-indigo-600/20">
              <RefreshCw size={24} />
            </span>
            <span>Configuration</span>
          </h2>
          <p className="text-gray-400 text-sm font-medium">Refine your engine parameters and model endpoints.</p>
        </div>
        <div className="flex items-center space-x-3">
           <button 
            onClick={loadConfig} 
            className="flex items-center space-x-2 px-4 py-2 bg-gray-800/80 hover:bg-gray-800 border border-gray-700 rounded-xl transition-all font-semibold text-gray-200"
          >
            <RefreshCw size={18} className={isLoading ? "animate-spin" : ""} />
            <span>Reload</span>
          </button>
          <button 
            onClick={handleSave} 
            disabled={saveStatus === "saving" || isLoading}
            className={`flex items-center space-x-2 px-6 py-2 rounded-xl transition-all font-bold shadow-lg shadow-indigo-600/10 ${
              saveStatus === "saved" 
                ? "bg-emerald-600 text-white" 
                : "bg-indigo-600 text-white hover:bg-indigo-500"
            }`}
          >
            {saveStatus === "saved" ? <CheckCircle2 size={18} /> : <Save size={18} />}
            <span>{saveStatus === "saving" ? "Saving..." : saveStatus === "saved" ? "Saved!" : "Save Changes"}</span>
          </button>
        </div>
      </div>

      <div className="flex-1 relative group">
        <div className="absolute inset-0 bg-indigo-600/5 rounded-2xl blur-2xl opacity-0 group-hover:opacity-100 transition-opacity" />
        <textarea
          className="w-full h-full bg-[#141414] border border-gray-800 focus:border-indigo-500 rounded-2xl p-6 font-mono text-sm leading-relaxed text-indigo-100/90 outline-none transition-all shadow-xl resize-none group-hover:bg-[#181818]"
          value={config}
          onChange={(e) => setConfig(e.target.value)}
          placeholder="# Loading configuration..."
          spellCheck={false}
        />
        {isLoading && (
          <div className="absolute inset-0 flex items-center justify-center bg-black/60 backdrop-blur-sm rounded-2xl z-10">
            <RefreshCw className="w-10 h-10 animate-spin text-indigo-500" />
          </div>
        )}
      </div>

      {error && (
        <div className="p-4 bg-rose-500/10 border border-rose-500/20 rounded-xl flex items-center space-x-3 text-rose-500 font-medium animate-in slide-in-from-bottom-2">
          <AlertCircle size={20} />
          <span>{error}</span>
        </div>
      )}
      
      <div className="grid grid-cols-1 md:grid-cols-2 gap-6 pb-4">
        <div className="p-6 bg-[#141414] border border-gray-800 rounded-2xl space-y-3 hover:border-gray-700 transition-colors shadow-lg">
          <h4 className="text-gray-100 font-bold uppercase tracking-widest text-[11px]">Server Mode</h4>
          <p className="text-gray-400 text-sm leading-relaxed font-medium">Default listening port is 9000. Ensure it's not occupied by other services.</p>
        </div>
        <div className="p-6 bg-[#141414] border border-gray-800 rounded-2xl space-y-3 hover:border-gray-700 transition-colors shadow-lg">
          <h4 className="text-gray-100 font-bold uppercase tracking-widest text-[11px]">LLM Provider</h4>
          <p className="text-gray-400 text-sm leading-relaxed font-medium">Supports OpenAI, local Llama.cpp, and Ollama compatible endpoints.</p>
        </div>
      </div>
    </div>
  );
};

export default Settings;
