#!/usr/bin/env python3
"""NVS 分区解析工具（esp32-ota-test 项目用）

三种用法：
  1. 解析已有 NVS 分区镜像（不连设备）
       python tools/nvs_partition_parse.py parse <nvs.bin>

  2. 离线生成一份示例 NVS 分区并解析（用于验证工具链/演示）
       python tools/nvs_partition_parse.py make-test [out.bin]

  3. 从板子读取 NVS 分区并解析（只读，不烧录）
       python tools/nvs_partition_parse.py read -p /dev/ttyACM0
"""

import argparse
import os
import subprocess
import sys
import tempfile

IDF_PATH = os.environ.get("IDF_PATH", "/home/liu/.espressif/v6.0.2/esp-idf")
NVS_TOOL = os.path.join(
    IDF_PATH, "components", "nvs_flash", "nvs_partition_tool", "nvs_tool.py"
)
GEN_TOOL = os.path.join(
    IDF_PATH,
    "components",
    "nvs_flash",
    "nvs_partition_generator",
    "nvs_partition_gen.py",
)

# 与 partitions/16m.csv 保持一致：nvs 分区偏移 0x9000，大小 0x4000
DEFAULT_OFFSET = 0x9000
DEFAULT_SIZE = 0x4000

# 示例数据：覆盖本项目使用的命名空间（device / wifi / mqtt / websocket）
TEST_CSV = """key,type,encoding,value
device,namespace,,
device_key,data,string,0123456789abcdef0123456789abcdef
wifi,namespace,,
ota_url,data,string,http://192.168.3.185:3001/api/device_manage/ota
group_key,data,string,test-group-key
ssid,data,string,MyWiFi
password,data,string,secret123
mqtt,namespace,,
endpoint,data,string,mqtt.example.com
client_id,data,string,GID_test@@@device1@@@client1
username,data,string,test_user
password,data,string,test_password
websocket,namespace,,
url,data,string,wss://api.example.com/xiaozhi/v1/
token,data,string,test-token-123
"""


def python():
    """优先使用 ESP-IDF 的 venv python，否则用当前解释器。"""
    venv = "/home/liu/.espressif/tools/python/v6.0.2/venv/bin/python"
    return venv if os.path.exists(venv) else sys.executable


def run(cmd):
    print(f"$ {' '.join(cmd)}")
    subprocess.run(cmd, check=True)


def parse(bin_path, dump):
    run([python(), NVS_TOOL, "-f", "text", "-d", dump, bin_path])


def make_test(out_path, dump):
    with tempfile.TemporaryDirectory() as d:
        csv_path = os.path.join(d, "test.csv")
        with open(csv_path, "w", encoding="utf-8") as f:
            f.write(TEST_CSV)
        run(
            [
                python(),
                GEN_TOOL,
                "generate",
                csv_path,
                out_path,
                hex(DEFAULT_SIZE),
            ]
        )
    print(f"\n===== 解析示例分区 {out_path} =====")
    parse(out_path, dump)


def read(port, offset, size, out_path, dump):
    run(
        [
            python(),
            "-m",
            "esptool",
            "--chip",
            "esp32s3",
            "-p",
            port,
            "-b",
            "460800",
            "read-flash",
            hex(offset),
            hex(size),
            out_path,
        ]
    )
    print(f"\n===== 解析设备 NVS 分区 {out_path} =====")
    parse(out_path, dump)


def main():
    parser = argparse.ArgumentParser(description="NVS 分区解析工具")
    sub = parser.add_subparsers(dest="command", required=True)

    p_parse = sub.add_parser("parse", help="解析已有的 NVS 分区镜像")
    p_parse.add_argument("bin", help="NVS 分区 bin 文件路径")
    p_parse.add_argument(
        "-d", "--dump", default="minimal",
        choices=["all", "written", "minimal", "blobs", "namespaces", "storage_info"],
        help="输出格式（默认 minimal：namespace:key = value）",
    )

    p_make = sub.add_parser("make-test", help="离线生成示例 NVS 分区并解析")
    p_make.add_argument("out", nargs="?", default="build/nvs_partition_test.bin",
                        help="输出 bin 路径（默认 build/nvs_partition_test.bin）")
    p_make.add_argument("-d", "--dump", default="minimal", choices=["all", "written", "minimal", "blobs", "namespaces", "storage_info"])

    p_read = sub.add_parser("read", help="从板子读取 NVS 分区并解析（只读）")
    p_read.add_argument("-p", "--port", required=True, help="串口，如 /dev/ttyACM0")
    p_read.add_argument("--offset", type=lambda x: int(x, 0), default=DEFAULT_OFFSET,
                        help=f"NVS 偏移（默认 0x{DEFAULT_OFFSET:x}）")
    p_read.add_argument("--size", type=lambda x: int(x, 0), default=DEFAULT_SIZE,
                        help=f"NVS 大小（默认 0x{DEFAULT_SIZE:x}）")
    p_read.add_argument("-o", "--out", default="build/nvs_partition.bin",
                        help="输出 bin 路径（默认 build/nvs_partition.bin）")
    p_read.add_argument("-d", "--dump", default="minimal", choices=["all", "written", "minimal", "blobs", "namespaces", "storage_info"])

    args = parser.parse_args()

    if args.command == "parse":
        parse(args.bin, args.dump)
    elif args.command == "make-test":
        make_test(args.out, args.dump)
    elif args.command == "read":
        read(args.port, args.offset, args.size, args.out, args.dump)


if __name__ == "__main__":
    main()
