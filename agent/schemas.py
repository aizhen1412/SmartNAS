from pydantic import BaseModel
from typing import Optional

from .config import RAG_TOP_K


class ChatRequest(BaseModel):
    prompt: str


class SummarizeRequest(BaseModel):
    hash: str
    force: bool = False


class FileQuestionRequest(BaseModel):
    hash: str
    question: str


class RagQueryRequest(BaseModel):
    query: str
    top_k: int = RAG_TOP_K
    file_hash: Optional[str] = None
    hash: Optional[str] = None
    directory: Optional[str] = None


class IndexRebuildRequest(BaseModel):
    hash: Optional[str] = None
    force: bool = False
    include_keyword: bool = True
