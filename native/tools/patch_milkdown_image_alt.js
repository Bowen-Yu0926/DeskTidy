// Patch @milkdown/components' image-block schema for DeskNote.
//
// Upstream Crepe stores the drag-resize ratio in the markdown `alt` slot and
// throws the real alt text away, so every existing `![说明文字](x.png)` would
// come back as `![1.00](x.png)`. DeskNote keeps the alt text and appends the
// ratio only when it differs from 1:
//
//   ![说明文字](assets/x.png)          natural size
//   ![说明文字|0.62](assets/x.png)     resized to 62% of the natural height
//
// The patch must be applied to node_modules BEFORE bundling (Milkdown registers
// the schema at import time, so a runtime override is too late).
//
// Usage: node patch_milkdown_image_alt.js <path-to-node_modules>
const fs = require("fs");
const path = require("path");

const root = process.argv[2];
if (!root) {
  console.error("usage: node patch_milkdown_image_alt.js <node_modules>");
  process.exit(2);
}
const target = path.join(
  root,
  "@milkdown",
  "components",
  "lib",
  "image-block",
  "index.js",
);
if (!fs.existsSync(target)) {
  console.error("not found: " + target);
  process.exit(2);
}

let s = fs.readFileSync(target, "utf8");
if (s.includes("DeskNote: alt carries")) {
  console.log("already patched: " + target);
  process.exit(0);
}

function replaceOnce(needle, replacement, label) {
  const at = s.indexOf(needle);
  if (at < 0) throw new Error("patch anchor not found: " + label);
  if (s.indexOf(needle, at + 1) >= 0) {
    throw new Error("patch anchor is ambiguous: " + label);
  }
  s = s.slice(0, at) + replacement + s.slice(at + needle.length);
}

replaceOnce(
  `    attrs: {
      src: { default: "", validate: "string" },
      caption: { default: "", validate: "string" },
      ratio: { default: 1, validate: "number" }
    },`,
  `    attrs: {
      src: { default: "", validate: "string" },
      caption: { default: "", validate: "string" },
      ratio: { default: 1, validate: "number" },
      alt: { default: "", validate: "string" }
    },`,
  "attrs",
);

replaceOnce(
  `            ratio: Number((_a = dom.getAttribute("ratio")) != null ? _a : 1)
          };`,
  `            ratio: Number((_a = dom.getAttribute("ratio")) != null ? _a : 1),
            alt: dom.getAttribute("alt") || ""
          };`,
  "parseDOM",
);

replaceOnce(
  `        const src = node.url;
        const caption = (_a = node.title) != null ? _a : "";
        let ratio = Number(node.alt || 1);
        if (Number.isNaN(ratio) || ratio === 0) ratio = 1;
        state.addNode(type, {
          src,
          caption,
          ratio
        });`,
  `        const src = node.url;
        const caption = (_a = node.title) != null ? _a : "";
        // DeskNote: alt carries "text|ratio"; the ratio suffix is optional so
        // a plain ![alt](src) image keeps its alt text verbatim.
        const rawAlt = node.alt == null ? "" : String(node.alt);
        const bar = rawAlt.lastIndexOf("|");
        let altText = rawAlt;
        let ratio = 1;
        if (bar >= 0) {
          const n = Number.parseFloat(rawAlt.slice(bar + 1));
          if (Number.isFinite(n) && n > 0) {
            ratio = n;
            altText = rawAlt.slice(0, bar);
          }
        }
        state.addNode(type, {
          src,
          caption,
          ratio,
          alt: altText
        });`,
  "parseMarkdown",
);

replaceOnce(
  "          alt: `${Number.parseFloat(node.attrs.ratio).toFixed(2)}`",
  `          alt: (function () {
            const a = node.attrs.alt || "";
            const r = Number(node.attrs.ratio);
            if (!Number.isFinite(r) || Math.abs(r - 1) < 0.005) return a;
            return a + "|" + String(Number(r.toFixed(2)));
          })()`,
  "toMarkdown",
);

fs.writeFileSync(target, s);
console.log("patched " + target);
