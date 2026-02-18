import requests
import json
import sseclient
import time

def test_react(prompt):
    url = "http://localhost:8080/api/chat"
    payload = {
        "message": prompt,
        "session_id": "test_react_session"
    }
    
    print(f"🚀 Prompt: {prompt}")
    print("-" * 50)
    
    try:
        response = requests.post(url, json=payload, stream=True)
        client = sseclient.SSEClient(response)
        
        for event in client.events():
            if not event.data:
                continue
            try:
                data = json.loads(event.data)
                etype = data.get("type")
                content = data.get("content", "")
                
                if etype == "token":
                    print(content, end="", flush=True)
                elif etype == "tool_start":
                    print(f"\n[TOOL START] {content}")
                elif etype == "tool_end":
                    print(f"\n[TOOL END] {content}")
                elif etype == "error":
                    print(f"\n❌ ERROR: {content}")
                elif etype == "done":
                    print("\n✅ DONE")
            except json.JSONDecodeError:
                print(f"\n⚠️ Raw: {event.data}")
    except Exception as e:
        print(f"\n💥 Connection error: {e}")

if __name__ == "__main__":
    import sys
    prompt = sys.argv[1] if len(sys.argv) > 1 else "Perform these exact steps: 1. Thought: I must write a file. Action: <tool name=\"write_file\">test_final.txt\nVERIFIED</tool> 2. Then say DONE."
    test_react(prompt)
