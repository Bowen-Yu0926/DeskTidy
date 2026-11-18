// DeskNote WYSIWYG editor bundle.
//
// Exposes the Milkdown Crepe editor plus the pieces the shell page needs, with
// every built-in label localized to Chinese (Crepe ships English strings only
// and has no i18n layer).
import { Crepe } from '@milkdown/crepe'
import { replaceAll, getMarkdown } from '@milkdown/kit/utils'
import { listenerCtx } from '@milkdown/kit/plugin/listener'
import { editorViewCtx, prosePluginsCtx } from '@milkdown/kit/core'
import {
  imageBlockSchema,
  remarkImageBlockPlugin,
} from '@milkdown/kit/component/image-block'
import {
  imageSchema,
  paragraphSchema,
  clearTextInCurrentBlockCommand,
} from '@milkdown/kit/preset/commonmark'
import { uploadConfig } from '@milkdown/kit/plugin/upload'
import { Plugin, PluginKey, TextSelection } from '@milkdown/kit/prose/state'
import { Fragment } from '@milkdown/kit/prose/model'

import '@milkdown/crepe/theme/common/style.css'
import '@milkdown/crepe/theme/frame.css'

// Check whether the current selection sits inside a list item (ordered, bullet,
// or task list). Headings are disallowed inside list items.
function isSelectionInListItem(state) {
  if (!state || !state.selection) return false
  const checkPos = ($pos) => {
    for (let d = $pos.depth; d > 0; d--) {
      if ($pos.node(d).type.name === 'list_item') return true
    }
    return false
  }
  return checkPos(state.selection.$from) || checkPos(state.selection.$to)
}

function docHasHeadingInListItem(doc) {
  let found = false
  doc.descendants((node) => {
    if (found) return false
    if (node.type.name === 'list_item') {
      node.descendants((child) => {
        if (child.type.name === 'heading') {
          found = true
          return false
        }
        return true
      })
      return false
    }
    return true
  })
  return found
}

// Insert an inline image node at the current cursor position so multiple
// images can sit side-by-side on the same line.
function insertInlineImageNode(ctx, url, alt) {
  const view = ctx.get(editorViewCtx)
  const imageType = imageSchema.type(ctx)
  const node = imageType.create({
    src: url || '',
    alt: alt || '',
    title: '',
  })
  if (!node) return
  const { state } = view
  let tr = state.tr
  if (state.selection.node && state.selection.node.type === imageType) {
    const insertPos = state.selection.to
    tr = tr.insert(insertPos, node)
    if (url) {
      tr = tr.setSelection(
        TextSelection.create(tr.doc, insertPos + node.nodeSize),
      )
    }
  } else if (state.selection.$from.parent.inlineContent) {
    tr = tr.replaceSelectionWith(node)
    if (url) {
      const afterPos = Math.min(tr.doc.content.size, tr.selection.to)
      const $after = tr.doc.resolve(afterPos)
      if ($after.parent.inlineContent) {
        tr = tr.setSelection(TextSelection.create(tr.doc, afterPos))
      }
    }
  } else {
    const pType = paragraphSchema.type(ctx)
    const pNode = pType ? pType.create(null, node) : node
    tr = tr.replaceSelectionWith(pNode)
  }
  view.dispatch(tr.scrollIntoView())
  view.focus()
}

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
  buildMenu: (builder) => {
    const textGroup = builder.getGroup('text')?.group
    if (textGroup && Array.isArray(textGroup.items)) {
      const headingKeys = new Set(['h1', 'h2', 'h3', 'h4', 'h5', 'h6'])
      textGroup.items.forEach((item) => {
        if (!headingKeys.has(item.key)) return
        const origRun = item.onRun
        item.onRun = (ctx) => {
          const view = ctx.get(editorViewCtx)
          if (isSelectionInListItem(view.state)) {
            ctx.get(clearTextInCurrentBlockCommand.key)()
            return
          }
          origRun(ctx)
        }
      })
    }
    const advGroup = builder.getGroup('advanced')?.group
    if (advGroup && Array.isArray(advGroup.items)) {
      const imgItem = advGroup.items.find((it) => it.key === 'image')
      if (imgItem) {
        imgItem.onRun = (ctx) => {
          ctx.get(clearTextInCurrentBlockCommand.key)()
          insertInlineImageNode(ctx, '', '')
        }
      }
    }
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
  buildTopBar: (builder) => {
    // Disallow turning list-item body text into headings from the top bar.
    const headingGroup = builder.getGroup('heading')?.group
    if (headingGroup && Array.isArray(headingGroup.items)) {
      const selectorItem = headingGroup.items.find(
        (it) => it.key === 'heading-selector',
      )
      const opts = selectorItem?.selector?.options
      if (Array.isArray(opts)) {
        opts.forEach((opt) => {
          const origSelect = opt.onSelect
          opt.onSelect = (ctx) => {
            const view = ctx.get(editorViewCtx)
            if (isSelectionInListItem(view.state)) return
            origSelect(ctx)
          }
        })
      }
    }
    // Insert inline images so multiple images can share the same line.
    const insertGroup = builder.getGroup('insert')?.group
    if (insertGroup && Array.isArray(insertGroup.items)) {
      const imgItem = insertGroup.items.find((it) => it.key === 'image')
      if (imgItem) {
        imgItem.onRun = (ctx) => {
          insertInlineImageNode(ctx, '', '')
        }
      }
    }
  },
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
function imageBlockConfig(baseHolder, onUpload) {
  const cfg = {
    blockCaptionPlaceholderText: '添加图注',
    blockUploadPlaceholderText: '或粘贴图片链接',
    inlineUploadPlaceholderText: '或粘贴图片链接',
    proxyDomURL: (url) => {
      const s = url == null ? '' : String(url)
      const base =
        baseHolder.url ||
        (typeof location !== 'undefined' && location.hostname === 'app.desknote'
          ? 'https://note.assets/'
          : '')
      if (!s || !base) return s
      if (/^(https?:|data:|file:|blob:|#)/i.test(s) || s.indexOf('://') >= 0) {
        return s
      }
      return base + s.replace(/^\.?\//, '')
    },
  }
  if (typeof onUpload === 'function') {
    cfg.onUpload = onUpload
    cfg.blockOnUpload = onUpload
    cfg.inlineOnUpload = onUpload
  }
  return cfg
}

// Resolve the ProseMirror position of the image (inline or block) that owns `el`.
function imagePosOf(ctx, view, el) {
  const host =
    el && el.closest
      ? el.closest('.milkdown-image-inline, .milkdown-image-block')
      : null
  if (!host || !view.dom.contains(host)) return -1
  const blockType = imageBlockSchema.type(ctx)
  const inlineType = imageSchema.type(ctx)
  const isImgNode = (n) =>
    n && (n.type === inlineType || n.type === blockType)
  const rawPos = view.posAtDOM(host, 0)
  if (rawPos >= 0) {
    if (isImgNode(view.state.doc.nodeAt(rawPos))) return rawPos
    if (rawPos > 0 && isImgNode(view.state.doc.nodeAt(rawPos - 1))) {
      return rawPos - 1
    }
  }
  return -1
}

export function createDeskNoteEditor(options) {
  const {
    root,
    defaultValue,
    assetBaseUrl,
    onUpload,
    onMarkdownUpdated,
    onMounted,
  } = options
  const baseHolder = { url: assetBaseUrl || '' }
  let isReady = false
  let isApplyingSetMarkdown = false

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
      [Crepe.Feature.ImageBlock]: imageBlockConfig(baseHolder, onUpload),
    },
  })

  // Keep single-image and multi-image paragraphs as inline images so multiple
  // images can sit side-by-side on the same line.
  crepe.editor.remove(remarkImageBlockPlugin)

  crepe.editor.config((ctx) => {
    // Override paste/drop image uploader to insert inline image nodes in the
    // current line instead of splitting the paragraph with a block node.
    ctx.update(uploadConfig.key, (prev) => ({
      ...prev,
      uploader: async (files, schema, uCtx, insertPos) => {
        const defaultUploader =
          typeof onUpload === 'function'
            ? onUpload
            : (file) => Promise.resolve(URL.createObjectURL(file))
        const imgs = []
        for (let i = 0; i < files.length; i++) {
          const file = files.item(i)
          if (!file || !file.type.includes('image')) continue
          imgs.push(file)
        }
        const imageType = imageSchema.type(uCtx)
        const nodes = (
          await Promise.all(
            imgs.map(async (img) => {
              const src = await defaultUploader(img)
              if (!src) return null
              return imageType.create({
                src,
                alt: img.name || '',
                title: '',
              })
            }),
          )
        ).filter(Boolean)
        if (!nodes.length) return Fragment.empty
        const view = uCtx.get(editorViewCtx)
        const safePos = Math.max(
          0,
          Math.min(view.state.doc.content.size, insertPos),
        )
        const $pos = view.state.doc.resolve(safePos)
        if ($pos.parent.inlineContent) {
          return Fragment.from(nodes)
        }
        const pType = schema.nodes.paragraph
        return pType
          ? Fragment.from(pType.create(null, nodes))
          : Fragment.from(nodes)
      },
      getInsertPos: (event, uCtx, defaultInsertPos) => {
        if (event.type === 'paste') {
          const view = uCtx.get(editorViewCtx)
          return view.state.selection.to
        }
        return defaultInsertPos
      },
    }))

    // Guard plugin:
    // 1. Prevent headings inside list items (and sync .in-list-item on top bar).
    // 2. Move the caret right after newly uploaded/inserted inline images so
    //    consecutive pastes/insertions land side-by-side on the same line.
    const guardKey = new PluginKey('DESKNOTE_GUARD')
    const guardPlugin = new Plugin({
      key: guardKey,
      filterTransaction(tr, state) {
        if (!tr.docChanged) return true
        if (isSelectionInListItem(state) && docHasHeadingInListItem(tr.doc)) {
          return false
        }
        return true
      },
      appendTransaction(trs, oldState, newState) {
        const pType = newState.schema.nodes.paragraph
        let fixTr = null

        // Normalize any heading that ended up inside a list_item into a paragraph.
        if (pType && docHasHeadingInListItem(newState.doc)) {
          fixTr = newState.tr
          newState.doc.descendants((node, pos) => {
            if (node.type.name === 'list_item') {
              node.descendants((child, childOffset) => {
                if (child.type.name === 'heading') {
                  fixTr.setNodeMarkup(pos + 1 + childOffset, pType, null, child.marks)
                }
                return true
              })
              return false
            }
            return true
          })
          return fixTr
        }

        // After an interactive inline image upload/paste completes, place the
        // text cursor immediately after the image on the same line.
        if (
          isReady &&
          !isApplyingSetMarkdown &&
          trs.some((tr) => tr.docChanged)
        ) {
          let afterImgPos = -1
          for (const tr of trs) {
            if (!tr.docChanged) continue
            for (let i = 0; i < tr.steps.length; i++) {
              const step = tr.steps[i]
              const map = step.getMap()
              const remMapping = tr.mapping.slice(i + 1)
              map.forEach((oldStart, oldEnd, newStart, newEnd) => {
                if (
                  oldStart === 0 &&
                  oldEnd === oldState.doc.content.size &&
                  oldEnd > 4
                ) {
                  return
                }
                const from = remMapping.map(newStart, -1)
                const to = remMapping.map(newEnd, 1)
                newState.doc.nodesBetween(
                  Math.max(0, from),
                  Math.min(newState.doc.content.size, to),
                  (node, pos) => {
                    if (node.type.name === 'image' && node.attrs.src) {
                      const oldNode =
                        pos < oldState.doc.content.size
                          ? oldState.doc.nodeAt(pos)
                          : null
                      const wasSameImg =
                        oldNode &&
                        oldNode.type.name === 'image' &&
                        oldNode.attrs.src === node.attrs.src
                      if (!wasSameImg) {
                        afterImgPos = pos + node.nodeSize
                      }
                    }
                  },
                )
              })
            }
          }
          if (
            afterImgPos > 0 &&
            afterImgPos <= newState.doc.content.size &&
            newState.selection.to <= afterImgPos
          ) {
            const $after = newState.doc.resolve(afterImgPos)
            if ($after.parent.inlineContent) {
              return newState.tr.setSelection(
                TextSelection.create(newState.doc, afterImgPos),
              )
            }
          }
        }
        return null
      },
      view() {
        const syncTopBar = (view) => {
          const inList = isSelectionInListItem(view.state)
          const rootEl =
            (view.dom && view.dom.closest && view.dom.closest('#editor')) ||
            document
          const topBar = rootEl.querySelector('.milkdown-top-bar')
          if (topBar) {
            topBar.classList.toggle('in-list-item', inList)
          }
        }
        return {
          update(view) {
            syncTopBar(view)
          },
        }
      },
    })
    ctx.update(prosePluginsCtx, (prev) => [...prev, guardPlugin])
  })

  if (onMarkdownUpdated) {
    crepe.on((listener) => {
      listener.markdownUpdated((_ctx, markdown) => {
        onMarkdownUpdated(markdown)
      })
    })
  }

  const ready = crepe.create().then(() => {
    isReady = true
    if (onMounted) onMounted(crepe)
    return crepe
  })

  return {
    crepe,
    ready,
    getMarkdown: () => (isReady ? crepe.getMarkdown() : defaultValue || ''),
    setMarkdown(md) {
      const apply = () => {
        isApplyingSetMarkdown = true
        try {
          crepe.editor.action(replaceAll(md || '', false))
        } finally {
          isApplyingSetMarkdown = false
        }
      }
      if (isReady) apply()
      else ready.then(apply)
    },
    setAssetBaseUrl(url) {
      baseHolder.url = url || ''
    },
    focus() {
      if (!isReady) return
      crepe.editor.action((ctx) => {
        const view = ctx.get(editorViewCtx)
        view.focus()
      })
    },
    placeCaretAfterImage(el, before) {
      if (!isReady) return false
      return crepe.editor.action((ctx) => {
        const view = ctx.get(editorViewCtx)
        const pos = imagePosOf(ctx, view, el)
        if (pos < 0) return false
        const node = view.state.doc.nodeAt(pos)
        if (!node) return false
        const target = before
          ? pos
          : Math.min(view.state.doc.content.size, pos + node.nodeSize)
        const $target = view.state.doc.resolve(target)
        const sel =
          TextSelection.near($target, before ? -1 : 1) ||
          TextSelection.near($target, before ? 1 : -1)
        if (sel) {
          view.dispatch(view.state.tr.setSelection(sel))
        }
        view.focus()
        return true
      })
    },
    insertImage(url, alt) {
      crepe.editor.action((ctx) => {
        insertInlineImageNode(ctx, url, alt)
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
