import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from agent import config, llm_client
from agent.schemas import FileQuestionRequest
from agent.sse import sse_event


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


if __name__ == "__main__":
    unittest.main()
