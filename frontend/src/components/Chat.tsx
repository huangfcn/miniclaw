import { useState, useRef, useEffect } from "react";
import { Send, User, Bot, AlertTriangle, Loader2 } from "lucide-react";
import axios from "axios";

interface Message {
  role: "user" | "bot";
  content: string;
}

const Chat = ({ isBackendRunning }: { isBackendRunning: boolean }) => {
  const [messages, setMessages] = useState<Message[]>([]);
  const [input, setInput] = useState("");
  const [isLoading, setIsLoading] = useState(false);
  const scrollRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (scrollRef.current) {
      scrollRef.current.scrollTop = scrollRef.current.scrollHeight;
    }
  }, [messages, isLoading]);

  const handleSendMessage = async () => {
    if (!input.trim() || !isBackendRunning || isLoading) return;

    const userMessage: Message = { role: "user", content: input };
    setMessages((prev) => [...prev, userMessage]);
    setInput("");
    setIsLoading(true);

    try {
      // The miniclaw backend (C++) listens on port 9000 by default (from config.yaml)
      // We use axios to send the POST request
      const response = await axios.post("http://localhost:9000/api/chat", {
        message: input,
        session_id: "default",
        model: "miniclaw"
      }, {
        // SSE support if needed, but for simplicity let's assume it's buffered for now
        // Or handle SSE streaming here
        responseType: 'text',
        onDownloadProgress: (progressEvent) => {
            // Placeholder for real SSE handling if we want to stream
        }
      });

      // Simple parsing of SSE for now (very basic)
      const dataStr = response.data;
      const botMessage: Message = { role: "bot", content: "" };
      
      // Basic SSE parsing logic
      const lines = dataStr.split('\n');
      for (const line of lines) {
        if (line.startsWith('data: ')) {
          try {
            const json = JSON.parse(line.substring(6));
            if (json.type === 'token') {
              botMessage.content += json.content;
            }
          } catch (e) { /* ignore partial/empty */ }
        }
      }

      if (botMessage.content) {
        setMessages((prev) => [...prev, botMessage]);
      } else {
         setMessages((prev) => [...prev, { role: "bot", content: "Error: No response content." }]);
      }
    } catch (error) {
      console.error("Chat error:", error);
      setMessages((prev) => [...prev, { role: "bot", content: "Error: Failed to connect to the backend engine." }]);
    } finally {
      setIsLoading(false);
    }
  };

  return (
    <div className="flex-1 flex flex-col h-full bg-black/20 backdrop-blur-xl">
      <div className="flex-1 overflow-y-auto p-4 space-y-4" ref={scrollRef}>
        {!isBackendRunning && (
          <div className="max-w-md mx-auto mt-20 p-6 bg-rose-500/10 border border-rose-500/20 rounded-xl text-center space-y-3">
            <AlertTriangle className="w-12 h-12 text-rose-500 mx-auto" />
            <h3 className="text-xl font-bold text-rose-500">Engine Offline</h3>
            <p className="text-sm text-rose-300">The miniclaw core is currently inactive. Head to the Monitoring tab to launch it.</p>
          </div>
        )}
        
        {messages.map((msg, i) => (
          <div key={i} className={`flex ${msg.role === "user" ? "justify-end" : "justify-start"}`}>
            <div className={`max-w-[85%] rounded-2xl p-4 shadow-sm flex space-x-3 ${
              msg.role === "user" 
                ? "bg-indigo-600 text-white" 
                : "bg-gray-800 text-gray-100 border border-gray-700/50"
            }`}>
              {msg.role === "bot" && <Bot className="w-5 h-5 mt-1 shrink-0 text-indigo-400" />}
              <div className="whitespace-pre-wrap leading-relaxed text-[15px]">{msg.content}</div>
              {msg.role === "user" && <User className="w-5 h-5 mt-1 shrink-0 opacity-80" />}
            </div>
          </div>
        ))}
        
        {isLoading && (
          <div className="flex justify-start">
            <div className="bg-gray-800/80 border border-gray-700/50 rounded-2xl p-4 flex items-center space-x-3 text-indigo-400">
              <Loader2 className="w-5 h-5 animate-spin" />
              <span className="text-sm font-medium tracking-wide">miniclaw is thinking...</span>
            </div>
          </div>
        )}
      </div>

      <div className="p-6 border-t border-gray-800 bg-black/40 shadow-inner">
        <div className="relative max-w-4xl mx-auto group">
          <input
            type="text"
            className={`w-full bg-gray-900 border ${isBackendRunning ? 'border-gray-700 focus:border-indigo-500' : 'border-rose-900/50 cursor-not-allowed opacity-50'} rounded-2xl px-5 py-4 pr-14 text-white placeholder-gray-500 outline-none transition-all shadow-lg group-hover:shadow-indigo-500/5 focus:shadow-indigo-500/10`}
            placeholder={isBackendRunning ? "Message miniclaw..." : "Backend offline..."}
            value={input}
            disabled={!isBackendRunning || isLoading}
            onChange={(e) => setInput(e.target.value)}
            onKeyDown={(e) => e.key === "Enter" && handleSendMessage()}
          />
          <button 
            disabled={!isBackendRunning || isLoading || !input.trim()}
            onClick={handleSendMessage}
            className={`absolute right-3 top-1/2 -translate-y-1/2 p-2 rounded-xl transition-all ${
              input.trim() && isBackendRunning && !isLoading
                ? "bg-indigo-600 text-white hover:bg-indigo-500 shadow-md"
                : "text-gray-500 bg-transparent"
            }`}
          >
            <Send size={18} strokeWidth={2.5} />
          </button>
        </div>
        <p className="mt-3 text-center text-[10px] text-gray-500 uppercase tracking-[0.2em] font-bold">
          LLM responses can be inaccurate. verify critical information.
        </p>
      </div>
    </div>
  );
};

export default Chat;
