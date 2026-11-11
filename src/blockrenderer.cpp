#include "blockrenderer.h"

#include "markdownhighlighter.h"

#include <QFontMetricsF>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>

namespace {

int leadingIndentation(const QString &text) {
    int columns = 0;
    for (const QChar character : text) {
        if (character == QLatin1Char(' '))
            ++columns;
        else if (character == QLatin1Char('\t'))
            columns = (columns / 4 + 1) * 4;
        else
            break;
    }
    return columns;
}

// True when the line has a pipe that is not backslash-escaped and is not
// inside an inline code span. A one-column table still writes its pipe. This
// scans without building cells because every prose line goes through it.
bool hasUnescapedPipe(const QString &text) {
    bool inCode = false;
    int codeTicks = 0;
    for (int i = 0; i < text.size();) {
        const QChar character = text.at(i);
        if (character == QLatin1Char('\\') && !inCode) {
            int backslashes = 0;
            while (i < text.size() && text.at(i) == QLatin1Char('\\')) {
                ++backslashes;
                ++i;
            }
            if (i < text.size() && text.at(i) == QLatin1Char('|')) {
                if (backslashes % 2 == 0)
                    return true;
                ++i;
            }
            continue;
        }
        if (character == QLatin1Char('`')) {
            int ticks = 0;
            while (i < text.size() && text.at(i) == QLatin1Char('`')) {
                ++ticks;
                ++i;
            }
            if (!inCode) {
                inCode = true;
                codeTicks = ticks;
            } else if (ticks == codeTicks) {
                inCode = false;
                codeTicks = 0;
            }
            continue;
        }
        if (character == QLatin1Char('|') && !inCode)
            return true;
        ++i;
    }
    return false;
}

// The cells of one pipe-table row. A leading and a trailing pipe are optional
// and only delimit the row, while interior empty cells stay. Backslash-escaped
// pipes are literal text, and a pipe inside `code` (however many backticks
// delimit it) does not split.
QStringList splitRow(const QString &line) {
    const QString text = line.trimmed();
    QStringList cells;
    QString cell;
    bool inCode = false;
    int codeTicks = 0;
    bool split = false;
    for (int i = 0; i < text.size();) {
        const QChar character = text.at(i);

        if (character == QLatin1Char('\\') && !inCode) {
            int backslashes = 0;
            while (i < text.size() && text.at(i) == QLatin1Char('\\')) {
                ++backslashes;
                ++i;
            }
            if (i < text.size() && text.at(i) == QLatin1Char('|')) {
                if (backslashes % 2 != 0) {
                    cell.append(QString(backslashes / 2, QLatin1Char('\\')));
                    cell.append(QLatin1Char('|'));
                    ++i;
                } else {
                    cell.append(QString(backslashes, QLatin1Char('\\')));
                }
            } else {
                cell.append(QString(backslashes, QLatin1Char('\\')));
            }
            continue;
        }

        if (character == QLatin1Char('`')) {
            int ticks = 0;
            while (i < text.size() && text.at(i) == QLatin1Char('`')) {
                ++ticks;
                ++i;
            }
            if (!inCode) {
                inCode = true;
                codeTicks = ticks;
            } else if (ticks == codeTicks) {
                inCode = false;
                codeTicks = 0;
            }
            cell.append(QString(ticks, QLatin1Char('`')));
            continue;
        }

        if (character == QLatin1Char('|') && !inCode) {
            cells.append(cell.trimmed());
            cell.clear();
            split = true;
            ++i;
            continue;
        }

        cell.append(character);
        ++i;
    }
    cells.append(cell.trimmed());

    if (split && !cells.isEmpty() && cells.first().isEmpty())
        cells.removeFirst();
    if (split && !cells.isEmpty() && cells.last().isEmpty())
        cells.removeLast();
    return cells;
}

// GFM's delimiter row: every cell is dashes with optional alignment colons,
// and it names one column per header cell.
bool delimiterAlignments(const QString &line, int columns,
                         QList<BlockRenderer::Alignment> *alignments) {
    if (leadingIndentation(line) > 3 || !hasUnescapedPipe(line))
        return false;

    static const QRegularExpression cellRe(QStringLiteral("^:?-+:?$"));
    const QStringList cells = splitRow(line);
    if (cells.isEmpty() || cells.size() != columns)
        return false;

    QList<BlockRenderer::Alignment> parsed;
    for (const QString &cell : cells) {
        const QString text = cell.trimmed();
        if (!cellRe.match(text).hasMatch())
            return false;
        const bool left = text.startsWith(QLatin1Char(':'));
        const bool right = text.endsWith(QLatin1Char(':'));
        parsed.append(left && right ? BlockRenderer::Alignment::Center
                    : right         ? BlockRenderer::Alignment::Right
                                    : BlockRenderer::Alignment::Left);
    }
    if (alignments)
        *alignments = parsed;
    return true;
}

// A body row ends the table when the line starts another block-level thing.
// Rows normally start with a pipe, so this only matters for a row without one.
bool startsAnotherBlock(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || MarkdownHighlighter::openingFenceLength(text) > 0
            || MarkdownHighlighter::isThematicBreak(text))
        return true;
    if (trimmed.startsWith(QLatin1Char('#')) || trimmed.startsWith(QLatin1Char('>')))
        return true;
    static const QRegularExpression listRe(
        QStringLiteral("^\\s*(?:[-+*]|\\d+[.)])\\s+"));
    return listRe.match(text).hasMatch();
}

// One wrapped line's height at the given width, using the same layout engine
// the editor uses, so a wrapped cell measures where QML draws it.
qreal wrappedHeight(const QString &text, const QFont &font, qreal width) {
    const QFontMetricsF metrics(font);
    if (text.isEmpty())
        return metrics.height();

    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    qreal height = 0;
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        line.setPosition(QPointF(0, height));
        height += line.height();
    }
    layout.endLayout();
    return height > 0 ? height : metrics.height();
}

void measureTable(BlockRenderer::Table &table, int areaWidth, const QFont &font) {
    if (!table.isValid() || table.rows.isEmpty() || table.alignments.isEmpty())
        return;

    const int columns = table.alignments.size();
    QFont headerFont = font;
    headerFont.setBold(true);
    const QFontMetricsF regularMetrics(font);
    const QFontMetricsF headerMetrics(headerFont);
    const qreal padding = qMax(6.0, regularMetrics.height() * 0.35);
    table.cellPadding = qRound(padding);
    const qreal minColumn = qMax(padding * 2 + 12, 40.0);

    // A column asks for its widest cell. The table then fills the writing
    // column, sharing the room past each column's minimum by that weight, so
    // a wide column gets the extra space and a narrow one stays readable.
    QList<qreal> natural(columns, padding * 2);
    for (int row = 0; row < table.rows.size(); ++row) {
        const QFontMetricsF &metrics = row == 0 ? headerMetrics : regularMetrics;
        for (int column = 0; column < columns; ++column) {
            natural[column] = qMax(natural[column],
                                   metrics.horizontalAdvance(table.rows.at(row).value(column))
                                       + padding * 2);
        }
    }

    qreal totalNatural = 0;
    for (const qreal width : natural)
        totalNatural += width;
    const qreal available = areaWidth > 0 ? areaWidth : 600.0;
    QList<qreal> widths(columns);
    if (available <= minColumn * columns) {
        for (int column = 0; column < columns; ++column)
            widths[column] = available / columns;
    } else {
        const qreal spare = available - minColumn * columns;
        for (int column = 0; column < columns; ++column)
            widths[column] = minColumn + spare * natural.at(column) / totalNatural;
    }

    int used = 0;
    for (int column = 0; column < columns; ++column) {
        const int width = column == columns - 1
            ? qMax(1, qRound(available) - used)
            : qMax(1, qRound(widths.at(column)));
        used += width;
        table.columnWidths.append(width);
    }

    const qreal verticalPadding = qMax(3.0, regularMetrics.height() * 0.25);
    for (int row = 0; row < table.rows.size(); ++row) {
        const QFont &cellFont = row == 0 ? headerFont : font;
        qreal tallest = regularMetrics.height();
        for (int column = 0; column < columns; ++column) {
            const qreal contentWidth =
                qMax(1.0, table.columnWidths.at(column) - padding * 2);
            tallest = qMax(tallest, wrappedHeight(table.rows.at(row).value(column),
                                                  cellFont, contentWidth));
        }
        const int height = qMax(1, qRound(tallest + verticalPadding * 2));
        table.rowHeights.append(height);
        table.height += height;
    }
}

// Parses the table whose header is this block, if the block below is its
// delimiter row. Geometry is filled in only when an area width is known.
BlockRenderer::Table parseTableAtHeader(const QTextBlock &header, int areaWidth,
                                        const QFont &font) {
    BlockRenderer::Table table;
    if (!header.isValid() || leadingIndentation(header.text()) > 3
            || !hasUnescapedPipe(header.text())) {
        return table;
    }

    const QStringList headerCells = splitRow(header.text());
    if (headerCells.isEmpty())
        return table;

    const QTextBlock delimiter = header.next();
    QList<BlockRenderer::Alignment> alignments;
    if (!delimiter.isValid()
            || !delimiterAlignments(delimiter.text(), headerCells.size(), &alignments)) {
        return table;
    }

    table.rows.append(headerCells);
    QTextBlock last = delimiter;
    for (QTextBlock body = delimiter.next(); body.isValid(); body = body.next()) {
        const QString text = body.text();
        if (text.trimmed().isEmpty() || leadingIndentation(text) > 3
                || !hasUnescapedPipe(text) || startsAnotherBlock(text)) {
            break;
        }
        QStringList cells = splitRow(text);
        while (cells.size() < headerCells.size())
            cells.append(QString());
        if (cells.size() > headerCells.size())
            cells = cells.mid(0, headerCells.size());
        table.rows.append(cells);
        last = body;
    }

    table.firstBlock = header.blockNumber();
    table.lastBlock = last.blockNumber();
    table.blockStart = header.position();
    table.blockEnd = last.position() + last.text().length();
    table.alignments = alignments;
    measureTable(table, areaWidth, font);
    return table;
}

} // namespace

QList<BlockRenderer::Table> BlockRenderer::tables(const QTextDocument *document,
                                                  int areaWidth, const QFont &font) {
    QList<Table> parsed;
    if (!document)
        return parsed;

    bool inFence = false;
    int fenceLength = 0;
    QChar fenceMarker = QLatin1Char('`');
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        const QString text = block.text();
        if (inFence) {
            if (MarkdownHighlighter::closesFence(text, fenceLength, fenceMarker))
                inFence = false;
            continue;
        }
        const int opening = MarkdownHighlighter::openingFenceLength(text);
        if (opening > 0) {
            inFence = true;
            fenceLength = opening;
            fenceMarker = text.trimmed().startsWith(QLatin1Char('~'))
                ? QLatin1Char('~') : QLatin1Char('`');
            continue;
        }

        Table table = parseTableAtHeader(block, areaWidth, font);
        if (!table.isValid())
            continue;
        parsed.append(table);
        // Continue after the rows just consumed rather than testing each body
        // row as a possible header of another table.
        block = document->findBlockByNumber(table.lastBlock);
    }
    return parsed;
}

BlockRenderer::Table BlockRenderer::tableAtBlock(const QTextDocument *document,
                                                 const QTextBlock &block,
                                                 int areaWidth, const QFont &font) {
    if (!document || !block.isValid()
            || block.userState() >= MarkdownHighlighter::InsideFence
            || block.previous().userState() >= MarkdownHighlighter::InsideFence) {
        return {};
    }

    // The block may be the header, or the delimiter whose header is above it.
    for (int back = 0; back <= 1; ++back) {
        const QTextBlock header = back == 0 ? block : block.previous();
        if (!header.isValid())
            continue;
        Table table = parseTableAtHeader(header, areaWidth, font);
        if (table.isValid() && table.containsBlock(block.blockNumber()))
            return table;
    }

    // A body row: walk up to the header and delimiter pair. A blank line or a
    // fence ends the search because a table cannot cross either.
    for (QTextBlock candidate = block.previous(); candidate.isValid();
         candidate = candidate.previous()) {
        const QString text = candidate.text();
        if (text.trimmed().isEmpty() || MarkdownHighlighter::openingFenceLength(text) > 0)
            break;
        if (!hasUnescapedPipe(text))
            break;
        if (!candidate.previous().isValid())
            break;
        Table table = parseTableAtHeader(candidate.previous(), areaWidth, font);
        if (table.isValid() && table.containsBlock(block.blockNumber()))
            return table;
    }
    return {};
}

QString BlockRenderer::alignmentName(Alignment alignment) {
    switch (alignment) {
    case Alignment::Center:
        return QStringLiteral("center");
    case Alignment::Right:
        return QStringLiteral("right");
    case Alignment::Left:
        break;
    }
    return QStringLiteral("left");
}
