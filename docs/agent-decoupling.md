# SmartNAS Agent Decoupling

## Boundary

SmartNAS Core is the NAS service. It owns authentication, upload, download, preview, delete, deduplication, and file metadata persistence.

SmartNAS Agent is a separate Python service. It owns prompts, DeepSeek API calls, tool calling, document conversion, summaries, vector search, and chat history.

The two services communicate only through HTTP APIs with the user's SmartNAS JWT. The C++ process must not import Python, call LangChain, load local LLMs, or block upload paths on AI work.

The `SmartNAS` CMake target contains only the NAS core files. Agent, CLIP, LLM API calls, and vector-search code are not linked into the C++ gateway.

## Core APIs for Agent

- `GET /api/v1/files/search?keyword=...`
  Searches the current user's filenames and stored summaries. Requires `Authorization: Bearer <token>`.

- `POST /api/v1/files/summary`
  Updates a file summary owned by the current user. Requires `Authorization: Bearer <token>` and `File-Hash: <hash>`. The raw request body is the new summary text.

Existing file APIs such as preview and download remain NAS Core APIs. The agent may call them as tools if it needs file content, but the core service does not call the agent.

Additional NAS Core APIs now cover normal cloud-drive workflows:

- `GET /api/list?dir=/path`
  Returns folders and active files in a directory.

- `GET /api/list?deleted=1`
  Returns files in the recycle bin.

- `POST /api/folders?path=/path`
  Creates a folder.

- `POST /api/rename?hash=...&name=...`
  Renames a file owned by the current user.

- `POST /api/move?hash=...&dir=/path`
  Moves a file to a directory.

- `POST /api/delete?hash=...`
  Moves a file to the recycle bin.

- `POST /api/restore?hash=...`
  Restores a recycled file.

- `POST /api/purge?hash=...`
  Permanently removes the user's file reference and deletes the physical blob when no user references remain.

- `POST /api/share/create?hash=...&hours=24`
  Creates a temporary public download link.

## Python Agent Shape

Keep `scripts/agent_service.py` as the service entrypoint:

- FastAPI exposes `/api/agent/chat`.
- FastAPI exposes `/api/agent/summarize`, `/api/agent/summarize/start`, `/api/agent/summarize/status/{task_id}`, and `/api/agent/summarize/missing`.
- FastAPI exposes `/api/agent/markdown/{hash}` for inspecting extracted Markdown.
- FastAPI exposes `/api/agent/file_qa` for asking questions grounded in a single file's extracted Markdown.
- FastAPI exposes `/api/agent/rag/query` for semantic retrieval over indexed Markdown chunks.
- Tools are built around NAS Core HTTP APIs.
- The bearer token from the browser is forwarded to NAS Core.
- Tool results are grounded in NAS Core responses.
- The agent never reads SQLite directly.
- Chat history is scoped per bearer token so users do not share agent context.
- Chat, summary, and file QA generation use the DeepSeek OpenAI-compatible `/chat/completions` API.
- Configure `SMARTNAS_DEEPSEEK_API_KEY` or `DEEPSEEK_API_KEY` before starting the Agent.
- Document summaries use MarkItDown and local metadata extractors to convert supported files into Markdown before summarization.
- Text/document formats include `pdf`, `docx`, `pptx`, `xlsx`, `txt`, `csv`, `json`, `html`, `md`, `xml`, `epub`, `zip`, and `ipynb`.
- Image formats include `jpg`, `jpeg`, `png`, `webp`, `gif`, `bmp`, `tiff`, `tif`, and `svg`. Image summaries are based on image dimensions, EXIF, embedded metadata, and text-like SVG content. A multimodal vision model can be added later for visual scene captions.
- Media formats include `wav`, `mp3`, `m4a`, and `mp4`. These summaries are based on metadata unless a transcription dependency is installed and MarkItDown can extract a transcript.
- Converted Markdown is cached under `var/cache/markdown` by file hash so repeated summaries do not reconvert the same content.
- RAG chunks and embeddings are cached under `var/cache/vector` per bearer-token namespace. Summarizing a file also indexes its Markdown chunks.
- Chat first tries RAG over original Markdown chunks, then falls back to the Core summary/name keyword search if vector dependencies or indexed chunks are unavailable.
- Long Markdown content is summarized in chunks and then merged into a final short summary.

## Startup

Run the services independently:

```bash
./bin/SmartNAS
python scripts/agent_service.py
```

The browser can talk to NAS Core on `8080` and Agent on `8081`.

The convenience script starts both services and reads DeepSeek settings from the environment:

```bash
export SMARTNAS_DEEPSEEK_API_KEY=sk-...
scripts/start_smartnas.sh
```
