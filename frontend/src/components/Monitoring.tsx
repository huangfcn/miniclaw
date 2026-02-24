import { useState, useEffect, useRef } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { Play, Square, Activity, Terminal, Shield, Cpu, Zap, Radio, Loader2 } from "lucide-react";

interface MonitoringProps {
  isBackendRunning: boolean;
  onStatusChange: () => void;
}

const Monitoring = ({ isBackendRunning, onStatusChange }: MonitoringProps) => {
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [logs, setLogs] = useState<string[]>([]);
  const logEndRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const unlisten = listen<string>("backend-log", (event) => {
      setLogs((prev) => [...prev, event.payload].slice(-100)); // Keep last 100 logs
    });

    return () => {
      unlisten.then((fn) => fn());
    };
  }, []);

  useEffect(() => {
    if (logEndRef.current) {
      logEndRef.current.scrollIntoView({ behavior: "smooth" });
    }
  }, [logs]);

  const startBackend = async () => {
    setIsLoading(true);
    setError(null);
    setLogs([]); // Clear logs on start
    try {
      await invoke("start_backend");
      onStatusChange();
    } catch (err) {
      setError("Failed to start backend: " + err);
    } finally {
      setIsLoading(false);
    }
  };

  const stopBackend = async () => {
    setIsLoading(true);
    setError(null);
    try {
      await invoke("stop_backend");
      onStatusChange();
    } catch (err) {
      setError("Failed to stop backend: " + err);
    } finally {
      setIsLoading(false);
    }
  };

  const stats = [
    { icon: Cpu, label: "Core Utilization", value: isBackendRunning ? "24%" : "0%", color: "text-blue-500" },
    { icon: Zap, label: "Memory Overhead", value: isBackendRunning ? "1.2 GB" : "0 GB", color: "text-amber-500" },
    { icon: Radio, label: "Network Activity", value: isBackendRunning ? "Stable" : "Idle", color: "text-emerald-500" },
  ];

  return (
    <div className="flex-1 p-10 bg-[#0b0b0b] space-y-10 max-w-6xl mx-auto w-full">
      <div className="flex items-center justify-between border-b border-gray-800 pb-8">
        <div className="space-y-1">
          <h2 className="text-4xl font-black tracking-tighter text-white flex items-center space-x-3">
            <span className="bg-indigo-600/10 text-indigo-400 p-2.5 rounded-2xl border border-indigo-600/20 shadow-lg shadow-indigo-600/5">
              <Activity size={32} />
            </span>
            <span>Dashboard</span>
          </h2>
          <p className="text-gray-400 text-sm font-semibold uppercase tracking-widest pl-1">Live Telemetry & Engine Control</p>
        </div>

        <div className="flex items-center space-x-4">
          {!isBackendRunning ? (
            <button
              onClick={startBackend}
              disabled={isLoading}
              className="flex items-center space-x-2 px-8 py-3 bg-emerald-600 hover:bg-emerald-500 text-white rounded-2xl transition-all font-bold shadow-xl shadow-emerald-600/10 hover:scale-105 active:scale-95"
            >
              <Play size={20} fill="currentColor" />
              <span>Launch Engine</span>
            </button>
          ) : (
            <button
              onClick={stopBackend}
              disabled={isLoading}
              className="flex items-center space-x-2 px-8 py-3 bg-rose-600 hover:bg-rose-500 text-white rounded-2xl transition-all font-bold shadow-xl shadow-rose-600/10 hover:scale-105 active:scale-95"
            >
              <Square size={20} fill="currentColor" />
              <span>Kill Process</span>
            </button>
          )}
        </div>
      </div>

      <div className="grid grid-cols-1 md:grid-cols-3 gap-8">
        {stats.map((stat, i) => {
          const Icon = stat.icon;
          return (
            <div key={i} className="p-8 bg-[#141414] border border-gray-800 rounded-3xl space-y-4 hover:border-gray-700 transition-all hover:translate-y-[-4px] shadow-2xl relative group overflow-hidden">
              <div className="absolute top-0 right-0 p-4 opacity-5 group-hover:opacity-20 transition-opacity">
                <Icon size={64} />
              </div>
              <div className="flex items-center space-x-3">
                <Icon className={stat.color} size={20} />
                <span className="text-gray-400 text-xs font-bold uppercase tracking-widest">{stat.label}</span>
              </div>
              <div className="text-3xl font-black text-white tabular-nums">{stat.value}</div>
            </div>
          );
        })}
      </div>

      <div className="bg-[#141414] border border-gray-800 rounded-3xl overflow-hidden shadow-2xl">
        <div className="p-6 border-b border-gray-800 bg-[#181818] flex items-center justify-between px-8">
          <div className="flex items-center space-x-3">
            <Terminal size={18} className="text-indigo-400" />
            <h3 className="font-bold text-gray-200 tracking-wide uppercase text-xs">Runtime Diagnostics</h3>
          </div>
          <div className="flex items-center space-x-2">
            <div className={`w-2 h-2 rounded-full ${isBackendRunning ? 'bg-emerald-500' : 'bg-gray-600'}`} />
            <span className="text-[10px] font-bold text-gray-500 uppercase tracking-widest">
              {isBackendRunning ? 'Processing' : 'Standby'}
            </span>
          </div>
        </div>
        <div className="p-10 font-mono text-sm bg-[#0a0a0a] min-h-[400px] max-h-[600px] overflow-y-auto custom-scrollbar">
          {logs.length > 0 ? (
            <div className="space-y-1">
              {logs.map((log, i) => (
                <div key={i} className={`${log.includes('[ERROR]') ? 'text-rose-400' : log.includes('[SYSTEM]') ? 'text-amber-400' : 'text-emerald-500/90'}`}>
                  {log}
                </div>
              ))}
              <div ref={logEndRef} />
            </div>
          ) : isBackendRunning ? (
            <div className="flex flex-col items-center justify-center h-full text-indigo-500 animate-pulse pt-20">
              <Loader2 size={32} className="animate-spin mb-4" />
              <p className="font-bold tracking-widest uppercase text-xs">Hooking into process streams...</p>
            </div>
          ) : (
            <div className="flex flex-col items-center justify-center h-full text-gray-600 space-y-4 pt-10">
              <Shield size={48} strokeWidth={1} />
              <p className="text-center font-bold tracking-widest uppercase text-xs">System in Secure Standby Mode</p>
            </div>
          )}
        </div>
      </div>

      {error && (
        <div className="p-5 bg-rose-500/10 border border-rose-500/20 rounded-2xl flex items-center space-x-4 text-rose-500 font-bold shadow-lg animate-in fade-in zoom-in-95">
          <Square size={20} className="fill-rose-500/20" />
          <span>{error}</span>
        </div>
      )}
    </div>
  );
};

// Add this to your global CSS if not present
// .custom-scrollbar::-webkit-scrollbar { width: 4px; }
// .custom-scrollbar::-webkit-scrollbar-thumb { background: #333; border-radius: 10px; }

export default Monitoring;
