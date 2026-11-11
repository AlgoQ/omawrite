import { describe, expect, it } from "vitest";
import { wordCount } from "../dom";
import { parseImageLine } from "./images";
import { inlineMarkdown } from "./table";
import { findWikiLinks } from "./wiki";

describe("findWikiLinks", () => {
  it("finds a plain link", () => {
    const links = findWikiLinks("See [[Note]] here");
    expect(links).toHaveLength(1);
    expect(links[0].target).toBe("Note");
    expect(links[0].from).toBe(4);
    expect(links[0].to).toBe(12);
  });

  it("keeps headings and aliases", () => {
    const links = findWikiLinks("[[Other#Heading|shown]]");
    expect(links[0].target).toBe("Other");
    expect(links[0].heading).toBe("Heading");
    expect(links[0].alias).toBe("shown");
  });

  it("marks embeds and skips empty targets", () => {
    const links = findWikiLinks("![[image.png]] and [[#heading]]");
    expect(links).toHaveLength(1);
    expect(links[0].embed).toBe(true);
    expect(links[0].target).toBe("image.png");
  });
});

describe("inlineMarkdown", () => {
  it("escapes HTML before rendering Markdown", () => {
    expect(inlineMarkdown("<script>alert(1)</script>")).toContain("&lt;script&gt;");
    expect(inlineMarkdown("<script>alert(1)</script>")).not.toContain("<script>");
  });

  it("renders emphasis, code and links", () => {
    const html = inlineMarkdown("**bold** and `code` and [note](path)");
    expect(html).toContain("<strong>bold</strong>");
    expect(html).toContain("<code>code</code>");
    expect(html).toContain("data-href=\"path\"");
  });

  it("renders wikilinks without a heading", () => {
    const html = inlineMarkdown("[[Target|shown]]");
    expect(html).toContain("data-target=\"Target\"");
    expect(html).toContain(">shown</span>");
  });
});

describe("parseImageLine", () => {
  it("recognizes both embed spellings", () => {
    expect(parseImageLine("![[shot.png]]")).toEqual({
      target: "shot.png",
      wiki: true,
      alt: "shot.png",
    });
    expect(parseImageLine("![alt](_attachments/shot.png)")).toEqual({
      target: "_attachments/shot.png",
      wiki: false,
      alt: "alt",
    });
  });

  it("does not treat prose as an image", () => {
    expect(parseImageLine("Look at ![[shot.png]]")).toBeNull();
  });
});

describe("wordCount", () => {
  it("counts whitespace-separated words", () => {
    expect(wordCount("one two\nthree")).toBe(3);
    expect(wordCount("   ")).toBe(0);
  });
});
