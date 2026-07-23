#!/usr/bin/env python3
import argparse
import hashlib
import json
import math
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


SUMMARIZABLE_EXTENSIONS = {
    ".pdf", ".docx", ".pptx", ".xlsx", ".txt", ".csv", ".json", ".html", ".htm",
    ".md", ".xml", ".epub", ".zip", ".ipynb",
    ".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp", ".tiff", ".tif", ".svg",
    ".wav", ".mp3", ".m4a", ".mp4",
}


def request_json(url: str, method: str = "GET", headers=None, data: bytes | None = None):
    request = urllib.request.Request(url, data=data, headers=headers or {}, method=method)
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            body = response.read().decode("utf-8", errors="replace")
            if not body:
                return response.status, {}
            try:
                return response.status, json.loads(body)
            except ValueError:
                return response.status, {"text": body}
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        try:
            payload = json.loads(body)
        except ValueError:
            payload = {"error": body}
        return exc.code, payload


def login(base_url: str, username: str, password: str) -> str:
    payload = json.dumps({"user": username, "password": password}).encode("utf-8")
    status, data = request_json(
        f"{base_url}/login",
        method="POST",
        headers={"Content-Type": "application/json"},
        data=payload,
    )
    if status == 200 and data.get("token"):
        return data["token"]

    register_status, _ = request_json(
        f"{base_url}/register",
        method="POST",
        headers={"Content-Type": "application/json"},
        data=payload,
    )
    if register_status not in {200, 409}:
        raise RuntimeError(f"register failed: HTTP {register_status}")

    status, data = request_json(
        f"{base_url}/login",
        method="POST",
        headers={"Content-Type": "application/json"},
        data=payload,
    )
    if status != 200 or not data.get("token"):
        raise RuntimeError(f"login failed after register: HTTP {status} {data}")
    return data["token"]


def create_folder(base_url: str, token: str, directory: str) -> None:
    if directory == "/":
        return
    parts = [part for part in directory.split("/") if part]
    current = ""
    for part in parts:
        current += "/" + part
        url = f"{base_url}/api/folders?path={urllib.parse.quote(current)}"
        status, data = request_json(url, method="POST", headers={"Authorization": f"Bearer {token}"})
        if status not in {200, 409, 500}:
            raise RuntimeError(f"create folder {current} failed: HTTP {status} {data}")


def sha256_file(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def upload_file(base_url: str, token: str, path: Path, name: str, directory: str, chunk_size: int) -> dict:
    size = path.stat().st_size
    file_hash = sha256_file(path)
    total_chunks = max(1, math.ceil(size / chunk_size))
    init_url = (
        f"{base_url}/api/upload/init?hash={file_hash}&total={total_chunks}"
        f"&size={size}&chunkSize={chunk_size}"
    )
    status, init_data = request_json(init_url, headers={"Authorization": f"Bearer {token}"})
    if status != 200:
        raise RuntimeError(f"init failed for {path}: HTTP {status} {init_data}")

    missing = init_data.get("missing", [])
    with path.open("rb") as handle:
        for index in missing:
            handle.seek(index * chunk_size)
            body = handle.read(chunk_size)
            request = urllib.request.Request(
                f"{base_url}/api/upload/chunk",
                data=body,
                headers={
                    "Authorization": f"Bearer {token}",
                    "File-Hash": file_hash,
                    "Chunk-Index": str(index),
                },
                method="POST",
            )
            try:
                with urllib.request.urlopen(request, timeout=120) as response:
                    if response.status != 200:
                        raise RuntimeError(f"chunk {index} failed: HTTP {response.status}")
            except urllib.error.HTTPError as exc:
                detail = exc.read().decode("utf-8", errors="replace")
                raise RuntimeError(f"chunk {index} failed: HTTP {exc.code} {detail}") from exc

    headers = {
        "Authorization": f"Bearer {token}",
        "File-Name": urllib.parse.quote(name),
        "File-Hash": file_hash,
        "Total-Chunks": str(total_chunks),
        "File-Size": str(size),
        "Directory": urllib.parse.quote(directory),
    }
    status, data = request_json(f"{base_url}/api/upload/merge", method="POST", headers=headers)
    if status != 200:
        raise RuntimeError(f"merge failed for {path}: HTTP {status} {data}")
    return {"hash": file_hash, "size": size, "chunks": total_chunks, "message": data.get("message", "")}


def list_files(base_url: str, token: str) -> list[dict]:
    status, data = request_json(f"{base_url}/api/v1/files/all", headers={"Authorization": f"Bearer {token}"})
    if status != 200:
        raise RuntimeError(f"list files failed: HTTP {status} {data}")
    return data.get("files", data if isinstance(data, list) else [])


def start_summary(agent_url: str, token: str, file_hash: str, force: bool = False) -> tuple[int, dict]:
    payload = json.dumps({"hash": file_hash, "force": force}).encode("utf-8")
    return request_json(
        f"{agent_url}/api/agent/summarize/start",
        method="POST",
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
        data=payload,
    )


def corpus_files(root: Path) -> list[Path]:
    return sorted(
        path for path in root.rglob("*")
        if path.is_file() and path.name not in {"manifest.json", "manifest.csv", "README.md"}
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--username", default="a")
    parser.add_argument("--password", default="1")
    parser.add_argument("--corpus", default="tests/data/corpus")
    parser.add_argument("--target-root", default="/test-corpus")
    parser.add_argument("--chunk-size", type=int, default=8 * 1024 * 1024)
    parser.add_argument("--agent-url", default="http://127.0.0.1:8081")
    parser.add_argument("--no-summarize", action="store_true")
    parser.add_argument("--force-summary", action="store_true")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    corpus_root = (repo_root / args.corpus).resolve()
    files = corpus_files(corpus_root)
    token = login(args.base_url, args.username, args.password)
    print(f"authenticated user={args.username}; files={len(files)}")

    uploaded = []
    failed = []
    summary_tasks = []
    summary_failed = []
    for index, path in enumerate(files, start=1):
        rel = path.relative_to(corpus_root)
        directory = args.target_root.rstrip("/")
        if rel.parent != Path("."):
            directory += "/" + str(rel.parent).replace("\\", "/")
        try:
            create_folder(args.base_url, token, directory)
            result = upload_file(args.base_url, token, path, path.name, directory, args.chunk_size)
            uploaded.append({"path": str(rel), "directory": directory, **result})
            if not args.no_summarize and path.suffix.lower() in SUMMARIZABLE_EXTENSIONS:
                status, task = start_summary(args.agent_url, token, result["hash"], args.force_summary)
                if status == 200:
                    summary_tasks.append({"path": str(rel), "hash": result["hash"], "task": task.get("id")})
                else:
                    summary_failed.append({"path": str(rel), "hash": result["hash"], "status": status, "response": task})
            print(f"[{index}/{len(files)}] OK {rel} -> {directory}")
        except Exception as exc:
            failed.append({"path": str(rel), "error": str(exc)})
            print(f"[{index}/{len(files)}] FAIL {rel}: {exc}")

    all_files = list_files(args.base_url, token)
    summary = {
        "attempted": len(files),
        "uploaded": len(uploaded),
        "failed": len(failed),
        "summary_tasks": len(summary_tasks),
        "summary_failed": len(summary_failed),
        "account_file_count": len(all_files),
        "target_root": args.target_root,
        "failed_items": failed,
        "summary_failed_items": summary_failed,
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
