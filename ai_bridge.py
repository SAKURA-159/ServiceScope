#!/usr/bin/env python3
"""
AI Bridge / Agent — called by ServiceScope C++ server to talk to any OpenAI-compatible AI API.

Single-shot → Agent upgrade:
  The bridge now runs a minimal tool-calling agent loop. The model is given three
  read-only tools (get_metrics / get_traces / get_trace) that fetch live data from
  the running ServiceScope HTTP server, so it can pull fresher / more detailed data
  and iterate on its diagnosis instead of relying only on the initial snapshot.

Input (stdin JSON):
  { "endpoint", "api_key", "model", "timeout", "base_url",
    "metrics": {...}, "anomalies": [...], "logs": [...] }

Output (stdout JSON):
  { "ok": true, "content": "...", "model": "...", "tokens_used": N,
    "iterations": N, "tool_calls": N, "tool_trace": [...] }
"""

import sys
import json
import os
from urllib import request, error as urllib_error

# Build an opener that routes localhost/127.0.0.1 directly (bypassing the
# system proxy). Tool calls hit the local ServiceScope server, and a local
# mock LLM in tests is also local — without this, a configured http_proxy
# turns localhost requests into HTTP 502. Setting the env var (not just the
# proxies dict) is required because urllib's proxy_bypass() re-reads no_proxy
# from the environment.
def _make_opener():
    no = os.environ.get("no_proxy") or os.environ.get("NO_PROXY") or ""
    no_proxy = ",".join(x for x in (no, "localhost", "127.0.0.1") if x)
    os.environ["no_proxy"] = no_proxy
    os.environ["NO_PROXY"] = no_proxy
    return request.build_opener(request.ProxyHandler())


_OPENER = _make_opener()

# ---------------------------------------------------------------------------
# Tool definitions (OpenAI function-calling schema)
# ---------------------------------------------------------------------------

TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "get_metrics",
            "description": "获取 ServiceScope 当前实时指标：QPS、P50/P90/P99/P999 延迟、错误率、活跃连接数、慢请求列表。",
            "parameters": {"type": "object", "properties": {}, "required": []},
        },
    },
    {
        "type": "function",
        "function": {
            "name": "get_traces",
            "description": "获取最近 N 条请求追踪（trace）的摘要。",
            "parameters": {
                "type": "object",
                "properties": {
                    "limit": {"type": "integer", "description": "返回条数，默认 20"}
                },
                "required": [],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "get_trace",
            "description": "根据 trace_id 获取单条请求追踪的详情（含 span 树、各阶段耗时）。",
            "parameters": {
                "type": "object",
                "properties": {
                    "trace_id": {"type": "string", "description": "trace 标识"}
                },
                "required": ["trace_id"],
            },
        },
    },
]

SYSTEM_PROMPT = """你是一名 SRE 运维诊断 Agent，负责诊断一个高并发 C++ 服务 ServiceScope。

你可以调用只读工具获取实时数据（工具调用不会改变服务状态）：
- get_metrics(): 拉取最新指标
- get_traces(limit): 拉取最近请求追踪
- get_trace(trace_id): 拉取单条追踪详情

诊断流程建议：先看初始快照判断有无异常；若有慢请求或错误，用 get_trace 追查具体请求的 span 耗时定位瓶颈；必要时用 get_metrics 拉最新数据复核。

最后输出（中文，500 字以内）：
1. 健康评估（一句话）
2. 关键问题（要点）
3. 根因分析
4. 操作建议（编号列表）
5. 置信度（百分比）

引用真实指标数值，不要泛泛而谈。"""


def _http_json(url, timeout=5):
    with _OPENER.open(url, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def call_tool(name, args, base_url):
    """Execute a read-only tool by hitting the ServiceScope HTTP server."""
    if name == "get_metrics":
        return _http_json(f"{base_url}/metrics")
    if name == "get_traces":
        limit = int(args.get("limit", 20))
        return _http_json(f"{base_url}/api/traces?limit={limit}")
    if name == "get_trace":
        tid = args["trace_id"]
        return _http_json(f"{base_url}/api/traces/{tid}")
    return {"error": f"unknown tool: {name}"}


def post_chat(endpoint, api_key, payload, timeout):
    body = json.dumps(payload).encode("utf-8")
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }
    req = request.Request(endpoint, data=body, headers=headers, method="POST")
    with _OPENER.open(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def single_shot(endpoint, api_key, model, messages, timeout):
    """Fallback: one call, no tools (for providers that reject `tools`)."""
    payload = {
        "model": model,
        "messages": messages,
        "max_tokens": 1024,
        "temperature": 0.3,
    }
    result = post_chat(endpoint, api_key, payload, timeout)
    msg = result["choices"][0]["message"]
    return {
        "ok": True,
        "model": result.get("model", model),
        "content": msg.get("content", ""),
        "tokens_used": result.get("usage", {}).get("total_tokens", 0),
        "iterations": 1,
        "tool_calls": 0,
        "tool_trace": [],
    }


def run_agent(data):
    endpoint = data.get("endpoint") or os.environ.get("AI_ENDPOINT", "")
    api_key = data.get("api_key") or os.environ.get("AI_API_KEY", "")
    model = data.get("model") or os.environ.get("AI_MODEL", "deepseek-chat")
    timeout = int(data.get("timeout", 30))
    base_url = data.get("base_url") or os.environ.get("AI_BASE_URL", "http://localhost:8080")
    max_iterations = int(data.get("max_iterations", 5))

    if not endpoint or not api_key:
        return {"ok": False, "error": "AI not configured: missing endpoint or api_key"}

    metrics_str = json.dumps(data.get("metrics", {}), indent=2, ensure_ascii=True)
    anomalies_str = json.dumps(data.get("anomalies", []), indent=2, ensure_ascii=True)
    logs_str = json.dumps(data.get("logs", []), indent=2, ensure_ascii=True)

    user_prompt = f"""## 初始快照（请求分析时刻）

### 当前指标
```json
{metrics_str}
```

### 近期异常
```json
{anomalies_str}
```

### 近期日志
```json
{logs_str}
```

请开始诊断，必要时调用工具获取更详细或更新的数据。"""

    messages = [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": user_prompt},
    ]

    tool_trace = []
    total_tokens = 0

    for iteration in range(1, max_iterations + 1):
        payload = {
            "model": model,
            "messages": messages,
            "max_tokens": 1024,
            "temperature": 0.3,
            "tools": TOOLS,
            "tool_choice": "auto",
        }
        try:
            result = post_chat(endpoint, api_key, payload, timeout)
        except urllib_error.HTTPError as e:
            # A provider that doesn't understand `tools` typically 400s on the
            # first call — degrade to a single-shot request without tools.
            if iteration == 1 and e.code in (400, 404, 422):
                try:
                    return single_shot(endpoint, api_key, model, messages, timeout)
                except Exception as se:
                    return {"ok": False, "error": str(se)}
            return {"ok": False, "error": f"HTTP {e.code}: {e.reason}"}
        except urllib_error.URLError as e:
            return {"ok": False, "error": f"Connection failed: {e.reason}"}

        total_tokens += result.get("usage", {}).get("total_tokens", 0)

        if "choices" not in result or not result["choices"]:
            return {"ok": False, "error": "Unexpected API response format"}

        msg = result["choices"][0]["message"]
        messages.append(msg)

        tool_calls = msg.get("tool_calls")
        if not tool_calls:
            # No tool calls → final answer.
            return {
                "ok": True,
                "model": result.get("model", model),
                "content": msg.get("content", ""),
                "tokens_used": total_tokens,
                "iterations": iteration,
                "tool_calls": len(tool_trace),
                "tool_trace": tool_trace,
            }

        # Execute each requested tool and append results for the next turn.
        for tc in tool_calls:
            fn = tc.get("function", {})
            name = fn.get("name", "")
            try:
                args = json.loads(fn.get("arguments") or "{}")
            except json.JSONDecodeError:
                args = {}
            try:
                result_obj = call_tool(name, args, base_url)
                content = json.dumps(result_obj, ensure_ascii=False)
            except Exception as e:
                content = json.dumps({"error": str(e)}, ensure_ascii=False)
            tool_trace.append({"call": name, "args": args})
            messages.append({
                "role": "tool",
                "tool_call_id": tc.get("id", ""),
                "content": content,
            })

    return {"ok": False, "error": f"Exceeded max iterations ({max_iterations}) without final answer"}


def main():
    # Force UTF-8 I/O — Windows Python defaults to the GBK locale, which would
    # corrupt Chinese metrics/logs on the way in and Chinese content on the way
    # out (the C++ side parses the result as UTF-8 JSON).
    for stream in (sys.stdin, sys.stdout):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")

    try:
        data = json.loads(sys.stdin.read())
    except Exception as e:
        print(json.dumps({"ok": False, "error": f"Failed to read input: {e}"}))
        sys.exit(1)

    out = run_agent(data)
    print(json.dumps(out, ensure_ascii=True))


if __name__ == "__main__":
    main()
