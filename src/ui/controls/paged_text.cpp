#include "ui/style/fonts.h"
#include "paged_text.h"
#include "ui/style/metrics.h"
#include "ui/style/theme.h"
#include <QPainter>
#include <QEvent>

namespace kobalt::ui {
PagedText::PagedText(QWidget *parent) : QWidget(parent)
{
    const int margin = metrics::acknowledgments::textMargin;
    margins = QMargins(margin, margin, margin, margin);
    setAttribute(Qt::WA_OpaquePaintEvent);
    QFont font = kobalt::ui::interfaceFont();
    font.setPixelSize(metrics::text::body);
    document.setFont(font);
}

void PagedText::clear()
{
    source.clear();
    document.clear();
    current = 0;
    stale = true;
    update();
}

void PagedText::setHtml(const QString &html)
{
    source = html;
    document.setHtml(html);
    current = 0;
    stale = true;
    update();
}

void PagedText::setPlainText(const QString &text)
{
    source.clear();
    document.setPlainText(text);
    current = 0;
    stale = true;
    update();
}

void PagedText::setTextFont(const QFont &font)
{
    document.setFont(font);
    stale = true;
    update();
}

void PagedText::setMinimumLineHeight(int percent)
{
    if (minimumLineHeight == percent)
        return;
    minimumLineHeight = percent;
    stale = true;
    update();
}

void PagedText::changeEvent(QEvent *event)
{
    // Rebuild inherited font attributes after interface settings change.
    if (event->type() == QEvent::FontChange) {
        document.setFont(renewedFont(document.font()));
        if (!source.isEmpty())
            document.setHtml(source);
        stale = true;
        update();
    }
    QWidget::changeEvent(event);
}

void PagedText::setMargins(const QMargins &value)
{
    if (margins == value)
        return;
    margins = value;
    stale = true;
    update();
}

int PagedText::pages() const
{
    ensureLayout();
    return document.pages();
}

void PagedText::setPage(int number)
{
    // A stale layout clamps the page when it is next laid out.
    current = qMax(0, number);
    if (!stale && layoutSize == textSize(size()))
        current = qMin(current, document.pages() - 1);
    update();
}

void PagedText::fitWithPager(const QRect &available, int pagerHeight)
{
    fitGeometry(available, pagerHeight);
}

void PagedText::fitToContent(const QRect &available)
{
    fitGeometry(available, 0);
}

QSize PagedText::textSize(QSize size) const
{
    return { qMax(1, size.width() - margins.left() - margins.right()),
             qMax(1, size.height() - margins.top() - margins.bottom()) };
}

void PagedText::fitGeometry(const QRect &available, int pagerHeight)
{
    ensurePolished();
    if (stale || fitSize != available.size() || fitPagerHeight != pagerHeight) {
        const auto full = textSize(available.size());
        fitHeight = available.height();
        if (pagerHeight > 0) {
            const int reduced = qMax(1, fitHeight - pagerHeight);
            if (document.layoutWithPager(full.width(), full.height(),
                                         textSize({ available.width(), reduced }).height(),
                                         minimumLineHeight))
                fitHeight = reduced;
        } else {
            document.layout(full.width(), full.height(), minimumLineHeight);
            // All laid-out lines fit above this new bottom, so they keep their positions.
            if (document.pages() == 1)
                fitHeight = qMin(fitHeight, document.height() + margins.top() + margins.bottom());
        }
        fitSize = available.size();
        fitPagerHeight = pagerHeight;
        layoutSize = textSize({ available.width(), fitHeight });
        stale = false;
        current = qBound(0, current, document.pages() - 1);
    }
    setGeometry(QRect(available.topLeft(), QSize(available.width(), fitHeight)));
}

void PagedText::ensureLayout() const
{
    const auto wanted = textSize(size());
    if (!stale && layoutSize == wanted)
        return;
    stale = false;
    fitSize = {};
    layoutSize = wanted;
    document.layout(wanted.width(), wanted.height(), minimumLineHeight);
    current = qBound(0, current, document.pages() - 1);
}

void PagedText::paintEvent(QPaintEvent *)
{
    ensureLayout();
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    const int pageHeight = qMax(1, height() - margins.top() - margins.bottom());
    const int start = current * pageHeight;
    painter.translate(margins.left(), margins.top() - start);
    document.draw(painter,
                  QRectF(0, start, qMax(1, width() - margins.left() - margins.right()), pageHeight),
                  palette());
}
} // namespace kobalt::ui
