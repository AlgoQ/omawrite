import { api } from "./api";
import { clear, el, showToast } from "./dom";
import { activeTab, type Tab } from "./state";
import type { FrontmatterEdit, FrontmatterEditResult, Property } from "./types";

export interface PropertiesHost {
  openNote(path: string): void;
  changed(tab: Tab): void;
}

let host: PropertiesHost | null = null;

export function setPropertiesHost(next: PropertiesHost): void {
  host = next;
}

export function renderProperties(): void {
  const panel = document.getElementById("properties");
  if (!panel) return;
  clear(panel);
  const tab = activeTab();
  if (!tab) return;

  const header = el("div", "properties-header");
  header.appendChild(el("span", "properties-title", "Properties"));
  panel.appendChild(header);

  for (const property of tab.doc.properties) {
    panel.appendChild(renderProperty(tab, property));
  }

  const add = el("button", "property-add", "+ Add property");
  add.addEventListener("click", () => showAddProperty(tab));
  panel.appendChild(add);
}

function renderProperty(tab: Tab, property: Property): HTMLElement {
  const row = el("div", "property-row");
  row.dataset.key = property.key;
  const key = el("span", "property-key", property.key);
  row.appendChild(key);

  const values = el("span", "property-values");
  switch (property.kind) {
    case "tags":
      for (const tag of property.values) {
        values.appendChild(chip(tag, () => edit(tab, { kind: "removeTag", tag })));
      }
      values.appendChild(input("Add tag", (value) => {
        if (value.trim()) void edit(tab, { kind: "addTag", tag: value.trim() });
      }));
      break;
    case "links":
      for (const link of property.values) {
        values.appendChild(
          chip(link, () => edit(tab, { kind: "removeUp", target: link }), () => {
            void openLink(tab, link);
          }),
        );
      }
      values.appendChild(input("Add parent", (value) => {
        if (value.trim()) void edit(tab, { kind: "addUp", target: value.trim() });
      }));
      break;
    case "list":
      for (let index = 0; index < property.values.length; index += 1) {
        const value = property.values[index];
        values.appendChild(
          chip(value, () => edit(tab, { kind: "removeValue", key: property.key, index })),
        );
      }
      values.appendChild(input("Add value", (value) => {
        if (value.trim()) void edit(tab, { kind: "addValue", key: property.key, value: value.trim() });
      }));
      break;
    case "checkbox": {
      const checkbox = document.createElement("input");
      checkbox.type = "checkbox";
      checkbox.checked = property.values[0] === "true";
      checkbox.addEventListener("change", () => {
        void edit(tab, {
          kind: "setValue",
          key: property.key,
          index: 0,
          value: checkbox.checked ? "true" : "false",
        });
      });
      values.appendChild(checkbox);
      break;
    }
    default: {
      const field = document.createElement("input");
      field.className = "property-input";
      field.value = property.values[0] ?? "";
      field.spellcheck = false;
      const commit = () => {
        if (field.value === (property.values[0] ?? "")) return;
        void edit(tab, {
          kind: "setValue",
          key: property.key,
          index: 0,
          value: field.value,
        });
      };
      field.addEventListener("blur", commit);
      field.addEventListener("keydown", (event) => {
        if (event.key === "Enter") {
          event.preventDefault();
          field.blur();
        }
      });
      values.appendChild(field);
      break;
    }
  }
  row.appendChild(values);

  const remove = el("button", "property-remove", "×");
  remove.title = `Remove ${property.key}`;
  remove.addEventListener("click", () => {
    void edit(tab, { kind: "remove", key: property.key });
  });
  row.appendChild(remove);
  return row;
}

function chip(
  text: string,
  onRemove: () => void,
  onOpen?: () => void,
): HTMLElement {
  const element = el("span", "chip");
  const label = el("button", "chip-label", text);
  if (onOpen) {
    label.classList.add("clickable");
    label.addEventListener("click", onOpen);
  } else {
    label.disabled = true;
  }
  element.appendChild(label);
  const remove = el("button", "chip-remove", "×");
  remove.title = `Remove ${text}`;
  remove.addEventListener("click", onRemove);
  element.appendChild(remove);
  return element;
}

function input(placeholder: string, onSubmit: (value: string) => void): HTMLElement {
  const field = document.createElement("input");
  field.className = "chip-input";
  field.placeholder = placeholder;
  field.spellcheck = false;
  field.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      const value = field.value;
      field.value = "";
      onSubmit(value);
    }
  });
  return field;
}

async function edit(tab: Tab, operation: FrontmatterEdit): Promise<void> {
  try {
    const result = await api.editFrontmatter(tab.doc.frontmatter, operation);
    applyEdit(tab, result);
  } catch (error) {
    showToast(String(error));
  }
}

function applyEdit(tab: Tab, result: FrontmatterEditResult): void {
  tab.doc.frontmatter = result.frontmatter;
  tab.doc.properties = result.properties;
  tab.doc.tags = result.tags;
  tab.doc.ups = result.ups;
  tab.doc.aliases = result.aliases;
  tab.dirty = true;
  renderProperties();
  host?.changed(tab);
}

async function openLink(tab: Tab, target: string): Promise<void> {
  try {
    const resolved = await api.resolveNote(target, tab.doc.path);
    if (!resolved) {
      showToast(`No note named ${target}`);
      return;
    }
    host?.openNote(resolved);
  } catch (error) {
    showToast(String(error));
  }
}

function showAddProperty(tab: Tab): void {
  const panel = document.getElementById("properties");
  if (!panel) return;
  const existing = panel.querySelector(".property-form");
  existing?.remove();

  const form = el("div", "property-form");
  const key = document.createElement("input");
  key.placeholder = "key";
  key.spellcheck = false;
  const value = document.createElement("input");
  value.placeholder = "value";
  value.spellcheck = false;
  const add = el("button", "text-button primary", "Add");
  const cancel = el("button", "text-button", "Cancel");

  const commit = () => {
    const name = key.value.trim();
    if (!name) {
      key.focus();
      return;
    }
    const values = value.value.trim() ? [value.value.trim()] : [];
    form.remove();
    void edit(tab, { kind: "set", key: name, values });
  };

  add.addEventListener("click", commit);
  cancel.addEventListener("click", () => form.remove());
  form.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      commit();
    } else if (event.key === "Escape") {
      form.remove();
    }
  });
  form.append(key, value, add, cancel);
  panel.appendChild(form);
  key.focus();
}
