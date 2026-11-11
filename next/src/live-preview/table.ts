import { syntaxTree } from "@codemirror/language";
import {
  RangeSetBuilder,
  StateField,
  type EditorState,
  type Extension,
} from "@codemirror/state";
import {
  Decoration,
  EditorView,
  WidgetType,
  type DecorationSet,
} from "@codemirror/view";
import { escapeHtml } from "../dom";

type Alignment = "left" | "center" | "right" | "none";

interface Cell {
  text: string;
  pos: number;
}

interface Table {
  raw: string;
  headers: Cell[];
  rows: Cell[][];
  alignments: Alignment[];
}

export function tableWidget(): Extension {
  return StateField.define<DecorationSet>({
    create: buildTableDecorations,
    update(decorations, transaction) {
      if (transaction.docChanged || transaction.selection !== undefined) {
        return buildTableDecorations(transaction.state);
      }
      return decorations;
    },
    provide: (field) => EditorView.decorations.from(field),
  });
}

function buildTableDecorations(state: EditorState): DecorationSet {
  const builder = new RangeSetBuilder<Decoration>();
  const selection = state.selection.main;
  syntaxTree(state).iterate({
    enter(node) {
      if (node.name !== "Table") return;
      const startLine = state.doc.lineAt(node.from);
      const endLine = state.doc.lineAt(node.to);
      const from = startLine.from;
      const to = endLine.to;
      if (selection.from <= to && selection.to >= from) {
        return;
      }
      const raw = state.doc.sliceString(from, to);
      const table = parseTable(raw, from);
      if (!table) return;
      builder.add(
        from,
        to,
        Decoration.replace({
          widget: new TableWidget(table, from),
          block: true,
        }),
      );
    },
  });
  return builder.finish();
}

export function tableRanges(state: EditorState): Array<{ from: number; to: number }> {
  const ranges: Array<{ from: number; to: number }> = [];
  const selection = state.selection.main;
  syntaxTree(state).iterate({
    enter(node) {
      if (node.name !== "Table") return;
      const startLine = state.doc.lineAt(node.from);
      const endLine = state.doc.lineAt(node.to);
      const from = startLine.from;
      const to = endLine.to;
      if (selection.from <= to && selection.to >= from) return;
      ranges.push({ from, to });
    },
  });
  return ranges;
}

export function isInRenderedTable(state: EditorState, pos: number): boolean {
  return tableRanges(state).some((range) => pos >= range.from && pos <= range.to);
}

function parseTable(raw: string, startPosition: number): Table | null {
  const lines = raw.split("\n");
  if (lines.length < 2) return null;
  const headerLine = lines[0];
  const delimiterLine = lines[1];
  const offsets: number[] = [];
  let offset = startPosition;
  for (const line of lines) {
    offsets.push(offset);
    offset += line.length + 1;
  }

  const header = splitRow(headerLine, offsets[0]);
  const delimiter = splitRow(delimiterLine, offsets[1]);
  if (!header || !delimiter) return null;
  const alignments: Alignment[] = delimiter.map((cell) => {
    const value = cell.text.trim();
    const left = value.startsWith(":");
    const right = value.endsWith(":");
    if (left && right) return "center";
    if (right) return "right";
    if (left) return "left";
    return "none";
  });
  if (!alignments.every((alignment) => alignment !== null)) return null;
  if (delimiter.some((cell) => !/^:?-{1,}:?$/.test(cell.text.trim()))) return null;

  const columns = delimiter.length;
  const headers = fitCells(header, columns);
  const rows: Cell[][] = [];
  for (let index = 2; index < lines.length; index += 1) {
    const line = lines[index];
    if (!line.trim()) continue;
    const row = splitRow(line, offsets[index]);
    if (!row) return null;
    rows.push(fitCells(row, columns));
  }
  return { raw, headers, rows, alignments };
}

function splitRow(line: string, lineStart: number): Cell[] | null {
  const cells: Cell[] = [];
  let segmentStart = 0;
  let inCode = false;
  let codeMarker = 0;
  let index = 0;
  const push = (end: number) => {
    const raw = line.slice(segmentStart, end);
    const leading = raw.length - raw.trimStart().length;
    cells.push({
      text: raw.trim(),
      pos: lineStart + segmentStart + leading,
    });
    segmentStart = end + 1;
  };

  while (index < line.length) {
    const character = line[index];
    if (character === "\\") {
      index += 2;
      continue;
    }
    if (character === "`") {
      let run = 0;
      while (line[index + run] === "`") run += 1;
      if (!inCode) {
        inCode = true;
        codeMarker = run;
      } else if (run === codeMarker) {
        inCode = false;
      }
      index += run;
      continue;
    }
    if (character === "|" && !inCode) {
      push(index);
    }
    index += 1;
  }
  push(line.length);

  if (line.trimStart().startsWith("|")) cells.shift();
  if (line.trimEnd().endsWith("|")) cells.pop();
  return cells;
}

function fitCells(cells: Cell[], columns: number): Cell[] {
  const fitted = cells.slice(0, columns);
  while (fitted.length < columns) {
    fitted.push({ text: "", pos: fitted.at(-1)?.pos ?? 0 });
  }
  return fitted;
}

class TableWidget extends WidgetType {
  readonly table: Table;
  readonly from: number;

  constructor(table: Table, from: number) {
    super();
    this.table = table;
    this.from = from;
  }

  eq(other: TableWidget): boolean {
    return other.from === this.from && other.table.raw === this.table.raw;
  }

  get estimatedHeight(): number {
    return Math.max(80, (this.table.rows.length + 1) * 34);
  }

  toDOM(view: EditorView): HTMLElement {
    const wrapper = document.createElement("div");
    wrapper.className = "cm-table-widget";
    const table = document.createElement("table");
    table.className = "cm-table";

    const head = document.createElement("thead");
    const headRow = document.createElement("tr");
    this.table.headers.forEach((cell, index) => {
      const th = document.createElement("th");
      th.innerHTML = inlineMarkdown(cell.text);
      th.dataset.pos = String(cell.pos);
      applyAlignment(th, this.table.alignments[index]);
      headRow.appendChild(th);
    });
    head.appendChild(headRow);
    table.appendChild(head);

    const body = document.createElement("tbody");
    for (const row of this.table.rows) {
      const tr = document.createElement("tr");
      row.forEach((cell, index) => {
        const td = document.createElement("td");
        td.innerHTML = inlineMarkdown(cell.text);
        td.dataset.pos = String(cell.pos);
        applyAlignment(td, this.table.alignments[index]);
        tr.appendChild(td);
      });
      body.appendChild(tr);
    }
    table.appendChild(body);
    wrapper.appendChild(table);

    table.addEventListener("click", (event) => {
      const target = event.target;
      if (!(target instanceof Element)) return;
      const wiki = target.closest<HTMLElement>(".cm-wiki-link");
      if (wiki?.dataset.target) {
        window.dispatchEvent(
          new CustomEvent("omawrite:open-wiki", {
            detail: wiki.dataset.target,
          }),
        );
        return;
      }
      const cell = target.closest<HTMLElement>("[data-pos]");
      if (!cell?.dataset.pos) return;
      const pos = Number(cell.dataset.pos);
      view.dispatch({ selection: { anchor: pos }, scrollIntoView: true });
      view.focus();
    });

    return wrapper;
  }

  ignoreEvent(): boolean {
    return false;
  }
}

function applyAlignment(cell: HTMLElement, alignment: Alignment): void {
  if (alignment !== "none") cell.style.textAlign = alignment;
}

export function inlineMarkdown(value: string): string {
  let html = escapeHtml(value);
  html = html.replace(/`([^`]+)`/g, "<code>$1</code>");
  html = html.replace(/~~([^~]+)~~/g, "<del>$1</del>");
  html = html.replace(/==([^=]+)==/g, "<mark>$1</mark>");
  html = html.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
  html = html.replace(/(^|[^*])\*([^*]+)\*/g, "$1<em>$2</em>");
  html = html.replace(
    /!?\[\[([^\]|#]+)(?:#([^\]|]+))?(?:\|([^\]]+))?\]\]/g,
    (_match, target: string, heading: string | undefined, alias: string | undefined) => {
      const label = alias ?? target;
      const headingAttribute = heading ? ` data-heading="${heading}"` : "";
      return `<span class="cm-wiki-link" data-target="${target}"${headingAttribute}>${label}</span>`;
    },
  );
  html = html.replace(
    /\[([^\]]+)\]\(([^)]+)\)/g,
    (_match, label: string, url: string) =>
      `<a href="#" data-href="${url}" class="cm-link">${label}</a>`,
  );
  html = html.replace(/(^|\s)(#[\w/-]+)/g, '$1<span class="cm-tag">$2</span>');
  return html;
}
