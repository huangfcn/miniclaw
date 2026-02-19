import requests
import threading
import time

def send_request(session_id, user_message):
    url = "http://localhost:8080/api/chat"
    payload = {
        "session_id": session_id,
        "message": user_message
    }
    try:
        print(f"[Client] Sending request for {session_id}")
        response = requests.post(url, json=payload, timeout=10)
        print(f"[Client] Response for {session_id}: {response.status_code}")
    except Exception as e:
        print(f"[Client] Error for {session_id}: {e}")

if __name__ == "__main__":
    # Start the miniclaw backend in a separate terminal or manually before running this
    
    threads = []
    for i in range(5):
        t = threading.Thread(target=send_request, args=(f"test_session_{i}", "Who are you?"))
        threads.append(t)
    
    for t in threads:
        t.start()
        time.sleep(0.1) # Small stagger
        
    for t in threads:
        t.join()

    print("Verification script finished.")
