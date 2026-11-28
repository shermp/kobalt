// Minimal DOM surface for deterministic Unicode boundary assertions. The
// retained-view fixture separately exercises WebKit's real Range geometry.
const Node = { TEXT_NODE: 3, ELEMENT_NODE: 1 };
const NodeFilter = { SHOW_TEXT: 4 };
function element(parent) {
    const node = { nodeType: 1, parentElement: parent, children: [], text: [] };
    if (parent)
        parent.children.push(node);
    return node;
}
function text(parent, data) {
    const node = { nodeType: 3, parentElement: parent, data, length: data.length };
    parent.text.push(node);
    return node;
}
function texts(node) {
    return [...node.text, ...node.children.flatMap(texts)];
}
const body = element(null);
const document = {
    body,
    documentElement: body,
    createTreeWalker(root) {
        const items = texts(root);
        let i = 0;
        return {
            nextNode: () => items[i++],
            lastChild: () => items[i = items.length - 1],
            previousNode: () => items[--i]
        };
    }
};
function check(condition, description) {
    if (!condition)
        throw new Error(description);
}
function same(actual, expected, description) {
    check(JSON.stringify(actual) === JSON.stringify(expected), description);
}

const chapter = element(body);
const intro = text(chapter, "A😀 \t\u00ad\u200b");
const inline = element(chapter);
const rest = text(inline, "\u2060\ufeffe\u0301א");
const empty = element(body);
same(sourcePosition(intro, 3), [[0], 2, false, 0], "surrogate pair counts as one");
same(sourcePosition(intro, 2), [[0], 1, true, 1], "split surrogate is approximate");
same(sourcePosition(intro, 4), [[0], 2, true, 1], "inside collapsed whitespace is approximate");
same(sourcePosition(rest, 2), [[0, 0], 0, true, 0], "element-only inline path");
same(sourcePosition(rest, 4, chapter), [[0], 4, true, 0], "combining mark and RTL count separately");
check(resolveSource([0], 2, false)[0] === intro && resolveSource([0], 2, false)[1] === 3,
      "before gap uses the end of the surrogate pair");
check(resolveSource([0], 2, true)[0] === rest && resolveSource([0], 2, true)[1] === 2,
      "after gap skips across the text node");
check(resolveSource([0], 30, true)[2] === 1, "offset beyond the element is approximate");
check(resolveSource([1], 1, true)[0] === empty && resolveSource([1], 1, true)[2] === 1,
      "an empty element falls back to the element");
check(resolveSource([2], 0, true) === null, "a missing element path is unresolved");
check(resolveSource([], -1, true)[0] === body, "empty path names the body");
check(!uncounted('\u00a0') && !uncounted('\u0301') && uncounted('\r') && uncounted('\f'),
      "Blaze uncounted set differs");
let direction = 'ltr';
function getComputedStyle() { return { direction }; }
const inside = { width: 0.25, height: 1, left: 0, right: 0.25, top: 0, bottom: 1 };
const farLeft = -16777216;
// The right edge of the single-precision union rounds to zero even though
// the thin rectangle has positive area within the viewport.
const rounded = { width: -farLeft, height: 1, left: farLeft, right: 0, top: 0, bottom: 1 };
const rightOfPage = { width: 1, height: 1, left: 100, right: 101, top: 0, bottom: 1 };
function trailingBatch(bounds, rects, expectedNodes, expectedQueries) {
    const work = [0, 0, 0];
    const range = {
        setStart() {},
        setEnd() {},
        getBoundingClientRect() {
            check(bounds !== null, 'RTL uses its original rectangle check');
            return bounds;
        },
        getClientRects() {
            check(rects !== null, 'offscreen minimum edge should avoid the rectangle list');
            return rects;
        }
    };
    const nodes = [...visibleSourceNodes(range, 100, 100, true, work, true)];
    check(nodes.length === expectedNodes && (!nodes.length || (nodes[0] === rest && nodes[1] === intro)),
          'batch filtering must preserve trailing source order');
    same(work, [2, 0, expectedQueries], 'geometry counters include an ambiguous bounds check');
}
trailingBatch(rightOfPage, null, 0, 1);
trailingBatch(rounded, [inside], 2, 2);
direction = 'rtl';
trailingBatch(null, [inside], 2, 1);
true;
