# ANCS 通知接收服务

这个独立 Go 服务通过 HTTP 接收 `lichuang-dev-ancs` 固件发送的 iOS ANCS 通知，并保存到 SQLite。它不参与小智原有的 WebSocket、MQTT 或音频处理。

> 当前版本只提供 HTTP，并且通知正文没有应用层加密或鉴权。请不要直接暴露在不可信网络中。

## Docker 部署

```sh
cd tools/ancs-relay-server
mkdir -p data
sudo chown 10001:10001 data
docker compose up -d --build
```

默认将宿主机 `./data` 映射到容器 `/data`，数据库文件位于：

```text
./data/ancs.db
```

默认宿主机端口是 `18081`。可通过 `.env` 选择其他未占用端口和绝对数据目录：

```dotenv
ANCS_HOST_PORT=18081
ANCS_DATA_DIR=/opt/xiaozhi-ancs-relay/data
```

然后检查运行状态：

```sh
docker compose ps
curl http://127.0.0.1:18081/healthz
```

没有 Docker Compose 的服务器可以直接运行同一个镜像：

```sh
docker build -t xiaozhi/ancs-relay-server:local .
mkdir -p /opt/xiaozhi-ancs-relay/data
chown 10001:10001 /opt/xiaozhi-ancs-relay/data
docker run -d \
  --name xiaozhi-ancs-relay-server \
  --restart unless-stopped \
  -p 18081:8080 \
  -e ANCS_LISTEN_ADDR=:8080 \
  -e ANCS_DB_FILE=/data/ancs.db \
  -v /opt/xiaozhi-ancs-relay/data:/data \
  xiaozhi/ancs-relay-server:local
```

## 固件地址

把实际服务器地址写入固件的 `main/bluetooth/ancs_relay_config.h`：

```cpp
inline constexpr char kAncsRelayEndpoint[] =
    "http://oracle.goclang.com:18081/api/v1/ancs/notifications";
```

修改地址后需要重新编译和刷写 `lichuang-dev-ancs` 固件。

## API

### 接收通知

```text
POST /api/v1/ancs/notifications
Content-Type: application/json
Device-Id: <Wi-Fi MAC>
Client-Id: <设备 UUID>
User-Agent: xiaozhi-esp32-ancs-relay/1
```

首次成功写入 SQLite 返回 `202`：

```json
{"status":"accepted","event_id":"<event_id>"}
```

SQLite 以 `event_id` 为主键。相同事件再次提交返回 `200`，不会重复写入：

```json
{"status":"duplicate","event_id":"<event_id>"}
```

无效 JSON、请求头不匹配或字段越界返回 `4xx`，固件会永久丢弃该事件。SQLite 暂时不可用返回 `503`，固件会按退避策略重试。

### 健康检查

```text
GET /healthz
```

## SQLite 查询

表名为 `notifications`。常用字段已拆列并建立设备、应用、会话 UID 和事件时间索引，同时在 `raw_json` 中保留原始请求。

```sh
sqlite3 ./data/ancs.db
```

最近 20 条通知：

```sql
SELECT received_at, app_identifier, event, title, message
FROM notifications
ORDER BY received_at DESC
LIMIT 20;
```

按设备和应用查询：

```sql
SELECT received_at, event, title, message
FROM notifications
WHERE device_id = '设备 UUID'
  AND app_identifier = 'com.example.app'
ORDER BY received_at DESC;
```

关键词查询：

```sql
SELECT received_at, app_identifier, title, message
FROM notifications
WHERE title LIKE '%关键词%' OR message LIKE '%关键词%'
ORDER BY received_at DESC;
```

## 备份与隐私

通知应用标识、标题和正文都保存在宿主机数据库中。限制 `ANCS_DATA_DIR` 的文件权限、登录权限和备份范围。

SQLite 使用 WAL 模式。在线备份应使用 SQLite 的备份命令，不要只复制主数据库文件：

```sh
sqlite3 ./data/ancs.db ".backup './data/ancs-backup.db'"
```
