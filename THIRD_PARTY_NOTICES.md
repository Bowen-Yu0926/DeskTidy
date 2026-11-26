# Third-Party Notices / 第三方声明

DeskTidy bundles third-party open-source components. Their licenses and copyright notices are listed below. The full license texts are available at the upstream URLs.

本产品包含以下第三方开源组件。各组件的许可证与版权信息列于下方，完整许可证文本请参见对应上游地址。

## Bundled runtime components / 随产品分发的组件

### Milkdown Crepe (DeskNote WYSIWYG editor)
- Version / 版本: 7.23.0 (and transitive Milkdown/ProseMirror packages)
- License: MIT
- Upstream: https://github.com/Milkdown/milkdown
- Files: `native/assets/md_preview/vendor/desknote-editor.js`, `desknote-editor.css`
- Copyright: Milkdown contributors, ProseMirror contributors and related package authors.
- Note: rebuilt from `native/tools/crepe_entry.js` with local customizations; see that source for the exact package graph (`native/tools/package-lock.json`).

### TOAST UI Editor
- License: Apache-2.0
- Upstream: https://github.com/nhn/tui.editor
- Files: `native/assets/md_preview/vendor/toastui-editor.min.js`, `toastui-editor-all.min.js`, `toastui-editor.min.css`
- Copyright: NHN Cloud Corporation and TOAST UI contributors.

### Microsoft Edge WebView2
- Components: WebView2 SDK headers and loader binaries
- License: MIT
- Upstream: https://learn.microsoft.com/microsoft-edge/webview2/
- Files: `native/third_party/webview2-include/`, `native/third_party/webview2-x64/`
- Copyright: Microsoft Corporation.

### Argon2 (phc-winner-argon2)
- License: CC0-1.0 OR Apache-2.0
- Upstream: https://github.com/P-H-C/phc-winner-argon2
- Files: `native/third_party/argon2/`, `native/third_party/argon2/NOTICE.txt`
- Copyright: Argon2 reference implementation authors.

### TweetNaCl
- License: Public domain
- Upstream: https://tweetnacl.cr.yp.to/
- Files: `native/third_party/tweetnacl/`
- Copyright: Daniel J. Bernstein, Bernard van Gastel, Wesley Janssen, Tanja Lange, Peter Schwabe, Sjaak Smetsers.
- Note: uses only the Ed25519 verify path (`crypto_sign_open`); `randombytes` is replaced with an aborting shim so the client cannot sign.

### fd (file search utility)
- License: MIT OR Apache-2.0
- Upstream: https://github.com/sharkdp/fd
- Files: `assets/fd/fd.exe`, `assets/fd/NOTICE.txt`
- Copyright: David Peter and fd contributors.

## Build-time tools / 仅构建期使用（不随产品分发）

### esbuild
- Version / 版本: 0.25.0
- License: MIT
- Upstream: https://github.com/evanw/esbuild
- Used by `native/tools/build_crepe.js` to produce the editor bundle.

## Notes / 说明

- Most of the above licenses require that the copyright notice and permission notice be included with distributed copies. This file, together with the per-directory NOTICE files and upstream license texts, satisfies that requirement.
- If you redistribute modified bundles, regenerate `desknote-editor.{js,css}` from `native/tools/` and keep this notice in sync with `native/tools/package-lock.json`.
