#include "reader_beautifier.h"
#include "stop_point.h"
#include <fb2def.h>
#include <lvtinydom.h>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <algorithm>
#include <vector>

namespace kobalt {
namespace {
QString text(const lString32 &value)
{
    return QString::fromUtf8(UnicodeToUtf8(value).c_str());
}

bool special(ldomNode *node, lUInt16 tag)
{
    static const QSet<lUInt16> tags{ el_blockquote, el_aside, el_address, el_figure, el_figcaption,
                                     el_table,      el_ul,    el_ol,      el_dl,     el_pre,
                                     el_code,       el_math,  el_ruby,    el_nav,    el_header,
                                     el_footer,     el_h1,    el_h2,      el_h3,     el_h4,
                                     el_h5,         el_h6 };
    if (tags.contains(tag))
        return true;
    static const QRegularExpression names(
            "(?:^|[\\s_-])(?:quote|quotation|blockquote|letter|signature|verse|poem|poetry|stanza|caption|epigraph|dedication|footnote|endnote|noteref|title|subtitle|heading|scene|break|separator|copyright|toc)(?:$|[\\s_-])",
            QRegularExpression::CaseInsensitiveOption);
    const auto &types = node->getAttributeValue(LXML_NS_ANY, attr_type);
    const auto matchesName = [&](const lString32 &value) {
        return !value.empty() && names.match(text(value)).hasMatch();
    };
    if (matchesName(node->getAttributeValue(attr_class)) || matchesName(types)
        || matchesName(node->getAttributeValue(attr_role)))
        return true;
    const auto style = node->getStyle();
    return !style.isNull()
            && (css_wm_is_vertical(style->writing_mode) || style->text_align == css_ta_center
                || style->text_align == css_ta_right || style->white_space == css_ws_pre
                || style->white_space == css_ws_pre_wrap);
}

bool initial(ldomNode *node, lUInt16 tag)
{
    if (tag != el_span)
        return false;
    static const QRegularExpression name("drop.?cap|initial",
                                         QRegularExpression::CaseInsensitiveOption);
    const auto style = node->getStyle();
    // Some publishers use generic names such as "big". A floated
    // single letter is also evidence of a text initial, regardless of its name.
    const bool floated = !style.isNull() && style->float_ == css_f_left;
    if (!floated) {
        const auto &classes = node->getAttributeValue(attr_class);
        if (classes.empty() || !name.match(text(classes)).hasMatch())
            return false;
    }
    const auto value = text(node->getText()).trimmed();
    // Include an opening quotation mark, but never a whole decorated word.
    if (value.isEmpty() || value.size() > 3)
        return false;
    bool letter = false;
    for (const auto c : value) {
        if (c.isLetter()) {
            if (letter)
                return false;
            letter = true;
        } else if (!c.isPunct()) {
            return false;
        }
    }
    return letter;
}
} // namespace

void markBeautifierContent(ldomDocument *document)
{
    if (!document || !document->getRootNode())
        return;
    const auto role = document->getAttrNameIndex("data-kobalt-beautify");
    const auto initialAttribute = document->getAttrNameIndex("data-kobalt-initial");
    const auto firstLetterAttribute = document->getAttrNameIndex("data-kobalt-first-letter");
    const auto engineFirstLetter = document->getAttrNameIndex("HasFirstLetter");

    const auto paragraph = document->getAttrNameIndex("data-kobalt-paragraph");

    struct Frame
    {
        ldomNode *node;
        bool blocked;
    };

    std::vector<Frame> stack{ { document->getRootNode(), false } };
    while (!stack.empty()) {
        stopPoint();
        const auto frame = stack.back();
        stack.pop_back();
        auto *node = frame.node;
        if (!node->isElement())
            continue;
        // Never trust source-supplied markers.
        if (node->hasAttribute(role))
            node->setAttributeValue(0, role, U"");
        if (node->hasAttribute(initialAttribute))
            node->setAttributeValue(0, initialAttribute, U"");
        if (node->hasAttribute(firstLetterAttribute))
            node->setAttributeValue(0, firstLetterAttribute, U"");
        // Text initials are independent of prose spacing and indentation. A
        // subtitle, leading gap or quotation must not keep a drop cap enlarged.
        const auto tag = node->getNodeId();
        if (initial(node, tag))
            node->setAttributeValue(0, initialAttribute, U"1");
        static const QRegularExpression opener("drop.?cap|stickupcap",
                                               QRegularExpression::CaseInsensitiveOption);
        bool firstLetter = node->hasAttribute(engineFirstLetter);
        if (!firstLetter && tag == el_p) {
            const auto &classes = node->getAttributeValue(attr_class);
            firstLetter = !classes.empty() && opener.match(text(classes)).hasMatch();
        }
        if (firstLetter)
            node->setAttributeValue(0, firstLetterAttribute, U"1");
        const bool blocked = frame.blocked || special(node, tag);
        for (int i = node->getChildCount() - 1; i >= 0; --i)
            stack.push_back({ node->getChildNode(i), blocked });
        if (blocked || tag != el_p || node->getAttributeValue(paragraph) != U"1")
            continue;
        const auto style = node->getStyle();
        if (style.isNull() || style->text_indent.value < 0 || style->margin[0].value
            || style->margin[1].value || style->padding[0].value || style->padding[1].value)
            continue;
        // A leading gap may be the only scene separator. Keep its paragraph
        // spacing and indentation. Zero margins are common in ordinary prose.
        if (style->margin[2].value > 0 || style->padding[2].value > 0)
            continue;
        if (text(node->getText()).trimmed().size() < 80)
            continue;
        bool safe = true;
        std::vector<ldomNode *> children, initials;
        for (int i = 0; i < node->getChildCount(); ++i)
            children.push_back(node->getChildNode(i));
        bool precedingText = false;
        // Traverse in source order, including through kepubify wrappers.
        std::reverse(children.begin(), children.end());
        while (!children.empty()) {
            auto *child = children.back();
            children.pop_back();
            if (child->isText()) {
                if (!precedingText)
                    precedingText = !text(child->getText()).trimmed().isEmpty();
                continue;
            }
            if (!child->isElement())
                continue;
            const auto childTag = child->getNodeId();
            static const QSet<lUInt16> inlineTags{ el_span, el_i,   el_em,  el_b,    el_strong,
                                                   el_a,    el_sup, el_sub, el_small };
            // CREngine boxes floats during parsing. Keep those internal nodes;
            // reflow updates their rendering when the source float is disabled.
            const bool leadingBox = !precedingText
                    && (childTag == el_floatBox || (firstLetter && childTag == el_pseudoElem));
            if ((!inlineTags.contains(childTag) && !leadingBox) || special(child, childTag)) {
                safe = false;
                break;
            }
            if (!precedingText && initial(child, childTag)) {
                initials.push_back(child);
            } else if (const auto s = child->getStyle();
                       !leadingBox && !s.isNull() && s->float_ != css_f_none) {
                safe = false;
                break;
            }
            for (int i = child->getChildCount() - 1; i >= 0; --i)
                children.push_back(child->getChildNode(i));
        }
        if (!safe)
            continue;
        node->setAttributeValue(0, role,
                                firstLetter                 ? U"first-letter"
                                        : !initials.empty() ? U"opening"
                                                            : U"body");
    }
}
} // namespace kobalt
