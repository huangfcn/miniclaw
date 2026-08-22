import { useState, useEffect } from "react";
import { AppProvider } from "./contexts/AppContext";
import Sidebar from "./components/Sidebar";
import BottomNav, { type Tab } from "./components/BottomNav";
import Chat from "./components/Chat";
import Monitoring from "./components/Monitoring";
import Settings from "./components/Settings";
import { isMobile, checkBackendStatus } from "./lib/agentApi";

const App = () => {
  const [activeTab, setActiveTab] = useState<Tab>("chat");
  const [isBackendRunning, setIsBackendRunning] = useState(false);

  useEffect(() => {
    const checkStatus = async () => {
      const status = await checkBackendStatus();
      setIsBackendRunning(status);
    };
    checkStatus();
    const interval = setInterval(checkStatus, isMobile ? 5000 : 2000);
    return () => clearInterval(interval);
  }, []);

  return (
    <AppProvider>
      <div className="h-screen w-screen bg-[#0b0b0b] text-gray-300 font-sans flex overflow-hidden">
        {/* Desktop sidebar */}
        {!isMobile && (
          <Sidebar activeTab={activeTab} setActiveTab={setActiveTab} isBackendRunning={isBackendRunning} />
        )}

        <div className="flex-1 flex flex-col min-w-0">
          {activeTab === "chat" && <Chat isBackendRunning={isBackendRunning} />}
          {activeTab === "monitoring" && (
            <Monitoring isBackendRunning={isBackendRunning} onStatusChange={() => {}} />
          )}
          {activeTab === "settings" && <Settings />}

          {/* Mobile bottom navigation */}
          {isMobile && <BottomNav active={activeTab} onChange={setActiveTab} />}
        </div>
      </div>
    </AppProvider>
  );
};

export default App;
