import React, { createContext, useContext, useState, ReactNode } from 'react';

// Define the shape of your shared state
interface Message {
  role: "user" | "bot";
  content: string;
}

interface EngineInfo {
  model: string;
  workspace: string;
  configPath: string;
  skills: string[];
  fiberNodes: number;
  uptime: string;
}

interface AppState {
  chat: {
    messages: Message[];
    input: string;
    isLoading: boolean;
  };
  settings: {
    config: string;
    isLoading: boolean;
    error: string | null;
    saveStatus: "idle" | "saving" | "saved" | "error";
  };
  monitoring: {
    isLoading: boolean;
    error: string | null;
    startTime: number | null;
    engineInfo: EngineInfo;
  };
}

// Define the shape of the context value
interface AppContextType {
  appState: AppState;
  setAppState: React.Dispatch<React.SetStateAction<AppState>>;
}

// Create the context with a default value
const AppContext = createContext<AppContextType | undefined>(undefined);

// Create a provider component
export const AppProvider: React.FC<{ children: ReactNode }> = ({ children }) => {
  const [appState, setAppState] = useState<AppState>({
    chat: {
      messages: [],
      input: "",
      isLoading: false,
    },
    settings: {
      config: '',
      isLoading: false,
      error: null,
      saveStatus: "idle",
    },
    monitoring: {
      isLoading: false,
      error: null,
      startTime: null,
      engineInfo: {
        model: "N/A",
        workspace: "N/A",
        configPath: "N/A",
        skills: [],
        fiberNodes: 0,
        uptime: "00:00:00",
      },
    },
  });

  return (
    <AppContext.Provider value={{ appState, setAppState }}>
      {children}
    </AppContext.Provider>
  );
};

// Create a custom hook for easy context access
export const useAppContext = () => {
  const context = useContext(AppContext);
  if (context === undefined) {
    throw new Error('useAppContext must be used within an AppProvider');
  }
  return context;
};
