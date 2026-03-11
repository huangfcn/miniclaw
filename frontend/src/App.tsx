import { useState, useEffect } from "react";
import Sidebar from "./components/Sidebar";
import Chat from "./components/Chat";
import Settings from "./components/Settings";
import Monitoring from "./components/Monitoring";
import { AppProvider } from "./contexts/AppContext";

function App() {
  const [activeTab, setActiveTab] = useState("chat");
  const [isBackendRunning, setIsBackendRunning] = useState(false);

  const checkStatus = async () => {
    try {
      const controller = new AbortController();
      const timeoutId = setTimeout(() => controller.abort(), 1000);
      // Use no-cors to avoid requiring changes to the C++ backend
      await fetch("http://localhost:9000/api/health", {
        mode: 'no-cors',
        signal: controller.signal
      });
      clearTimeout(timeoutId);
      // If fetch didn't throw a network error, the server is reachable!
      setIsBackendRunning(true);
    } catch (error) {
      // If the server is offline, it throws a TypeError "Failed to fetch"
      setIsBackendRunning(false);
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
      
      <AppProvider>
        <main className="flex-1 flex flex-col overflow-hidden">
          {activeTab === "chat" && <Chat isBackendRunning={isBackendRunning} />}
          {activeTab === "monitoring" && <Monitoring isBackendRunning={isBackendRunning} onStatusChange={checkStatus} />}
          {activeTab === "settings" && <Settings />}
        </main>
      </AppProvider>
    </div>
  );
}

export default App;
