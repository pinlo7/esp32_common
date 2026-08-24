# firmware/

把测试固件放这个目录，文件名格式 `<版本号>.bin`。

示例:
```
firmware/
  1.0.0.bin
  2.0.0.bin
```

服务器启动时自动扫描，版本号最大的作为最新版本，供 OTA 检查使用。

固件下载地址: `http://<server>:<port>/firmware/<filename>`
