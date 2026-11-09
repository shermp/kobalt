#pragma once
#include "publication/opening_result.h"
#include "reading/chapter.h"
#include "assets/font_catalog.h"
#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QPair>
#include <QRect>
#include <QSet>
#include "publication/resource_cache.h"
#include "platform/api/position_codec.h"
#include "engine/crengine/reader_links.h"
#include "engine/crengine/reader_word_counts.h"
#include "reading/reading_density.h"
#include "reading/appearance_settings.h"
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>
#include <functional>
#include <utility>
#include <tuple>

namespace kobalt {

namespace nickel {
class NickelPositionCodec;
}

// Bundled defaults for generic families.
inline constexpr auto publisherSansFontFace = "NV Legible Next";
// Bundled face for the generic monospace family.
inline constexpr auto monospaceFontFace = "IBM Plex Mono";
// Bundled face for MathML and the generic math family.
inline constexpr auto mathFontFace = "Noto Sans Math";
// Bundled faces, in order, for characters that the reading font lacks.
// Text faces precede Math so its letter glyphs cannot interrupt script shaping.
// Math precedes CJK so shared mathematical symbols keep their math metrics.
// The CJK face has full coverage and localized glyphs selected by HarfBuzz
// from the book or element language. Untagged text uses its Japanese forms.
inline constexpr auto fallbackFontFaces =
        "Noto Serif|Noto Naskh Arabic|Noto Serif Hebrew|Noto Serif Armenian|Noto Serif Georgian|Noto Serif Devanagari|Noto Serif Bengali|Noto Serif Tamil|Noto Serif Thai|Noto Sans Math|Noto Serif CJK JP";
enum class OpeningStage { Content, Text, Styles, Pages, Ready };
using OpeningProgress = std::function<void(OpeningStage, int)>;

struct ChapterProgress
{
    QString title;
    int page = 0, pages = 0;
};

struct SearchHit
{
    QString start, end, before, text, after;
    int page = -1;
};

struct SearchBatch
{
    QVector<SearchHit> hits;
    int nextPage = 0;
    bool done = false, limited = false;
    // A restarted scan replaces results from the previous layout.
    bool reset = false;
};

struct TextRange
{
    QString start, end, text;
    QString chosenEnd = {};
    QString chosenStart = {};
    bool sentenceStartPolicy = false;
    bool sentenceEndPolicy = false;
};

struct WordSelection
{
    QString text, start, end;
    QVector<QRect> rectangles;
};

struct SelectionEndpoint
{
    // True terminal edge span in viewport pixels, never a clipped range edge.
    QPoint first, last;
    // Axis unit normal pointing away from selected text. Caps point inward.
    QPoint outward;
    bool visible = false; // False for endpoints outside the current page/viewport.
};

struct AnnotationGeometry
{
    QVector<QRect> rectangles;
    SelectionEndpoint start, end;
    bool vertical = false; // Current page's writing axis, independent of range shape.
};

struct AnnotationToken : WordSelection
{
    // Source boundaries for automatic paired punctuation. No whitespace is included.
    QString startBeforePunctuation, endAfterPunctuation;
    bool punctuation = false;
};

struct AnnotationPageEdges
{
    AnnotationToken first, last;
    SelectionEndpoint start, end;
    int columns = 1;
    bool vertical = false;
    bool continuesBefore = false, continuesAfter = false;
};

} // namespace kobalt
