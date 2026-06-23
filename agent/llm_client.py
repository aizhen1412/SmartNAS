import json
import threading
from typing import Any, Dict, Iterator, List, Optional

import requests
from fastapi import HTTPException

from .config import (
    DEEPSEEK_API_BASE,
    DEEPSEEK_API_KEY,
    DEEPSEEK_MODEL,
    DEEPSEEK_TIMEOUT,
    GENERATION_TEMPERATURE,
    GENERATION_TOP_P,
)

_request_lock = threading.Lock()


def clean_model_text(value: str) -> str:
    """Remove Markdown emphasis markers that should not be shown in the UI."""
    return value.replace("*", "")


def create_chat_message(messages: List[dict], tools: Optional[List[dict]] = None) -> Dict[str, Any]:
    if not DEEPSEEK_API_KEY:
        raise HTTPException(
            status_code=503,
            detail="缺少 DeepSeek API Key，请设置 SMARTNAS_DEEPSEEK_API_KEY 或 DEEPSEEK_API_KEY",
        )

    payload: Dict[str, Any] = {
        "model": DEEPSEEK_MODEL,
        "messages": messages,
        "temperature": GENERATION_TEMPERATURE,
        "top_p": GENERATION_TOP_P,
        "stream": False,
    }
    if tools:
        payload["tools"] = tools
        payload["tool_choice"] = "auto"

    try:
        with _request_lock:
            response = requests.post(
                f"{DEEPSEEK_API_BASE}/chat/completions",
                headers={
                    "Authorization": f"Bearer {DEEPSEEK_API_KEY}",
                    "Content-Type": "application/json",
                },
                json=payload,
                timeout=DEEPSEEK_TIMEOUT,
            )
    except requests.RequestException as exc:
        raise HTTPException(status_code=502, detail=f"DeepSeek API 请求失败: {exc}") from exc

    if response.status_code != 200:
        detail = response.text
        try:
            detail = response.json().get("error", {}).get("message", detail)
        except ValueError:
            pass
        raise HTTPException(status_code=response.status_code, detail=f"DeepSeek API 返回错误: {detail}")

    try:
        data = response.json()
        message = data["choices"][0]["message"]
        if isinstance(message.get("content"), str):
            message["content"] = clean_model_text(message["content"])
        return message
    except (KeyError, IndexError, TypeError, ValueError) as exc:
        raise HTTPException(status_code=502, detail=f"DeepSeek API 响应格式异常: {exc}") from exc


def create_chat_completion(messages: List[dict]) -> str:
    message = create_chat_message(messages)
    return (message.get("content") or "").strip()


def stream_chat_completion(messages: List[dict]) -> Iterator[str]:
    if not DEEPSEEK_API_KEY:
        raise HTTPException(status_code=503, detail="缺少 DeepSeek API Key")
    payload = {
        "model": DEEPSEEK_MODEL,
        "messages": messages,
        "temperature": GENERATION_TEMPERATURE,
        "top_p": GENERATION_TOP_P,
        "stream": True,
    }
    try:
        with requests.post(
            f"{DEEPSEEK_API_BASE}/chat/completions",
            headers={
                "Authorization": f"Bearer {DEEPSEEK_API_KEY}",
                "Content-Type": "application/json",
            },
            json=payload,
            timeout=DEEPSEEK_TIMEOUT,
            stream=True,
        ) as response:
            if response.status_code != 200:
                raise HTTPException(
                    status_code=response.status_code,
                    detail=f"DeepSeek API 返回错误: {response.text}",
                )
            for line in response.iter_lines(chunk_size=1, decode_unicode=True):
                if not line or not line.startswith("data:"):
                    continue
                data = line[5:].strip()
                if data == "[DONE]":
                    break
                try:
                    event = json.loads(data)
                    content = event["choices"][0].get("delta", {}).get("content")
                except (KeyError, IndexError, TypeError, ValueError):
                    continue
                if content:
                    cleaned = clean_model_text(content)
                    if cleaned:
                        yield cleaned
    except requests.RequestException as exc:
        raise HTTPException(status_code=502, detail=f"DeepSeek 流式请求失败: {exc}") from exc


