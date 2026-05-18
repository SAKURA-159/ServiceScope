#!/usr/bin/env python3
"""
AI Bridge — called by ServiceScope C++ server to talk to any AI API.
Python handles HTTPS (SSL built-in), C++ handles the rest.

Usage:
  echo '{"endpoint":"...","api_key":"...","model":"...","metrics":{...},"anomalies":[...],"logs":[...]}' \
    | python ai_bridge.py
"""

import sys, json, os
from urllib import request, error as urllib_error


def main():
    try:
        raw = sys.stdin.read()
        data = json.loads(raw)
    except Exception as e:
        print(json.dumps({"ok": False, "error": f"Failed to read input: {e}"}))
        sys.exit(1)

    endpoint = data.get("endpoint") or os.environ.get("AI_ENDPOINT", "")
    api_key  = data.get("api_key")  or os.environ.get("AI_API_KEY", "")
    model    = data.get("model")    or os.environ.get("AI_MODEL", "deepseek-chat")
    timeout  = int(data.get("timeout", 30))

    if not endpoint or not api_key:
        print(json.dumps({"ok": False, "error": "AI not configured: missing endpoint or api_key"}))
        sys.exit(1)

    # Build the prompt from metrics + anomalies + logs
    metrics_str   = json.dumps(data.get("metrics", {}), indent=2, ensure_ascii=True)
    anomalies_str = json.dumps(data.get("anomalies", []), indent=2, ensure_ascii=True)
    logs_str      = json.dumps(data.get("logs", []), indent=2, ensure_ascii=True)

    prompt = f"""You are analyzing a production C++ high-concurrency service called ServiceScope.

## Current Metrics
```json
{metrics_str}
```

## Recent Anomalies
```json
{anomalies_str}
```

## Recent Logs
```json
{logs_str}
```

Please provide:
1. **Health Assessment**: Is the service healthy/degraded/critical? (one sentence)
2. **Key Issues**: What are the main problems right now? (bullet points)
3. **Root Cause Analysis**: What is the most likely root cause?
4. **Recommended Actions**: What should the operator do? (numbered list)
5. **Confidence**: How confident are you in this analysis? (percentage)

Be specific and reference actual metric values. Keep the response under 500 words.
Respond in Chinese (中文)."""

    req_body = json.dumps({
        "model": model,
        "messages": [
            {"role": "system", "content": "你是一个 SRE 专家，请用中文简洁回答。"},
            {"role": "user", "content": prompt}
        ],
        "max_tokens": 1024,
        "temperature": 0.3
    }).encode("utf-8")

    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    try:
        req = request.Request(endpoint, data=req_body, headers=headers, method="POST")
        with request.urlopen(req, timeout=timeout) as resp:
            result = json.loads(resp.read().decode("utf-8"))

        if "choices" in result and len(result["choices"]) > 0:
            content = result["choices"][0]["message"]["content"]
            print(json.dumps({
                "ok": True,
                "model": result.get("model", model),
                "content": content,
                "tokens_used": result.get("usage", {}).get("total_tokens", 0)
            }, ensure_ascii=True))
        elif "error" in result:
            print(json.dumps({"ok": False, "error": result["error"].get("message", str(result["error"]))}))
        else:
            print(json.dumps({"ok": False, "error": "Unexpected API response format"}))
    except urllib_error.HTTPError as e:
        print(json.dumps({"ok": False, "error": f"HTTP {e.code}: {e.reason}"}))
    except urllib_error.URLError as e:
        print(json.dumps({"ok": False, "error": f"Connection failed: {e.reason}"}))
    except Exception as e:
        print(json.dumps({"ok": False, "error": str(e)}))


if __name__ == "__main__":
    main()
