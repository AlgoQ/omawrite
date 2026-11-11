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
import { tableRanges } from "./table";

export function taskCheckboxes(): Extension {
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

interface Entry {
  from: number;
  to: number;
  decoration: Decoration;
}

function build(state: EditorState): DecorationSet {
  const entries: Entry[] = [];
  const lineClasses = new Map<number, string>();
  const tables = tableRanges(state);
  syntaxTree(state).iterate({
    enter(node) {
      if (node.name !== "TaskMarker") return;
      if (tables.some((range) => node.from >= range.from && node.from <= range.to)) return;
      const source = state.doc.sliceString(node.from, node.to);
      const checked = /\[[xX]\]/.test(source);
      entries.push({
        from: node.from,
        to: node.to,
        decoration: Decoration.replace({
          widget: new TaskWidget(checked, node.from, node.to),
        }),
      });
      const line = state.doc.lineAt(node.from);
      lineClasses.set(line.from, checked ? "cm-task-done" : "cm-task-open");
    },
  });
  for (const [from, className] of [...lineClasses.entries()].sort((a, b) => a[0] - b[0])) {
    entries.push({
      from,
      to: from,
      decoration: Decoration.line({ class: className }),
    });
  }
  entries.sort((a, b) => a.from - b.from || a.to - b.to);
  const builder = new RangeSetBuilder<Decoration>();
  for (const entry of entries) {
    builder.add(entry.from, entry.to, entry.decoration);
  }
  return builder.finish();
}

class TaskWidget extends WidgetType {
  readonly checked: boolean;
  readonly from: number;
  readonly to: number;

  constructor(checked: boolean, from: number, to: number) {
    super();
    this.checked = checked;
    this.from = from;
    this.to = to;
  }

  eq(other: TaskWidget): boolean {
    return other.checked === this.checked && other.from === this.from && other.to === this.to;
  }

  toDOM(view: EditorView): HTMLElement {
    const label = document.createElement("label");
    label.className = "cm-task";
    const input = document.createElement("input");
    input.type = "checkbox";
    input.checked = this.checked;
    input.setAttribute("aria-label", this.checked ? "Mark task as open" : "Mark task as done");
    input.addEventListener("change", () => {
      view.dispatch({
        changes: {
          from: this.from,
          to: this.to,
          insert: input.checked ? "[x]" : "[ ]",
        },
      });
      view.focus();
    });
    label.appendChild(input);
    return label;
  }

  ignoreEvent(): boolean {
    return true;
  }
}
