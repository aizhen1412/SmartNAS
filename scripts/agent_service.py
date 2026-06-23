"""Backward-compatible launcher for the SmartNAS Agent package."""
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

import uvicorn
from agent.config import AGENT_HOST, AGENT_PORT
from agent.service import app

if __name__ == "__main__":
    uvicorn.run(app, host=AGENT_HOST, port=AGENT_PORT)
