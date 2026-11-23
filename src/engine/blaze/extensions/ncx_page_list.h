#pragma once

#include "lvtinydom.h"
#include "lvxml.h"

#include <memory>
#include <vector>

namespace blaze {
class NcxPageListWriter : public ldomDocumentWriter
{
public:
    using ldomDocumentWriter::ldomDocumentWriter;

    ldomNode *OnTagOpen(const lChar32 *ns, const lChar32 *name) override
    {
        if (!skipped.empty()) {
            skipped.push_back(_document->getElementNameIndex(name));
            return nullptr;
        }
        auto *node = ldomDocumentWriter::OnTagOpen(ns, name);
        if (node && !lStr_cmp(name, U"navMap")) {
            auto *parent = node->getParentNode();
            if (parent && parent->getNodeName() == U"ncx"
                && parent->getParentNode() == _document->getRootNode())
                skipped.push_back(node->getNodeId());
        }
        return node;
    }

    void OnAttribute(const lChar32 *ns, const lChar32 *name, const lChar32 *value) override
    {
        if (skipped.size() <= 1)
            ldomDocumentWriter::OnAttribute(ns, name, value);
    }

    void OnTagBody() override
    {
        if (skipped.size() <= 1)
            ldomDocumentWriter::OnTagBody();
    }

    void OnText(const lChar32 *text, int len, lUInt32 flags) override
    {
        if (skipped.empty())
            ldomDocumentWriter::OnText(text, len, flags);
    }

    void OnTagClose(const lChar32 *ns, const lChar32 *name, bool selfClosing) override
    {
        if (!skipped.empty()) {
            // The DOM writer can recover mismatched tags by popping intervening
            // elements. Let it rebuild the full document if this happens here.
            if (skipped.back() != _document->getElementNameIndex(name)) {
                _parser->Stop();
                return;
            }
            skipped.pop_back();
            if (!skipped.empty())
                return;
        }
        ldomDocumentWriter::OnTagClose(ns, name, selfClosing);
    }

    bool needsFullParse() const { return !skipped.empty(); }

private:
    std::vector<lUInt16> skipped;
};

inline ldomDocument *parseNcxPageList(LVStreamRef stream)
{
    if (ldomDocument::koblazeFreeDocumentSlots() <= 0)
        return nullptr;
    {
        std::unique_ptr<ldomDocument> doc(new ldomDocument());
        doc->setDocFlags(0);
        NcxPageListWriter writer(doc.get());
        LVXMLParser parser(stream, &writer);
        if (!parser.CheckFormat())
            return nullptr;
        const bool parsed = parser.Parse();
        if (!writer.needsFullParse())
            return parsed ? doc.release() : nullptr;
    }
    return LVParseXMLStream(stream);
}
} // namespace blaze
