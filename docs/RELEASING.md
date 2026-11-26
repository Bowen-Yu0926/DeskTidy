# 自动发版说明

当推送形如 `v4.0.52` 的 tag 时，`.github/workflows/release.yml` 会在 Windows runner 上：

1. 校验 tag 与仓库根目录 `VERSION` 一致；
2. 用 CMake/VS2022 + Inno Setup 构建安装包；
3. 计算 `.exe.sha256`（产品内更新器会用到）；
4. 运行 `test_update_verify`（Ed25519 验签模块的自检，失败则中止发版）；
5. 若已配置签名密钥，生成 `.exe.manifest` 与 `.exe.sig` 并自校验；
6. 将安装包、校验文件与签名文件发布到 `Bowen-Yu0926/DeskTidy` 的 GitHub Release；
7. 上传构建产物为 Actions artifact（保留 14 天）。

## 发版步骤

```bash
# 1. 更新版本号（同步 VERSION / installer/*.iss / native/src/version.rc）
python scripts/bump_version.py --bump

# 2. 确认版本和变更日志无误，提交改动
git add -A && git commit -m "chore: release vX.Y.Z"
git push origin main

# 3. 打 tag 并推送，自动触发发版
git tag vX.Y.Z
git push origin vX.Y.Z
```

也可在 GitHub Actions 页面手动触发 `Release` 工作流并填写 release notes。

## 需要配置的 Secrets

在仓库 Settings → Secrets and variables → Actions 中添加：

- `GH_TOKEN`：具有 `repo` 权限的 Personal Access Token（用于向 `DeskTidy` 源仓库创建 Release 和上传文件）。
- `UPDATE_SIGNING_KEY_B64`（可选）：更新签名私钥的 base64（原始 32 字节 Ed25519 私钥）。配置后 CI 会为安装包生成并附带 `.manifest`/`.sig`。

## 更新签名（Ed25519）

**为什么需要**：`.exe.sha256` 与安装包来自同一个 release，账号被盗即可同时替换两者，校验形同虚设。Ed25519 签名解决了这点——**只有持有离线私钥的人**才能签出客户端会接受的清单，公钥编译进程序本身。

### 一次性配置

```bash
# 生成密钥对；私钥写到 ~/.desktidy/update_signing.key（0600，绝不可提交）
python scripts/update_signing.py genkey

# 把输出的公钥粘贴到 native/src/update_verify.cpp 的 kBuiltinSigningKeyB64
# 把输出的"CI secret"base64 存进 GitHub Secrets：UPDATE_SIGNING_KEY_B64
```

### 强制约束（CI 会拦截）

- **公钥已嵌入** → 必须有 `UPDATE_SIGNING_KEY_B64`，否则发版失败（否则客户端会拒绝更新）。
- **有密钥但公钥未嵌入** → 发版失败（用户无法校验签名）。
- **两者都没有** → 允许发版，客户端跳过签名校验并打日志，仍强制 sha256。

> 密钥泄露 = 任何人可伪造更新。轮换流程：生成新密钥对 → 发布带新公钥的版本 → 销毁旧私钥。

> 站点上传（Cloudflare Pages / ByetHost）仍由本地 `python scripts/release.py` 执行，未放进 CI：这需要你的私有部署凭据，避免把 token 交给 Actions。

## 本地一键发版（可选）

```bat
scripts\release.bat --bump --notes "本次更新说明"
```

该脚本会额外更新官网下载链接并上传站点（需要本地 `.env.cloudflare` 或 `.env.byethost`）。
