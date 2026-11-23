#include "check.h"
#include "engine_fixtures.h"
#include "engine/blaze/extensions/word_cache.h"
#include "reader_inputs.h"
#include "engine/blaze/adapter/engine_document.h"
#include <QGuiApplication>
#include <QTemporaryDir>
#include <koblaze.h>
#include <lvfntman.h>
#include <lvdrawbuf.h>

using kobalt::EngineDocument;

namespace {
const QSize viewport(800, 1136);

// A hit on the oldest glyph must protect it from the next eviction.
void checkGlyphEviction()
{
    const int bytes = offsetof(LVFontGlyphCacheItem, bmp) + 1;
    LVFontGlobalGlyphCache global(2 * bytes);
    LVFontLocalGlyphCache local(&global);
    auto add = [&](lChar32 value) {
        local.put(LVFontGlyphCacheItem::newItem(&local, value, 1, 1));
    };
    add(U'A');
    add(U'a');
    require(local.getByChar(U'A'), "Cannot find the oldest glyph");
    add(U'!');
    require(local.getByChar(U'A') && !local.getByChar(U'a') && local.getByChar(U'!'),
            "A recently used glyph was evicted");
    global.clear();
    require(!local.getByChar(U'!'), "Global eviction left a local glyph");
}

void checkShortRtlRuns()
{
    require(InitFontManager(lString8()), "Cannot initialize shaping fonts");
    require(fontMan->RegisterFont(lString8(KOBALT_FONT_DIRECTORY "/NotoSerifHebrew-Regular.ttf"))
                    && fontMan->RegisterFont(
                            lString8(KOBALT_FONT_DIRECTORY "/NotoNaskhArabic-Regular.ttf")),
            "Cannot register shaping fonts");
    fontMan->SetKerningMode(KERNING_MODE_HARFBUZZ);
    for (const auto &family : { "Noto Serif Hebrew", "Noto Naskh Arabic" }) {
        auto font = fontMan->GetFont(48, 400, false, css_ff_serif, lString8(family));
        require(!font.isNull() && font->getTypeFace() == lString8(family), "Wrong shaping font");
        const auto render = [&](const lString32 &text, bool measureFirst) {
            const unsigned hints = LFNT_HINT_DIRECTION_KNOWN | LFNT_HINT_DIRECTION_IS_RTL;
            lUInt16 widths[8]{};
            lUInt8 flags[8]{};
            const auto measure = [&] {
                require(font->measureText(text.c_str(), text.length(), widths, flags, 512, U'?',
                                          nullptr, 0, false, hints)
                                == text.length(),
                        "Incomplete short run");
            };
            LVColorDrawBuf buf(256, 96, 32);
            buf.Clear(0xffffff);
            buf.SetTextColor(0);
            if (measureFirst)
                measure();
            const int advance = font->DrawTextString(&buf, 0, 0, text.c_str(), text.length(), U'?',
                                                     nullptr, false, nullptr, hints);
            if (!measureFirst)
                measure();
            QByteArray result(reinterpret_cast<const char *>(widths), sizeof(widths));
            result.append(reinterpret_cast<const char *>(flags), sizeof(flags));
            result.append(QByteArray::number(advance));
            for (int y = 0; y < buf.GetHeight(); ++y)
                result.append(reinterpret_cast<const char *>(buf.GetScanLine(y)), buf.GetRowSize());
            return result;
        };
        // The first two words collide in a 64-slot cache. The marked words can
        // produce a different number of glyphs than input codepoints.
        const lString32 runs[] = { U"אר", U"בכ", U"שָׁלוֹם", U"لَا" };
        QVector<QByteArray> expected;
        for (const auto &run : runs) {
            font->clearCache();
            expected.append(render(run, false));
        }
        require(expected[0] != expected[1] || font->getTypeFace() != lString8("Noto Serif Hebrew"),
                "Colliding Hebrew words must have different glyphs");
        for (int i = 0; i < expected.size(); ++i) {
            require(render(runs[i], true) == expected[i] && render(runs[i], false) == expected[i],
                    "Reused RTL shaping changed the glyphs or their widths");
        }
    }
    ShutdownFontManager();
}

void checkWordCache()
{
    blaze::WordCache cache;
    const auto english = hb_language_from_string("en", -1);
    const auto turkish = hb_language_from_string("tr", -1);
    cache.selectLanguage(english);
    const auto first = cache.findOrAdd(U"ink", 3, 42);
    const auto collision = cache.findOrAdd(U"pen", 3, 42);
    require(first.inserted && collision.inserted && first.offset != collision.offset,
            "Different words with the same hash must have separate records");
    cache.record(first.offset)[blaze::word::KW_LAST_ADVANCE] = 17;

    // Grow past the initial table's load limit, then revisit both collided words.
    for (unsigned int i = 0; i < 2100; ++i) {
        const lChar32 letter = 0x3000 + i;
        cache.findOrAdd(&letter, 1, i);
    }
    for (unsigned int i = 0; i < 2100; ++i) {
        const lChar32 letter = 0x3000 + i;
        require(!cache.findOrAdd(&letter, 1, i).inserted,
                "Growing the table lost the word that triggered the rehash");
    }
    cache.selectLanguage(turkish);
    const auto other = cache.findOrAdd(U"ink", 3, 42);
    require(other.inserted, "A new language must shape the word for itself");
    cache.record(other.offset)[blaze::word::KW_LAST_ADVANCE] = 23;
    cache.selectLanguage(english);
    const auto ink = cache.findOrAdd(U"ink", 3, 42);
    const auto pen = cache.findOrAdd(U"pen", 3, 42);
    require(!ink.inserted && !pen.inserted && ink.offset == first.offset
                    && pen.offset == collision.offset
                    && cache.record(ink.offset)[blaze::word::KW_LAST_ADVANCE] == 17,
            "Changing language and growing the table must preserve earlier word metrics");

    cache.glyphs().char_glyph.resize(4096);
    cache.glyphs().pair_kern.reserve(4096);
    cache.clear();
    require(cache.glyphs().char_glyph.capacity() == 0
                    && cache.glyphs().pair_kern.bucket_count()
                            == blaze::WordCache::Glyphs{}.pair_kern.bucket_count(),
            "Clearing a face retained its glyph storage");

    for (const auto language : { english, turkish }) {
        cache.selectLanguage(language);
        require(cache.findOrAdd(U"ink", 3, 42).inserted,
                "Clearing the face must discard records in every language");
    }
    // A language that reaches the word limit starts a fresh cache on the next run.
    for (unsigned i = 0; i <= 50000; ++i) {
        const lChar32 letter = 0x10000 + i;
        cache.findOrAdd(&letter, 1, i);
    }
    cache.glyphs().char_advance.resize(4096);
    cache.selectLanguage(turkish);
    require(cache.language() == turkish && cache.glyphs().char_advance.capacity() == 0
                    && cache.findOrAdd(U"ink", 3, 42).inserted,
            "The word limit did not release storage and restart the selected language");
}

void open(EngineDocument &model, const QByteArray &body, const QString &face)
{
    auto files = kobalt::test::engineBook(false, 1, 1);
    files["OEBPS/c1.xhtml"] = kobalt::test::publisherHtml(body);
    QMap<QString, qint64> sizes;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        sizes[it.key()] = it.value().size();
    auto resources = std::make_shared<kobalt::ResourceCache>(
            files.keys(),
            [files](const QString &name, const kobalt::ResourceCache::Sink &sink, QString *) {
                const auto bytes = files.value(name);
                return sink(bytes.constData(), bytes.size());
            },
            std::make_shared<std::atomic_bool>(false));
    QString error;
    require(model.openResources(resources, sizes, viewport, { { "font_face", face } }, &error),
            qPrintable(error));
}

void open(EngineDocument &model, int characters, const QString &face)
{
    const QByteArray prose("The reader crossed the quiet room and opened the book. ");
    QByteArray body;
    int remaining = characters;
    while (remaining > 0) {
        const auto text = prose.left(qMin(remaining, prose.size()));
        body += "<p>" + text + "</p>";
        remaining -= text.size();
    }
    open(model, body, face);
}

void checkChoice(EngineDocument &model, bool expected)
{
    require(model.resourceStats().value("fast_shaping").toBool() == expected,
            "Document selected the wrong shaping path");
    model.releaseImages();
    require(!model.image(0).isNull(), "Cannot draw the shaping fixture");
    require(koblazeWordWidths() == expected && koblazePairKerning() == expected
                    && koblazeWordPairs() == expected && koblazePlainJoiner() == expected,
            "Another document changed the active shaping path");
}

void compare(EngineDocument &automatic, EngineDocument &reference)
{
    require(automatic.pages() == reference.pages(), "Conditional shaping changed page count");
    for (int page : { 0, automatic.pages() / 2, automatic.pages() - 1 }) {
        require(automatic.pageStart(page) == reference.pageStart(page),
                "Conditional shaping changed page boundaries");
        require(automatic.image(page) == reference.image(page),
                "Conditional shaping changed page pixels");
    }
}

void checkExtendedLatin()
{
    // Warm the word cache past its initial shaping checks before introducing ḷ.
    // The old glyph index for ḷ was also the index for w.
    QByteArray body("<p>w a ");
    for (int word = 0; word < 853; ++word) {
        int digits = word;
        for (int i = 0; i < 5; ++i) {
            body += "abcd"[digits % 4];
            digits /= 4;
        }
        body += ' ';
    }
    body += "</p>";
    body += ("<p>" + QByteArray(u8"ḷ ").repeated(71) + "</p>").repeated(95);
    EngineDocument fast(kobalt::test::readerFonts()), normal(kobalt::test::readerFonts());
    qputenv("KOBALT_FAST_SHAPING", "1");
    open(fast, body, "Libron");
    qputenv("KOBALT_FAST_SHAPING", "0");
    open(normal, body, "Libron");
    qunsetenv("KOBALT_FAST_SHAPING");
    compare(fast, normal);
}
} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    checkGlyphEviction();
    checkShortRtlRuns();
    checkWordCache();
    checkExtendedLatin();
    for (const auto &face : { QStringLiteral("Libron"), kobalt::test::alternateReadingFace(),
                              kobalt::test::headingReadingFace() }) {
        for (int characters : { 8000, 249999, 250000 }) {
            const bool fast = characters >= 250000;
            qunsetenv("KOBALT_FAST_SHAPING");
            EngineDocument automatic(kobalt::test::readerFonts());
            open(automatic, characters, face);
            checkChoice(automatic, fast);
            qputenv("KOBALT_FAST_SHAPING", fast ? "1" : "0");
            EngineDocument reference(kobalt::test::readerFonts());
            open(reference, characters, face);
            qunsetenv("KOBALT_FAST_SHAPING");
            compare(automatic, reference);
            if (fast) {
                qputenv("KOBALT_FAST_SHAPING", "0");
                EngineDocument normal(kobalt::test::readerFonts());
                open(normal, characters, face);
                qunsetenv("KOBALT_FAST_SHAPING");
                compare(automatic, normal);
            }
            for (auto *model : { &automatic, &reference })
                require(model->applyAppearance(
                                kobalt::appearance::Patch(QJsonObject{ { "font_pixels", 52 } }),
                                viewport),
                        "Cannot reflow shaping fixture");
            checkChoice(automatic, fast);
            compare(automatic, reference);
        }
    }
    qunsetenv("KOBALT_FAST_SHAPING");
    EngineDocument large(kobalt::test::readerFonts()), small(kobalt::test::readerFonts());
    open(large, 300000, "Libron");
    open(small, 8000, "Libron");
    checkChoice(large, true);
    checkChoice(small, false);
    checkChoice(large, true);
    large.setChapterSlices(2);
    QString error;
    require(large.showSpineItems(0, 0, &error), qPrintable(error));
    checkChoice(large, false);
    QTemporaryDir cache;
    require(cache.isValid(), "Cannot prepare shaping cache");
    const auto prepare = [&](EngineDocument &model) {
        model.setDocumentCache(cache.path(), QByteArray(64, 'a'), "fast-shaping", 1024 * 1024);
        open(model, 300000, "Libron");
    };
    {
        EngineDocument cached(kobalt::test::readerFonts());
        prepare(cached);
        require(cached.saveDocumentCache(), "Cannot save shaping cache");
    }
    {
        EngineDocument restored(kobalt::test::readerFonts());
        prepare(restored);
        require(restored.openedFromCache(), "Shaping count discarded the parsed cache");
        require(restored.resourceStats().value("shaping_text_characters").toInt() == 250000,
                "Cached shaping count was lost");
        checkChoice(restored, true);
    }
    return 0;
}
