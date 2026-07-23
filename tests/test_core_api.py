"""Black-box regression test for the SmartNAS Core HTTP API.

It starts an isolated Core instance with a temporary database and data
directory, so it never touches the developer's local files or login state.
"""

import hashlib
import json
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


class CoreApiIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path("build/bin/SmartNAS").resolve()
        if not cls.binary.exists():
            raise unittest.SkipTest(f"Core binary not found: {cls.binary}")
        cls.tempdir = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tempdir.name)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            cls.port = probe.getsockname()[1]
        config = {
            "core_host": "127.0.0.1",
            "core_port": cls.port,
            "agent_port": cls.port + 1,
            "database_path": str(cls.root / "smartnas.db"),
            "data_dir": str(cls.root / "data"),
            "web_dir": str(Path(__file__).resolve().parents[1] / "web"),
            "upload_chunk_size": 1024,
            "upload_concurrency": 1,
        }
        cls.config_path = cls.root / "config.json"
        cls.config_path.write_text(json.dumps(config), encoding="utf-8")
        cls.process = subprocess.Popen(
            [str(cls.binary), str(cls.config_path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        cls.base_url = f"http://127.0.0.1:{cls.port}"
        for _ in range(40):
            try:
                if cls.request("GET", "/ping")[0] == 200:
                    return
            except OSError:
                time.sleep(0.1)
        cls.tearDownClass()
        raise RuntimeError("Core did not start for integration tests")

    @classmethod
    def tearDownClass(cls):
        process = getattr(cls, "process", None)
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        tempdir = getattr(cls, "tempdir", None)
        if tempdir:
            tempdir.cleanup()

    @classmethod
    def request(cls, method, path, body=None, headers=None):
        request = Request(cls.base_url + path, data=body, method=method, headers=headers or {})
        try:
            with urlopen(request, timeout=3) as response:
                return response.status, response.read().decode("utf-8")
        except HTTPError as error:
            return error.code, error.read().decode("utf-8")

    def request_json(self, method, path, body=None, headers=None):
        status, text = self.request(method, path, body, headers)
        return status, json.loads(text) if text else {}

    def test_file_lifecycle(self):
        username, password = "integration-user", "integration-password"
        payload = json.dumps({"user": username, "password": password}).encode("utf-8")
        status, _ = self.request("POST", "/register", payload, {"Content-Type": "application/json"})
        self.assertEqual(status, 200)

        status, login = self.request_json("POST", "/login", payload, {"Content-Type": "application/json"})
        self.assertEqual(status, 200)
        token = login["token"]
        auth = {"Authorization": f"Bearer {token}"}

        status, _ = self.request_json("POST", "/api/folders?path=/docs", headers=auth)
        self.assertEqual(status, 200)

        content = b"SmartNAS integration upload"
        status, uploaded = self.request_json(
            "POST", "/upload", content,
            {**auth, "File-Name": "note.txt", "Directory": "/docs"},
        )
        self.assertEqual(status, 200)
        self.assertEqual(uploaded["hash"], hashlib.sha256(content).hexdigest())
        file_hash = uploaded["hash"]

        status, listing = self.request_json("GET", "/api/list?dir=/docs", headers=auth)
        self.assertEqual(status, 200)
        self.assertEqual([(item["name"], item["hash"]) for item in listing["files"]], [("note.txt", file_hash)])

        status, _ = self.request_json("POST", "/api/rename?" + urlencode({"hash": file_hash, "name": "renamed.txt"}), headers=auth)
        self.assertEqual(status, 200)
        status, _ = self.request_json("POST", "/api/move?" + urlencode({"hash": file_hash, "dir": "/archive"}), headers=auth)
        self.assertEqual(status, 200)

        status, listing = self.request_json("GET", "/api/list?dir=/archive", headers=auth)
        self.assertEqual(status, 200)
        self.assertEqual(listing["files"][0]["name"], "renamed.txt")

        status, _ = self.request_json("POST", "/api/delete?" + urlencode({"hash": file_hash}), headers=auth)
        self.assertEqual(status, 200)
        status, deleted = self.request_json("GET", "/api/list?deleted=1", headers=auth)
        self.assertEqual(status, 200)
        self.assertEqual(deleted["files"][0]["hash"], file_hash)
        status, _ = self.request_json("POST", "/api/restore?" + urlencode({"hash": file_hash}), headers=auth)
        self.assertEqual(status, 200)

    def test_frontend_state_asset_is_served(self):
        status, content = self.request("GET", "/assets/js/state.js")
        self.assertEqual(status, 200)
        self.assertIn("SmartNASState", content)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
