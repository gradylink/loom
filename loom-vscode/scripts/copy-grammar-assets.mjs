import { copyFileSync, mkdirSync } from "fs";
import { dirname, join } from "path";
import { fileURLToPath } from "url";

const root = dirname(fileURLToPath(import.meta.url));
const extensionDir = join(root, "..");
const treeSitterLoomDir = join(extensionDir, "..", "tree-sitter-loom");
const outDir = join(extensionDir, "out");

mkdirSync(outDir, { recursive: true });

copyFileSync(
  join(extensionDir, "node_modules", "web-tree-sitter", "web-tree-sitter.wasm"),
  join(outDir, "web-tree-sitter.wasm"),
);
copyFileSync(
  join(treeSitterLoomDir, "tree-sitter-loom.wasm"),
  join(outDir, "tree-sitter-loom.wasm"),
);
copyFileSync(
  join(treeSitterLoomDir, "queries", "highlights.scm"),
  join(outDir, "highlights.scm"),
);
