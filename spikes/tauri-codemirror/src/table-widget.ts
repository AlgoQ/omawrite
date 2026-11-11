import { StateField, type EditorState, type Extension } from "@codemirror/state";
import {
  Decoration,
  EditorView,
  WidgetType,
  type DecorationSet,
} from "@codemirror/view";

type Alignment = "left" | "center" | "right" | null;

interface ParsedTable {
  from: number;
  to: number;
  header: string[];
  rows: string[][];
  alignments: Alignment[];
}

/**
 * Split one GFM table row into cells.
 *
 * This deliberately does not handle escaped pipes (`\|`) or pipes inside code
 * spans. The spike proves the replace-with-widget seam; a production parser
 * should read the table node out of the Lezer syntax tree instead.
 */
function splitRow(text: string): string[] {
  let value = text.trim();
  if (value.startsWith("|")) value = value.slice(1);
  if (value.endsWith("|")) value = value.slice(0, -1);
  return value.split("|").map((cell) => cell.trim());
}

function isSeparator(text: string): boolean {
  const cells = splitRow(text);
  return cells.length > 0 && cells.every((cell) => /^:?-{3,}:?$/.test(cell));
}

function alignmentsFor(text: string): Alignment[] {
  return splitRow(text).map((cell) => {
    const left = cell.startsWith(":");
    const right = cell.endsWith(":");
    if (left && right) return "center";
    if (right) return "right";
    if (left) return "left";
    return null;
  });
}

/**
 * Find GFM tables by scanning lines. A production implementation would walk
 * the Lezer tree and only look at visible ranges; this is O(document) per
 * update, which is fine for a spike-sized note and honest about the shortcut.
 */
function findTables(state: EditorState): ParsedTable[] {
  const tables: ParsedTable[] = [];
  const doc = state.doc;
  let lineNumber = 1;

  while (lineNumber <= doc.lines) {
    const line = doc.line(lineNumber);
    const separator = lineNumber < doc.lines ? doc.line(lineNumber + 1) : null;

    if (
      separator &&
      line.text.includes("|") &&
      isSeparator(separator.text)
    ) {
      const header = splitRow(line.text);
      const alignments = alignmentsFor(separator.text);

      if (header.length === alignments.length) {
        const rows: string[][] = [];
        let endLine = lineNumber + 1;
        let nextLine = lineNumber + 2;

        while (nextLine <= doc.lines) {
          const rowLine = doc.line(nextLine);
          if (rowLine.text.trim() === "" || !rowLine.text.includes("|")) break;
          if (isSeparator(rowLine.text)) break;

          const cells = splitRow(rowLine.text);
          rows.push(
            Array.from({ length: header.length }, (_, index) => cells[index] ?? ""),
          );
          endLine = nextLine;
          nextLine += 1;
        }

        tables.push({
          from: line.from,
          to: doc.line(endLine).to,
          header,
          rows,
          alignments,
        });

        lineNumber = endLine + 1;
        continue;
      }
    }

    lineNumber += 1;
  }

  return tables;
}

function selectionTouches(
  state: EditorState,
  from: number,
  to: number,
): boolean {
  return state.selection.ranges.some(
    (range) => range.from <= to && range.to >= from,
  );
}

class TableWidget extends WidgetType {
  readonly table: ParsedTable;

  constructor(table: ParsedTable) {
    super();
    this.table = table;
  }

  eq(other: TableWidget): boolean {
    return (
      this.table.from === other.table.from &&
      this.table.to === other.table.to &&
      JSON.stringify(this.table.header) === JSON.stringify(other.table.header) &&
      JSON.stringify(this.table.rows) === JSON.stringify(other.table.rows) &&
      JSON.stringify(this.table.alignments) ===
        JSON.stringify(other.table.alignments)
    );
  }

  toDOM(view: EditorView): HTMLElement {
    const wrapper = document.createElement("div");
    wrapper.className = "cm-table-widget";

    const table = document.createElement("table");
    const head = table.createTHead();
    const headRow = head.insertRow();

    this.table.header.forEach((cell, index) => {
      const th = document.createElement("th");
      th.textContent = cell;
      const alignment = this.table.alignments[index];
      if (alignment) th.style.textAlign = alignment;
      headRow.appendChild(th);
    });

    const body = table.createTBody();
    for (const row of this.table.rows) {
      const bodyRow = body.insertRow();
      row.forEach((cell, index) => {
        const td = bodyRow.insertCell();
        td.textContent = cell;
        const alignment = this.table.alignments[index];
        if (alignment) td.style.textAlign = alignment;
      });
    }

    wrapper.appendChild(table);

    // A click puts the caret back into the table, which makes the state field
    // stop replacing it on the next update and reveals the raw Markdown.
    wrapper.addEventListener("mousedown", (event) => {
      event.preventDefault();
      view.dispatch({
        selection: { anchor: this.table.from },
        scrollIntoView: true,
      });
      view.focus();
    });

    return wrapper;
  }

  ignoreEvent(): boolean {
    return true;
  }
}

function buildDecorations(state: EditorState): DecorationSet {
  const ranges = findTables(state)
    .filter((table) => !selectionTouches(state, table.from, table.to))
    .map((table) =>
      Decoration.replace({
        block: true,
        widget: new TableWidget(table),
      }).range(table.from, table.to),
    );

  return Decoration.set(ranges, true);
}

const tableDecorations = StateField.define<DecorationSet>({
  create(state) {
    return buildDecorations(state);
  },
  update(value, transaction) {
    if (transaction.docChanged || transaction.selection) {
      return buildDecorations(transaction.state);
    }
    return value;
  },
  provide: (field) => EditorView.decorations.from(field),
});

const tableTheme = EditorView.baseTheme({
  ".cm-table-widget": {
    padding: "10px 0 16px",
    overflowX: "auto",
  },
  ".cm-table-widget table": {
    borderCollapse: "collapse",
    fontFamily: '"IBM Plex Mono", "JetBrains Mono", monospace',
    fontSize: "0.9em",
    minWidth: "70%",
  },
  ".cm-table-widget th, .cm-table-widget td": {
    border: "1px solid rgba(128, 128, 128, 0.45)",
    padding: "6px 10px",
    verticalAlign: "top",
  },
  ".cm-table-widget th": {
    background: "rgba(128, 128, 128, 0.12)",
    fontWeight: "600",
  },
});

export function tableWidgetExtension(): Extension {
  return [tableDecorations, tableTheme];
}
