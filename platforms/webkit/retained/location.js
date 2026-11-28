// DOM-only helpers, evaluated in the isolated renderer script world.
// Keep this list identical to Blaze locationUncounted().
const uncounted = character => /[ \t\n\r\f\u00ad\u200b\u2060\ufeff]/u.test(character);
const maxPath = 128;
const maxText = 2 * 1024 * 1024;

function elementPath(element) {
    const path = [];
    while (element && element !== document.body) {
        const parent = element.parentElement;
        if (!parent || path.length === maxPath)
            return null;
        const index = Array.prototype.indexOf.call(parent.children, element);
        if (index < 0)
            return null;
        path.push(index);
        element = parent;
    }
    return element ? path.reverse() : null;
}

function sourceText(element) {
    return document.createTreeWalker(element, NodeFilter.SHOW_TEXT);
}

function sourcePosition(node, offset, element = node.parentElement) {
    const path = elementPath(element);
    if (!path || node.nodeType !== Node.TEXT_NODE || offset < 0 || offset > node.data.length)
        return null;
    const walker = sourceText(element);
    let count = 0, units = 0;
    for (let text; (text = walker.nextNode());) {
        const found = text === node;
        const end = found ? offset : text.length;
        units += end;
        if (units > maxText)
            return null;
        let index = 0;
        for (const character of text.data) {
            if (index >= end)
                break;
            index += character.length;
            if (index > end)
                return [path, count, true, 1]; // UTF-16 offset splits a surrogate pair.
            if (!uncounted(character))
                ++count;
        }
        if (found) {
            const next = offset < text.length && !uncounted(String.fromCodePoint(text.data.codePointAt(offset)));
            const previous = offset > 0 && !uncounted(text.data[offset - 1]);
            return [path, count, next || !previous, next || previous ? 0 : 1];
        }
    }
    return null;
}

function resolveSource(path, offset, after) {
    let element = document.body;
    if (!element || path.length > maxPath)
        return null;
    for (const index of path) {
        if (!Number.isInteger(index) || index < 0 || index >= element.children.length)
            return null;
        element = element.children[index];
    }
    if (offset < 0)
        return [element, 0, 0];
    const walker = sourceText(element);
    const target = after ? offset : offset - 1;
    let count = 0, units = 0, last;
    for (let text; (text = walker.nextNode());) {
        last = text;
        units += text.length;
        if (units > maxText)
            return [element, 0, 1];
        if (target < 0)
            return [text, 0, 0];
        let index = 0;
        for (const character of text.data) {
            if (!uncounted(character) && count++ === target)
                return [text, after ? index : index + character.length, 0];
            index += character.length;
        }
    }
    return last ? [last, last.length, offset === count ? 0 : 1] : [element, 0, 1];
}

function positionRect(node, offset, after) {
    if (node.nodeType === Node.ELEMENT_NODE)
        return node.getClientRects()[0];
    // Use the adjacent counted glyph when possible. A collapsed DOM range
    // at a line break may otherwise report the opposite side of the gap.
    const forward = (after && offset < node.length) || !offset;
    let start = forward ? offset : offset - 1;
    const unit = node.data.charCodeAt(start);
    if (!forward && unit >= 0xdc00 && unit <= 0xdfff && start > 0)
        --start;
    const range = document.createRange();
    range.setStart(node, start);
    range.setEnd(node, Math.min(node.length,
        start + (node.data.codePointAt(start) > 0xffff ? 2 : 1)));
    return range.getClientRects()[0];
}

function rangeIntersectsViewport(range, width, height) {
    for (const rect of range.getClientRects()) {
        if (rect.width > 0 && rect.height > 0 && rect.right > 0 && rect.left < width
            && rect.bottom > 0 && rect.top < height)
            return true;
    }
    return false;
}

// WebKit includes each text node's client rects in the enclosing range,
// even when it also reports an element's border box. Extra element rects may
// keep a batch in the scan. They cannot cause us to skip visible text.
function* visibleSourceNodes(range, width, height, trailing, work, batchNodes) {
    const walker = sourceText(document.body);
    const batch = [];
    const tryBounds = batchNodes && trailing && getComputedStyle(document.documentElement).direction !== 'rtl';
    const batchVisible = () => {
        if (batch.length === 1)
            return true;
        const first = trailing ? batch[batch.length - 1] : batch[0];
        const last = trailing ? batch[0] : batch[batch.length - 1];
        range.setStart(first, 0);
        range.setEnd(last, last.length);
        if (tryBounds) {
            if (work) ++work[2];
            const rect = range.getBoundingClientRect();
            // Only the minimum edges can safely reject a batch. WebKit's
            // float union can round its far edges inward on very wide ranges.
            if (rect.left >= width || rect.top >= height)
                return false;
            if (rect.width > 0 && rect.height > 0 && rect.right > 0 && rect.bottom > 0)
                return true;
        }
        if (work) ++work[2];
        return rangeIntersectsViewport(range, width, height);
    };
    const batchTextLimit = 4096;
    let visited = 0, units = 0, batchUnits = 0;
    for (let node = trailing ? walker.lastChild() : walker.nextNode(); node;
         node = trailing ? walker.previousNode() : walker.nextNode()) {
        units += node.length;
        if (++visited > 65536 || units > maxText)
            break;
        if (work) ++work[0];
        if (!node.length)
            continue;
        if (!batchNodes) {
            yield node;
            continue;
        }
        if (batch.length && (batch.length === 32 || batchUnits + node.length > batchTextLimit)) {
            if (batchVisible())
                yield* batch;
            batch.length = 0;
            batchUnits = 0;
        }
        if (node.length > batchTextLimit) {
            yield node;
            continue;
        }
        batch.push(node);
        batchUnits += node.length;
    }
    if (batch.length && batchVisible())
        yield* batch;
}

function visibleSource(width, height, trailing = false, work = null, batchNodes = false) {
    // Find the first visible text range in source order. Hit testing alone
    // can return a caret from an adjacent column in a fragmented paragraph.
    const range = document.createRange();
    const visible = () => {
        if (work) ++work[1];
        return rangeIntersectsViewport(range, width, height);
    };
    for (const node of visibleSourceNodes(range, width, height, trailing, work, batchNodes)) {
        range.selectNodeContents(node);
        if (!visible())
            continue;
        // The prefix begins intersecting the viewport at the first visible
        // character. Search in UTF-16 and round back from any split pair.
        let offset;
        if (trailing) {
            let lo = 0, hi = node.length - 1;
            while (lo < hi) {
                const mid = Math.ceil((lo + hi) / 2);
                range.setStart(node, mid);
                if (visible())
                    lo = mid;
                else
                    hi = mid - 1;
            }
            const low = node.data.charCodeAt(lo);
            if (lo > 0 && low >= 0xdc00 && low <= 0xdfff)
                --lo;
            offset = lo + (node.data.codePointAt(lo) > 0xffff ? 2 : 1);
        } else {
            let lo = 1, hi = node.length;
            while (lo < hi) {
                const mid = Math.floor((lo + hi) / 2);
                range.setEnd(node, mid);
                if (visible())
                    hi = mid;
                else
                    lo = mid + 1;
            }
            offset = lo - 1;
            const unit = node.data.charCodeAt(offset);
            if (offset > 0 && unit >= 0xdc00 && unit <= 0xdfff)
                --offset;
        }
        const position = sourcePosition(node, offset);
        if (position)
            return position;
    }
    if (trailing)
        return null;
    const elements = document.createTreeWalker(document.body, NodeFilter.SHOW_ELEMENT);
    for (let node, checked = 0; checked < 65536 && (node = elements.nextNode()); ++checked) {
        if (!['IMG', 'SVG', 'VIDEO', 'CANVAS', 'HR'].includes(node.tagName.toUpperCase()))
            continue;
        const rect = node.getClientRects()[0];
        if (rect && rect.width > 0 && rect.height > 0 && rect.right > 0 && rect.left < width
            && rect.bottom > 0 && rect.top < height) {
            const path = elementPath(node);
            if (path)
                return [path, -1, true, 0];
        }
    }
    return [[], -1, true, 1];
}

function visibleLinks(width, height, work = null, deferResolution = false) {
    const links = [];
    const anchors = document.getElementsByTagName('a');
    const source = new URL(document.URL);
    source.hash = '';
    const epub = 'http://www.idpf.org/2007/ops';
    const tokens = (element, name) => (element.getAttribute(name) || '').split(/\s+/);
    const epubTypes = element => (element.getAttributeNS(epub, 'type') || '').split(/\s+/);
    const sameBookUrl = value => {
        try {
            const uri = new URL(value, document.URL);
            if (uri.protocol === 'kobalt-epub:' && uri.host === source.host && !uri.search
                && !uri.username && !uri.password && uri.href.length <= 1800)
                return uri;
        } catch {
            // An invalid URL cannot identify a link.
        }
        return null;
    };
    const isNote = (element, uri) => {
        // A cross-chapter reference must identify itself. A same-chapter
        // reference can also identify the note through its destination.
        let note = uri.hash && (epubTypes(element).includes('noteref')
            || tokens(element, 'role').includes('doc-noteref')
            || tokens(element, 'rel').includes('footnote'));
        if (!note && uri.hash && uri.href.slice(0, -uri.hash.length) === source.href) {
            try {
                const target = document.getElementById(decodeURIComponent(uri.hash.slice(1)));
                note = target && (epubTypes(target).some(type => ['footnote', 'endnote'].includes(type))
                    || tokens(target, 'role').some(role => ['doc-footnote', 'doc-endnote'].includes(role)));
            } catch {
                // An invalid escape cannot identify an element.
            }
        }
        return Boolean(note);
    };
    for (let i = 0; i < Math.min(anchors.length, 65536) && links.length < 24; ++i) {
        if (work) ++work[0];
        const element = anchors[i];
        const value = element.getAttribute('href');
        if (!value || value.length > 1600)
            continue;
        let uri = deferResolution ? undefined : sameBookUrl(value);
        if (!deferResolution && !uri)
            continue;
        let note = uri ? isNote(element, uri) : undefined;
        if (work) ++work[1];
        for (const rect of element.getClientRects()) {
            const x = Math.max(0, Math.floor(rect.left));
            const y = Math.max(0, Math.floor(rect.top));
            const right = Math.min(width, Math.ceil(rect.right));
            const bottom = Math.min(height, Math.ceil(rect.bottom));
            if (right > x && bottom > y) {
                uri ??= sameBookUrl(value);
                if (!uri)
                    break;
                note ??= isNote(element, uri);
                links.push([uri.href, x, y, right - x, bottom - y, note]);
            }
            if (links.length === 24)
                break;
        }
    }
    return links;
}

function wordAtPoint(x, y) {
    const caret = document.caretRangeFromPoint(x, y);
    const node = caret && caret.startContainer;
    if (!node || node.nodeType !== Node.TEXT_NODE || !document.body.contains(node)
        || node.length > maxText || !Intl.Segmenter)
        return null;
    const segments = new Intl.Segmenter(undefined, {granularity: 'word'}).segment(node.data);
    // WebKit can place the caret on either edge of the glyph under the
    // pointer. Only accept a candidate that actually covers the hit.
    for (const offset of [caret.startOffset, caret.startOffset - 1]) {
        const word = segments.containing(offset);
        if (!word || !word.isWordLike || word.segment.length > 256)
            continue;
        const range = document.createRange();
        range.setStart(node, word.index);
        range.setEnd(node, word.index + word.segment.length);
        const rects = Array.from(range.getClientRects());
        if (!rects.some(r => r.left <= x && x < r.right && r.top <= y && y < r.bottom))
            continue;
        const start = sourcePosition(node, range.startOffset);
        const end = sourcePosition(node, range.endOffset);
        if (!start || !end || start[1] >= end[1])
            return null;
        const bounds = range.getBoundingClientRect();
        const left = Math.max(0, Math.floor(bounds.left));
        const top = Math.max(0, Math.floor(bounds.top));
        const right = Math.min(innerWidth, Math.ceil(bounds.right));
        const bottom = Math.min(innerHeight, Math.ceil(bounds.bottom));
        return [word.segment, start, end, left, top, right - left, bottom - top];
    }
    return null;
}

function rangeRects(first, last) {
    const resolve = value => resolveSource(
        value[0] ? value[0].split(',').map(Number) : [], value[1], Boolean(value[2]));
    const start = resolve(first), end = resolve(last);
    if (!start || start[2] || !end || end[2])
        return null;
    const range = document.createRange();
    range.setStart(start[0], start[1]);
    range.setEnd(end[0], end[1]);
    if (range.collapsed)
        return null;
    const visible = [];
    for (const rect of range.getClientRects()) {
        const x = Math.max(0, Math.floor(rect.left));
        const y = Math.max(0, Math.floor(rect.top));
        const right = Math.min(innerWidth, Math.ceil(rect.right));
        const bottom = Math.min(innerHeight, Math.ceil(rect.bottom));
        if (right > x && bottom > y)
            visible.push([x, y, right - x, bottom - y]);
        if (visible.length === 64)
            break;
    }
    const endpoint = (position, leading) => {
        const [node, offset] = position;
        if (node.nodeType !== Node.TEXT_NODE)
            return [];
        const rect = positionRect(node, offset, leading);
        if (!rect)
            return [];
        const style = getComputedStyle(node.parentElement);
        const vertical = style.writingMode.startsWith('vertical');
        const backwards = leading !== (style.direction === 'rtl');
        const fixed = Math.round(vertical
            ? (backwards ? rect.top : rect.bottom) : (backwards ? rect.left : rect.right));
        const first = Math.round(vertical ? rect.left : rect.top);
        const last = Math.round(vertical ? rect.right : rect.bottom);
        if (fixed < 0 || fixed > (vertical ? innerHeight : innerWidth) || first < 0
            || last > (vertical ? innerWidth : innerHeight) || first >= last)
            return [];
        return [fixed, first, last, (vertical ? 3 : 1) + Number(!backwards)];
    };
    return [visible, endpoint(start, true), endpoint(end, false)];
}
