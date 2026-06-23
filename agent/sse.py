import json
from typing import Iterable

from fastapi.responses import StreamingResponse

SSE_HEADERS = {
    "Cache-Control": "no-cache, no-transform",
    "X-Accel-Buffering": "no",
    "Connection": "keep-alive",
}


def sse_event(event_type: str, **payload) -> str:
    return f"data: {json.dumps({'type': event_type, **payload}, ensure_ascii=False)}\n\n"


def sse_response(events: Iterable[str]) -> StreamingResponse:
    return StreamingResponse(events, media_type="text/event-stream", headers=SSE_HEADERS)


