import os.path
import datetime
import argparse
from google.auth.transport.requests import Request
from google.oauth2.credentials import Credentials
from google_auth_oauthlib.flow import InstalledAppFlow
from googleapiclient.discovery import build

# If modifying these scopes, delete the file token.json.
SCOPES = ['https://www.googleapis.com/auth/gmail.readonly']

def get_service():
    """Shows basic usage of the Gmail API.
    Lists the user's Gmail labels.
    """
    creds = None
    # The file token.json stores the user's access and refresh tokens, and is
    # created automatically when the authorization flow completes for the first
    # time.
    token_path = os.path.join(os.path.dirname(__file__), 'token.json')
    credentials_path = os.path.join(os.path.dirname(__file__), 'credentials.json')
    
    if os.path.exists(token_path):
        creds = Credentials.from_authorized_user_file(token_path, SCOPES)
    # If there are no (valid) credentials available, let the user log in.
    if not creds or not creds.valid:
        if creds and creds.expired and creds.refresh_token:
            creds.refresh(Request())
        else:
            if not os.path.exists(credentials_path):
                print(f"Error: {credentials_path} not found. Please download OAuth client ID credentials from Google Cloud Console.")
                exit(1)
            flow = InstalledAppFlow.from_client_secrets_file(
                credentials_path, SCOPES)
            creds = flow.run_local_server(port=0)
        # Save the credentials for the next run
        with open(token_path, 'w') as token:
            token.write(creds.to_json())

    return build('gmail', 'v1', credentials=creds)

def read_recent_emails(service):
    # Calculate timestamp for 24 hours ago
    yesterday = datetime.datetime.now() - datetime.timedelta(days=1)
    timestamp = int(yesterday.timestamp())
    
    # Query: unread messages from the last 24 hours
    query = f"is:unread after:{timestamp}"
    
    try:
        results = service.users().messages().list(userId='me', q=query, maxResults=20).execute()
        messages = results.get('messages', [])

        if not messages:
            print("No new unread emails in the last 24 hours.")
            return

        print(f"Found {len(messages)} unread email(s) in the last 24 hours:\n")
        
        for msg in messages:
            msg_id = msg['id']
            message = service.users().messages().get(userId='me', id=msg_id, format='full').execute()
            
            headers = message['payload']['headers']
            subject = next((h['value'] for h in headers if h['name'].lower() == 'subject'), "No Subject")
            sender = next((h['value'] for h in headers if h['name'].lower() == 'from'), "Unknown Sender")
            date = next((h['value'] for h in headers if h['name'].lower() == 'date'), "Unknown Date")
            
            # Simple body extraction
            body = "No plain text body found."
            if 'parts' in message['payload']:
                for part in message['payload']['parts']:
                    if part['mimeType'] == 'text/plain':
                        if 'data' in part['body']:
                            import base64
                            body = base64.urlsafe_b64decode(part['body']['data']).decode('utf-8')
                        break
            elif 'body' in message['payload'] and 'data' in message['payload']['body']:
               import base64
               body = base64.urlsafe_b64decode(message['payload']['body']['data']).decode('utf-8')
               
            print("-" * 50)
            print(f"From: {sender}")
            print(f"Subject: {subject}")
            print(f"Date: {date}")
            print(f"ID: {msg_id}")
            print("Body Preview:")
            # Print at most 500 chars of body
            print(body[:500] + ("..." if len(body) > 500 else ""))
            print("-" * 50 + "\n")
            
    except Exception as error:
        print(f'An error occurred: {error}')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Gmail Tool for Miniclaw")
    parser.add_argument("action", choices=["read"], help="Action to perform (e.g., 'read')")
    
    args = parser.parse_args()
    
    if args.action == "read":
        service = get_service()
        read_recent_emails(service)
