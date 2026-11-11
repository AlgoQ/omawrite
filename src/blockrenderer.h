#pragma once

#include <QFont>
#include <QList>
#include <QString>
#include <QStringList>

class QTextBlock;
class QTextDocument;

// The seam for blocks the editor draws as themselves instead of as their
// Markdown source. A block kind parses its source once and measures the
// geometry it will be drawn at, so the highlighter, Backend's block formats,
// and the QML overlay all agree on its range, columns and height.
//
// Tables are the first kind. Images keep their older single-line overlay in
// Backend and Main.qml; headings and task items can join by returning their
// own entries from Backend::renderedBlocks() and a delegate beside the table
// in Main.qml. Nothing outside this module should split table rows.
namespace BlockRenderer {

enum class Alignment { Left, Center, Right };

struct Table {
    int firstBlock = -1;
    int lastBlock = -1;
    // Document positions of the first character and of the caret just past
    // the last one, so the editor can place and hide the overlay.
    int blockStart = -1;
    int blockEnd = -1;
    int height = 0;
    int cellPadding = 0;
    QList<Alignment> alignments;
    // The header first, then one entry per body row. Every row has one cell
    // per column, padded or truncated to the header's width.
    QList<QStringList> rows;
    // Measured for the editor's writing column; empty until an area width is
    // given. QML reads these rather than measuring the table again.
    QList<int> columnWidths;
    QList<int> rowHeights;

    bool isValid() const { return firstBlock >= 0; }
    bool containsBlock(int blockNumber) const {
        return isValid() && blockNumber >= firstBlock && blockNumber <= lastBlock;
    }
};

// Every pipe table in the document, in order. Fenced code is skipped. With an
// area width the columns are laid out to that width and every row is measured
// with the font, which is what the editor overlay and the reserved space use.
QList<Table> tables(const QTextDocument *document, int areaWidth = 0,
                    const QFont &font = QFont());

// The table the block belongs to, if any. It looks around the block rather
// than parsing the whole document, since the highlighter asks per line.
Table tableAtBlock(const QTextDocument *document, const QTextBlock &block,
                   int areaWidth = 0, const QFont &font = QFont());

QString alignmentName(Alignment alignment);

} // namespace BlockRenderer
