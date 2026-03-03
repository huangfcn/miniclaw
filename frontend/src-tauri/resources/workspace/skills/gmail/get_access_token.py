import os
from google_auth_oauthlib.flow import InstalledAppFlow
from google.auth.transport.requests import Request
from google.oauth2.credentials import Credentials

# Define the scopes (e.g., read-only)
# Change this line:
SCOPES = ['https://www.googleapis.com/auth/gmail.readonly'] 

def get_access_token():
    creds = None
    # Load existing token or run authorization flow
    if os.path.exists('token.json'):
        creds = Credentials.from_authorized_user_file('token.json', SCOPES)
    
    if not creds or not creds.valid:
        if creds and creds.expired and creds.refresh_token:
            creds.refresh(Request())
        else:
            flow = InstalledAppFlow.from_client_secrets_file('credentials_huangfcn.json', SCOPES)
            creds = flow.run_local_server(port=0)
        # Save token for future use
        with open('token.json', 'w') as token:
            token.write(creds.to_json())

    print("Access Token:", creds.token)
    return creds.token

if __name__ == '__main__':
    get_access_token()
