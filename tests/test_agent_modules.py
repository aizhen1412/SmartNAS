import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from agent import config, llm_client
from agent import keyword_store
from agent import rag_store
from agent.index_service import compute_missing_index_files
from agent.markdown_service import MarkdownService, filename_from_content_disposition
from agent.ranker import merge_retrieval_results, rank_chunk_results
from agent.retrieval_service import RetrievalFilters, search_documents
from agent.schemas import FileQuestionRequest, IndexRebuildRequest
from agent.sse import sse_event
from agent.task_store import TaskStore


class FakeStreamResponse:
    status_code = 200
    text = ""

    def __enter__(self):
        return self

    def __exit__(self, *_):
        return False

    def iter_lines(self, **_):
        return iter([
            'data: {"choices":[{"delta":{"content":"**你好"}}]}',
            'data: {"choices":[{"delta":{"content":"世界**"}}]}',
            "data: [DONE]",
        ])


class AgentModuleTests(unittest.TestCase):
    def test_request_schema(self):
        request = FileQuestionRequest(hash="abc", question="内容是什么？")
        self.assertEqual(request.hash, "abc")

    def test_index_rebuild_schema_defaults(self):
        request = IndexRebuildRequest()
        self.assertIsNone(request.hash)
        self.assertFalse(request.force)
        self.assertTrue(request.include_keyword)

    def test_audit_full_content_disabled_by_default(self):
        self.assertFalse(config.AUDIT_LOG_FULL_CONTENT)

    def test_sse_event(self):
        event = sse_event("delta", content="你好")
        self.assertTrue(event.startswith("data: "))
        self.assertTrue(event.endswith("\n\n"))

    def test_stream_client_removes_markdown_stars(self):
        with patch.object(llm_client, "DEEPSEEK_API_KEY", "test-key"), patch.object(
            llm_client.requests, "post", return_value=FakeStreamResponse()
        ):
            chunks = list(llm_client.stream_chat_completion([{"role": "user", "content": "hi"}]))
        self.assertEqual(chunks, ["你好", "世界"])

    def test_rank_chunk_results_orders_by_score(self):
        results = [
            {"hash": "a", "filename": "a.txt", "chunk_index": 0, "score": 0.3},
            {"hash": "b", "filename": "b.txt", "chunk_index": 0, "score": 0.9},
            {"hash": "c", "filename": "c.txt", "chunk_index": 0, "score": 0.5},
        ]
        ranked = rank_chunk_results(results, 2)
        self.assertEqual([item["hash"] for item in ranked], ["b", "c"])

    def test_merge_retrieval_results_boosts_shared_chunks(self):
        merged = merge_retrieval_results(
            [{"hash": "a", "chunk_index": 0, "score": 0.4, "source": "vector"}],
            [{"hash": "a", "chunk_index": 0, "score": 0.5, "source": "keyword"}],
        )
        self.assertEqual(len(merged), 1)
        self.assertEqual(merged[0]["source"], "keyword+vector")
        self.assertGreater(merged[0]["score"], 0.5)

    def test_search_documents_passes_file_filter(self):
        with patch("agent.retrieval_service.search_rag_chunks") as vector_search, patch(
            "agent.retrieval_service.search_keyword_chunks"
        ) as keyword_search:
            vector_search.return_value = {
                "available": True,
                "results": [{"hash": "a", "filename": "a.txt", "chunk_index": 0, "score": 0.4}],
                "reason": None,
            }
            keyword_search.return_value = {"available": True, "results": [], "reason": None}
            result = search_documents("query", "Bearer token", 3, filters=RetrievalFilters(file_hash="a", directory="/docs"))
        vector_search.assert_called_once_with("query", "Bearer token", 3, file_hash="a", directory="/docs")
        keyword_search.assert_called_once_with("query", "Bearer token", 6, file_hash="a", directory="/docs")
        self.assertEqual(result["results"][0]["hash"], "a")

    def test_index_manifest_status_counts(self):
        with tempfile.TemporaryDirectory() as tmpdir, patch.object(rag_store, "VECTOR_DIR", Path(tmpdir)), patch(
            "agent.rag_store.get_user_identity", return_value="alice"
        ):
            rag_store.mark_file_index_status("Bearer token", "hash-a", "a.txt", "indexed", chunk_count=2)
            status = rag_store.index_status("Bearer token")
        self.assertEqual(status["file_count"], 1)
        self.assertEqual(status["status_counts"], {"indexed": 1})
        self.assertEqual(status["files"][0]["chunk_count"], 2)

    def test_keyword_index_searches_metadata_and_text(self):
        token = "Bearer token"
        with tempfile.TemporaryDirectory() as tmpdir, patch.object(rag_store, "VECTOR_DIR", Path(tmpdir)), patch(
            "agent.rag_store.get_user_identity", return_value="alice"
        ), patch(
            "agent.keyword_store.fetch_all_user_files",
            return_value=[
                {
                    "hash": "hash-a",
                    "name": "项目计划.txt",
                    "summary": "SmartNAS 检索计划",
                    "tags": ["检索", "成本"],
                    "directory": "/docs",
                }
            ],
        ), patch(
            "agent.keyword_store.load_vector_chunks",
            return_value=[
                {
                    "id": "hash-a:0",
                    "hash": "hash-a",
                    "filename": "old.txt",
                    "chunk_index": 0,
                    "text": "召回 排序 索引 筛选",
                }
            ],
        ):
            keyword_store.keyword_index_cache.clear()
            keyword_store.rebuild_keyword_index(token)
            result = keyword_store.search_keyword_chunks("排序", token, 3, directory="/docs")
        self.assertEqual(result["results"][0]["hash"], "hash-a")
        self.assertEqual(result["results"][0]["filename"], "项目计划.txt")

    def test_keyword_index_dirty_status(self):
        token = "Bearer token"
        with tempfile.TemporaryDirectory() as tmpdir, patch.object(rag_store, "VECTOR_DIR", Path(tmpdir)), patch(
            "agent.rag_store.get_user_identity", return_value="alice"
        ):
            keyword_store.mark_keyword_index_dirty(token, "test")
            status = keyword_store.keyword_index_status(token)
        self.assertTrue(status["dirty"])
        self.assertEqual(status["reason"], "test")

    def test_keyword_search_uses_clean_index_without_rebuild(self):
        token = "Bearer token"
        with tempfile.TemporaryDirectory() as tmpdir, patch.object(rag_store, "VECTOR_DIR", Path(tmpdir)), patch(
            "agent.rag_store.get_user_identity", return_value="alice"
        ), patch(
            "agent.keyword_store.fetch_all_user_files",
            return_value=[
                {
                    "hash": "hash-a",
                    "name": "项目计划.txt",
                    "summary": "SmartNAS 检索计划",
                    "tags": ["检索", "成本"],
                    "directory": "/docs",
                }
            ],
        ), patch(
            "agent.keyword_store.load_vector_chunks",
            return_value=[
                {
                    "id": "hash-a:0",
                    "hash": "hash-a",
                    "filename": "old.txt",
                    "chunk_index": 0,
                    "text": "召回 排序 索引 筛选",
                }
            ],
        ):
            keyword_store.keyword_index_cache.clear()
            keyword_store.rebuild_keyword_index(token)
            keyword_store.keyword_index_cache.clear()
            with patch("agent.keyword_store.rebuild_keyword_index") as rebuild:
                result = keyword_store.search_keyword_chunks("排序", token, 3)
        rebuild.assert_not_called()
        self.assertEqual(result["results"][0]["hash"], "hash-a")

    def test_compute_missing_index_files(self):
        result = compute_missing_index_files(
            [
                {"hash": "indexed", "name": "a.pdf", "directory": "/docs"},
                {"hash": "missing", "name": "b.txt", "directory": "/docs"},
                {"hash": "unsupported", "name": "c.exe", "directory": "/bin"},
            ],
            {
                "files": [
                    {"hash": "indexed", "status": "indexed", "chunk_count": 2},
                    {"hash": "empty", "status": "indexed", "chunk_count": 0},
                ]
            },
            {".pdf", ".txt"},
        )
        self.assertEqual(result["scanned"], 3)
        self.assertEqual([item["hash"] for item in result["missing"]], ["missing"])
        self.assertEqual(result["skipped_unsupported_count"], 1)

    def test_task_store_restores_index_tasks_and_marks_interrupted_work_failed(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            store = TaskStore(Path(tmpdir) / "tasks.db")
            store.initialize()
            pending = {
                "id": "index-1", "owner": "alice", "hash": "file-a", "force": False,
                "include_keyword": True, "status": "running", "message": "running",
                "created_at": 1.0, "updated_at": 1.0,
            }
            completed = {
                "id": "summary-1", "owner": "alice", "hash": "file-b", "force": False,
                "status": "success", "message": "done", "result": {"summary": "ok"},
                "created_at": 2.0, "updated_at": 2.0,
            }
            store.save(pending, "index")
            store.save(completed, "summary")
            restored = store.initialize()
        self.assertEqual(restored["index"][0]["status"], "failed")
        self.assertEqual(restored["index"][0]["error"], "interrupted")
        self.assertTrue(restored["index"][0]["include_keyword"])
        self.assertEqual(restored["summary"][0]["result"], {"summary": "ok"})

    def test_markdown_service_text_fallback_and_filename_decoding(self):
        markdown = MarkdownService.fallback_markdown("计划.txt", ".txt", "检索计划".encode("utf-8"))
        self.assertIn("检索计划", markdown)
        self.assertEqual(
            filename_from_content_disposition("attachment; filename*=UTF-8''%E7%90%86%E6%83%B3%E5%9B%BD.txt"),
            "理想国.txt",
        )


if __name__ == "__main__":
    unittest.main()
