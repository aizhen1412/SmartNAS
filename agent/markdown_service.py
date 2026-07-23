"""Download and convert user files into cached Markdown."""

import io
import mimetypes
import re
import tempfile
from pathlib import Path
from typing import Callable, Set, Tuple
from urllib.parse import unquote

import requests
from fastapi import HTTPException
from PIL import ExifTags, Image

from .config import CACHE_DIR, CACHE_FORMAT_VERSION, NAS_CORE_API


IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp", ".tiff", ".tif"}
TEXT_LIKE_EXTENSIONS = {".txt", ".csv", ".json", ".html", ".htm", ".md", ".xml", ".svg"}
MEDIA_EXTENSIONS = {".wav", ".mp3", ".m4a", ".mp4"}


def filename_from_content_disposition(header_value: str) -> str:
    if not header_value:
        return "document"
    match = re.search(r'filename\*?=(?:UTF-8\'\')?"?([^";]+)"?', header_value, re.IGNORECASE)
    return unquote(match.group(1)).strip() if match else "document"


class MarkdownService:
    def __init__(self, supported_extensions: Set[str], converter_factory: Callable[[], object]):
        self.supported_extensions = supported_extensions
        self.converter_factory = converter_factory

    @staticmethod
    def cache_path(file_hash: str) -> Path:
        safe_hash = re.sub(r"[^a-zA-Z0-9_.-]", "_", file_hash)
        return CACHE_DIR / f"{safe_hash}.v{CACHE_FORMAT_VERSION}.md"

    def fetch_metadata(self, file_hash: str, token: str) -> Tuple[str, str]:
        try:
            response = requests.head(f"{NAS_CORE_API}/download", params={"hash": file_hash}, headers={"Authorization": token}, timeout=60)
        except requests.RequestException as exc:
            raise HTTPException(status_code=502, detail=f"无法连接核心下载接口: {exc}") from exc
        try:
            if response.status_code == 403:
                raise HTTPException(status_code=403, detail="没有权限读取该文件")
            if response.status_code == 404:
                raise HTTPException(status_code=404, detail="文件不存在")
            if response.status_code != 200:
                raise HTTPException(status_code=response.status_code, detail=f"核心下载接口返回 {response.status_code}")
            filename = filename_from_content_disposition(response.headers.get("Content-Disposition", ""))
            suffix = Path(filename).suffix.lower()
            if suffix not in self.supported_extensions:
                raise HTTPException(status_code=415, detail=f"暂不支持该文件类型: {suffix or 'unknown'}")
            return filename, suffix
        finally:
            response.close()

    def download(self, file_hash: str, token: str, suffix: str) -> Path:
        try:
            response = requests.get(f"{NAS_CORE_API}/download", params={"hash": file_hash}, headers={"Authorization": token}, timeout=60, stream=True)
        except requests.RequestException as exc:
            raise HTTPException(status_code=502, detail=f"无法连接核心下载接口: {exc}") from exc
        path = None
        try:
            if response.status_code == 403:
                raise HTTPException(status_code=403, detail="没有权限读取该文件")
            if response.status_code == 404:
                raise HTTPException(status_code=404, detail="文件不存在")
            if response.status_code != 200:
                raise HTTPException(status_code=response.status_code, detail=f"核心下载接口返回 {response.status_code}")
            with tempfile.NamedTemporaryFile(prefix="smartnas_", suffix=suffix, delete=False) as output:
                path = Path(output.name)
                for chunk in response.iter_content(chunk_size=1024 * 1024):
                    if chunk:
                        output.write(chunk)
            return path
        except Exception:
            if path:
                path.unlink(missing_ok=True)
            raise
        finally:
            response.close()

    @staticmethod
    def fallback_markdown(filename: str, suffix: str, content: bytes) -> str:
        if suffix in IMAGE_EXTENSIONS:
            with Image.open(io.BytesIO(content)) as image:
                lines = [f"# {filename}", "", "## Image Metadata", f"- Format: {image.format or 'unknown'}", f"- Size: {image.width} x {image.height}", f"- Mode: {image.mode}"]
                exif = getattr(image, "getexif", lambda: {})()
                if exif:
                    names = {value: key for key, value in ExifTags.TAGS.items()}
                    lines.extend(["", "## EXIF"])
                    for name in ("ImageDescription", "Make", "Model", "DateTime", "DateTimeOriginal", "Artist", "Copyright"):
                        tag = names.get(name)
                        if tag in exif:
                            value = exif.get(tag)
                            value = value.decode("utf-8", errors="ignore").strip("\x00") if isinstance(value, bytes) else value
                            lines.append(f"- {name}: {value}")
                return "\n".join(lines).strip()
        if suffix in TEXT_LIKE_EXTENSIONS:
            return f"# {filename}\n\n```text\n{content.decode('utf-8', errors='replace')}\n```"
        if suffix in MEDIA_EXTENSIONS:
            mime_type, _ = mimetypes.guess_type(filename)
            return "\n".join([f"# {filename}", "", "## Media Metadata", f"- Type: {suffix.lstrip('.') or 'unknown'}", f"- MIME: {mime_type or 'unknown'}", f"- Size: {len(content)} bytes"])
        return ""

    def convert(self, file_hash: str, token: str, force: bool = False):
        cache_path = self.cache_path(file_hash)
        filename, suffix = self.fetch_metadata(file_hash, token)
        if cache_path.exists() and not force:
            return filename, cache_path.read_text(encoding="utf-8"), True
        temp_path = self.download(file_hash, token, suffix)
        try:
            CACHE_DIR.mkdir(parents=True, exist_ok=True)
            try:
                if suffix in IMAGE_EXTENSIONS:
                    markdown = self.fallback_markdown(filename, suffix, temp_path.read_bytes())
                else:
                    converter = self.converter_factory()
                    converted = converter.convert_local(str(temp_path)) if hasattr(converter, "convert_local") else converter.convert(str(temp_path))
                    markdown = converted.text_content
            except Exception as exc:
                if suffix not in IMAGE_EXTENSIONS | TEXT_LIKE_EXTENSIONS | MEDIA_EXTENSIONS:
                    raise HTTPException(status_code=422, detail=f"MarkItDown 转换失败: {exc}") from exc
                markdown = ""
            if not markdown.strip():
                try:
                    markdown = self.fallback_markdown(filename, suffix, temp_path.read_bytes())
                except Exception as exc:
                    raise HTTPException(status_code=422, detail=f"文件转换失败: {exc}") from exc
            cache_path.write_text(markdown, encoding="utf-8")
            return filename, markdown, False
        finally:
            temp_path.unlink(missing_ok=True)
