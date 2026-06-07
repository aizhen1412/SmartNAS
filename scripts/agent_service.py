import uvicorn
from fastapi import FastAPI, Header, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
import os
import requests
import re
from typing import Dict, List, Optional

try:
    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer
except Exception as exc:
    torch = None
    AutoModelForCausalLM = None
    AutoTokenizer = None
    TRANSFORMERS_IMPORT_ERROR = exc
else:
    TRANSFORMERS_IMPORT_ERROR = None

app = FastAPI(title="SmartNAS Agent Service")

# 添加 CORS 支持，允许所有来源跨域
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

NAS_CORE_API = os.getenv("SMARTNAS_CORE_API", "http://127.0.0.1:8080")
MODEL_PATH = os.getenv("SMARTNAS_MODEL_PATH", "models/llm/qwen2.5-7b-instruct")
MAX_NEW_TOKENS = int(os.getenv("SMARTNAS_MAX_NEW_TOKENS", "512"))
model_bundle = None

sessions: Dict[str, List[dict]] = {}
system_prompt = """你是 SmartNAS 的智能管家。你可以通过查询数据库来帮助用户找文件。
【规则1】当用户向你询问文件内容、搜索文件时，你必须调用搜索功能，此时必须【有且仅有】回答如下格式的指令：
CALL: search_files(关键词)
【规则2】当你收到系统返回的【后台数据库查询返回】信息时，说明检索已完成。此时你必须用自然语言总结查询结果并直接回答用户。"""

class ChatRequest(BaseModel):
    prompt: str

def get_model_bundle():
    global model_bundle
    if model_bundle is not None:
        return model_bundle

    if AutoTokenizer is None or AutoModelForCausalLM is None or torch is None:
        raise HTTPException(status_code=503, detail=f"transformers/torch 加载失败: {TRANSFORMERS_IMPORT_ERROR}")
    if not os.path.exists(MODEL_PATH):
        raise HTTPException(status_code=503, detail=f"Transformers 模型目录不存在: {MODEL_PATH}")
    if not os.path.isdir(MODEL_PATH):
        raise HTTPException(status_code=503, detail=f"Transformers 需要模型目录，不支持 GGUF 文件: {MODEL_PATH}")

    print(f"Loading Transformers model: {MODEL_PATH}")
    tokenizer = AutoTokenizer.from_pretrained(
        MODEL_PATH,
        trust_remote_code=True,
    )
    model = AutoModelForCausalLM.from_pretrained(
        MODEL_PATH,
        torch_dtype="auto",
        device_map="auto",
        trust_remote_code=True,
    )
    model.eval()
    model_bundle = {"tokenizer": tokenizer, "model": model}
    return model_bundle

def create_chat_completion(messages: List[dict]) -> str:
    bundle = get_model_bundle()
    tokenizer = bundle["tokenizer"]
    model = bundle["model"]

    text = tokenizer.apply_chat_template(
        messages,
        tokenize=False,
        add_generation_prompt=True,
    )
    inputs = tokenizer([text], return_tensors="pt")
    inputs = {k: v.to(model.device) for k, v in inputs.items()}

    with torch.no_grad():
        generated_ids = model.generate(
            **inputs,
            max_new_tokens=MAX_NEW_TOKENS,
            do_sample=True,
            temperature=0.7,
            top_p=0.8,
            repetition_penalty=1.05,
        )

    new_tokens = generated_ids[0][inputs["input_ids"].shape[-1]:]
    return tokenizer.decode(new_tokens, skip_special_tokens=True).strip()

def require_bearer_token(authorization: Optional[str]) -> str:
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="Missing SmartNAS bearer token")
    return authorization

@app.get("/api/agent/health")
async def health():
    return {
        "status": "ok",
        "core_api": NAS_CORE_API,
        "backend": "transformers",
        "model_path": MODEL_PATH,
        "model_exists": os.path.exists(MODEL_PATH),
        "model_is_directory": os.path.isdir(MODEL_PATH),
        "model_loaded": model_bundle is not None,
        "transformers_available": AutoModelForCausalLM is not None and AutoTokenizer is not None and torch is not None,
        "transformers_error": str(TRANSFORMERS_IMPORT_ERROR) if TRANSFORMERS_IMPORT_ERROR else None,
    }

@app.post("/api/agent/chat")
async def chat_endpoint(request: ChatRequest, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    history = sessions.setdefault(token, [])

    if not history:
        history.append({"role": "system", "content": system_prompt})

    if len(history) > 10:
        # 简单截断防止超出上下文
        sessions[token] = [history[0]] + history[-5:]
        history = sessions[token]

    history.append({"role": "user", "content": request.prompt})

    try:
        text = create_chat_completion(history)

        match = re.search(r"CALL:\s*search_files\((.*?)\)", text)
        if match:
            keyword = match.group(1).strip("'\"")
            print(f"[Tool] Searching files for: {keyword}")
            try:
                r = requests.get(
                    f"{NAS_CORE_API}/api/v1/files/search",
                    params={"keyword": keyword},
                    headers={"Authorization": token},
                    timeout=10,
                )
                if r.status_code == 200:
                    data = r.json()
                    db_result = f"【后台数据库查询返回】共找到 {len(data)} 个文件。\n"
                    for f in data:
                        db_result += f"- {f.get('filename')} (摘要: {f.get('summary')})\n"
                    if not data:
                        db_result += "未找到任何记录。"
                else:
                    db_result = f"核心 API 错误: {r.status_code}"
            except Exception as e:
                db_result = f"无法连接到核心服务: {e}"

            sys_msg = f"【系统消息】查询完成。结果如下：\n{db_result}\n请根据此结果使用自然语言回答用户的原始问题，绝不要再输出CALL指令。"
            history.append({"role": "assistant", "content": text})
            history.append({"role": "system", "content": sys_msg})

            final_text = create_chat_completion(history)
            history.append({"role": "assistant", "content": final_text})

            return {"status": "success", "response": final_text}
        else:
            history.append({"role": "assistant", "content": text})
            return {"status": "success", "response": text}
    except HTTPException:
        raise
    except Exception as e:
        return {"status": "error", "message": f"Agent 执行异常: {str(e)}"}

@app.post("/api/agent/clear_history")
async def clear_history(authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    sessions.pop(token, None)
    return {"status": "success"}

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8081)
