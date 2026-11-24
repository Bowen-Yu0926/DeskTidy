# 自动发版说明

当推送形如 `v4.0.52` 的 tag 时，`.github/workflows/release.yml` 会在 Windows runner 上：

1. 校验 tag 与仓库根目录 `VERSION` 一致；
2. 用 CMake/VS2022 + Inno Setup 构建安装包；
3. 计算 `.exe.sha256`（产品内更新器会用到）；
4. 将安装包与校验文件发布到 `Bowen-Yu0926/DeskTidy` 的 GitHub Release；
5. 上传构建产物为 Actions artifact（保留 14 天）。

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

> 站点上传（Cloudflare Pages / ByetHost）仍由本地 `python scripts/release.py` 执行，未放进 CI：这需要你的私有部署凭据，避免把 token 交给 Actions。

## 本地一键发版（可选）

```bat
scripts\release.bat --bump --notes "本次更新说明"
```

该脚本会额外更新官网下载链接并上传站点（需要本地 `.env.cloudflare` 或 `.env.byethost`）。
