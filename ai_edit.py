import os
import sys
from openai import OpenAI

prompt = os.environ["PROMPT"]
api_key = os.environ["OPENAI_API_KEY"]

file_path = "LakeLanierDash_GITHUB_OTA_CONFIGURED.ino"

if not os.path.isfile(file_path):
    print(f"ERROR: Could not find source file: {file_path}")
    print("Available files:")
    for root, _, files in os.walk("."):
        for name in files:
            print(os.path.join(root, name))
    sys.exit(1)

print(f"Editing file: {file_path}")

with open(file_path, "r", encoding="utf-8") as f:
    code = f.read()

client = OpenAI(api_key=api_key)

response = client.chat.completions.create(
    model="gpt-4.1",
    messages=[
        {
            "role": "system",
            "content": (
                "You are editing an ESP32 / Arduino dashboard project. "
                "Keep existing behavior unless the prompt requires a change. "
                "Return ONLY the complete updated source file. "
                "No markdown. No explanation."
            )
        },
        {
            "role": "user",
            "content": (
                f"Current file path: {file_path}\n\n"
                f"Current code:\n{code}\n\n"
                f"Requested change:\n{prompt}\n\n"
                "Preserve the current structure unless necessary. "
                "Output only the complete updated file."
            )
        }
    ]
)

new_code = response.choices[0].message.content.strip()

with open(file_path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_code)

print("AI edit complete.")
