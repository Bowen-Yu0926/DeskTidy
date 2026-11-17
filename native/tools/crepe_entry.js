// DeskNote WYSIWYG editor bundle.
//
// Exposes the Milkdown Crepe editor plus the pieces the shell page needs, with
// every built-in label localized to Chinese (Crepe ships English strings only
// and has no i18n layer).
import { Crepe } from '@milkdown/crepe'
import { replaceAll, getMarkdown } from '@milkdown/kit/utils'
import { listenerCtx } from '@milkdown/kit/plugin/listener'
import { editorViewCtx } from '@milkdown/kit/core'
import { imageBlockSchema } from '@milkdown/kit/component/image-block'

import '@milkdown/crepe/theme/common/style.css'
import '@milkdown/crepe/theme/frame.css'

// --- Chinese UI ------------------------------------------------------------
// Slash menu + selection toolbar labels. Crepe reads these from
// featureConfigs, so this is config rather than a patch.
const blockEditConfig = {
  textGroup: {
    label: '文本',
    text: { label: '正文' },
    h1: { label: '一级标题' },
    h2: { label: '二级标题' },
    h3: { label: '三级标题' },
    h4: { label: '四级标题' },
    h5: { label: '五级标题' },
    h6: { label: '六级标题' },
    quote: { label: '引用' },
    divider: { label: '分割线' },
  },
  listGroup: {
    label: '列表',
    bulletList: { label: '无序列表' },
    orderedList: { label: '有序列表' },
    taskList: { label: '任务列表' },
  },
  advancedGroup: {
    label: '插入',
    image: { label: '图片' },
    codeBlock: { label: '代码块' },
    table: { label: '表格' },
    math: { label: '公式' },
  },
}

const toolbarConfig = {
  boldLabel: '加粗',
  codeLabel: '行内代码',
  italicLabel: '斜体',
  linkLabel: '链接',
  strikethroughLabel: '删除线',
  latexLabel: '公式',
  aiLabel: 'AI',
}

// Fixed top bar (DingTalk-doc style). Crepe ships English heading labels only.
const topBarConfig = {
  headingOptions: [
    { label: '正文', level: null },
    { label: '标题 1', level: 1 },
    { label: '标题 2', level: 2 },
    { label: '标题 3', level: 3 },
    { label: '标题 4', level: 4 },
    { label: '标题 5', level: 5 },
    { label: '标题 6', level: 6 },
  ],
}

const codeMirrorConfig = {
  searchPlaceholder: '搜索语言',
  noResultText: '无匹配',
}

const placeholderConfig = { text: '输入「/」插入内容…' }

const linkTooltipConfig = {
  editButton: '编辑',
  removeButton: '移除',
  confirmButton: '确定',
  inputPlaceholder: '粘贴或输入链接',
}

// The page is served from https://app.desknote/, so a relative `assets/x.png`
// would resolve inside the shell folder and 404. `proxyDomURL` rewrites only
// the DOM src; the node keeps the relative path, so the saved markdown is
// unchanged. It reads through a holder because the note folder changes on
// every tab switch.
function imageBlockConfig(baseHolder) {
  return {
    blockCaptionPlaceholderText: '添加图注',
    blockUploadPlaceholderText: '或粘贴图片链接',
    inlineUploadPlaceholderText: '或粘贴图片链接',
    proxyDomURL: (url) => {
      const s = url == null ? '' : String(url)
      const base = baseHolder.url || ''
      if (!s || !base) return s
      if (/^(https?:|data:|file:|blob:|#)/i.test(s) || s.indexOf('://') >= 0) {
        return s
      }
      return base + s.replace(/^\.?\//, '')
    },
  }
}

// Resolve the ProseMirror position of the image block that owns `el`. The node
// view wraps the <img> in `.milkdown-image-block`; posAtDOM on that wrapper
// gives the node's position inside the document.
function imagePosOf(ctx, view, el) {
  const host = el && el.closest ? el.closest('.milkdown-image-block') : null
  if (!host || !view.dom.contains(host)) return -1
  const pos = view.posAtDOM(host, 0)
  if (pos < 0) return -1
  const node = view.state.doc.nodeAt(pos)
  if (!node || node.type !== imageBlockSchema.type(ctx)) return -1
  return pos
}

export function createDeskNoteEditor(options) {
  const {
    root,
    defaultValue,
    assetBaseUrl,
    onMarkdownUpdated,
    onMounted,
  } = options
  const baseHolder = { url: assetBaseUrl || '' }
  const crepe = new Crepe({
    root,
    defaultValue: defaultValue || '',
    // TopBar is opt-in in Crepe; Toolbar (the floating bubble over a
    // selection) is on by default. DeskNote shows a fixed bar on top instead,
    // matching DingTalk docs, so swap one for the other.
    features: {
      [Crepe.Feature.TopBar]: true,
      [Crepe.Feature.Toolbar]: false,
    },
    featureConfigs: {
      [Crepe.Feature.BlockEdit]: blockEditConfig,
      [Crepe.Feature.Toolbar]: toolbarConfig,
      [Crepe.Feature.TopBar]: topBarConfig,
      [Crepe.Feature.CodeMirror]: codeMirrorConfig,
      [Crepe.Feature.Placeholder]: placeholderConfig,
      [Crepe.Feature.LinkTooltip]: linkTooltipConfig,
      [Crepe.Feature.ImageBlock]: imageBlockConfig(baseHolder),
    },
  })

  if (onMarkdownUpdated) {
    crepe.on((listener) => {
      listener.markdownUpdated((_ctx, markdown) => {
        onMarkdownUpdated(markdown)
      })
    })
  }

  const ready = crepe.create().then(() => {
    if (onMounted) onMounted(crepe)
    return crepe
  })

  return {
    crepe,
    ready,
    getMarkdown: () => crepe.getMarkdown(),
    setMarkdown(md) {
      crepe.editor.action(replaceAll(md || '', false))
    },
    setAssetBaseUrl(url) {
      baseHolder.url = url || ''
    },
    focus() {
      crepe.editor.action((ctx) => {
        const view = ctx.get(editorViewCtx)
        view.focus()
      })
    },
    insertImage(url, alt) {
      crepe.editor.action((ctx) => {
        const view = ctx.get(editorViewCtx)
        const type = imageBlockSchema.type(ctx)
        const node = type.createAndFill({
          src: url,
          caption: '',
          ratio: 1,
          alt: alt || '',
        })
        if (!node) return
        view.dispatch(
          view.state.tr.replaceSelectionWith(node).scrollIntoView(),
        )
        view.focus()
      })
    },
    // Image chrome (the floating widget in the shell page) works from the DOM
    // node, because Crepe's node view gives no handle to the ProseMirror
    // position. posAtDOM resolves it, then a plain transaction edits the node.
    deleteImage(el) {
      return crepe.editor.action((ctx) => {
        const view = ctx.get(editorViewCtx)
        const pos = imagePosOf(ctx, view, el)
        if (pos < 0) return false
        const node = view.state.doc.nodeAt(pos)
        if (!node) return false
        view.dispatch(view.state.tr.delete(pos, pos + node.nodeSize))
        view.focus()
        return true
      })
    },
    setImageRatio(el, ratio) {
      return crepe.editor.action((ctx) => {
        const view = ctx.get(editorViewCtx)
        const pos = imagePosOf(ctx, view, el)
        if (pos < 0) return false
        const n = Math.max(0.05, Math.min(4, Number(ratio) || 1))
        view.dispatch(
          view.state.tr.setNodeAttribute(pos, 'ratio', Number(n.toFixed(3))),
        )
        return true
      })
    },
    insertHardBreak() {
      crepe.editor.action((ctx) => {
        const view = ctx.get(editorViewCtx)
        const type = view.state.schema.nodes.hardbreak
        if (!type) return
        view.dispatch(view.state.tr.replaceSelectionWith(type.create()))
      })
    },
    insertText(text) {
      crepe.editor.action((ctx) => {
        const view = ctx.get(editorViewCtx)
        view.dispatch(view.state.tr.insertText(text || ''))
        view.focus()
      })
    },
    destroy: () => crepe.destroy(),
  }
}

export const __getMarkdown = getMarkdown
export { Crepe, listenerCtx }

// esbuild's iife format drops ESM exports on the floor; the shell page loads
// this as a plain <script> and reads the factory off `window`.
window.__desknoteEditor = {
  createDeskNoteEditor,
  Crepe,
  listenerCtx,
  getMarkdown,
}
