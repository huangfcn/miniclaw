import { useEffect } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { Play, Square, Activity, Shield, Cpu, Zap, Globe, Folder, FileCode, CheckCircle2, Box } from "lucide-react";
import { useAppContext } from "../contexts/AppContext";

interface MonitoringProps {
  isBackendRunning: boolean;
  onStatusChange: () => void;
}

const Monitoring = ({ isBackendRunning, onStatusChange }: MonitoringProps) => {
  const { appState, setAppState } = useAppContext();
  const { isLoading, error, startTime, engineInfo } = appState.monitoring;

  useEffect(() => {
    let interval: number;
    if (isBackendRunning && startTime) {
      interval = window.setInterval(() => {
        const diff = Math.floor((Date.now() - startTime) / 1000);
        const hours = Math.floor(diff / 3600).toString().padStart(2, "0");
        const minutes = Math.floor((diff % 3600) / 60).toString().padStart(2, "0");
        const seconds = (diff % 60).toString().padStart(2, "0");
        setAppState(prev => ({ ...prev, monitoring: { ...prev.monitoring, engineInfo: { ...prev.monitoring.engineInfo, uptime: `${hours}:${minutes}:${seconds}` } } }));
      }, 1000);
    }
    return () => clearInterval(interval);
  }, [isBackendRunning, startTime]);

  useEffect(() => {
    const unlisten = listen<string>("backend-log", (event) => {
      const log = event.payload;

      // Basic log parsing for engine info
      if (log.includes("Config file:")) {
        const path = log.split("Config file:")[1].trim();
        setAppState(prev => ({ ...prev, monitoring: { ...prev.monitoring, engineInfo: { ...prev.monitoring.engineInfo, configPath: path } } }));
      }
      if (log.includes("Memory workspace:")) {
        const path = log.split("Memory workspace:")[1].trim();
        setAppState(prev => ({ ...prev, monitoring: { ...prev.monitoring, engineInfo: { ...prev.monitoring.engineInfo, workspace: path } } }));
      }
      if (log.includes("Agent initialized: model=")) {
        const modelPart = log.split("model=")[1].split(" ")[0];
        setAppState(prev => ({ ...prev, monitoring: { ...prev.monitoring, engineInfo: { ...prev.monitoring.engineInfo, model: modelPart } } }));
      }
      if (log.includes("Available skills: [")) {
        const skillsString = log.split("Available skills: [")[1].split("]")[0];
        const skills = skillsString.split(", ").map(s => s.trim());
        setAppState(prev => ({ ...prev, monitoring: { ...prev.monitoring, engineInfo: { ...prev.monitoring.engineInfo, skills } } }));
      }
      if (log.includes("Initializing FiberPool with")) {
        const count = parseInt(log.split("with")[1].split("nodes")[0].trim());
        setAppState(prev => ({ ...prev, monitoring: { ...prev.monitoring, engineInfo: { ...prev.monitoring.engineInfo, fiberNodes: count } } }));
      }
      if (log.includes("Starting miniclaw Backend")) {
        setAppState(prev => ({ ...prev, monitoring: { ...prev.monitoring, startTime: Date.now() } }));
      }
    });

    return () => {
      unlisten.then((fn) => fn());
    };
  }, []);

  const startBackend = async () => {
    setAppState(prev => ({ ...prev, monitoring: { ...prev.monitoring, isLoading: true, error: null } }));
    try {
      await invoke("start_backend");
      onStatusChange();
    } catch (err) {
      setAppState(prev => ({ ...prev, monitoring: { ...prev.monitoring, error: "Failed to start backend: " + err } }));
    } finally {
      setAppState(prev => ({ ...prev, monitoring: { ...prev.monitoring, isLoading: false } }));
    }
  };

  const stopBackend = async () => {
    setAppState(prev => ({ ...prev, monitoring: { ...prev.monitoring, isLoading: true, error: null } }));
    try {
      await invoke("stop_backend");
      setAppState(prev => ({ ...prev, monitoring: { ...prev.monitoring, startTime: null, engineInfo: { ...prev.monitoring.engineInfo, uptime: "00:00:00" } } }));
      onStatusChange();
    } catch (err) {
      setAppState(prev => ({ ...prev, monitoring: { ...prev.monitoring, error: "Failed to stop backend: " + err } }));
    } finally {
      setAppState(prev => ({ ...prev, monitoring: { ...prev.monitoring, isLoading: false } }));
    }
  };

  const stats = [
    { icon: Cpu, label: "Active Fibers", value: isBackendRunning ? engineInfo.fiberNodes : "0", color: "text-blue-500" },
    { icon: Zap, label: "System Uptime", value: isBackendRunning ? engineInfo.uptime : "00:00:00", color: "text-amber-500" },
    { icon: Globe, label: "Endpoint Status", value: isBackendRunning ? "Online" : "Offline", color: "text-emerald-500" },
  ];

  return (
    <div className="flex-1 p-10 bg-[#0b0b0b] space-y-10 max-w-6xl mx-auto w-full overflow-y-auto">
      <div className="flex items-center justify-between border-b border-gray-800 pb-8">
        <div className="space-y-1">
          <h2 className="text-4xl font-black tracking-tighter text-white flex items-center space-x-3">
            <span className="bg-indigo-600/10 text-indigo-400 p-2.5 rounded-2xl border border-indigo-600/20 shadow-lg shadow-indigo-600/5">
              <Activity size={32} />
            </span>
            <span>Monitoring</span>
          </h2>
          <p className="text-gray-400 text-sm font-semibold uppercase tracking-widest pl-1">Engine Deployment & Metrics</p>
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
              <span>Shutdown Engine</span>
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

      <div className="grid grid-cols-1 lg:grid-cols-2 gap-8">
        {/* Left Panel: Engine Details */}
        <div className="bg-[#141414] border border-gray-800 rounded-3xl overflow-hidden shadow-2xl flex flex-col">
          <div className="p-6 border-b border-gray-800 bg-[#181818] flex items-center justify-between px-8 text-indigo-400 font-bold tracking-wide uppercase text-xs">
            Engine Configuration
          </div>
          <div className="p-8 space-y-6">
            <div className="flex items-start space-x-4">
              <div className="p-3 bg-gray-900 rounded-2xl border border-gray-800">
                <Cpu size={20} className="text-blue-500" />
              </div>
              <div>
                <div className="text-xs font-bold text-gray-500 uppercase tracking-widest mb-1">Active Model</div>
                <div className="text-lg font-bold text-white">{engineInfo.model}</div>
              </div>
            </div>
            <div className="flex items-start space-x-4">
              <div className="p-3 bg-gray-900 rounded-2xl border border-gray-800">
                <Folder size={20} className="text-amber-500" />
              </div>
              <div className="overflow-hidden">
                <div className="text-xs font-bold text-gray-500 uppercase tracking-widest mb-1">Workspace Path</div>
                <div className="text-sm font-medium text-gray-300 truncate">{engineInfo.workspace}</div>
              </div>
            </div>
            <div className="flex items-start space-x-4">
              <div className="p-3 bg-gray-900 rounded-2xl border border-gray-800">
                <FileCode size={20} className="text-indigo-500" />
              </div>
              <div className="overflow-hidden">
                <div className="text-xs font-bold text-gray-500 uppercase tracking-widest mb-1">Configuration Source</div>
                <div className="text-sm font-medium text-gray-300 truncate">{engineInfo.configPath}</div>
              </div>
            </div>
          </div>
        </div>

        {/* Right Panel: Skill Ecosystem */}
        <div className="bg-[#141414] border border-gray-800 rounded-3xl overflow-hidden shadow-2xl flex flex-col">
          <div className="p-6 border-b border-gray-800 bg-[#181818] flex items-center justify-between px-8 text-emerald-400 font-bold tracking-wide uppercase text-xs">
            Skill Ecosystem
          </div>
          <div className="p-8 flex-1">
            {engineInfo.skills.length > 0 ? (
              <div className="grid grid-cols-2 sm:grid-cols-3 gap-3">
                {engineInfo.skills.map(skill => (
                  <div key={skill} className="flex items-center space-x-2 p-3 bg-gray-900 border border-gray-800 rounded-xl hover:border-emerald-500/30 transition-colors group">
                    <CheckCircle2 size={14} className="text-emerald-500 opacity-60 group-hover:opacity-100" />
                    <span className="text-xs font-medium text-gray-300">{skill}</span>
                  </div>
                ))}
              </div>
            ) : (
              <div className="flex flex-col items-center justify-center h-full text-gray-600 space-y-3 py-10">
                <Box size={40} className="opacity-20" />
                <p className="text-[10px] font-bold uppercase tracking-widest">No active skills discovered</p>
              </div>
            )}
          </div>
        </div>
      </div>

      {error && (
        <div className="p-5 bg-rose-500/10 border border-rose-500/20 rounded-2xl flex items-center space-x-4 text-rose-500 font-bold shadow-lg animate-in fade-in zoom-in-95">
          <Square size={20} className="fill-rose-500/20" />
          <span>{error}</span>
        </div>
      )}

      {/* Shutdown Notice */}
      {!isBackendRunning && !isLoading && (
        <div className="flex items-center justify-center p-12 border-2 border-dashed border-gray-800 rounded-[2.5rem] bg-gray-900/10">
          <div className="text-center space-y-4">
            <Shield size={48} className="mx-auto text-gray-700" strokeWidth={1} />
            <p className="text-gray-500 font-bold tracking-widest uppercase text-xs">Engine offline. Launch to activate telemetry.</p>
          </div>
        </div>
      )}
    </div>
  );
};

export default Monitoring;
