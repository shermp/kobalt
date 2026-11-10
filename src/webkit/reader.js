// This function body runs only through the native API in the kobalt-reader
// isolated world. Book markup cannot execute scripts or replace this state.
if (document.URL !== options.uri) return { source: document.URL };
let sourcePosition = null;
try { sourcePosition = options.restore ? JSON.parse(options.restore) : null; }
catch (_) { return { source: document.URL, error: 'invalid-source-position' }; }
if (options.restore && (!sourcePosition || typeof sourcePosition !== 'object' || Array.isArray(sourcePosition)))
  return { source: document.URL, error: 'invalid-source-position' };
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
const walker = document.createTreeWalker(body, NodeFilter.SHOW_TEXT | NodeFilter.SHOW_CDATA_SECTION, {
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
const text = nodes.map(entry => entry.node.data).join('');
function textPoint(offset) {
  const entry = nodes.find(entry => entry.offset + entry.node.length > offset) || nodes.at(-1);
  if (!entry) return null;
  let local = Math.max(0, Math.min(entry.node.length, offset - entry.offset));
  // Positions are UTF-16 offsets between Unicode characters.
  if (local && /[\uD800-\uDBFF]/u.test(entry.node.data[local - 1]) &&
      /[\uDC00-\uDFFF]/u.test(entry.node.data[local] || '')) --local;
  return { node: entry.node, local, offset: entry.offset + local };
}
function objectPoint(node) {
  const next = nodes.find(entry => node.compareDocumentPosition(entry.node) & Node.DOCUMENT_POSITION_FOLLOWING);
  return { node, local: null, offset: next ? next.offset : length };
}
function cfiPath(node, offset = null) {
  const steps = [];
  if (offset !== null) {
    let elements = 0, precedingText = 0;
    for (const sibling of node.parentNode.childNodes) {
      if (sibling === node) break;
      if (sibling.nodeType === Node.ELEMENT_NODE) { ++elements; precedingText = 0; }
      else if (sibling.nodeType === Node.TEXT_NODE || sibling.nodeType === Node.CDATA_SECTION_NODE)
        precedingText += sibling.length;
    }
    steps.unshift(`${2 * elements + 1}:${precedingText + offset}`);
    node = node.parentElement;
  }
  while (node && node !== document.documentElement) {
    if (!node.parentElement || steps.length >= 256) throw new Error('invalid-source-position');
    steps.unshift(String(2 * (Array.prototype.indexOf.call(node.parentElement.children, node) + 1)));
    node = node.parentElement;
  }
  return '/' + steps.join('/');
}
function cfiPoint(cfi) {
  const prefix = `epubcfi(${options.cfiBase}!`;
  if (typeof cfi !== 'string' || cfi.length > 2048 || !cfi.startsWith(prefix) || !cfi.endsWith(')'))
    return null;
  const match = /^((?:\/[1-9][0-9]*)+)(?::(0|[1-9][0-9]*))?$/u.exec(cfi.slice(prefix.length, -1));
  if (!match) return null;
  const steps = match[1].slice(1).split('/').map(Number);
  if (steps.length > 256) return null;
  let node = document.documentElement;
  for (let index = 0; index < steps.length; ++index) {
    const step = steps[index];
    if (step % 2 === 0) node = node?.children[step / 2 - 1];
    else {
      if (index !== steps.length - 1 || match[2] === undefined || !node) return null;
      let elements = 0, local = Number(match[2]);
      for (const child of node.childNodes) {
        if (child.nodeType === Node.ELEMENT_NODE) ++elements;
        else if (elements === (step - 1) / 2 &&
                 (child.nodeType === Node.TEXT_NODE || child.nodeType === Node.CDATA_SECTION_NODE)) {
          if (local <= child.length) {
            const entry = nodes.find(entry => entry.node === child);
            return entry ? textPoint(entry.offset + local) : null;
          }
          local -= child.length;
        }
      }
      return null;
    }
    if (!node) return null;
  }
  return match[2] === undefined && node && body.contains(node) ? objectPoint(node) : null;
}
function validPosition(saved) {
  return saved && saved.version === 1 && Number.isInteger(saved.offset) && saved.offset >= 0 &&
    typeof saved.before === 'string' && saved.before.length <= 64 &&
    typeof saved.after === 'string' && saved.after.length <= 64;
}
function resolvePosition(saved) {
  if (!validPosition(saved)) return null;
  let restored = cfiPoint(saved.cfi);
  const asserts = offset => offset >= saved.before.length && offset <= text.length &&
    text.slice(offset - saved.before.length, offset) === saved.before &&
    text.slice(offset, offset + saved.after.length) === saved.after;
  if (!restored || (restored.local !== null && !asserts(restored.offset))) {
    // A text assertion can recover a parser-dependent path, but an ambiguous
    // quote must never silently move a saved anchor to another passage.
    if (asserts(saved.offset) && (saved.before || saved.after)) restored = textPoint(saved.offset);
    else {
      const quote = saved.before + saved.after;
      const first = quote ? text.indexOf(quote) : -1;
      if (first < 0 || text.indexOf(quote, first + 1) !== -1)
        return null;
      restored = textPoint(first + saved.before.length);
    }
  }
  return restored;
}
let restored = sourcePosition ? resolvePosition(sourcePosition) : null;
if (sourcePosition && !restored)
  return { source: document.URL, error: !validPosition(sourcePosition)
    ? 'invalid-source-position' : 'unresolved-source-position' };
let page = options.page, fragmentPoint = null;
if (options.anchor !== null) {
  const object = options.element ? elementAt(options.element) : null;
  page = object ? Math.floor((object.getBoundingClientRect().left - margin + 0.5) / options.width)
    : pageForOffset(options.anchor);
}
if (options.fragment) {
  const target = document.getElementById(options.fragment) ||
    Array.from(document.getElementsByName(options.fragment))[0];
  if (!target) return { source: document.URL, error: 'missing-link-target' };
  const first = nodes.find(entry => target.contains(entry.node));
  fragmentPoint = first ? textPoint(first.offset) : objectPoint(target);
  page = Math.floor((target.getBoundingClientRect().left - margin + 0.5) / options.width);
}
if (restored) {
  page = restored.local === null
    ? Math.floor((restored.node.getBoundingClientRect().left - margin + 0.5) / options.width)
    : pageAt(restored.node, restored.local);
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
let point = restored || fragmentPoint || (start < end ? textPoint(start) : null);
if (!point && objects.length) point = objectPoint(elementAt(objects[0].path));
if (!point) point = objectPoint(body);
function locatorAt(point) {
  let quoteStart = Math.max(0, point.offset - 32), quoteEnd = Math.min(text.length, point.offset + 32);
  if (/[\uDC00-\uDFFF]/u.test(text[quoteStart] || '')) ++quoteStart;
  if (/[\uDC00-\uDFFF]/u.test(text[quoteEnd] || '')) --quoteEnd;
  return { version: 1, cfi: `epubcfi(${options.cfiBase}!${cfiPath(point.node, point.local)})`,
    offset: point.offset,
    before: point.local === null ? '' : text.slice(quoteStart, point.offset),
    after: point.local === null ? '' : text.slice(point.offset, quoteEnd) };
}
const locator = locatorAt(point);
// Queries retain this layout's source map in the isolated world. They never
// change selection, styles, scrolling or the logical reading anchor.
let visible = null, searchIndex = null;
function visibleText() {
  if (visible) return visible;
  if (text.length > 2000000) throw new Error('query-text-too-large');
  const parts = [], starts = [], ends = [];
  let previousBlock = null, lineBreak = false;
  const sourceNodes = new Map(nodes.map(entry => [entry.node, entry]));
  const walker = document.createTreeWalker(body, NodeFilter.SHOW_ELEMENT |
    NodeFilter.SHOW_TEXT | NodeFilter.SHOW_CDATA_SECTION);
  for (let node; (node = walker.nextNode());) {
    if (node.nodeType === Node.ELEMENT_NODE) {
      if (node.localName === 'br' && getComputedStyle(node).display !== 'none' &&
          Array.from(node.getClientRects()).some(r => r.height > 0)) lineBreak = true;
      continue;
    }
    const entry = sourceNodes.get(node);
    if (!entry) continue;
    const range = document.createRange();
    range.selectNodeContents(entry.node);
    if (!Array.from(range.getClientRects()).some(r => r.width > 0 && r.height > 0) ||
        getComputedStyle(entry.node.parentElement).visibility !== 'visible') continue;
    let block = entry.node.parentElement;
    while (block !== body && /^(inline|contents)$/u.test(getComputedStyle(block).display))
      block = block.parentElement;
    if (previousBlock && (previousBlock !== block || lineBreak)) {
      parts.push('\n');
      starts.push(entry.offset);
      ends.push(entry.offset);
    }
    previousBlock = block;
    lineBreak = false;
    parts.push(entry.node.data);
    for (let i = 0; i < entry.node.length; ++i) {
      starts.push(entry.offset + i);
      ends.push(entry.offset + i + 1);
    }
  }
  return visible = { text: parts.join(''), starts, ends };
}
function cleanText(value) {
  return value.replace(/[\u00ad\u200b\u2060\ufeff]/gu, '').replace(/\s+/gu, ' ');
}
function rangeResult(first, last) {
  if (!first || !last || first.local === null || last.local === null) return null;
  if (first.offset > last.offset) [first, last] = [last, first];
  if (first.offset === last.offset) return null;
  const rectangles = [];
  for (const entry of nodes) {
    const begin = Math.max(0, first.offset - entry.offset);
    const finish = Math.min(entry.node.length, last.offset - entry.offset);
    if (finish <= begin || getComputedStyle(entry.node.parentElement).visibility !== 'visible') continue;
    const range = document.createRange();
    range.setStart(entry.node, begin);
    range.setEnd(entry.node, finish);
    for (const rect of range.getClientRects()) {
      const x = Math.max(margin, rect.left), y = Math.max(margin, rect.top);
      const right = Math.min(options.width - margin, rect.right);
      const bottom = Math.min(options.height - margin, rect.bottom);
      if (right > x && bottom > y) {
        if (rectangles.length >= 512) throw new Error('query-range-too-large');
        rectangles.push({ x, y, width: right - x, height: bottom - y });
      }
    }
  }
  const stream = visibleText();
  let begin = stream.ends.findIndex(end => end > first.offset);
  let finish = begin;
  while (finish >= 0 && finish < stream.text.length && stream.starts[finish] < last.offset) ++finish;
  const selected = begin < 0 ? '' : cleanText(stream.text.slice(begin, finish)).trim();
  if (selected.length > 8192) throw new Error('query-range-too-large');
  return { start: locatorAt(first), end: locatorAt(last), text: selected, rectangles };
}
function fold(value) {
  return value.normalize('NFKC').toLowerCase().replace(/[\u00df]/gu, 'ss')
    .replace(/\u03c2/gu, '\u03c3').replace(/\p{Cf}/gu, '')
    .replace(/[\u2018\u2019\u02bc]/gu, "'").replace(/[\u2010-\u2015\u2212]/gu, '-')
    .replace(/\s+/gu, ' ');
}
function sourceIndex(values, target) {
  let low = 0, high = values.length;
  while (low < high) {
    const middle = (low + high) >>> 1;
    if (values[middle] < target) low = middle + 1;
    else high = middle;
  }
  return low;
}
function search(query, requestedPage) {
  if (typeof query !== 'string' || !query.trim() || query.length > 256 ||
      !Number.isInteger(requestedPage) || requestedPage < 0 || requestedPage >= pages)
    throw new Error('invalid-query');
  const stream = visibleText();
  if (!searchIndex) {
    const normalized = [], starts = [], ends = [];
    for (const part of new Intl.Segmenter(undefined, { granularity: 'grapheme' }).segment(stream.text)) {
      for (const char of fold(part.segment)) {
        const first = stream.starts[part.index], last = stream.ends[part.index + part.segment.length - 1];
        if (char === ' ' && normalized.at(-1) === ' ') { ends[ends.length - 1] = last; continue; }
        normalized.push(char);
        for (let i = 0; i < char.length; ++i) { starts.push(first); ends.push(last); }
      }
    }
    searchIndex = { text: normalized.join(''), starts, ends };
  }
  const { text: haystack, starts, ends } = searchIndex, needle = fold(query.trim());
  const hits = [];
  let limited = false, previous = -1;
  if (!needle) return { hits, limited };
  for (let at = haystack.indexOf(needle); at >= 0; at = haystack.indexOf(needle, at + needle.length)) {
    const begin = starts[at], end = ends[at + needle.length - 1];
    if (begin === previous) continue; // A compatibility expansion can match twice.
    previous = begin;
    const first = textPoint(begin), last = textPoint(end);
    const matchPage = Math.max(0, Math.min(pages - 1, pageAt(first.node, first.local) + page));
    if (matchPage !== requestedPage) continue;
    if (hits.length === 100) { limited = true; break; }
    const rawBegin = sourceIndex(stream.starts, begin);
    let rawEnd = rawBegin;
    while (rawEnd < stream.ends.length && stream.ends[rawEnd] <= end) ++rawEnd;
    hits.push({ start: locatorAt(first), end: locatorAt(last), page: matchPage,
      before: cleanText(stream.text.slice(Math.max(0, rawBegin - 48), rawBegin)),
      text: cleanText(stream.text.slice(rawBegin, rawEnd)).trim(),
      after: cleanText(stream.text.slice(rawEnd, rawEnd + 96)) });
  }
  return { hits, limited };
}
globalThis.kobaltQuery = function(query) {
  try {
    let result;
    if (!query || typeof query !== 'object') throw new Error('invalid-query');
    if (query.kind === 'search') result = search(query.text, query.page);
    else if (query.kind === 'range')
      result = rangeResult(resolvePosition(query.start), resolvePosition(query.end));
    else if (query.kind === 'word') {
      const { x, y } = query;
      if (!Number.isFinite(x) || !Number.isFinite(y) || x < margin || y < margin ||
          x >= options.width - margin || y >= options.height - margin)
        throw new Error('invalid-query-point');
      const caret = document.caretRangeFromPoint(x, y);
      const entry = caret && nodes.find(entry => entry.node === caret.startContainer);
      result = null;
      if (entry) {
        const stream = visibleText(), offset = entry.offset + caret.startOffset;
        const index = stream.ends.findIndex(end => end > offset);
        const words = new Intl.Segmenter(undefined, { granularity: 'word' }).segment(stream.text);
        for (const at of [index < 0 ? stream.text.length - 1 : index, (index < 0 ? stream.text.length : index) - 1]) {
          const word = at >= 0 ? words.containing(at) : null;
          if (!word?.isWordLike) continue;
          const candidate = rangeResult(textPoint(stream.starts[word.index]),
            textPoint(stream.ends[word.index + word.segment.length - 1]));
          if (candidate?.rectangles.some(r => x >= r.x && x <= r.x + r.width && y >= r.y && y <= r.y + r.height)) {
            result = candidate;
            break;
          }
        }
      }
    } else throw new Error('invalid-query');
    return { source: document.URL, result };
  } catch (error) { return { source: document.URL, error: String(error.message) }; }
};
const excerpt = text.slice(start, start + 160);
return { source: document.URL, title: document.title, page, pages,
  locator,
  location: { version: 1, unit: 'utf16-text', start, end, element: objects[0]?.path || null },
  viewport: { width: window.innerWidth, height: window.innerHeight, scale: window.devicePixelRatio },
  content: { x: margin, y: margin, width: options.width - 2 * margin,
    height: options.height - 2 * margin }, links, objects, excerpt };
