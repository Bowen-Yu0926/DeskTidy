/* bridge.js — deskNote offline Markdown viewer (marked v12) */
(function () {
  "use strict";

  var bridge = null;
  var suppressScrollEmit = false;
  var mermaidId = 0;
  var lastHeadings = [];

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
        '<sup class="fn-ref"><a href="#fn-' +
        key +
        '" id="fnref-' +
        key +
        '">[' +
        key +
        "]</a></sup>"
      );
    });
    var keys = Object.keys(defs);
    if (keys.length) {
      text += '\n\n<div class="footnote"><ol>';
      for (var k = 0; k < keys.length; k++) {
        var key = keys[k];
        text +=
          '<li id="fn-' +
          key +
          '">' +
          escapeHtml(defs[key]) +
          ' <a href="#fnref-' +
          key +
          '">↩</a></li>';
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
        "<li style=\"margin-left:" +
        (h.level - 1) * 12 +
        'px"><a href="#' +
        h.id +
        '">' +
        escapeHtml(h.text) +
        "</a></li>";
    }
    html += "</ul></div>";
    return html;
  }

  function parseMarkdown(md) {
    lastHeadings = [];
    var renderer = {
      heading: function (token) {
        var text = this.parser.parseInline(token.tokens);
        var plain = text.replace(/<[^>]+>/g, "");
        var id = slugify(plain);
        lastHeadings.push({ level: token.depth, text: plain, id: id });
        return "<h" + token.depth + ' id="' + id + '">' + text + "</h" + token.depth + ">\n";
      },
      code: function (token) {
        var code = token.text || "";
        var lang = (token.lang || "").trim().split(/\s+/)[0] || "";
        if (lang === "mermaid") {
          mermaidId += 1;
          return (
            '<div class="mermaid" id="mermaid-' +
            mermaidId +
            '">' +
            escapeHtml(code) +
            "</div>\n"
          );
        }
        var highlighted;
        try {
          if (lang && window.hljs && hljs.getLanguage(lang)) {
            highlighted = hljs.highlight(code, { language: lang }).value;
          } else if (window.hljs) {
            highlighted = hljs.highlightAuto(code).value;
          } else {
            highlighted = escapeHtml(code);
          }
        } catch (e) {
          highlighted = escapeHtml(code);
        }
        var cls = lang ? ' class="hljs language-' + lang + '"' : ' class="hljs"';
        return "<pre><code" + cls + ">" + highlighted + "</code></pre>\n";
      },
    };
    marked.use({ renderer: renderer, gfm: true, breaks: false });
    return marked.parse(md);
  }

  function rewriteRelativeImages(root, baseUrl) {
    if (!baseUrl) return;
    var imgs = root.querySelectorAll("img[src]");
    for (var i = 0; i < imgs.length; i++) {
      var src = imgs[i].getAttribute("src") || "";
      if (
        !src ||
        /^(https?:|data:|file:|#|qrc:)/i.test(src) ||
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
    if (suppressScrollEmit || !bridge || !bridge.onPreviewScroll) return;
    var el = document.documentElement;
    var max = Math.max(1, el.scrollHeight - el.clientHeight);
    bridge.onPreviewScroll(el.scrollTop / max);
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
          theme: document.body.classList.contains("theme-dark")
            ? "dark"
            : "default",
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
    setTimeout(function () {
      suppressScrollEmit = false;
    }, 80);
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
    if (/^(https?:|mailto:|file:)/i.test(href)) {
      ev.preventDefault();
      if (bridge && bridge.openExternal) bridge.openExternal(href);
    }
  });

  function ready() {
    if (typeof qt !== "undefined" && qt.webChannelTransport) {
      new QWebChannel(qt.webChannelTransport, function (channel) {
        bridge = channel.objects.bridge;
        if (bridge && bridge.previewReady) bridge.previewReady();
      });
    }
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", ready);
  } else {
    ready();
  }
})();
