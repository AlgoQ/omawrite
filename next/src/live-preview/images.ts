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
import { api } from "../api";
import { tableRanges } from "./table";

interface Embed {
  from: number;
  to: number;
  target: string;
  wiki: boolean;
  alt: string;
}

export function imageEmbeds(notePath: string): Extension {
  return StateField.define<DecorationSet>({
    create: (state) => build(state, notePath),
    update(decorations, transaction) {
      if (transaction.docChanged || transaction.selection !== undefined) {
        return build(transaction.state, notePath);
      }
      return decorations;
    },
    provide: (field) => EditorView.decorations.from(field),
  });
}

function build(state: EditorState, notePath: string): DecorationSet {
  const entries: Array<{ from: number; to: number; decoration: Decoration }> = [];
  const tables = tableRanges(state);
  const selectionLine = state.doc.lineAt(state.selection.main.head).number;
  let fence: { marker: string; length: number } | null = null;

  for (let lineNumber = 1; lineNumber <= state.doc.lines; lineNumber += 1) {
    const line = state.doc.line(lineNumber);
    const trimmed = line.text.trim();
    const fenceMatch = /^(`{3,}|~{3,})/.exec(trimmed);
    if (fenceMatch) {
      const marker = fenceMatch[1][0];
      const length = fenceMatch[1].length;
      if (!fence) fence = { marker, length };
      else if (fence.marker === marker && length >= fence.length) fence = null;
      continue;
    }
    if (fence) continue;
    if (tables.some((range) => line.from >= range.from && line.from <= range.to)) continue;
    if (lineNumber === selectionLine) continue;
    const embed = parseImageLine(trimmed);
    if (!embed) continue;
    entries.push({
      from: line.from,
      to: line.to,
      decoration: Decoration.replace({
        widget: new ImageWidget(embed.target, embed.wiki, notePath, embed.alt),
        block: true,
      }),
    });
  }

  entries.sort((a, b) => a.from - b.from);
  const builder = new RangeSetBuilder<Decoration>();
  for (const entry of entries) {
    builder.add(entry.from, entry.to, entry.decoration);
  }
  return builder.finish();
}

export function parseImageLine(text: string): Omit<Embed, "from" | "to"> | null {
  const wiki = /^!\[\[([^\]|#]+)(?:#[^\]|]+)?(?:\|[^\]]+)?\]\]$/.exec(text);
  if (wiki) {
    return { target: wiki[1].trim(), wiki: true, alt: wiki[1].trim() };
  }
  const markdown = /^!\[([^\]]*)\]\(([^)]+)\)$/.exec(text);
  if (markdown) {
    return { target: markdown[2].trim(), wiki: false, alt: markdown[1] };
  }
  return null;
}

class ImageWidget extends WidgetType {
  readonly target: string;
  readonly wiki: boolean;
  readonly notePath: string;
  readonly alt: string;

  constructor(target: string, wiki: boolean, notePath: string, alt: string) {
    super();
    this.target = target;
    this.wiki = wiki;
    this.notePath = notePath;
    this.alt = alt;
  }

  eq(other: ImageWidget): boolean {
    return (
      other.target === this.target &&
      other.wiki === this.wiki &&
      other.notePath === this.notePath
    );
  }

  get estimatedHeight(): number {
    return 220;
  }

  toDOM(): HTMLElement {
    const wrapper = document.createElement("div");
    wrapper.className = "cm-image-widget";
    const image = document.createElement("img");
    image.alt = this.alt;
    image.loading = "lazy";
    const placeholder = document.createElement("span");
    placeholder.className = "cm-image-placeholder";
    placeholder.textContent = this.alt || "image";
    wrapper.appendChild(placeholder);
    wrapper.appendChild(image);

    void (async () => {
      try {
        const resolved = await api.resolveAttachment(this.target, this.notePath, this.wiki);
        if (!resolved) {
          placeholder.textContent = `Missing image: ${this.target}`;
          placeholder.classList.add("missing");
          return;
        }
        const attachment = await api.readAttachment(resolved);
        image.src = `data:${attachment.mime};base64,${attachment.data}`;
        image.addEventListener("load", () => placeholder.remove(), { once: true });
        image.addEventListener("error", () => {
          placeholder.textContent = `Cannot display: ${this.target}`;
          placeholder.classList.add("missing");
        });
      } catch (error) {
        placeholder.textContent = `Cannot load: ${this.target}`;
        placeholder.classList.add("missing");
        console.warn(error);
      }
    })();
    return wrapper;
  }
}
