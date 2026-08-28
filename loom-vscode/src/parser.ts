import { readFileSync } from "fs";
import * as path from "path";
import { Language, Parser, Query } from "web-tree-sitter";

let initPromise: Promise<void> | undefined;

export async function loadLoomGrammar(
  extensionPath: string,
): Promise<{ parser: Parser; query: Query }> {
  const wasmDir = path.join(extensionPath, "out");

  if (!initPromise) {
    initPromise = Parser.init({
      locateFile: () => path.join(wasmDir, "web-tree-sitter.wasm"),
    });
  }
  await initPromise;

  const language = await Language.load(
    readFileSync(path.join(wasmDir, "tree-sitter-loom.wasm")),
  );

  const parser = new Parser();
  parser.setLanguage(language);

  const highlightsSource = readFileSync(path.join(wasmDir, "highlights.scm"), "utf8");
  const query = new Query(language, highlightsSource);

  return { parser, query };
}
