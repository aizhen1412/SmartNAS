from pathlib import Path
from typing import Any, Dict, Iterable, Set


def compute_missing_index_files(
    file_items: Iterable[Dict[str, Any]],
    index_status: Dict[str, Any],
    supported_extensions: Set[str],
) -> Dict[str, Any]:
    indexed = {
        item.get("hash")
        for item in index_status.get("files", [])
        if item.get("hash") and item.get("status") == "indexed" and int(item.get("chunk_count") or 0) > 0
    }
    missing = []
    skipped_unsupported = []
    scanned = 0
    for file_info in file_items:
        scanned += 1
        file_hash = file_info.get("hash", "")
        name = file_info.get("name", "")
        if not file_hash:
            continue
        if Path(name).suffix.lower() not in supported_extensions:
            skipped_unsupported.append({"hash": file_hash, "name": name})
            continue
        if file_hash not in indexed:
            missing.append({"hash": file_hash, "name": name, "directory": file_info.get("directory") or "/"})
    return {
        "scanned": scanned,
        "indexed": len(indexed),
        "missing": missing,
        "missing_count": len(missing),
        "skipped_unsupported": skipped_unsupported,
        "skipped_unsupported_count": len(skipped_unsupported),
    }
