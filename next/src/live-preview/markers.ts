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
  type DecorationSet,
} from "@codemirror/view";
import { tableRanges } from "./table";

interface DecorationEntry {
  from: number;
  to: number;
  decoration: Decoration;
}

export function liveMarkers(): Extension {
  return StateField.define<DecorationSet>({
    create: build,
    update(decorations, transaction) {
      if (transaction.docChanged || transaction.selection !== undefined) {
        return build(transaction.state);
      }
      return decorations;
    },
    provide: (field) => EditorView.decorations.from(field),
  });
}

function build(state: EditorState): DecorationSet {
  const selection = state.selection.main;
  const cursorLine = state.doc.lineAt(selection.head).number;
  const tables = tableRanges(state);
  const entries: DecorationEntry[] = [];
  const lineClasses = new Map<number, Set<string>>();

  const inRenderedTable = (position: number) =>
    tables.some((range) => position >= range.from && position <= range.to);

  const addLine = (position: number, className: string) => {
    const line = state.doc.lineAt(position);
    const classes = lineClasses.get(line.from) ?? new Set<string>();
    classes.add(className);
    lineClasses.set(line.from, classes);
  };

  const selectionInside = (from: number, to: number) =>
    selection.from >= from && selection.to <= to;

  const hide = (from: number, to: number) => {
    if (from < to) {
      entries.push({ from, to, decoration: Decoration.replace({}) });
    }
  };

  syntaxTree(state).iterate({
    enter(node) {
      if (node.name === "Table") {
        if (inRenderedTable(node.from)) return false;
      }
      if (inRenderedTable(node.from)) return;
      if (node.name === "Image") return false;

      if (/^ATXHeading[1-6]$/.test(node.name) || node.name === "SetextHeading1" || node.name === "SetextHeading2") {
        const level = node.name.endsWith("1") ? 1 : node.name.endsWith("2") ? 2 : Number(node.name.at(-1));
        addLine(node.from, `cm-heading cm-h${level}`);
        return;
      }

      if (node.name === "HeaderMark") {
        const member = state.doc.lineAt(node.from).number;
        if (cursorLine !== member) hide(node.from, node.to);
        return;
      }

      if (
        node.name === "EmphasisMark" ||
        node.name === "StrongEmphasisMark" ||
        node.name === "StrikethroughMark"
      ) {
        const parent = node.node.parent;
        if (!parent || !selectionInside(parent.from, parent.to)) hide(node.from, node.to);
        if (node.name === "StrongEmphasisMark" || node.name === "EmphasisMark") {
          const parentNode = node.node.parent;
          if (parentNode) {
            entries.push({
              from: parentNode.from,
              to: parentNode.to,
              decoration: Decoration.mark({
                class: node.name === "StrongEmphasisMark" ? "cm-strong" : "cm-em",
              }),
            });
          }
        }
        return;
      }

      if (node.name === "InlineCode") {
        entries.push({
          from: node.from,
          to: node.to,
          decoration: Decoration.mark({ class: "cm-inline-code" }),
        });
        return;
      }

      if (node.name === "CodeMark") {
        const parent = node.node.parent;
        if (parent && parent.name === "InlineCode") {
          if (!selectionInside(parent.from, parent.to)) hide(node.from, node.to);
        } else if (parent && parent.name === "FencedCode") {
          if (!selectionInside(parent.from, parent.to)) hide(node.from, node.to);
        }
        return;
      }

      if (node.name === "CodeInfo") {
        const parent = node.node.parent;
        if (parent && parent.name === "FencedCode" && !selectionInside(parent.from, parent.to)) {
          hide(node.from, node.to);
        }
        return;
      }

      if (node.name === "FencedCode") {
        const start = state.doc.lineAt(node.from).number;
        const end = state.doc.lineAt(node.to).number;
        for (let line = start; line <= end; line += 1) {
          addLine(state.doc.line(line).from, "cm-code-block");
        }
        return;
      }

      if (node.name === "Blockquote") {
        const start = state.doc.lineAt(node.from).number;
        const end = state.doc.lineAt(node.to).number;
        for (let line = start; line <= end; line += 1) {
          addLine(state.doc.line(line).from, "cm-quote");
        }
        if (!selectionInside(node.from, node.to)) {
          let child = node.node.firstChild;
          while (child) {
            if (child.name === "QuoteMark") hide(child.from, child.to);
            child = child.nextSibling;
          }
        }
        return;
      }

      if (node.name === "QuoteMark") {
        const member = state.doc.lineAt(node.from).number;
        if (cursorLine !== member) hide(node.from, node.to);
        return;
      }

      if (node.name === "Link") {
        entries.push({
          from: node.from,
          to: node.to,
          decoration: Decoration.mark({ class: "cm-link" }),
        });
        if (!selectionInside(node.from, node.to)) {
          let child = node.node.firstChild;
          while (child) {
            if (child.name === "LinkMark" || child.name === "URL") {
              hide(child.from, child.to);
            }
            child = child.nextSibling;
          }
        }
        return false;
      }

      if (node.name === "LinkMark" || node.name === "URL") {
        const parent = node.node.parent;
        if (parent && parent.name === "Link" && !selectionInside(parent.from, parent.to)) {
          hide(node.from, node.to);
        }
        return;
      }

      if (node.name === "Strikethrough") {
        entries.push({
          from: node.from,
          to: node.to,
          decoration: Decoration.mark({ class: "cm-strikethrough" }),
        });
        return;
      }

      if (node.name === "TaskMarker") {
        const member = state.doc.lineAt(node.from).number;
        addLine(node.from, "cm-task-line");
        if (cursorLine !== member) {
          // The task widget replaces the marker; leave it to that extension.
          return;
        }
      }

      if (node.name === "ListMark") {
        addLine(node.from, "cm-list-line");
        return;
      }

      if (node.name === "HorizontalRule") {
        addLine(node.from, "cm-rule");
        hide(node.from, node.to);
      }
    },
  });

  for (const [from, classes] of [...lineClasses.entries()].sort((a, b) => a[0] - b[0])) {
    entries.push({
      from,
      to: from,
      decoration: Decoration.line({ class: [...classes].join(" ") }),
    });
  }

  entries.sort((a, b) => a.from - b.from || a.to - b.to);
  const builder = new RangeSetBuilder<Decoration>();
  for (const entry of entries) {
    if (entry.from <= entry.to) {
      builder.add(entry.from, entry.to, entry.decoration);
    }
  }
  return builder.finish();
}
