# ANCS 通知接收服务

这个独立 Go 服务接收 `lichuang-dev-ancs` 固件发送的 iOS ANCS 通知。它不会参与小智原有的 WebSocket、MQTT 或音频处理。

## 运行

需要 Go 1.22 或更高版本：

```powershell
cd tools/ancs-relay-server
go test ./...
go run .
```

默认监听 `:8080`，数据追加写入 `data/ancs-events.ndjson`。服务启动时会读取已有文件并恢复 `event_id` 去重集合。

环境变量：

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `ANCS_LISTEN_ADDR` | `:8080` | HTTP/HTTPS 监听地址 |
| `ANCS_DATA_FILE` | `data/ancs-events.ndjson` | 通知持久化文件 |
| `ANCS_TLS_CERT_FILE` | 空 | TLS 证书链文件 |
| `ANCS_TLS_KEY_FILE` | 空 | TLS 私钥文件 |

固件要求 HTTPS。生产环境应在服务前配置 Caddy、Nginx 等 HTTPS 反向代理，或者同时设置 TLS 证书和私钥变量。证书必须能被 ESP-IDF HTTP 客户端信任。

将反向代理地址写入固件的 `main/bluetooth/ancs_relay_config.h`：

```cpp
inline constexpr char kAncsRelayEndpoint[] =
    "https://your-domain.example/api/v1/ancs/notifications";
```

## API

### 接收通知

```text
POST /api/v1/ancs/notifications
Content-Type: application/json
Device-Id: <Wi-Fi MAC>
Client-Id: <设备 UUID>
User-Agent: xiaozhi-esp32-ancs-relay/1
```

首次成功持久化返回 `202`：

```json
{"status":"accepted","event_id":"<event_id>"}
```

相同 `event_id` 再次提交返回 `200`，不会重复写入：

```json
{"status":"duplicate","event_id":"<event_id>"}
```

无效 JSON、请求头不匹配或字段越界返回 `4xx`，固件会永久丢弃该事件。存储暂时不可用返回 `503`，固件会按退避策略重试。

### 健康检查

```text
GET /healthz
```

## 隐私与后续处理

NDJSON 文件包含应用标识、通知标题和正文，文件权限默认为仅当前服务账户可读写。仍应限制服务器登录权限、备份范围和日志访问权限。

后续大模型或智能体处理可以按行消费 `ANCS_DATA_FILE`，并继续使用 `event_id` 作为幂等键。不要单独使用 ANCS UID 去重，因为 UID 只在一次蓝牙连接会话内有效。
