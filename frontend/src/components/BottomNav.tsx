import { MessageSquare, Activity, Settings } from "lucide-react";

export type Tab = "chat" | "monitoring" | "settings";

const BottomNav = ({
  active,
  onChange,
}: {
  active: Tab;
  onChange: (t: Tab) => void;
}) => {
  const items: { id: Tab; label: string; icon: typeof MessageSquare }[] = [
    { id: "chat", label: "Chat", icon: MessageSquare },
    { id: "monitoring", label: "Status", icon: Activity },
    { id: "settings", label: "Settings", icon: Settings },
  ];

  return (
    <nav
      className="md:hidden flex items-stretch border-t border-gray-800 bg-[#0b0b0b]/95 backdrop-blur"
      style={{ paddingBottom: "env(safe-area-inset-bottom)" }}
    >
      {items.map(({ id, label, icon: Icon }) => (
        <button
          key={id}
          onClick={() => onChange(id)}
          className={`flex-1 flex flex-col items-center gap-1 py-2.5 transition-colors ${
            active === id ? "text-indigo-400" : "text-gray-600"
          }`}
        >
          <Icon size={20} strokeWidth={active === id ? 2.2 : 1.8} />
          <span className="text-[10px] font-bold uppercase tracking-wider">{label}</span>
        </button>
      ))}
    </nav>
  );
};

export default BottomNav;
