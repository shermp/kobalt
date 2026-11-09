// This function body runs only through the native API in the kobalt-reader
// isolated world. Book markup cannot execute scripts or replace this state.
if (document.URL !== options.uri) return { source: document.URL };
const body = document.body;
if (!body) throw new Error('missing-body');
if (getComputedStyle(body).writingMode !== 'horizontal-tb' ||
    getComputedStyle(body).direction !== 'ltr') throw new Error('unsupported-writing-direction');
const state = globalThis.kobaltReaderState ||= { page: 0, start: 0 };
const margin = options.margin;
// Restore publisher declarations before resolving the cascade again. Keeping
// only our previous computed override would freeze media-query break rules.
for (const override of state.breakOverrides || []) {
  if (override.style !== null)
    override.element.setAttribute('style', override.style);
  else
    override.element.removeAttribute('style');
}
state.breakOverrides = [];
state.style?.remove();
body.style.removeProperty('transform');
const style = document.createElementNS('http://www.w3.org/1999/xhtml', 'style');
state.style = style;
style.textContent = `
html { width: ${options.width}px !important; height: ${options.height}px !important;
  overflow: hidden !important; margin: 0 !important; padding: 0 !important;
  background: ${options.dark ? '#181818' : '#ffffff'} !important; }
body { position: relative !important; box-sizing: border-box !important;
  width: ${options.width - 2 * margin}px !important;
  height: ${options.height - 2 * margin}px !important;
  min-height: 0 !important; max-height: none !important;
  margin: ${margin}px !important; padding: 0 !important;
  column-width: ${options.width - 2 * margin}px !important;
  column-gap: ${2 * margin}px !important; column-fill: auto !important;
  column-count: auto !important; overflow: visible !important;
  transform: none !important; font-size: ${options.fontSize}px !important;
  line-height: ${options.lineHeight / 100} !important;
  color: ${options.dark ? '#eeeeee' : '#111111'} !important;
  background: transparent !important;
  ${options.family === 'publisher' ? '' : `font-family: ${options.family} !important;`}
}
body * { animation: none !important; transition: none !important;
  ${options.dark ? 'color: #eeeeee !important; background-color: transparent !important;' : ''}
  letter-spacing: ${options.letterSpacing / 100}em !important;
  word-spacing: ${options.wordSpacing / 100}em !important; }
img, svg, math { max-width: 100% !important;
  max-height: ${options.height - 2 * margin}px !important; object-fit: contain; }
pre { white-space: pre-wrap !important; overflow-wrap: anywhere; }
`;
document.head.appendChild(style);
// Trigger font selection before awaiting its readiness, then measure breaks
// against the actual glyph/image geometry rather than loading placeholders.
void body.offsetHeight;
await document.fonts.ready;
await Promise.all(Array.from(document.images, image => image.decode().catch(() => {})));
const elements = Array.from(body.querySelectorAll('*'));
if (elements.length > 200000) throw new Error('chapter-too-large');
// Read the complete cascade before writing overrides, including legacy
// page-break-* aliases, inline declarations and author !important rules.
const breakChanges = [];
const breakElements = new Map();
let firstContent = null;
const contentWalker = document.createTreeWalker(body, NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_TEXT);
for (let node; (node = contentWalker.nextNode());) {
  if (node.nodeType === Node.TEXT_NODE && node.data.trim()) {
    const range = document.createRange();
    range.selectNodeContents(node);
    if (Array.from(range.getClientRects()).some(rect => rect.width > 0 && rect.height > 0)) {
      firstContent = node;
      break;
    }
  } else if (node.nodeType === Node.ELEMENT_NODE && /^(img|svg|math)$/i.test(node.localName)) {
    const rect = node.getBoundingClientRect();
    if (rect.width > 0 && rect.height > 0) {
      firstContent = node;
      break;
    }
  }
}
for (const element of elements) {
  const computed = getComputedStyle(element);
  for (const property of ['break-before', 'break-after', 'break-inside']) {
    const value = computed.getPropertyValue(property);
    let mapped = value === 'avoid-page' ? 'avoid-column'
      : /^(page|left|right|recto|verso)$/.test(value) ? 'column' : null;
    if (mapped === 'column' && property === 'break-before') {
      // Each spine starts on a fresh reader page. Nested chapter/heading rules
      // at its beginning must not create a page containing only empty spacers.
      if (!firstContent || element.contains(firstContent)
          || !(firstContent.compareDocumentPosition(element) & Node.DOCUMENT_POSITION_FOLLOWING))
        mapped = 'auto';
    }
    if (mapped) {
      breakChanges.push({ element, property, mapped });
      breakElements.set(element, element.getAttribute('style'));
    }
  }
}
for (const change of breakChanges) {
  change.element.style.setProperty(change.property, change.mapped, 'important');
}
await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
// WebKit's column layout does not enforce avoid-before/after sibling boundaries.
// Move a fitting normal-flow group intact, without wrapping or rewriting book DOM.
const avoids = value => /^(avoid|avoid-column|avoid-page)$/.test(value);
const forces = value => /^(column|page|left|right|recto|verso)$/.test(value);
function keepGroup(group) {
  if (group.length < 2) return;
  const rects = group.map(element => Array.from(element.getClientRects()));
  if (rects.some(rects => rects.length !== 1)) return;
  const boxes = rects.map(rects => rects[0]);
  const firstPage = Math.floor((boxes[0].left - margin + 0.5) / options.width);
  if (boxes.every(box => Math.floor((box.left - margin + 0.5) / options.width) === firstPage)) return;
  let extent = boxes.reduce((sum, box) => sum + box.height, 0);
  for (let index = 1; index < group.length; ++index)
    extent += Math.max(0, parseFloat(getComputedStyle(group[index - 1]).marginBottom) || 0,
      parseFloat(getComputedStyle(group[index]).marginTop) || 0);
  if (extent > options.height - 2 * margin) return;
  const first = group[0];
  if (!breakElements.has(first)) breakElements.set(first, first.getAttribute('style'));
  first.style.setProperty('break-before', 'column', 'important');
}
for (const parent of [body, ...elements]) {
  if (!/^(block|flow-root|list-item)$/.test(getComputedStyle(parent).display)) continue;
  let group = [];
  for (const element of parent.children) {
    const style = getComputedStyle(element);
    if (style.display === 'none') continue;
    if (!/^(block|flow-root|list-item|table)$/.test(style.display)
        || style.position === 'absolute' || style.position === 'fixed' || style.cssFloat !== 'none') {
      keepGroup(group);
      group = [];
      continue;
    }
    const previous = group[group.length - 1];
    if (previous) {
      const after = getComputedStyle(previous).breakAfter, before = style.breakBefore;
      if (forces(after) || forces(before) || !(avoids(after) || avoids(before))) {
        keepGroup(group);
        group = [];
      }
    }
    group.push(element);
  }
  keepGroup(group);
}
state.breakOverrides = Array.from(breakElements, ([element, style]) => ({ element, style }));
await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
const pages = Math.max(1, Math.ceil((body.scrollWidth + 2 * margin - 1) / options.width));
if (pages > 100000) throw new Error('chapter-too-large');
const walker = document.createTreeWalker(body, NodeFilter.SHOW_TEXT, {
  acceptNode(node) {
    return /^(script|style|noscript)$/i.test(node.parentElement?.localName || '')
      ? NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT;
  }
});
const nodes = [];
let length = 0;
for (let node; (node = walker.nextNode());) {
  nodes.push({ node, offset: length });
  length += node.length;
  if (nodes.length > 200000) throw new Error('chapter-too-large');
}
function rectAt(node, offset) {
  // Collapsed leading/trailing whitespace has no rect. Mapping it to x=0
  // would make character positions non-monotone across columns.
  if (/\s/u.test(node.data[offset] || '')) {
    const following = node.data.slice(offset).search(/\S/u);
    offset = following >= 0 ? offset + following : node.data.search(/\S\s*$/u);
  }
  offset = Math.max(0, Math.min(node.length - 1, offset));
  const range = document.createRange();
  range.setStart(node, offset);
  range.setEnd(node, Math.min(node.length, offset + 1));
  return range.getBoundingClientRect();
}
function pageAt(node, offset) {
  return Math.max(0, Math.floor((rectAt(node, offset).left - margin + 0.5) / options.width));
}
function pageForOffset(offset) {
  const entry = nodes.find(entry => entry.offset + entry.node.length > offset);
  return entry ? pageAt(entry.node, Math.max(0, offset - entry.offset)) : pages - 1;
}
function elementAt(path) {
  return path.reduce((element, index) => element?.children[index], body);
}
function elementPath(element) {
  const path = [];
  while (element !== body) {
    const parent = element.parentElement;
    if (!parent || path.length > 1024) return null;
    path.unshift(Array.prototype.indexOf.call(parent.children, element));
    element = parent;
  }
  return path;
}
let page = options.page;
if (options.anchor !== null) {
  const object = options.element ? elementAt(options.element) : null;
  page = object ? Math.floor((object.getBoundingClientRect().left - margin + 0.5) / options.width)
    : pageForOffset(options.anchor);
}
if (options.fragment) {
  const target = document.getElementById(options.fragment) ||
    Array.from(document.getElementsByName(options.fragment))[0];
  if (!target) return { source: document.URL, error: 'missing-link-target' };
  page = Math.floor((target.getBoundingClientRect().left - margin + 0.5) / options.width);
}
if (page < 0) page = pages - 1;
page = Math.max(0, Math.min(pages - 1, page));
let start = length, end = length;
for (const entry of nodes) {
  const { node, offset } = entry;
  if (!node.length || !node.data.trim()) continue;
  const range = document.createRange();
  range.selectNodeContents(node);
  const rects = Array.from(range.getClientRects());
  if (!rects.some(r => r.width > 0 && r.height > 0)) continue;
  // Text can span columns. Find the first character of this page and the next
  // using the monotone horizontal column position, including split text nodes.
  function lowerBound(target) {
    let low = 0, high = node.length;
    while (low < high) {
      const middle = (low + high) >>> 1;
      if (pageAt(node, middle) < target) low = middle + 1;
      else high = middle;
    }
    return low;
  }
  const first = lowerBound(page), next = lowerBound(page + 1);
  if (first < next) {
    start = Math.min(start, offset + first);
    end = end === length ? offset + next : Math.max(end, offset + next);
  }
}
body.style.setProperty('transform', `translateX(${-page * options.width}px)`, 'important');
window.scrollTo(0, 0);
await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
const links = [];
for (const anchor of body.querySelectorAll('a[href]')) {
  for (const rect of anchor.getClientRects()) {
    const x = Math.max(margin, rect.left), y = Math.max(margin, rect.top);
    const right = Math.min(options.width - margin, rect.right);
    const bottom = Math.min(options.height - margin, rect.bottom);
    if (right > x && bottom > y && links.length < 1024)
      links.push({ href: anchor.getAttribute('href'), x, y, width: right - x, height: bottom - y });
  }
}
const objects = [];
for (const element of body.querySelectorAll('img,svg,math')) {
  for (const rect of element.getClientRects()) {
    const x = Math.max(margin, rect.left), y = Math.max(margin, rect.top);
    const right = Math.min(options.width - margin, rect.right);
    const bottom = Math.min(options.height - margin, rect.bottom);
    if (right > x && bottom > y && objects.length < 1024)
      objects.push({ path: elementPath(element), x, y, width: right - x, height: bottom - y });
  }
}
state.page = page;
state.start = start;
const excerpt = nodes.map(entry => entry.node.textContent).join('').slice(start, start + 160);
return { source: document.URL, page, pages,
  location: { version: 1, unit: 'utf16-text', start, end, element: objects[0]?.path || null },
  viewport: { width: window.innerWidth, height: window.innerHeight, scale: window.devicePixelRatio },
  content: { x: margin, y: margin, width: options.width - 2 * margin,
    height: options.height - 2 * margin }, links, objects, excerpt };
