/* bridge_native.js — deskNote offline Markdown viewer (WebView2, marked v12) */
(function () {
  "use strict";

  var suppressScrollEmit = false;
  var mermaidId = 0;
  var lastHeadings = [];

  function post(msg) {
    try {
      if (window.chrome && chrome.webview && chrome.webview.postMessage) {
        chrome.webview.postMessage(msg);
      }
    } catch (e) {}
  }

  function stripFrontMatter(md) {
    if (!md || md.indexOf("---") !== 0) return md;
    var end = md.indexOf("\n---", 3);
    if (end < 0) return md;
    var after = md.slice(end + 4);
    if (after.charAt(0) === "\n") after = after.slice(1);
    return after;
  }

  function slugify(text) {
    return String(text || "")
      .trim()
      .toLowerCase()
      .replace(/[^\w\u4e00-\u9fff\- ]+/g, "")
      .replace(/\s+/g, "-")
      .replace(/-+/g, "-")
      .replace(/^-|-$/g, "") || "section";
  }

  function escapeHtml(s) {
    return String(s)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  function preprocessFootnotes(md) {
    var defs = {};
    var lines = md.split("\n");
    var body = [];
    var defRe = /^\[\^([^\]]+)\]:\s*(.*)$/;
    for (var i = 0; i < lines.length; i++) {
      var m = lines[i].match(defRe);
      if (m) defs[m[1]] = m[2];
      else body.push(lines[i]);
    }
    var text = body.join("\n");
    text = text.replace(/\[\^([^\]]+)\]/g, function (_, key) {
      return (
        '<sup class="fn-ref"><a href="#fn-' + key + '" id="fnref-' + key +
        '">[' + key + "]</a></sup>"
      );
    });
    var keys = Object.keys(defs);
    if (keys.length) {
      text += '\n\n<div class="footnote"><ol>';
      for (var k = 0; k < keys.length; k++) {
        var key = keys[k];
        text +=
          '<li id="fn-' + key + '">' + escapeHtml(defs[key]) +
          ' <a href="#fnref-' + key + '">↩</a></li>';
      }
      text += "</ol></div>";
    }
    return text;
  }

  function buildTocHtml(headings) {
    if (!headings.length) return "";
    var html = '<div class="toc"><div class="toc-title">目录</div><ul>';
    for (var i = 0; i < headings.length; i++) {
      var h = headings[i];
      html +=
        "<li style=\"margin-left:" + (h.level - 1) * 12 + 'px"><a href="#' +
        h.id + '">' + escapeHtml(h.text) + "</a></li>";
    }
    html += "</ul></div>";
    return html;
  }

  // marked v12 Renderer overrides still use classic signatures
  // (heading(text, level, raw) / code(code, infostring, escaped)), not tokens.
  // Calling this.parser.parseInline here throws: parser is undefined on Renderer.
  var markedConfigured = false;
  function ensureMarked() {
    if (markedConfigured || !window.marked) return;
    markedConfigured = true;
    marked.use({
      gfm: true,
      breaks: false,
      renderer: {
        heading: function (text, level, raw) {
          var plain = String(raw != null ? raw : text || "").replace(/<[^>]+>/g, "");
          var id = slugify(plain);
          lastHeadings.push({ level: level, text: plain, id: id });
          return (
            "<h" + level + ' id="' + id + '">' + text + "</h" + level + ">\n"
          );
        },
        code: function (code, infostring) {
          var src = code || "";
          var lang = String(infostring || "")
            .trim()
            .split(/\s+/)[0] || "";
          if (lang === "mermaid") {
            mermaidId += 1;
            return (
              '<div class="mermaid" id="mermaid-' +
              mermaidId +
              '">' +
              escapeHtml(src) +
              "</div>\n"
            );
          }
          var highlighted;
          try {
            if (lang && window.hljs && hljs.getLanguage(lang)) {
              highlighted = hljs.highlight(src, { language: lang }).value;
            } else if (window.hljs) {
              highlighted = hljs.highlightAuto(src).value;
            } else {
              highlighted = escapeHtml(src);
            }
          } catch (e) {
            highlighted = escapeHtml(src);
          }
          var cls = lang
            ? ' class="hljs language-' + lang + '"'
            : ' class="hljs"';
          return "<pre><code" + cls + ">" + highlighted + "</code></pre>\n";
        },
      },
    });
  }

  function parseMarkdown(md) {
    lastHeadings = [];
    ensureMarked();
    return marked.parse(md);
  }

  function rewriteRelativeImages(root, baseUrl) {
    if (!baseUrl) return;
    var imgs = root.querySelectorAll("img[src]");
    for (var i = 0; i < imgs.length; i++) {
      var src = imgs[i].getAttribute("src") || "";
      if (
        !src ||
        /^(https?:|data:|file:|#)/i.test(src) ||
        src.indexOf("://") >= 0
      ) {
        continue;
      }
      var joined = String(baseUrl).replace(/\\/g, "/");
      if (joined.slice(-1) !== "/") joined += "/";
      imgs[i].setAttribute("src", joined + src.replace(/^\.\//, ""));
    }
  }

  function setTheme(theme) {
    var dark = String(theme || "").toLowerCase().indexOf("dark") >= 0;
    document.body.classList.toggle("theme-dark", dark);
  }

  function emitScroll() {
    if (suppressScrollEmit) return;
    var el = document.documentElement;
    var max = Math.max(1, el.scrollHeight - el.clientHeight);
    post({ type: "scroll", ratio: el.scrollTop / max });
  }

  window.__desknoteSetMarkdown = function (md, opts) {
    opts = opts || {};
    setTheme(opts.theme);
    mermaidId = 0;
    var raw = String(md || "");
    var text = stripFrontMatter(raw);
    text = preprocessFootnotes(text);
    var html = parseMarkdown(text);
    var toc = buildTocHtml(lastHeadings);
    if (toc && /\[TOC\]/i.test(raw)) {
      html = html.replace(/<p>\s*\[TOC\]\s*<\/p>/i, toc);
      html = html.replace(/\[TOC\]/i, toc);
    }
    var root = document.getElementById("content");
    root.innerHTML = html;
    rewriteRelativeImages(root, opts.baseUrl || "");
    try {
      if (window.renderMathInElement) {
        renderMathInElement(root, {
          delimiters: [
            { left: "$$", right: "$$", display: true },
            { left: "$", right: "$", display: false },
            { left: "\\(", right: "\\)", display: false },
            { left: "\\[", right: "\\]", display: true },
          ],
          throwOnError: false,
        });
      }
    } catch (e) {}
    try {
      if (window.mermaid) {
        mermaid.initialize({
          startOnLoad: false,
          securityLevel: "strict",
          theme: document.body.classList.contains("theme-dark") ? "dark" : "default",
        });
        var nodes = root.querySelectorAll(".mermaid");
        if (nodes.length) mermaid.run({ nodes: Array.prototype.slice.call(nodes) });
      }
    } catch (e2) {}
  };

  window.__desknoteSetScrollRatio = function (ratio) {
    suppressScrollEmit = true;
    var el = document.documentElement;
    var max = Math.max(0, el.scrollHeight - el.clientHeight);
    el.scrollTop = Math.max(0, Math.min(1, Number(ratio) || 0)) * max;
    setTimeout(function () { suppressScrollEmit = false; }, 80);
  };

  window.__desknoteGetScrollRatio = function () {
    var el = document.documentElement;
    var max = Math.max(1, el.scrollHeight - el.clientHeight);
    return el.scrollTop / max;
  };

  document.addEventListener("scroll", emitScroll);

  document.addEventListener("click", function (ev) {
    var a = ev.target.closest ? ev.target.closest("a") : null;
    if (!a) return;
    var href = a.getAttribute("href") || "";
    if (href.indexOf("#") === 0) return;
    if (href.indexOf("desknote-vault://") === 0) {
      ev.preventDefault();
      var q = {};
      href.replace(/[?&]([^=]+)=([^&]*)/g, function (_, k, v) {
        q[k] = decodeURIComponent(v || "");
      });
      post({
        type: "vaultCopy",
        i: q.i || "",
        field: q.f || q.field || "pass",
        title: "",
        user: ""
      });
      return;
    }
    if (/^(https?:|mailto:|file:)/i.test(href)) {
      ev.preventDefault();
      post({ type: "openExternal", href: href });
    }
  });

  try {
    if (window.chrome && chrome.webview) {
      chrome.webview.addEventListener("message", function (e) {
        var msg = e.data || {};
        if (msg.type === "set") {
          try {
            window.__desknoteSetMarkdown(msg.md || "", {
              theme: msg.theme || "",
              baseUrl: msg.baseUrl || "",
            });
          } catch (err) {
            var root = document.getElementById("content");
            if (root) {
              root.textContent =
                "预览渲染失败：" + (err && err.message ? err.message : String(err));
            }
          }
        } else if (msg.type === "scrollTo") {
          window.__desknoteSetScrollRatio(msg.ratio);
        } else if (msg.type === "getScroll") {
          post({ type: "scroll", ratio: window.__desknoteGetScrollRatio() });
        }
      });
    }
  } catch (e) {}

  function ready() {
    post({ type: "previewReady" });
  }
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", ready);
  } else {
    ready();
  }
})();
