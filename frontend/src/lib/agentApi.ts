// Platform detection + unified agent API.
//
// Desktop: the C++ engine runs as a sidecar and we talk HTTP to
//          http://localhost:9000 (SSE-style "data: {...}" lines).
// Mobile:  the engine is embedded in-process; we call Tauri commands and
//          receive "agent-event" events from the Rust layer.

import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import axios from "axios";

export const isMobile = (() => {
  if (typeof navigator === "undefined") return false;
  const ua = navigator.userAgent || "";
  if (/Android|iPhone|iPad|iPod/i.test(ua)) return true;
  // iPadOS 13+ reports as Mac; check touch points.
  const nav = navigator as Navigator & { platform?: string; maxTouchPoints?: number };
  return (nav.platform?.startsWith("Mac") ?? false) && (nav.maxTouchPoints ?? 0) > 1;
})();

export interface AgentEvent {
  type: "status" | "token" | "tool_start" | "tool_end" | "error" | "done" | string;
  content: string;
}

/**
 * Send one message to the agent and stream events back.
 * Resolves when the turn finishes (done/error), rejects on transport failure.
 */
export async function sendAgentMessage(
  sessionId: string,
  message: string,
  onEvent: (e: AgentEvent) => void
): Promise<void> {
  if (isMobile) {
    return sendAgentMessageMobile(sessionId, message, onEvent);
  }
  return sendAgentMessageDesktop(sessionId, message, onEvent);
}

// ── Mobile: Tauri commands + events ────────────────────────────────────────

async function sendAgentMessageMobile(
  sessionId: string,
  message: string,
  onEvent: (e: AgentEvent) => void
): Promise<void> {
  return new Promise<void>((resolve, reject) => {
    let unlisten: UnlistenFn | null = null;
    let settled = false;

    const finish = (fn: () => void) => {
      if (settled) return;
      settled = true;
      if (unlisten) unlisten();
      fn();
    };

    listen<AgentEvent>("agent-event", (event) => {
      const e = event.payload;
      onEvent(e);
      if (e.type === "done") finish(resolve);
      else if (e.type === "error") {
        // Surface the error, then settle.
        finish(() => resolve());
      }
    })
      .then((un) => {
        unlisten = un;
        invoke("mobile_send_message", { sessionId, message })
          .then(() => {
            /* events stream in; settled via done/error */
          })
          .catch((err) => finish(() => reject(err)));
      })
      .catch((err) => reject(err));

    // Safety net: if the engine never reports done (e.g. crash), give up
    // after 10 minutes.
    setTimeout(() => finish(resolve), 10 * 60 * 1000);
  });
}

// ── Desktop: HTTP/SSE to the sidecar ───────────────────────────────────────

async function sendAgentMessageDesktop(
  sessionId: string,
  message: string,
  onEvent: (e: AgentEvent) => void
): Promise<void> {
  const response = await axios.post(
    "http://localhost:9000/api/chat",
    { message, session_id: sessionId, model: "miniclaw" },
    { responseType: "text" }
  );

  const lines = String(response.data).split("\n");
  let gotDone = false;
  for (const line of lines) {
    if (!line.startsWith("data: ")) continue;
    try {
      const json = JSON.parse(line.substring(6));
      onEvent({ type: json.type, content: json.content ?? "" });
      if (json.type === "done") gotDone = true;
    } catch {
      /* ignore partial/empty */
    }
  }
  if (!gotDone) onEvent({ type: "done", content: "" });
}

// ── Config access (both platforms) ─────────────────────────────────────────

export async function loadConfig(): Promise<string> {
  if (isMobile) {
    return invoke<string>("mobile_get_config");
  }
  return invoke<string>("read_config");
}

export async function saveConfig(content: string): Promise<void> {
  if (isMobile) {
    await invoke("mobile_save_config", { content });
    return;
  }
  await invoke("save_config", { content });
}

// ── Backend status ─────────────────────────────────────────────────────────

export async function checkBackendStatus(): Promise<boolean> {
  if (isMobile) {
    try {
      return await invoke<boolean>("mobile_status");
    } catch {
      return false;
    }
  }
  try {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 1000);
    await fetch("http://localhost:9000/api/health", {
      mode: "no-cors",
      signal: controller.signal,
    });
    clearTimeout(timeoutId);
    return true;
  } catch {
    return false;
  }
}
