from dataclasses import dataclass
from typing import Any, Dict, Optional

from .config import RAG_TOP_K
from .keyword_store import search_keyword_chunks
from .rag_store import search_rag_chunks
from .ranker import merge_retrieval_results, rank_chunk_results


@dataclass(frozen=True)
class RetrievalFilters:
    file_hash: Optional[str] = None
    directory: Optional[str] = None


def search_documents(
    query: str,
    token: str,
    top_k: int = RAG_TOP_K,
    filters: Optional[RetrievalFilters] = None,
) -> Dict[str, Any]:
    filters = filters or RetrievalFilters()
    try:
        vector_search = search_rag_chunks(
            query,
            token,
            top_k,
            file_hash=filters.file_hash,
            directory=filters.directory,
        )
    except Exception as exc:
        vector_search = {"available": False, "results": [], "reason": str(exc)}
    try:
        keyword_search = search_keyword_chunks(
            query,
            token,
            max(top_k * 2, top_k),
            file_hash=filters.file_hash,
            directory=filters.directory,
        )
    except Exception as exc:
        keyword_search = {"available": False, "results": [], "reason": str(exc)}
    results = merge_retrieval_results(
        vector_search.get("results", []),
        keyword_search.get("results", []),
    )
    return {
        "available": bool(vector_search.get("available") or keyword_search.get("available")),
        "results": rank_chunk_results(results, top_k),
        "reason": None if results else vector_search.get("reason") or keyword_search.get("reason"),
        "retrieval": {
            "vector": {
                "available": vector_search.get("available", False),
                "count": len(vector_search.get("results", [])),
                "reason": vector_search.get("reason"),
            },
            "keyword": {
                "available": keyword_search.get("available", False),
                "count": len(keyword_search.get("results", [])),
                "reason": keyword_search.get("reason"),
            },
        },
    }
