#!/usr/bin/env python3
"""
OTA 测试服务器（模拟生产后端 /api/device_manage/ota）

与生产后端行为对齐：
- 首次激活：必须携带 X-Provision-Key（组密钥）+ Authorization: Bearer 签名，
  服务端用组密钥验签，**不校验时间戳**（设备时钟尚未同步，首次激活会拿到 server_time 校时）
- 已注册设备：用 device_secret 验签，并校验时间戳 ±TIMESTAMP_TOLERANCE 秒
- 响应结构与生产一致：device_secret / mqtt / websocket / server_time / firmware
- 签名算法：SHA256(密钥 + 时间戳 + device_id + SHA256(body))

用法:
    python server.py [port]

环境变量:
    PROVISION_KEYS         逗号分隔的组密钥（默认 test-provision-key）
    TIMESTAMP_TOLERANCE    时间戳容差秒（默认 300）
    MQTT_ENDPOINT          返回给设备的 MQTT 地址（默认 mqtt://localhost:1883）
    WEBSOCKET_URL          返回给设备的 WebSocket 地址（默认 wss://api.example.com/ws/）
    DEVICES_FILE           设备记录持久化文件（默认不持久化，仅内存）
    RATE_LIMIT             每 IP 10 分钟内允许的 OTA 请求数（默认 600）

API:
    POST /api/device_manage/ota   - OTA 检查 / 首次激活（生产路径）
    POST /api/ota                 - 兼容别名
    GET  /firmware/<file>         - 固件下载
    GET  /                        - 状态页（固件 + 已注册设备）
"""

import hashlib
import hmac
import json
import os
import re
import secrets
import sys
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path

# ─── 配置 ───────────────────────────────────────────────────────

HOST = "0.0.0.0"
DEFAULT_PORT = 8080
FIRMWARE_DIR = Path(__file__).parent / "firmware"

PROVISION_KEYS = [k.strip() for k in os.environ.get("PROVISION_KEYS", "test-provision-key").split(",") if k.strip()]
TIMESTAMP_TOLERANCE = int(os.environ.get("TIMESTAMP_TOLERANCE", "300"))
MQTT_ENDPOINT = os.environ.get("MQTT_ENDPOINT", "mqtt://localhost:1883")
WEBSOCKET_URL = os.environ.get("WEBSOCKET_URL", "wss://api.example.com/ws/")
DEVICES_FILE = os.environ.get("DEVICES_FILE", "") or None
RATE_LIMIT = int(os.environ.get("RATE_LIMIT", "600"))
RATE_WINDOW = 10 * 60

DEVICE_ID_RE = re.compile(r"^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$")
VERSION_RE = re.compile(r"(\d+\.\d+\.\d+)")


# ─── 固件扫描 ───────────────────────────────────────────────────


def scan_firmware():
    """扫描 firmware/ 目录，返回 {version: filename}，版本降序"""
    if not FIRMWARE_DIR.is_dir():
        return {}
    fw = {}
    for f in sorted(FIRMWARE_DIR.iterdir()):
        if f.suffix == ".bin":
            m = VERSION_RE.search(f.name)
            if m:
                fw[m.group(1)] = f.name
    return dict(sorted(fw.items(), key=lambda x: list(map(int, x[0].split("."))), reverse=True))


def get_latest_firmware():
    fw = scan_firmware()
    if not fw:
        return None, None
    version = next(iter(fw))
    return version, fw[version]


def is_newer_version(latest, current):
    if not latest or not current:
        return False
    l = list(map(int, latest.split(".")))
    c = list(map(int, current.split(".")))
    return l > c


# ─── 签名 / 认证 ────────────────────────────────────────────────


def verify_signature(secret, timestamp, device_id, body_json, signature, check_timestamp=True):
    """与生产后端 verifySignature 一致：SHA256(secret + timestamp + device_id + SHA256(body))"""
    if check_timestamp:
        if not re.fullmatch(r"\d{10}", timestamp or ""):
            return False
        try:
            if abs(int(time.time()) - int(timestamp)) > TIMESTAMP_TOLERANCE:
                return False
        except ValueError:
            return False
    body_hash = hashlib.sha256(body_json.encode("utf-8")).hexdigest()
    expected = hashlib.sha256((secret + timestamp + device_id + body_hash).encode("utf-8")).hexdigest()
    return hmac.compare_digest(expected, signature or "")


# ─── 设备存储 ───────────────────────────────────────────────────


class DeviceStore:
    def __init__(self, path=None):
        self.path = path
        self.devices = {}
        if path and Path(path).is_file():
            try:
                self.devices = json.loads(Path(path).read_text("utf-8"))
            except (json.JSONDecodeError, OSError):
                self.devices = {}

    def get(self, device_id):
        return self.devices.get(device_id)

    def put(self, device_id, data):
        self.devices[device_id] = data
        if self.path:
            tmp = self.path + ".tmp"
            Path(tmp).write_text(json.dumps(self.devices, ensure_ascii=False, indent=2), "utf-8")
            os.replace(tmp, self.path)


store = DeviceStore(DEVICES_FILE)


# ─── 限流 ──────────────────────────────────────────────────────

_rate_hits = {}


def rate_allowed(ip):
    now = time.time()
    q = _rate_hits.setdefault(ip, [])
    while q and q[0] < now - RATE_WINDOW:
        q.pop(0)
    if len(q) >= RATE_LIMIT:
        return False
    q.append(now)
    return True


# ─── 响应构建（与生产一致）─────────────────────────────────────


def build_mqtt(device_id, device_secret):
    password = hmac.new(device_secret.encode("utf-8"), b"mqtt:v1", hashlib.sha256).hexdigest()
    return {
        "endpoint": MQTT_ENDPOINT,
        "client_id": f"GID_{device_id}",
        "username": f"device_{device_id}",
        "password": password,
        "publish_topic": f"device/{device_id}/status",
        "publish_topics": {
            "status": f"device/{device_id}/status",
            "location": f"device/{device_id}/location",
            "event": f"device/{device_id}/event",
        },
        "subscribe_topics": {
            "command": f"device/{device_id}/command",
            "config": f"device/{device_id}/config",
        },
    }


def build_websocket():
    return {"url": WEBSOCKET_URL, "token": "xxx"}


def build_server_time():
    return {
        "timestamp": int(time.time() * 1000),  # UTC epoch 毫秒，设备直接写入系统时钟
        "timezone": "Asia/Shanghai",
        "timezone_offset": 480,  # 东区为正，UTC+8 = 480，仅供显示
    }


def build_firmware(current_version, first_activation):
    latest_ver, latest_file = get_latest_firmware()
    has_new = latest_ver and (first_activation or is_newer_version(latest_ver, current_version))
    checksum = None
    file_size = None
    if latest_file:
        fpath = FIRMWARE_DIR / latest_file
        file_size = fpath.stat().st_size
        checksum = hashlib.sha256(fpath.read_bytes()).hexdigest()
    return {
        "version": latest_ver or current_version,
        "url": f"/firmware/{latest_file}" if has_new else "",
        "rotate_key": "",
        "notes": None,  # 测试服务器无 notes 元数据；生产固件有版本说明时返回该字段
        "checksum": checksum,
        "file_size": file_size,
    }


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
        if self.path in ("/api/device_manage/ota", "/api/device_manage/ota/", "/api/ota", "/api/ota/"):
            self.handle_ota()
        else:
            self.send_error(404, "Not Found")

    # ── 状态页 ──────────────────────────────────────────────────

    def handle_status(self):
        fw = scan_firmware()
        lines = [
            "<!DOCTYPE html><html><head><meta charset='utf-8'><title>OTA Test Server</title>",
            "<style>body{font-family:monospace;max-width:760px;margin:40px auto;padding:0 20px}",
            "table{border-collapse:collapse;width:100%}td,th{border:1px solid #ddd;padding:6px 10px;text-align:left}",
            "</style></head><body>",
            "<h2>OTA Test Server</h2>",
            f"<p>Provision keys: {len(PROVISION_KEYS)} configured | Devices: {len(store.devices)} | "
            f"MQTT endpoint: {MQTT_ENDPOINT}</p>",
            "<h3>Firmware</h3>",
        ]
        if fw:
            lines.append("<table><tr><th>Version</th><th>File</th><th>Size</th></tr>")
            for ver, fname in fw.items():
                fpath = FIRMWARE_DIR / fname
                lines.append(f"<tr><td>{ver}</td><td>{fname}</td><td>{fpath.stat().st_size:,} bytes</td></tr>")
            lines.append("</table>")
        else:
            lines.append(f"<p>No firmware files in <code>{FIRMWARE_DIR}</code></p>")

        lines.append("<h3>Registered devices</h3>")
        if store.devices:
            lines.append("<table><tr><th>Device-Id</th><th>Board</th><th>Firmware</th><th>Secret</th><th>Last seen</th></tr>")
            for dev_id, d in store.devices.items():
                secret = (d.get("device_secret", "") or "")[:12] + "..." if d.get("device_secret") else "-"
                last = time.strftime("%H:%M:%S", time.localtime(d.get("last_seen", 0)))
                lines.append(f"<tr><td>{dev_id}</td><td>{d.get('board_name') or d.get('board_type') or '-'}</td>"
                             f"<td>{d.get('firmware_version') or '-'}</td><td>{secret}</td><td>{last}</td></tr>")
            lines.append("</table>")
        else:
            lines.append("<p>No devices registered yet.</p>")

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

    # ── OTA 检查 / 首次激活 ─────────────────────────────────────

    def handle_ota(self):
        if not rate_allowed(self.client_address[0]):
            return self.send_json(429, {"error": "too many requests, please retry later"})

        content_length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(content_length).decode("utf-8", errors="replace")
        try:
            req = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            return self.send_json(400, {"error": "invalid json body"})

        device_id = self.headers.get("Device-Id", "")
        timestamp = self.headers.get("X-Timestamp", "")
        auth = self.headers.get("Authorization", "")
        provision_key = self.headers.get("X-Provision-Key", "")

        # 打印请求
        self.log(f"\n{'='*60}")
        self.log(f"[OTA Request] {time.strftime('%H:%M:%S')} from {self.client_address[0]}")
        self.log(f"  Device-Id: {device_id} | X-Timestamp: {timestamp} | Provision-Key: {'***' if provision_key else '-'}")
        self.log(f"  Authorization: {'Bearer ' + auth[7:20] + '...' if auth.startswith('Bearer ') and len(auth) > 27 else auth or '-'}")
        self.log(f"  Body: {json.dumps(req, ensure_ascii=False)}")

        if not device_id or not timestamp:
            return self.send_json(400, {"error": "missing required headers"})
        if not DEVICE_ID_RE.fullmatch(device_id):
            return self.send_json(400, {"error": "invalid device id format (expected MAC address)"})

        body_json = json.dumps(req, separators=(",", ":"), ensure_ascii=False)
        current_version = req.get("application", {}).get("version", "0.0.0")
        board = req.get("board", {}) or {}
        device = store.get(device_id)

        # ── 首次激活：组密钥验签，跳过时间戳校验 ──
        if device is None:
            if not provision_key or not auth.startswith("Bearer "):
                return self.send_json(401, {"error": "provision key and signature required for first activation"})
            if provision_key not in PROVISION_KEYS:
                return self.send_json(401, {"error": "invalid or disabled provision key"})
            signature = auth[7:]
            if not verify_signature(provision_key, timestamp, device_id, body_json, signature, check_timestamp=False):
                return self.send_json(401, {"error": "provision signature invalid"})

            device_secret = secrets.token_hex(32)
            store.put(device_id, {
                "device_secret": device_secret,
                "board_type": board.get("type"),
                "board_name": board.get("name"),
                "firmware_version": current_version,
                "ssid": board.get("ssid"),
                "rssi": board.get("rssi"),
                "ip": board.get("ip"),
                "created": time.time(),
                "last_seen": time.time(),
            })
            self.log(f"  -> FIRST ACTIVATION OK, device {device_id} registered")
            return self.send_json(200, {
                "device_secret": device_secret,
                "mqtt": build_mqtt(device_id, device_secret),
                "websocket": build_websocket(),
                "server_time": build_server_time(),
                "firmware": build_firmware(current_version, first_activation=True),
            })

        # ── 已注册设备：device_secret 验签 + 时间戳校验 ──
        if not auth.startswith("Bearer "):
            return self.send_json(401, {"error": "missing authorization"})
        signature = auth[7:]
        if not verify_signature(device["device_secret"], timestamp, device_id, body_json, signature, check_timestamp=True):
            return self.send_json(401, {"error": "signature invalid"})

        device.update({
            "firmware_version": current_version,
            "ssid": board.get("ssid"),
            "rssi": board.get("rssi"),
            "ip": board.get("ip"),
            "last_seen": time.time(),
        })
        store.put(device_id, device)
        self.log(f"  -> OTA CHECK OK, current={current_version}")
        return self.send_json(200, {
            "mqtt": build_mqtt(device_id, device["device_secret"]),
            "websocket": build_websocket(),
            "server_time": build_server_time(),
            "firmware": build_firmware(current_version, first_activation=False),
        })

    # ── 工具方法 ────────────────────────────────────────────────

    def send_json(self, status, data):
        body = json.dumps(data, ensure_ascii=False, indent=2).encode("utf-8")
        self.log(f"  Response [{status}]: {json.dumps(data, ensure_ascii=False)}")
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
    print("OTA Test Server (aligned with production backend)")
    print(f"  Listen:          {HOST}:{port}")
    print(f"  Provision keys:  {len(PROVISION_KEYS)} configured")
    print(f"  Timestamp tol:   ±{TIMESTAMP_TOLERANCE}s (registered devices only)")
    print(f"  MQTT endpoint:   {MQTT_ENDPOINT}")
    print(f"  Firmware:        {FIRMWARE_DIR} ({len(fw)} file(s))")
    print()
    print("Endpoints:")
    print("  GET  /                            - Status page")
    print("  POST /api/device_manage/ota       - OTA check / first activation")
    print("  POST /api/ota                     - Alias")
    print("  GET  /firmware/<file>             - Firmware download")
    print()

    server = HTTPServer((HOST, port), OtaHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped")
        server.server_close()


if __name__ == "__main__":
    main()
