import { useState, useRef, useEffect } from "react";
import { Send, Bot, User, Wrench, AlertCircle, Loader2 } from "lucide-react";
import { useAppContext } from "../contexts/AppContext";
import { sendAgentMessage, isMobile, type AgentEvent } from "../lib/agentApi";

interface Message {
  id: string;
  role: "user" | "bot";
  content: string;
  activities?: string[]; // tool / status lines shown under the message
  isError?: boolean;
}

const Chat = ({ isBackendRunning }: { isBackendRunning: boolean }) => {
  const { setAppState } = useAppContext();
  const [input, setInput] = useState("");
  const [messages, setMessages] = useState<Message[]>([]);
  const [isSending, setIsSending] = useState(false);
  const messagesEndRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    messagesEndRef.current?.scrollIntoView({ behavior: "smooth" });
  }, [messages]);

  const handleSendMessage = async () => {
    if (!input.trim() || isSending) return;

    const userMessage = input.trim();
    setInput("");
    setIsSending(true);

    const userMsgId = `msg-${Date.now()}`;
    const botMsgId = `msg-${Date.now()}-bot`;
    setMessages(prev => [
      ...prev,
      { id: userMsgId, role: "user", content: userMessage },
      { id: botMsgId, role: "bot", content: "", activities: [] }
    ]);

    const onEvent = (e: AgentEvent) => {
      setMessages(prev => {
        const next = [...prev];
        const i = next.findIndex(m => m.id === botMsgId);
        if (i === -1) return prev;
        const msg = { ...next[i], activities: [...(next[i].activities ?? [])] };
        switch (e.type) {
          case "token":
            msg.content += e.content;
            break;
          case "status":
            if (e.content) msg.activities.push(`⚡ ${e.content}`);
            break;
          case "tool_start":
            msg.activities.push(`🔧 ${e.content}`);
            break;
          case "tool_end":
            // Keep it quiet: tool output can be huge. Just mark completion.
            msg.activities.push(`✓ ${e.content.split("\n")[0].slice(0, 80)}`);
            break;
          case "error":
            msg.isError = true;
            if (e.content && !msg.content) msg.content = e.content;
            else if (e.content) msg.activities.push(`⚠ ${e.content}`);
            break;
          case "done":
            break;
        }
        next[i] = msg;
        return next;
      });
    };

    try {
      await sendAgentMessage("main", userMessage, onEvent);
    } catch (err) {
      setMessages(prev => {
        const next = [...prev];
        const i = next.findIndex(m => m.id === botMsgId);
        if (i !== -1) {
          next[i] = { ...next[i], isError: true, content: `Connection error: ${err}` };
        }
        return next;
      });
    } finally {
      setIsSending(false);
      setAppState(prev => ({
        ...prev,
        chat: {
          ...prev.chat,
          lastMessage: userMessage,
          lastResponse: "", // streamed into messages directly
          lastError: null,
          timestamp: new Date().toISOString()
        }
      }));
    }
  };

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      handleSendMessage();
    }
  };

  return (
    <div className="flex-1 flex flex-col bg-[#0b0b0b] relative overflow-hidden">
      {/* Messages */}
      <div className="flex-1 overflow-y-auto p-4 md:p-8 space-y-6 pb-24 md:pb-8">
        {messages.length === 0 ? (
          <div className="h-full flex flex-col items-center justify-center text-gray-700 space-y-4 select-none">
            <div className="p-5 bg-gray-900/50 rounded-full border border-gray-800">
              <Bot size={40} strokeWidth={1.5} />
            </div>
            <div className="text-center space-y-1">
              <p className="font-bold text-gray-600 tracking-widest uppercase text-xs">Miniclaw Assistant</p>
              <p className="text-sm font-medium max-w-xs mx-auto opacity-70">
                {isBackendRunning
                  ? "Ready to help. Ask me anything or give me a task."
                  : isMobile
                    ? "Starting up…"
                    : "Engine offline. Launch it from the Monitoring tab."}
              </p>
            </div>
          </div>
        ) : (
          messages.map((msg) => (
            <div key={msg.id} className={`flex ${msg.role === 'user' ? 'justify-end' : 'justify-start'} animate-in fade-in slide-in-from-bottom-2 duration-200`}>
              {msg.role === 'bot' ? (
                <div className="flex items-start space-x-3 max-w-[90%] md:max-w-[75%]">
                  <div className={`p-2 rounded-xl border flex-shrink-0 mt-1 ${msg.isError ? "bg-rose-500/10 border-rose-500/20 text-rose-500" : "bg-indigo-500/10 border-indigo-500/20 text-indigo-400"}`}>
                    {msg.isError ? <AlertCircle size={18} /> : <Bot size={18} />}
                  </div>
                  <div className="space-y-2 min-w-0">
                    {(msg.activities && msg.activities.length > 0) && (
                      <div className="bg-gray-900/60 border border-gray-800 rounded-xl p-3 space-y-1 max-h-32 overflow-y-auto">
                        {msg.activities.map((a, i) => (
                          <div key={i} className="text-[11px] font-mono text-gray-500 flex items-center space-x-1.5 truncate">
                            <Wrench size={10} className="flex-shrink-0 opacity-50" />
                            <span className="truncate">{a}</span>
                          </div>
                        ))}
                      </div>
                    )}
                    {msg.content && (
                      <div className={`p-4 rounded-2xl text-sm font-medium leading-relaxed whitespace-pre-wrap break-words ${
                        msg.isError
                          ? "bg-rose-500/10 border border-rose-500/20 text-rose-300"
                          : "bg-[#141414] border border-gray-800 text-gray-200"
                      }`}>
                        {msg.content}
                      </div>
                    )}
                  </div>
                </div>
              ) : (
                <div className="flex items-start space-x-3 max-w-[90%] md:max-w-[75%] flex-row-reverse">
                  <div className="p-2 rounded-xl bg-emerald-500/10 border border-emerald-500/20 text-emerald-400 flex-shrink-0 mt-1">
                    <User size={18} />
                  </div>
                  <div className="p-4 rounded-2xl bg-[#1a1a1a] border border-gray-800 text-sm font-medium leading-relaxed whitespace-pre-wrap break-words text-gray-200">
                    {msg.content}
                  </div>
                </div>
              )}
            </div>
          ))
        )}

        {isSending && (
          <div className="flex items-center space-x-3 text-gray-500 text-xs font-bold uppercase tracking-widest pl-1">
            <Loader2 size={14} className="animate-spin" />
            <span>Thinking…</span>
          </div>
        )}

        <div ref={messagesEndRef} />
      </div>

      {/* Input area */}
      <div className={`absolute bottom-0 left-0 right-0 p-3 md:p-6 bg-gradient-to-t from-[#0b0b0b] via-[#0b0b0b]/95 to-transparent ${isMobile ? "pb-[calc(0.75rem+env(safe-area-inset-bottom))]" : ""}`}>
        <div className="flex items-end space-x-2 md:space-x-3 bg-[#141414] border border-gray-800 rounded-2xl p-2 focus-within:border-indigo-500/50 transition-colors shadow-2xl">
          <textarea
            value={input}
            onChange={(e) => setInput(e.target.value)}
            onKeyDown={handleKeyDown}
            placeholder="Type a message…"
            rows={1}
            className="flex-1 bg-transparent resize-none outline-none text-sm font-medium p-3 text-gray-200 placeholder-gray-600 max-h-32"
            style={{ minHeight: "2.5rem" }}
          />
          <button
            onClick={handleSendMessage}
            disabled={!input.trim() || isSending}
            className={`p-3 rounded-xl transition-all flex-shrink-0 ${
              input.trim() && !isSending
                ? "bg-indigo-600 hover:bg-indigo-500 text-white shadow-lg shadow-indigo-600/20 active:scale-95"
                : "bg-gray-800 text-gray-600 cursor-not-allowed"
            }`}
          >
            {isSending ? <Loader2 size={18} className="animate-spin" /> : <Send size={18} />}
          </button>
        </div>
      </div>
    </div>
  );
};

export default Chat;
