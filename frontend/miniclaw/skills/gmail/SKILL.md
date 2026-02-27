---
name: gmail
description: "A skill to read recent unread emails, summarize them, and generate auto-reply suggestions."
always: "true"
---

# Gmail Skill

This skill allows you to read the user's recent (last 24 hours) unread emails.
Once you read the emails, you should provide:
1. A summary of each email.
2. An auto-reply suggestion for each email.

## Tools
You have access to a script `gmail_tool.py` that interacts with the Gmail API.

To read the recent unread emails, use the `exec` tool to run:
```bash
conda run -n miniclaw python {{skills_path}}/gmail/gmail_tool.py read
```
**Note:** `{{skills_path}}` should be replaced with the path to the skills folder. For example, if the frontend workspace is `d:\workspace\miniclaw\frontend\miniclaw`, then run `conda run -n miniclaw python d:\workspace\miniclaw\frontend\miniclaw\skills\gmail\gmail_tool.py read`.

## Usage
When the user asks you to check their emails, summarize them, or suggest replies:
1. Run the `gmail_tool.py read` command using your `exec` tool and the `miniclaw` conda environment.
2. Parse the standard output. It will contain the `From`, `Subject`, `Date`, and a `Body Preview` of each recent unread email.
3. If there is an error about `credentials.json` missing, politely inform the user that they need to download their OAuth 2.0 Client ID credentials from the Google Cloud Console and save it as `credentials.json` in the `skills/gmail` directory.
4. For every email found:
   - Provide a concise 1-2 sentence summary.
   - Provide a drafted professional auto-reply suggestion based on the context of the email.

**Important:** You must install the required dependencies first if they are not installed. You can do this by running `conda run -n miniclaw pip install -r {{skills_path}}/gmail/requirements.txt`.
