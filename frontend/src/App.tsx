import { useState, useEffect } from "react";
import { invoke } from "@tauri-apps/api/core";
import Sidebar from "./components/Sidebar";
import Chat from "./components/Chat";
import Settings from "./components/Settings";
import Monitoring from "./components/Monitoring";
import { Terminal, Settings as SettingsIcon, MessageSquare, Activity } from "lucide-react";

function App() {
  const [activeTab, setActiveTab] = useState("chat");
  const [isBackendRunning, setIsBackendRunning] = useState(false);

  const checkStatus = async () => {
    try {
      const status = await invoke("get_backend_status");
      setIsBackendRunning(status as boolean);
    } catch (error) {
      console.error("Failed to check status", error);
    }
  };

  useEffect(() => {
    checkStatus();
    const interval = setInterval(checkStatus, 2000);
    return () => clearInterval(interval);
  }, []);

  return (
    <div className="flex h-screen w-full bg-[#0b0b0b] text-white">
      <Sidebar 
        activeTab={activeTab} 
        setActiveTab={setActiveTab} 
        isBackendRunning={isBackendRunning}
      />
      
      <main className="flex-1 flex flex-col overflow-hidden">
        {activeTab === "chat" && <Chat isBackendRunning={isBackendRunning} />}
        {activeTab === "monitoring" && <Monitoring isBackendRunning={isBackendRunning} onStatusChange={checkStatus} />}
        {activeTab === "settings" && <Settings />}
      </main>
    </div>
  );
}

export default App;
