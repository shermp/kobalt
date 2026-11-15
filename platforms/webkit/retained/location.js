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

function visibleSource(width, height) {
    // Find the first visible text range in source order. Hit testing alone
    // can return a caret from an adjacent column in a fragmented paragraph.
    const walker = sourceText(document.body);
    const range = document.createRange();
    const visible = () => Array.from(range.getClientRects()).some(rect =>
        rect.width > 0 && rect.height > 0 && rect.right > 0 && rect.left < width
        && rect.bottom > 0 && rect.top < height);
    let visited = 0, units = 0;
    for (let node; (node = walker.nextNode());) {
        units += node.length;
        if (++visited > 65536 || units > maxText)
            break;
        if (!node.length)
            continue;
        range.selectNodeContents(node);
        if (!visible())
            continue;
        // The prefix begins intersecting the viewport at the first visible
        // character. Search in UTF-16 and round back from any split pair.
        let lo = 1, hi = node.length;
        while (lo < hi) {
            const mid = Math.floor((lo + hi) / 2);
            range.setEnd(node, mid);
            if (visible())
                hi = mid;
            else
                lo = mid + 1;
        }
        let offset = lo - 1;
        const unit = node.data.charCodeAt(offset);
        if (offset > 0 && unit >= 0xdc00 && unit <= 0xdfff)
            --offset;
        const position = sourcePosition(node, offset);
        if (position)
            return position;
    }
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

function visibleLinks(width, height) {
    const links = [];
    const anchors = document.getElementsByTagName('a');
    for (let i = 0; i < Math.min(anchors.length, 65536) && links.length < 24; ++i) {
        const element = anchors[i];
        const value = element.getAttribute('href');
        if (!value || value.length > 1600)
            continue;
        let uri;
        try {
            uri = new URL(value, document.URL);
        } catch {
            continue;
        }
        const source = new URL(document.URL);
        if (uri.protocol !== 'kobalt-epub:' || uri.host !== source.host || uri.search
            || uri.username || uri.password || uri.href.length > 1800)
            continue;
        for (const rect of element.getClientRects()) {
            const x = Math.max(0, Math.floor(rect.left));
            const y = Math.max(0, Math.floor(rect.top));
            const right = Math.min(width, Math.ceil(rect.right));
            const bottom = Math.min(height, Math.ceil(rect.bottom));
            if (right > x && bottom > y)
                links.push([uri.href, x, y, right - x, bottom - y]);
            if (links.length === 24)
                break;
        }
    }
    return links;
}
