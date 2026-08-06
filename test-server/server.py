#!/usr/bin/env python3
"""
OTA 测试服务器

自动扫描 firmware/ 目录下的 .bin 文件，无条件返回最新固件地址。

固件命名: <version>.bin  (如 1.0.0.bin, 2.0.0.bin)
固件目录: ./firmware/

使用:
    python server.py [port]

API:
    POST /api/ota          - OTA 检查（始终返回固件信息）
    GET  /firmware/<file>  - 固件下载
    GET  /                 - 状态页
"""

import json
import re
import sys
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path

# ─── 配置 ───────────────────────────────────────────────────────

HOST = "0.0.0.0"
DEFAULT_PORT = 8080
FIRMWARE_DIR = Path(__file__).parent / "firmware"

# ─── 固件扫描 ───────────────────────────────────────────────────


def scan_firmware():
    """扫描 firmware/ 目录，返回 {version: filename} 字典，版本降序排列"""
    if not FIRMWARE_DIR.is_dir():
        return {}

    fw = {}
    for f in sorted(FIRMWARE_DIR.iterdir()):
        if f.suffix == ".bin":
            m = re.search(r'(\d+\.\d+\.\d+)', f.name)
            if m:
                fw[m.group(1)] = f.name

    return dict(sorted(fw.items(), key=lambda x: list(map(int, x[0].split('.'))), reverse=True))


def get_latest_firmware():
    """获取最新版本信息 (version, filename)，无固件返回 (None, None)"""
    fw = scan_firmware()
    if not fw:
        return None, None
    version = next(iter(fw))
    return version, fw[version]


def get_host_url(handler):
    host = handler.headers.get("Host", f"localhost:{DEFAULT_PORT}")
    return f"http://{host}"


# ─── HTTP Handler ───────────────────────────────────────────────


class OtaHandler(BaseHTTPRequestHandler):

    def do_GET(self):
        if self.path in ("/", ""):
            self.handle_status()
        elif self.path.startswith("/firmware/"):
            self.handle_firmware_download()
        else:
            self.send_error(404, "Not Found")

    def do_POST(self):
        if self.path in ("/api/ota", "/api/ota/"):
            self.handle_ota()
        else:
            self.send_error(404, "Not Found")

    # ── 状态页 ──────────────────────────────────────────────────

    def handle_status(self):
        fw = scan_firmware()
        lines = [
            "<!DOCTYPE html><html><head><meta charset='utf-8'><title>OTA Test Server</title>",
            "<style>body{font-family:monospace;max-width:600px;margin:40px auto;padding:0 20px}",
            "table{border-collapse:collapse;width:100%}td,th{border:1px solid #ddd;padding:6px 10px;text-align:left}",
            "</style></head><body>",
            "<h2>OTA Test Server</h2>",
            "<h3>Firmware</h3>",
        ]

        if fw:
            lines.append("<table><tr><th>Version</th><th>File</th><th>Size</th></tr>")
            for ver, fname in fw.items():
                fpath = FIRMWARE_DIR / fname
                size = fpath.stat().st_size
                lines.append(f"<tr><td>{ver}</td><td><a href='/firmware/{fname}'>{fname}</a></td><td>{size:,} bytes</td></tr>")
            lines.append("</table>")
        else:
            lines.append(f"<p>No firmware files in <code>{FIRMWARE_DIR}</code></p>")

        lines.append("</body></html>")
        body = "\n".join(lines).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # ── 固件下载 ────────────────────────────────────────────────

    def handle_firmware_download(self):
        filename = self.path.split("/")[-1]
        filepath = FIRMWARE_DIR / filename

        if not filepath.is_file():
            self.send_error(404, "Firmware not found")
            return

        size = filepath.stat().st_size
        self.log(f"\n[Firmware] {filename} ({size:,} bytes)")

        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.send_header("Content-Disposition", f'attachment; filename="{filename}"')
        self.end_headers()

        with open(filepath, "rb") as f:
            while chunk := f.read(4096):
                self.wfile.write(chunk)

    # ── OTA 检查 ────────────────────────────────────────────────

    def handle_ota(self):
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length).decode("utf-8")

        # 打印请求
        self.log(f"\n{'='*60}")
        self.log(f"[OTA Request] {time.strftime('%H:%M:%S')}")
        self.log(f"  Headers:")
        for key in ("Device-Id", "Client-Id", "User-Agent", "X-Timestamp", "Authorization"):
            val = self.headers.get(key, "")
            if key == "Authorization" and len(val) > 60:
                val = val[:60] + "..."
            self.log(f"    {key}: {val}")
        self.log(f"  Body:")
        try:
            req = json.loads(body) if body else {}
            self.log(f"    {json.dumps(req, ensure_ascii=False, indent=4).replace(chr(10), chr(10) + '    ')}")
        except json.JSONDecodeError:
            req = {}
            self.log(f"    (raw) {body}")

        current_version = req.get("application", {}).get("version", "0.0.0")

        # 构建响应：始终返回固件信息
        latest_ver, latest_file = get_latest_firmware()
        base_url = get_host_url(self)

        response = {
            "activation": {"code": "", "message": ""},
            "mqtt": {
                "endpoint": "mqtt.example.com",
                "client_id": f"GID_test@@@{self.headers.get('Device-Id', '')}@@@{self.headers.get('Client-Id', '')}",
                "username": "test_user",
                "password": "test_password",
                "publish_topic": "device-server",
            },
            "websocket": {
                "url": "wss://api.example.com/xiaozhi/v1/",
                "token": "test-token-123",
            },
            "server_time": {
                "timestamp": int(time.time() * 1000),
                "timezone": "Asia/Shanghai",
                "timezone_offset": -480,
            },
            "firmware": {
                "version": latest_ver or current_version,
                "url": f"{base_url}/firmware/{latest_file}" if latest_ver else "",
                "rotate_key": "",
            },
        }

        self.log(f"  -> current={current_version}, latest={latest_ver or 'N/A'}")
        self.send_json(200, response)

    # ── 工具方法 ────────────────────────────────────────────────

    def send_json(self, status, data):
        body = json.dumps(data, ensure_ascii=False, indent=2).encode("utf-8")
        self.log(f"  Response [{status}]:")
        self.log(f"    {json.dumps(data, ensure_ascii=False, indent=4).replace(chr(10), chr(10) + '    ')}")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log(self, msg):
        print(msg)

    def log_message(self, format, *args):
        pass


# ─── 启动 ───────────────────────────────────────────────────────

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PORT
    FIRMWARE_DIR.mkdir(exist_ok=True)

    fw = scan_firmware()
    print(f"OTA Test Server")
    print(f"  Listen:   {HOST}:{port}")
    print(f"  Firmware: {FIRMWARE_DIR}")
    if fw:
        latest = next(iter(fw))
        print(f"  Latest:   {latest} ({fw[latest]})")
    else:
        print(f"  Latest:   (none)")
    print()
    print(f"Endpoints:")
    print(f"  GET  /                - Status page")
    print(f"  POST /api/ota         - OTA check")
    print(f"  GET  /firmware/<file> - Firmware download")
    print()

    server = HTTPServer((HOST, port), OtaHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped")
        server.server_close()


if __name__ == "__main__":
    main()
