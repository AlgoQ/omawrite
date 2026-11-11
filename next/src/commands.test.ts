import { beforeAll, describe, expect, it, vi } from "vitest";
import { commands, matchingCommands } from "./commands";
import { eventToSequence } from "./keymap";

describe("command registry", () => {
  it("has one palette entry and one file-search entry", () => {
    expect(commands.find((command) => command.id === "palette")?.keys).toBe("Mod+p");
    expect(commands.find((command) => command.id === "palette-files")?.keys).toBe(
      "Mod+Shift+o",
    );
  });

  it("filters by words in the title and category", () => {
    expect(matchingCommands("save").map((command) => command.id)).toContain("save");
    expect(matchingCommands("navigate").every((command) => command.category === "Navigate")).toBe(
      true,
    );
  });
});

beforeAll(() => {
  vi.stubGlobal("navigator", { platform: "Linux" });
});

describe("key sequences", () => {
  it("normalizes shifted letters", () => {
    const event = {
      key: "P",
      ctrlKey: true,
      shiftKey: true,
      altKey: false,
      metaKey: false,
    } as KeyboardEvent;
    expect(eventToSequence(event)).toBe("Mod+Shift+p");
  });

  it("keeps arrow names for history bindings", () => {
    const event = {
      key: "ArrowLeft",
      ctrlKey: false,
      shiftKey: false,
      altKey: true,
      metaKey: false,
    } as KeyboardEvent;
    expect(eventToSequence(event)).toBe("Alt+ArrowLeft");
  });
});
