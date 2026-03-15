import requests
import json
import sseclient
import threading
import time

def test_chat(message, session_id):
    url = "http://localhost:8080/api/chat"
    payload = {
        "message": message,
        "session_id": session_id
    }
    
    print(f"🚀 Sending request: {message} (Session: {session_id})")
    try:
        response = requests.post(url, json=payload, stream=True)
        client = sseclient.SSEClient(response)
        
        for event in client.events():
            if not event.data:
                continue
            try:
                data = json.loads(event.data)
                if data["type"] == "token":
                    print(data["content"], end="", flush=True)
                elif data["type"] == "error":
                    print(f"\n❌ Error: {data['content']}")
                elif data["type"] == "done":
                    print("\n✅ Done")
            except json.JSONDecodeError:
                print(f"\n⚠️ Raw: {event.data}")
    except Exception as e:
        print(f"\n💥 Connection error: {e}")

if __name__ == "__main__":
    # Test single request
    test_chat("Hello, who are you? Please reply in one short sentence.", "test_session_1")
    
    print("\n" + "="*50 + "\n")
    
    # Test "concurrent" requests (to see if fibers actually work without blocking)
    t1 = threading.Thread(target=test_chat, args=("Tell me a very short joke.", "session_a"))
    t2 = threading.Thread(target=test_chat, args=("What is 2+2? Answer in one word.", "session_b"))
    
    t1.start()
    t2.start()
    
    t1.join()
    t2.join()
