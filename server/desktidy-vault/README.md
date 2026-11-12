# DeskTidy Account Vault API

云账号服务：登录后同步 **账号库凭据** 与 **在线笔记**（DeskNote 转云 / 同步）。

## 生产环境（客户端默认）

| | |
|--|--|
| **Base** | `https://chromhome.kdns.fr/api/` |
| **边缘** | Cloudflare（响应可见 `CF-RAY` / `Server: cloudflare`） |
| **客户端** | `native/src/vault_http.cpp` 中 `kDefaultBase`；会话文件可覆盖 `api_base` |

健康检查 / 路由前缀以生产部署为准（路径形如 `auth/login`、`credentials`、`notes`，挂在 `/api/` 下）。

> **ByetHost 下载站**（`docs/*.html` FTP）与本 API **无关**。仓库里 `scripts/upload_byethost.py` 只传介绍页，不传 vault。

## API

Base（生产）: `https://chromhome.kdns.fr/api/`

| Method | Path | Auth | Body |
|--------|------|------|------|
| POST | `auth/register` | no | `{login,password}` |
| POST | `auth/login` | no | `{login,password}` |
| POST | `auth/logout` | Bearer | |
| GET | `credentials` | Bearer | |
| POST | `credentials` | Bearer | `{title,username,password,url,note}` |
| PUT | `credentials/{id}` | Bearer | same |
| DELETE | `credentials/{id}` | Bearer | |
| GET | `notes` | Bearer | list `{id,title,updated_at,…}`（无 body） |
| GET | `notes/{id}` | Bearer | 全文含 `body` |
| POST | `notes` | Bearer | `{title,body}` |
| PUT | `notes/{id}` | Bearer | `{title,body}` |
| DELETE | `notes/{id}` | Bearer | |

笔记正文服务端存 `body_enc`（与 credentials 同类加密约定）。

## Curl smoke（生产）

```bash
curl -s -X POST https://chromhome.kdns.fr/api/auth/register \
  -H "Content-Type: application/json" \
  -d "{\"login\":\"you@example.com\",\"password\":\"secret1\"}"
```

## 可选：本仓库 PHP 镜像（ByetHost，遗留）

`server/desktidy-vault/` 仍是一套 **PHP + MySQL** 实现，可自建或部署到 ByetHost，供对照 / 灾备；**不是**当前客户端默认后端。

1. 在主机面板建库，phpMyAdmin 导入 `schema.sql`。
2. 复制 `config.example.php` → `config.local.php`（勿提交）。
3. 配置 `db_*` 与 `vault_data_key_hex`。
4. 上传：`python scripts/upload_vault_api.py`（走 `.env.byethost` FTP，跳过 `config.local.php`）。
5. 旧健康检查示例：`https://bowen-yu0926.byethost10.com/desktidy-vault/`  
   客户端仍可处理 ByetHost `__test` 防机器人 Cookie（若把设置里的 API 指回该地址）。

无 rewrite 时可用：`index.php?r=auth/login`。

**Never commit `config.local.php`.**
