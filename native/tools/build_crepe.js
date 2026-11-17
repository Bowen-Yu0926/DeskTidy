// Build the DeskNote WYSIWYG editor bundle (Milkdown Crepe) as a single
// self-contained file pair for the WebView2 shell.
//
// Why a bundler and a vendored artifact: the note editor page must boot from
// disk with no network, and Crepe ships as ESM with ~200 transitive packages.
// esbuild flattens it into one IIFE + one CSS file; fonts are inlined as data
// URLs so nothing else has to be copied.
//
// The image schema patch (tools/patch_milkdown_image_alt.js) has to be applied
// to node_modules before bundling: Milkdown registers node schemas at import
// time, so a runtime override is too late.
//
// Usage:
//   cd native/tools && npm install && node build_crepe.js
const { execFileSync } = require("child_process");
const fs = require("fs");
const path = require("path");

const here = __dirname;
const outDir = path.join(here, "..", "assets", "md_preview", "vendor");
const entry = path.join(here, "crepe_entry.js");

execFileSync(process.execPath, [path.join(here, "patch_milkdown_image_alt.js"),
  path.join(here, "node_modules")], { stdio: "inherit" });

const esbuild = path.join(
  here,
  "node_modules",
  "@esbuild",
  process.platform === "win32" ? "win32-x64" : "linux-x64",
  process.platform === "win32" ? "esbuild.exe" : "esbuild",
);
if (!fs.existsSync(esbuild)) {
  console.error("esbuild not found at " + esbuild + " — run npm install first");
  process.exit(2);
}
fs.mkdirSync(outDir, { recursive: true });

execFileSync(
  esbuild,
  [
    entry,
    "--bundle",
    "--format=iife",
    "--minify",
    "--target=chrome110",
    "--loader:.css=css",
    "--loader:.woff=dataurl",
    "--loader:.woff2=dataurl",
    "--loader:.ttf=dataurl",
    "--outfile=" + path.join(outDir, "desknote-editor.js"),
  ],
  { stdio: "inherit" },
);

for (const f of ["desknote-editor.js", "desknote-editor.css"]) {
  const p = path.join(outDir, f);
  const kb = (fs.statSync(p).size / 1024).toFixed(0);
  console.log("built " + p + " (" + kb + " KB)");
}
