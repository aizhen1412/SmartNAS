from pydantic import BaseModel

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


