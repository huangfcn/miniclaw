import { MessageSquare, Settings as SettingsIcon, Activity } from "lucide-react";

interface SidebarProps {
  activeTab: string;
  setActiveTab: (tab: string) => void;
  isBackendRunning: boolean;
}

const Sidebar = ({ activeTab, setActiveTab, isBackendRunning }: SidebarProps) => {
  const tabs = [
    { id: "chat", icon: MessageSquare, label: "Chat" },
    { id: "monitoring", icon: Activity, label: "Monitoring" },
    { id: "settings", icon: SettingsIcon, label: "Settings" },
  ];

  return (
    <aside className="w-64 bg-[#141414] border-r border-gray-800 flex flex-col p-4 space-y-2">
      <div className="flex items-center space-x-2 mb-8 px-2">
        <div className="w-8 h-8 bg-indigo-600 rounded flex items-center justify-center font-bold text-white shadow-lg">
          M
        </div>
        <span className="text-xl font-semibold tracking-tight">miniclaw</span>
      </div>

      <div className="flex-1 space-y-1">
        {tabs.map((tab) => {
          const Icon = tab.icon;
          return (
            <button
              key={tab.id}
              onClick={() => setActiveTab(tab.id)}
              className={`w-full flex items-center space-x-3 px-3 py-2.5 rounded-lg transition-all ${activeTab === tab.id
                  ? "bg-gray-800 text-white shadow-sm"
                  : "text-gray-400 hover:bg-gray-800/50 hover:text-gray-200"
                }`}
            >
              <Icon size={18} strokeWidth={2} />
              <span className="font-medium">{tab.label}</span>
            </button>
          );
        })}
      </div>

      <div className="mt-auto pt-4 border-t border-gray-800 flex items-center justify-between px-2 py-3">
        <div className="flex items-center space-x-2 overflow-hidden">
          <div className={`w-2.5 h-2.5 rounded-full ${isBackendRunning ? 'bg-emerald-500 animate-pulse' : 'bg-rose-500'}`} />
          <span className="text-sm font-medium text-gray-400 truncate">
            {isBackendRunning ? 'Engine Active' : 'Engine Offline'}
          </span>
        </div>
        {isBackendRunning && (
          <span className="text-[10px] px-1.5 py-0.5 bg-emerald-500/10 text-emerald-500 border border-emerald-500/20 rounded font-bold uppercase tracking-wider">
            Live
          </span>
        )}
      </div>
    </aside>
  );
};

export default Sidebar;
