# Agent 指令

## ESP 文档查询

处理 ESP 芯片或 ESP-IDF、ESP-ADF 等 SDK 相关问题时，请始终主动调用乐鑫文档 MCP 服务器，无需每次单独提醒。

## 编译环境（本机）

- 默认目标芯片：esp32s3n16r8（参考 `sdkconfig.defaults.esp32s3`）
- 编译工具：`idf.py`
- 激活方式（Linux shell）：先执行
  `source /home/liu/.espressif/tools/activate_idf_v6.0.2.sh`，然后 `idf.py build`
