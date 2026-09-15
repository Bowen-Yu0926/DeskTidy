# DeskTidy 账号管理（云端保险库）

**日期：** 2026-09-15  
**状态：** 已实现（待部署 MySQL `config.local.php`）  
**产品决策：** 云端 MySQL（方案 B）+ 无验证码注册 + 账号/密码登录（登录态方案 A）+ 字段方案 A + DeskNote 联动方案 A  
**技术选型：** 方案 1 — PHP REST API（byethost）+ MySQL + DeskTidy 独立浮层窗口  

---

## 1. 背景与目标

用户需要在 DeskTidy 中维护私人账号/密码清单：扩展功能可开关；开启后绑定手机号或邮箱并设置登录密码；数据按注册账号存云端，换设备登录后可同步；清单便于查看，右侧一键复制账号、密码、网址；可维护（增删改）；可与 DeskNote 弱联动（某条写入/打开备注笔记）。

**首期目标：**

- 扩展功能配置：启用开关 + API 根地址
- 注册 / 登录（手机号 **或** 邮箱 + 登录密码；**无**短信/邮箱验证码）
- 分页栏入口打开独立账号清单浮层
- 条目字段：名称、账号、密码、网址、可选备注；右侧复制账号 / 密码 / 网址
- CRUD 走云端 API；按 `user_id` 隔离
- DeskNote：条目可「写备注 / 打开笔记」（本地 md，不反向写库）

**非目标（首期不做）：**

- 短信/邮箱验证码、找回密码邮件流、OAuth
- 零知识客户端加密、保险库主密码（登录态方案 C）
- 分组/标签、自定义字段、浏览器自动填充、导入 LastPass/Chrome
- DeskNote ↔ 云端字段双向同步
- 多设备实时推送（拉取 + 写后刷新即可）

---

## 2. 托管环境（部署约束）

用户现有 byethost 虚拟主机（PHP + MySQL）：

| 项 | 值（公开主机信息；密钥不进仓库） |
|----|----------------------------------|
| 站点 | `https://bowen-yu0926.byethost10.com/` |
| MySQL 主机 | `sql110.byethost10.com` |
| MySQL / FTP 账号名 | 面板账号（密码仅写在服务器 `config.local.php`，**禁止提交 git**） |
| FTP | `ftpupload.net`（可用现有 `scripts/upload_byethost.py` 部署） |

API 建议路径：`https://bowen-yu0926.byethost10.com/desktidy-vault/`  
客户端扩展里默认填上述根地址，允许用户改。

仓库内仅提供：

- `server/desktidy-vault/`：PHP 源码 + `config.example.php` + `schema.sql`
- 部署说明：复制为 `config.local.php` 填写 DB 密码后 FTP 上传

---

## 3. 用户可见行为

### 3.1 扩展功能

- 新增卡片「账号管理」：
  - 勾选「在分页栏显示「账号」」（`account_vault.enabled`，默认关）
  - 「API 根地址」文本框（`account_vault.api_base`）
- 未登录时，打开清单先弹出注册/登录对话框；登录成功后进列表。
- 扩展页可显示当前登录身份（脱敏）与「退出登录」。

### 3.2 分页栏

- 与「待办 / 纪要」同类：启用后显示「账号」按钮；单击显示/置顶账号浮层。

### 3.3 账号浮层（清单）

- 顶部：搜索（按名称/账号/网址过滤）、「新建」、刷新、当前用户、退出
- 列表每行：名称；账号 / 密码（默认掩码，可点眼睛显示）/ 网址摘要；右侧三个复制图标 + 编辑 + 删除 + 「DeskNote 备注」
- 复制成功 toast（「已复制密码」等）；密码复制后可选短时清空剪贴板（首期：复制成功提示即可，清空剪贴板可作增强项）
- 新建/编辑对话框：名称（必填）、账号、密码、网址、备注（可选，存云端字段 `note`；与 DeskNote 笔记文件是两回事）

### 3.4 注册 / 登录

- 注册：手机号 **或** 邮箱（二选一作登录名）+ 密码（≥6）+ 确认密码
- 登录：登录名 + 密码 → 返回 `token`；客户端持久化到 `%USERPROFILE%\.desktidy\account_vault_session.json`
- 无验证码；服务端校验格式（简单邮箱 / 中国手机号正则）与唯一性

### 3.5 DeskNote 联动

- 「DeskNote 备注」：在 DeskNote 默认笔记目录下使用固定命名  
  `账号备注_<条目id>_<安全名称>.md`  
  若不存在则用模板创建（含名称、账号、网址、云端备注摘要与「勿把密码明文长期放笔记」提示），再调用现有 DeskNote 打开逻辑。
- 不把笔记内容回写 MySQL；云端 `note` 字段仅在账号库对话框维护。

---

## 4. 架构

```
DeskTidy (PyQt6)
  extensions_widget     → account_vault.enabled / api_base
  page_indicator        → 「账号」按钮
  account_vault_widget  → 浮层清单 / 复制 / CRUD UI
  account_vault_api     → HTTP JSON 客户端（token）
  account_vault_store   → 本地 session；可选短缓存列表
  desknote_launch       → 打开/创建备注 md

byethost PHP
  desktidy-vault/
    index.php           → 路由
    auth.php            → register / login
    credentials.php     → list / create / update / delete
    lib/db.php, crypto.php, response.php
    config.local.php    → DB DSN（不入库）
```

**数据流：** 启用 → 登录拿 token → `GET /credentials` → 展示 → 写操作走 API → 可选打开 DeskNote md。

---

## 5. 数据模型（MySQL）

### 5.1 `vault_users`

| 列 | 类型 | 说明 |
|----|------|------|
| id | BIGINT PK AI | |
| login | VARCHAR(191) UNIQUE | 规范化后的手机号或邮箱（小写） |
| login_type | ENUM('phone','email') | |
| password_hash | VARCHAR(255) | `password_hash` (PASSWORD_DEFAULT) |
| created_at / updated_at | DATETIME | |

### 5.2 `vault_tokens`

| 列 | 类型 | 说明 |
|----|------|------|
| id | BIGINT PK AI | |
| user_id | BIGINT FK | |
| token_hash | CHAR(64) UNIQUE | SHA-256(token) |
| expires_at | DATETIME | 默认 90 天 |
| created_at | DATETIME | |

明文 token 只在登录响应返回一次；库中只存哈希。

### 5.3 `vault_credentials`

| 列 | 类型 | 说明 |
|----|------|------|
| id | BIGINT PK AI | |
| user_id | BIGINT FK | 隔离 |
| title | VARCHAR(200) | 名称 |
| username_enc | TEXT | 加密后的账号 |
| password_enc | TEXT | 加密后的密码 |
| url_enc | TEXT | 加密后的网址 |
| note_enc | TEXT | 加密后的可选备注 |
| created_at / updated_at | DATETIME | |

服务端用 `config.local.php` 中的 `VAULT_DATA_KEY`（32 字节）做 AES-256-GCM；密钥与 DB 密码同级保密。响应 JSON 返回解密后的明文供客户端展示（HTTPS 传输）。

---

## 6. API（REST 风格，JSON）

基址：`{api_base}`，例如 `https://bowen-yu0926.byethost10.com/desktidy-vault/`

统一响应：`{ "ok": true, "data": ... }` / `{ "ok": false, "error": "code", "message": "..." }`  
认证：`Authorization: Bearer <token>`（除 register/login）

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `auth/register` | `{ login, password }` → `{ user_id, login, token }` |
| POST | `auth/login` | `{ login, password }` → `{ user_id, login, token }` |
| POST | `auth/logout` | 作废当前 token |
| GET | `credentials` | 当前用户条目列表 |
| POST | `credentials` | 新建 `{ title, username, password, url, note }` |
| PUT | `credentials/{id}` | 更新 |
| DELETE | `credentials/{id}` | 删除 |

错误码示例：`invalid_login`、`login_taken`、`bad_credentials`、`unauthorized`、`not_found`、`validation`。

CORS：允许 DeskTidy 桌面客户端；若仅原生 HTTPS 客户端可无浏览器 CORS，仍建议 API 返回宽松 CORS 便于排查。

---

## 7. 客户端设置与文件

`settings` 新增：

```json
"account_vault": {
  "enabled": false,
  "api_base": "https://bowen-yu0926.byethost10.com/desktidy-vault/"
}
```

本地 session（非 settings，避免导出泄露）：

`%USERPROFILE%\.desktidy\account_vault_session.json`  
`{ "api_base", "login", "token", "user_id" }`

---

## 8. 错误处理与安全

- 网络失败：toast，保留本地已展示列表（不静默丢数据）
- 401：清 session，弹登录框
- 登录密码仅哈希；条目字段 AES 存库；传输依赖 HTTPS（byethost 站点需可 https）
- `config.local.php`、真实 DB 密码、`VAULT_DATA_KEY` **永不提交**
- 首期不做爆破限流硬门槛；PHP 侧可对 login 做简单同 IP 冷却（可选，有则写、无则文档标明后续）

---

## 9. 测试

- PHP：`schema.sql` 可导入；register → login → CRUD 用 curl/自测脚本
- 客户端 selftest：settings 开关、session 读写、API 客户端对 mock 的解析、DeskNote 备注路径命名安全（非法文件名字符剥离）
- 手工：扩展启用 → 注册 → 建一条 → 复制三项 → 换「用户」登录看不到对方数据 → DeskNote 打开备注

---

## 10. 实现分期（单计划内可顺序交付）

1. 服务端：schema + auth + credentials API + 示例配置  
2. 客户端 API/session + 扩展开关  
3. 账号浮层 UI + 分页栏入口  
4. DeskNote 备注联动  
5. 部署到 byethost + selftest  

---

## 11. 已确认决策摘要

| 决策点 | 选择 |
|--------|------|
| 存储 | 云端 MySQL |
| 宿主 | PHP 虚拟主机（byethost） |
| 验证码 | 无，直接注册 |
| 登录 | 手机号或邮箱 + 密码 |
| 字段 | 名称、账号、密码、网址、可选备注 |
| DeskNote | 独立窗口 + 一键写/开备注 md |
| 入口 | 扩展开关 + 分页栏「账号」 |
