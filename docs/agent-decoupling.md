# SmartNAS Agent Decoupling

## Boundary

SmartNAS Core is the NAS service. It owns authentication, upload, download, preview, delete, deduplication, and file metadata persistence.

SmartNAS Agent is a separate Python service. It owns LangChain orchestration, prompts, model loading, tool calling, summaries, vector search, and chat history.

The two services communicate only through HTTP APIs with the user's SmartNAS JWT. The C++ process must not import Python, call LangChain, load local LLMs, or block upload paths on AI work.

The `SmartNAS` CMake target contains only the NAS core files. Agent, CLIP, model loading, and vector-search code are not linked into the C++ gateway.

## Core APIs for Agent

- `GET /api/v1/files/search?keyword=...`
  Searches the current user's filenames and stored summaries. Requires `Authorization: Bearer <token>`.

- `POST /api/v1/files/summary`
  Updates a file summary owned by the current user. Requires `Authorization: Bearer <token>` and `File-Hash: <hash>`. The raw request body is the new summary text.

Existing file APIs such as preview and download remain NAS Core APIs. The agent may call them as tools if it needs file content, but the core service does not call the agent.

## Python Agent Shape

Keep `scripts/agent_service.py` as the service entrypoint:

- FastAPI exposes `/api/agent/chat`.
- Tools are built around NAS Core HTTP APIs.
- The bearer token from the browser is forwarded to NAS Core.
- Tool results are grounded in NAS Core responses.
- The agent never reads SQLite directly.
- Chat history is scoped per bearer token so users do not share agent context.
- Local chat generation uses the Python `transformers` backend. `SMARTNAS_MODEL_PATH` must point to a HuggingFace model directory, not a GGUF file.

## Startup

Run the services independently:

```bash
./bin/SmartNAS
python scripts/agent_service.py
```

The browser can talk to NAS Core on `8080` and Agent on `8081`.
