import * as vscode from "vscode";
import type { Node, Parser, Query } from "web-tree-sitter";

const CAPTURE_MAP: Record<string, { type: string; modifiers?: string[] }> = {
  "keyword.function": { type: "keyword" },
  "keyword.return": { type: "keyword" },
  "keyword.import": { type: "keyword" },
  "keyword.storage": { type: "keyword" },
  "keyword.conditional": { type: "keyword" },
  "keyword.repeat": { type: "keyword" },
  attribute: { type: "decorator" },
  "type.builtin": { type: "type", modifiers: ["defaultLibrary"] },
  module: { type: "namespace" },
  "constant.builtin": {
    type: "variable",
    modifiers: ["readonly", "defaultLibrary"],
  },
  constant: { type: "variable", modifiers: ["readonly"] },
  "variable.builtin": { type: "variable", modifiers: ["defaultLibrary"] },
  "variable.parameter": { type: "parameter" },
  type: { type: "type" },
  property: { type: "property" },
  method: { type: "method" },
  "method.call": { type: "method" },
  function: { type: "function" },
  "function.call": { type: "function" },
  "function.builtin": { type: "function", modifiers: ["defaultLibrary"] },
  variable: { type: "variable" },
  "variable.reference": { type: "variable" },
  string: { type: "string" },
  number: { type: "number" },
  boolean: { type: "keyword" },
  operator: { type: "operator" },
  comment: { type: "comment" },
};

export const tokenTypes = [
  ...new Set(Object.values(CAPTURE_MAP).map((v) => v.type)),
];
export const tokenModifiers = [
  ...new Set(Object.values(CAPTURE_MAP).flatMap((v) => v.modifiers ?? [])),
];

export const legend = new vscode.SemanticTokensLegend(
  tokenTypes,
  tokenModifiers,
);

function byteColumnToUtf16(lineText: string, byteColumn: number): number {
  if (byteColumn <= 0) return 0;
  const bytes = Buffer.from(lineText, "utf8");
  return bytes.subarray(0, Math.min(byteColumn, bytes.length)).toString("utf8")
    .length;
}

interface PendingToken {
  line: number;
  startChar: number;
  endChar: number;
  type: string;
  modifiers: string[];
}

function collectTokensForNode(
  document: vscode.TextDocument,
  node: Node,
  type: string,
  modifiers: string[],
  out: PendingToken[],
): void {
  const startRow = node.startPosition.row;
  const endRow = node.endPosition.row;

  for (let row = startRow; row <= endRow && row < document.lineCount; row++) {
    const lineText = document.lineAt(row).text;
    const startChar = row === startRow
      ? byteColumnToUtf16(lineText, node.startPosition.column)
      : 0;
    const endChar = row === endRow
      ? byteColumnToUtf16(lineText, node.endPosition.column)
      : lineText.length;
    if (endChar > startChar) {
      out.push({ line: row, startChar, endChar, type, modifiers });
    }
  }
}

export function createSemanticTokensProvider(
  parser: Parser,
  query: Query,
): vscode.DocumentSemanticTokensProvider {
  return {
    provideDocumentSemanticTokens(document) {
      const tree = parser.parse(document.getText());
      if (!tree) return undefined;

      const winners = new Map<
        string,
        { patternIndex: number; name: string; node: Node }
      >();
      for (const capture of query.captures(tree.rootNode)) {
        const key = `${capture.node.startIndex}-${capture.node.endIndex}`;
        const existing = winners.get(key);
        if (!existing || capture.patternIndex < existing.patternIndex) {
          winners.set(key, capture);
        }
      }

      const pending: PendingToken[] = [];
      for (const { name, node } of winners.values()) {
        const mapping = CAPTURE_MAP[name];
        if (!mapping) continue;
        collectTokensForNode(
          document,
          node,
          mapping.type,
          mapping.modifiers ?? [],
          pending,
        );
      }

      pending.sort((a, b) => a.line - b.line || a.startChar - b.startChar);

      const builder = new vscode.SemanticTokensBuilder(legend);
      for (const token of pending) {
        builder.push(
          new vscode.Range(
            token.line,
            token.startChar,
            token.line,
            token.endChar,
          ),
          token.type,
          token.modifiers,
        );
      }
      return builder.build();
    },
  };
}
