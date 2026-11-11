import { clear, el } from "./dom";
import { app } from "./state";
import type { TreeNode } from "./types";

export interface TreeHost {
  openNote(path: string): void;
  showContextMenu(path: string, x: number, y: number): void;
}

let host: TreeHost | null = null;

export function setTreeHost(next: TreeHost): void {
  host = next;
}

export function renderTree(): void {
  const tree = document.getElementById("tree");
  const name = document.getElementById("vault-name");
  if (!tree) return;
  if (name) name.textContent = app.vault?.name ?? "slipbox";
  clear(tree);

  if (!app.vault) {
    tree.appendChild(el("p", "tree-empty", "No slipbox open."));
    return;
  }

  const needle = app.treeFilter.trim().toLowerCase();
  const nodes = needle ? filterTree(app.tree, needle) : app.tree;
  if (!nodes.length) {
    tree.appendChild(
      el("p", "tree-empty", needle ? "No notes match." : "No notes yet."),
    );
    return;
  }
  for (const node of nodes) {
    tree.appendChild(renderNode(node, 0));
  }
}

function renderNode(node: TreeNode, depth: number): HTMLElement {
  const wrapper = el("div", "tree-node");
  const row = el("div", "tree-row");
  row.style.paddingLeft = `${8 + depth * 14}px`;
  row.dataset.path = node.path;
  row.setAttribute("role", "treeitem");
  row.tabIndex = 0;

  if (node.path === app.activePath) row.classList.add("active");

  const folded = app.closedFolds.has(node.path);
  if (node.childCount > 0) {
    row.setAttribute("aria-expanded", String(!folded));
    const chevron = el("button", "tree-chevron", folded ? "▸" : "▾");
    chevron.title = folded ? "Unfold" : "Fold";
    chevron.addEventListener("click", (event) => {
      event.stopPropagation();
      if (folded) app.closedFolds.delete(node.path);
      else app.closedFolds.add(node.path);
      try {
        window.localStorage.setItem(
          "omawrite.folds",
          JSON.stringify([...app.closedFolds]),
        );
      } catch {
        // Ignore a full localStorage.
      }
      renderTree();
    });
    row.appendChild(chevron);
  } else {
    row.appendChild(el("span", "tree-chevron-spacer"));
  }

  row.appendChild(el("span", "tree-name", node.name));
  if (node.childCount > 0) {
    row.appendChild(el("span", "tree-count", String(node.childCount)));
  }
  if (node.tags.length) {
    row.appendChild(el("span", "tree-tag", node.tags[0]));
  }

  row.addEventListener("click", () => host?.openNote(node.path));
  row.addEventListener("keydown", (event) => {
    if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      host?.openNote(node.path);
    }
  });
  row.addEventListener("contextmenu", (event) => {
    event.preventDefault();
    event.stopPropagation();
    host?.showContextMenu(node.path, event.clientX, event.clientY);
  });
  wrapper.appendChild(row);

  if (!folded && node.children.length) {
    const children = el("div", "tree-children");
    children.setAttribute("role", "group");
    for (const child of node.children) {
      children.appendChild(renderNode(child, depth + 1));
    }
    wrapper.appendChild(children);
  }
  return wrapper;
}

function filterTree(nodes: TreeNode[], needle: string): TreeNode[] {
  const filtered: TreeNode[] = [];
  for (const node of nodes) {
    const children = filterTree(node.children, needle);
    const selfMatch =
      node.name.toLowerCase().includes(needle) ||
      node.path.toLowerCase().includes(needle) ||
      node.tags.some((tag) => tag.toLowerCase().includes(needle));
    if (!selfMatch && !children.length) continue;
    filtered.push({
      ...node,
      // A matching parent shows its whole subtree, not just matching leaves.
      children: selfMatch ? node.children : children,
    });
  }
  return filtered;
}

export function focusTreeFilter(): void {
  const filter = document.getElementById("tree-filter") as HTMLInputElement | null;
  filter?.focus();
}
