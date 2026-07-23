from typing import Any, Dict, List, Optional


def merge_retrieval_results(*result_sets: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    merged: Dict[tuple, Dict[str, Any]] = {}
    for results in result_sets:
        for item in results:
            key = (item.get("hash"), item.get("chunk_index"))
            if key in merged:
                current = merged[key]
                current["score"] = round(max(float(current.get("score") or 0), float(item.get("score") or 0)) + 0.08, 4)
                sources = set(str(current.get("source") or "").split("+"))
                sources.update(str(item.get("source") or "").split("+"))
                current["source"] = "+".join(sorted(source for source in sources if source))
                continue
            merged[key] = dict(item)
    return list(merged.values())


def rank_chunk_results(
    results: List[Dict[str, Any]],
    top_k: int,
    *,
    per_file_limit: Optional[int] = None,
) -> List[Dict[str, Any]]:
    if not results:
        return []

    ranked = sorted(
        results,
        key=lambda item: (
            float(item.get("score") or 0),
            str(item.get("filename") or ""),
            -int(item.get("chunk_index") or 0),
        ),
        reverse=True,
    )

    selected = []
    file_counts: Dict[str, int] = {}
    limit = max(1, top_k)
    for item in ranked:
        file_hash = str(item.get("hash") or "")
        if per_file_limit is not None and file_hash:
            current = file_counts.get(file_hash, 0)
            if current >= per_file_limit:
                continue
            file_counts[file_hash] = current + 1
        selected.append(item)
        if len(selected) >= limit:
            break
    return selected
