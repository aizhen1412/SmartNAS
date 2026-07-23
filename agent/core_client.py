import hashlib
import threading
import time
from typing import Any, Dict, List

import requests
from fastapi import HTTPException

from .config import NAS_CORE_API


identity_cache: Dict[str, Dict[str, Any]] = {}
identity_cache_lock = threading.Lock()


def get_user_identity(token: str) -> str:
    cache_key = hashlib.sha256(token.encode("utf-8")).hexdigest()
    now = time.time()
    with identity_cache_lock:
        cached = identity_cache.get(cache_key)
        if cached and cached["expires_at"] > now:
            return cached["username"]
    try:
        response = requests.get(
            f"{NAS_CORE_API}/api/v1/me",
            headers={"Authorization": token},
            timeout=10,
        )
    except requests.RequestException as exc:
        raise HTTPException(status_code=502, detail=f"无法验证用户身份: {exc}")
    if response.status_code != 200:
        raise HTTPException(status_code=401, detail="SmartNAS 登录状态无效或已过期")
    username = (response.json().get("username") or "").strip()
    if not username:
        raise HTTPException(status_code=502, detail="核心服务未返回用户身份")
    with identity_cache_lock:
        identity_cache[cache_key] = {"username": username, "expires_at": now + 30}
    return username


def fetch_all_user_files(token: str) -> List[Dict[str, Any]]:
    try:
        response = requests.get(
            f"{NAS_CORE_API}/api/v1/files/all",
            headers={"Authorization": token},
            timeout=20,
        )
    except requests.RequestException as exc:
        raise HTTPException(status_code=502, detail=f"无法读取完整文件清单: {exc}")
    if response.status_code != 200:
        raise HTTPException(status_code=response.status_code, detail="核心服务无法返回完整文件清单")
    payload = response.json()
    return payload.get("files", payload if isinstance(payload, list) else [])
