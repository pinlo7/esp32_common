# Agent 指令

## ESP 文档查询

处理 ESP 芯片或 ESP-IDF、ESP-ADF 等 SDK 相关问题时，请始终主动调用乐鑫文档 MCP 服务器，无需每次单独提醒。

## 编译环境（本机）

- IDF：`C:\esp\v5.5.4\esp-idf`
- 工具链 / Python venv：`C:\Espressif\tools`
- 激活方式（PowerShell）：先执行
  `C:\Espressif\tools\idf_v5.5.4.ps1`（官方安装器生成，会设置 IDF_PATH /
  IDF_TOOLS_PATH / IDF_PYTHON_ENV_PATH 并激活 venv），然后 `idf.py build`
- 默认目标芯片：esp32s3（参考 `sdkconfig.defaults.esp32s3`）
