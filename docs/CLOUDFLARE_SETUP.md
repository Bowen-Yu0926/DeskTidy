# Cloudflare Pages 自动部署首页

首页文件在 `docs/`（`index.html`、`desktidy.html`、`desknote.html`、`images/`）。把 Cloudflare Pages 连接到 GitHub 仓库后，push 到 `main` 就会自动部署首页，不再需要手动运行 `scripts/upload_cloudflare_pages.py`。

## 一次性配置（在 Cloudflare 控制台操作）

1. 登录 [Cloudflare Dashboard](https://dash.cloudflare.com/) → **Workers & Pages** → **Create application** → **Pages** → **Connect to Git**。
2. 选择 GitHub，授权 Cloudflare 访问仓库 `Bowen-Yu0926/DeskTidy`。
3. 构建配置：
   - **Framework preset**：`None`
   - **Build command**：留空
   - **Build output directory**：`docs`
   - **Root directory**：`/`（默认）
   - **Branch**：`main`
4. 环境变量：无需配置（纯静态页面）。
5. 保存并部署。Cloudflare 会分配一个 `*.pages.dev` 域名，先确认页面能打开。

## 绑定自定义域名

1. 在该 Pages 项目 → **Custom domains** → **Set up a custom domain**。
2. 填入 `desktidy.bowenyu.kdns.fr`。
3. 如果域名已经在 Cloudflare DNS 里（当前就是），Cloudflare 会自动添加/更新记录，通常需要几分钟生效。
4. 等 HTTPS 证书签发完成（Cloudflare 自动处理），访问 `https://desktidy.bowenyu.kdns.fr/` 确认是最新版首页。

## 减少无关部署（可选）

默认每次 push 到 `main` 都会重新部署（纯静态很快，无副作用）。如果你只想让 `docs/` 的变化触发部署：

- Pages 项目 → **Settings** → **Builds & deployments** → **Build configuration** → **Ignored build paths**
- 添加：`native/**`、`scripts/**`、`installer/**`、`testcases/**`、`assets/**`（按需）

> 注意：Ignored build paths 是"路径变化时跳过构建"，语法为 gitignore 风格；配置后请做一次测试 push 确认行为符合预期。

## 以后如何更新首页

```bash
# 1. 修改 docs/ 下的 HTML / 图片
# 2. 提交并推送
git add docs/
git commit -m "docs: update homepage"
git push origin main
```

等待几秒到几十秒，Cloudflare 自动部署完成，刷新 `https://desktidy.bowenyu.kdns.fr/`（必要时强制刷新或等 CDN 缓存过期）。

## 与自动发版工作流的关系

- **首页更新**：Cloudflare Pages Git 集成自动完成（改 `docs/` 并 push）。
- **安装包发版**：GitHub Actions `Release` 工作流完成（手动 Run workflow 或 push `v*` tag）。
- 二者互相独立。如果想要"发版时顺便更新首页下载链接"，可以改 `docs/desktidy.html` 里的下载 URL，然后 push——首页会自动更新。

## 手动上传（备用）

Cloudflare Git 集成不可用时，仍可用旧脚本应急（需要本地 `.env.cloudflare`）：

```bash
python scripts/upload_cloudflare_pages.py
```
