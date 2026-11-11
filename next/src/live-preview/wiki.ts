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

interface WikiMatch {
  from: number;
  to: number;
  visibleFrom: number;
  visibleTo: number;
  target: string;
  heading: string | null;
  alias: string | null;
  embed: boolean;
}

export function wikiLinks(): Extension {
  return [
    StateField.define<DecorationSet>({
      create: build,
      update(decorations, transaction) {
        if (transaction.docChanged) return build(transaction.state);
        return decorations;
      },
      provide: (field) => EditorView.decorations.from(field),
    }),
    EditorView.domEventHandlers({
      mousedown(event) {
        if (event.button !== 0) return false;
        const target = event.target;
        if (!(target instanceof Element)) return false;
        const link = target.closest<HTMLElement>(".cm-wiki-link");
        if (!link?.dataset.target) return false;
        if (event.altKey) return false;
        event.preventDefault();
        window.dispatchEvent(
          new CustomEvent("omawrite:open-wiki", {
            detail: link.dataset.target,
          }),
        );
        return true;
      },
    }),
  ];
}

function build(state: EditorState): DecorationSet {
  const entries: Array<{ from: number; to: number; decoration: Decoration }> = [];
  const tables = tableRanges(state);
  const inTable = (position: number) =>
    tables.some((range) => position >= range.from && position <= range.to);

  let fence: { marker: string; length: number } | null = null;
  for (let lineNumber = 1; lineNumber <= state.doc.lines; lineNumber += 1) {
    const line = state.doc.line(lineNumber);
    const trimmed = line.text.trimStart();
    const fenceMatch = /^(`{3,}|~{3,})/.exec(trimmed);
    if (fenceMatch) {
      const marker = fenceMatch[1][0];
      const length = fenceMatch[1].length;
      if (!fence) {
        fence = { marker, length };
      } else if (fence.marker === marker && length >= fence.length) {
        fence = null;
      }
      continue;
    }
    if (fence || inTable(line.from)) continue;

    for (const match of findWikiLinks(line.text)) {
      if (match.embed) continue;
      const from = line.from + match.from;
      const to = line.from + match.to;
      const visibleFrom = line.from + match.visibleFrom;
      const visibleTo = line.from + match.visibleTo;
      const label = line.text.slice(match.visibleFrom, match.visibleTo);
      if (!label) continue;

      entries.push({
        from: visibleFrom,
        to: visibleTo,
        decoration: Decoration.mark({
          class: "cm-wiki-link",
          attributes: {
            "data-target": match.target,
            ...(match.heading ? { "data-heading": match.heading } : {}),
          },
        }),
      });
      if (visibleFrom > from) {
        entries.push({
          from,
          to: visibleFrom,
          decoration: Decoration.replace({}),
        });
      }
      if (visibleTo < to) {
        entries.push({
          from: visibleTo,
          to,
          decoration: Decoration.replace({}),
        });
      }
    }
  }

  entries.sort((a, b) => a.from - b.from || a.to - b.to);
  const builder = new RangeSetBuilder<Decoration>();
  let lastFrom = -1;
  for (const entry of entries) {
    if (entry.from < lastFrom) continue;
    builder.add(entry.from, entry.to, entry.decoration);
    lastFrom = entry.from;
  }
  return builder.finish();
}

export function findWikiLinks(text: string): WikiMatch[] {
  const matches: WikiMatch[] = [];
  let index = 0;
  while (index < text.length) {
    const open = text.indexOf("[[", index);
    if (open < 0) break;
    const close = text.indexOf("]]", open + 2);
    if (close < 0) break;
    const inner = text.slice(open + 2, close);
    if (inner.includes("[") || inner.includes("\n")) {
      index = open + 2;
      continue;
    }
    const embed = open > 0 && text[open - 1] === "!";
    const pipe = inner.indexOf("|");
    const hash = inner.indexOf("#");
    const targetEnd = hash >= 0 && (pipe < 0 || hash < pipe) ? hash : pipe >= 0 ? pipe : inner.length;
    const target = inner.slice(0, targetEnd).trim();
    if (!target) {
      index = close + 2;
      continue;
    }
    const heading = hash >= 0 && (pipe < 0 || hash < pipe)
      ? inner.slice(hash + 1, pipe >= 0 ? pipe : inner.length).trim()
      : null;
    const alias = pipe >= 0 ? inner.slice(pipe + 1).trim() : null;
    const from = embed ? open - 1 : open;
    const to = close + 2;
    const visibleFrom = open + 2 + (pipe >= 0 ? pipe + 1 : 0);
    const visibleTo = pipe >= 0 ? close : hash >= 0 ? open + 2 + hash : close;
    matches.push({
      from,
      to,
      visibleFrom,
      visibleTo,
      target,
      heading,
      alias,
      embed,
    });
    index = close + 2;
  }
  return matches;
}
