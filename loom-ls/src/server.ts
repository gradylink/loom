import {
  CompletionItem,
  CompletionItemKind,
  createConnection,
  type DefinitionParams,
  Diagnostic,
  DiagnosticSeverity,
  type Hover,
  type HoverParams,
  type InitializeParams,
  type InitializeResult,
  Location,
  ProposedFeatures,
  type TextDocumentPositionParams,
  TextDocuments,
  TextDocumentSyncKind,
} from "vscode-languageserver/node";
import { TextDocument } from "vscode-languageserver-textdocument";
import { Language, Node, Parser } from "web-tree-sitter";
import path from "node:path";
import fs from "node:fs";
import os from "node:os";
import { exec } from "child_process";

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments(TextDocument);

let parser: Parser;
const trees: Map<string, any> = new Map();

const COMPILER_PATH = "/home/grady.link/loom/build/loom" as const;
const WASM_PATH =
  "/home/grady.link/loom/tree-sitter-loom/tree-sitter-loom.wasm" as const;

connection.onInitialize(
  async (params: InitializeParams): Promise<InitializeResult> => {
    await Parser.init();
    parser = new Parser();

    const LoomLanguage = await Language.load(WASM_PATH);
    parser.setLanguage(LoomLanguage);

    return {
      capabilities: {
        textDocumentSync: TextDocumentSyncKind.Incremental,
        definitionProvider: true,
        hoverProvider: true,
        completionProvider: {
          resolveProvider: false,
          triggerCharacters: ["@", "#", "."],
        },
      },
    };
  },
);

documents.onDidChangeContent((change) => {
  if (!parser) return;

  const document = change.document;
  const text = document.getText();
  const tree = parser.parse(text);

  if (!tree) return;

  trees.set(document.uri, tree);

  const diagnostics: Diagnostic[] = [];

  const traverse = (node: Node) => {
    if (node.type === "ERROR" || node.isMissing) {
      diagnostics.push({
        severity: DiagnosticSeverity.Error,
        range: {
          start: {
            line: node.startPosition.row,
            character: node.startPosition.column,
          },
          end: {
            line: node.endPosition.row,
            character: node.endPosition.column,
          },
        },
        message: node.type === "ERROR"
          ? "Syntax error: unexpected token"
          : `Missing token: "${node.type}"`,
        source: "loom-lsp",
      });
    }

    for (let i = 0; i < node.childCount; i++) {
      traverse(node.child(i)!);
    }
  };

  traverse(tree.rootNode);

  const tempDir = path.join(os.tmpdir(), "loom-lsp");
  if (!fs.existsSync(tempDir)) fs.mkdirSync(tempDir, { recursive: true });

  const tempSourceFile = path.join(
    tempDir,
    `check-${path.basename(document.uri)}.loom`,
  );
  const tempOutDir = path.join(tempDir, "out");

  fs.writeFileSync(tempSourceFile, text, "utf8");

  const compileCmd =
    `"${COMPILER_PATH}" "${tempSourceFile}" -o "${tempOutDir}"`;

  exec(compileCmd, (error, stdout, stderr) => {
    const errorStream = stderr || stdout || "";

    if (errorStream) {
      const errorRegex = /line\s+(\d+),\s+col\s+(\d+):\s+(.*)/g;
      let match;

      while ((match = errorRegex.exec(errorStream)) !== null) {
        const cppLine = parseInt(match[1]!, 10) - 1;
        const cppCol = parseInt(match[2]!, 10) - 1;
        const message = match[3]!.trim();

        const errorNode = tree.rootNode.namedDescendantForPosition({
          row: cppLine,
          column: cppCol,
        });

        const range = errorNode
          ? {
            start: {
              line: errorNode.startPosition.row,
              character: errorNode.startPosition.column,
            },
            end: {
              line: errorNode.endPosition.row,
              character: errorNode.endPosition.column,
            },
          }
          : {
            start: { line: cppLine, character: cppCol },
            end: { line: cppLine, character: cppCol + 1 },
          };

        diagnostics.push({
          severity: DiagnosticSeverity.Error,
          range: range,
          message: message,
          source: "loom-compiler",
        });
      }
    }

    try {
      fs.unlinkSync(tempSourceFile);
    } catch {}

    connection.sendDiagnostics({ uri: document.uri, diagnostics });
  });
});

const findGlobalEnum = (root: Node, targetName: string): Node | null => {
  let found: Node | null = null;
  const traverse = (node: Node) => {
    if (found) return;
    if (node.type === "enum_definition") {
      const nameNode = node.childForFieldName("name");
      if (nameNode && nameNode.text === targetName) {
        found = node;
        return;
      }
    }
    for (let i = 0; i < node.childCount; i++) traverse(node.child(i)!);
  };
  traverse(root);
  return found;
};

const findEnumVariant = (enumNode: Node, variantName: string): Node | null => {
  let found: Node | null = null;
  const traverse = (node: Node) => {
    if (found) return;
    if (node.type === "enum_variant") {
      const nameNode = node.childForFieldName("name");
      if (nameNode && nameNode.text === variantName) {
        found = node;
        return;
      }
    }
    for (let i = 0; i < node.childCount; i++) traverse(node.child(i)!);
  };
  traverse(enumNode);
  return found;
};

const getEnumVariantValueText = (variantNode: Node): string => {
  const valueNode = variantNode.childForFieldName("value");
  if (valueNode) return valueNode.text;

  let enumNode: Node | null = variantNode.parent;
  while (enumNode && enumNode.type !== "enum_definition") {
    enumNode = enumNode.parent;
  }
  if (!enumNode) return "unknown";

  let isStringEnum = false;
  const variants: Node[] = [];

  const traverse = (n: Node) => {
    if (n.type === "enum_variant") {
      variants.push(n);
      const vNode = n.childForFieldName("value");
      if (
        vNode && (vNode.type === "string_literal" || vNode.type === "string")
      ) {
        isStringEnum = true;
      }
    }
    for (let i = 0; i < n.childCount; i++) traverse(n.child(i)!);
  };
  traverse(enumNode);

  if (isStringEnum) return "unknown";

  let currentVal = 0;
  for (const v of variants) {
    const vNode = v.childForFieldName("value");
    if (vNode && (vNode.type === "integer" || vNode.type === "number")) {
      currentVal = parseInt(vNode.text, 10);
    }
    if (v.id === variantNode.id) {
      return currentVal.toString();
    }
    currentVal++;
  }
  return "unknown";
};

function getDeclarationNode(cursorNode: Node, targetName: string): Node | null {
  let curr: Node | null = cursorNode;

  while (curr) {
    if (curr.type === "for") {
      const iteratorNode = curr.childForFieldName("iterator");
      if (iteratorNode && iteratorNode.text === targetName) {
        return curr;
      }
    }

    if (curr.type === "block" || curr.type === "source_file") {
      for (let i = 0; i < curr.childCount; i++) {
        const child = curr.child(i)!;

        if (child.startPosition.row > cursorNode.startPosition.row) break;

        if (child.type === "variable_declaration") {
          const nameNode = child.childForFieldName("name");
          if (nameNode && nameNode.text === targetName) {
            return child;
          }
        }
      }
    }

    if (curr.type === "function_definition") {
      const funcNameNode = curr.childForFieldName("name");
      if (funcNameNode && funcNameNode.text === targetName) {
        return curr;
      }

      const paramsNode = curr.childForFieldName("parameters");
      if (paramsNode) {
        let foundParam: Node | null = null;
        const checkParam = (n: Node) => {
          if (n.type === "parameter") {
            const pName = n.childForFieldName("name");
            if (pName && pName.text === targetName) {
              foundParam = n;
            }
          }
          for (let i = 0; i < n.childCount; i++) checkParam(n.child(i)!);
        };
        checkParam(paramsNode);
        if (foundParam) return foundParam;
      }
    }

    curr = curr.parent;
  }

  let root = cursorNode;
  while (root.parent) root = root.parent;

  const globalEnum = findGlobalEnum(root, targetName);
  if (globalEnum) return globalEnum;

  let globalFunc: Node | null = null;
  const findGlobalFunc = (node: Node) => {
    if (globalFunc) return;
    if (node.type === "function_definition") {
      const nameNode = node.childForFieldName("name");
      if (nameNode && nameNode.text === targetName) {
        globalFunc = node;
        return;
      }
    }
    for (let i = 0; i < node.childCount; i++) findGlobalFunc(node.child(i)!);
  };
  findGlobalFunc(root);

  return globalFunc;
}

connection.onDefinition((params: DefinitionParams): Location | null => {
  const tree = trees.get(params.textDocument.uri);
  if (!tree) return null;

  const cursorPoint = {
    row: params.position.line,
    column: params.position.character,
  };
  const cursorNode: Node = tree.rootNode.namedDescendantForPosition(
    cursorPoint,
  );
  if (!cursorNode || cursorNode.type !== "identifier") return null;

  let targetNode: Node | null = null;

  if (cursorNode.parent && cursorNode.parent.type === "member_expression") {
    const objNode = cursorNode.parent.childForFieldName("object");
    const propNode = cursorNode.parent.childForFieldName("property");
    if (objNode && propNode && cursorNode.text === propNode.text) {
      const enumNode = findGlobalEnum(tree.rootNode, objNode.text);
      if (enumNode) {
        const variantNode = findEnumVariant(enumNode, propNode.text);
        if (variantNode) {
          targetNode = variantNode.childForFieldName("name") || variantNode;
        }
      }
    }
  }

  if (!targetNode) {
    const declarationNode = getDeclarationNode(cursorNode, cursorNode.text);
    if (declarationNode) {
      targetNode = declarationNode.childForFieldName("name") ||
        declarationNode.childForFieldName("iterator") ||
        declarationNode.namedChild(0) ||
        declarationNode;
    }
  }

  if (targetNode) {
    return {
      uri: params.textDocument.uri,
      range: {
        start: {
          line: targetNode.startPosition.row,
          character: targetNode.startPosition.column,
        },
        end: {
          line: targetNode.endPosition.row,
          character: targetNode.endPosition.column,
        },
      },
    };
  }

  return null;
});

connection.onHover((params: HoverParams): Hover | null => {
  const tree = trees.get(params.textDocument.uri);
  if (!tree) return null;

  const cursorPoint = {
    row: params.position.line,
    column: params.position.character,
  };
  const cursorNode: Node = tree.rootNode.namedDescendantForPosition(
    cursorPoint,
  );
  if (!cursorNode || cursorNode.type !== "identifier") return null;

  let hoverText = "";

  if (cursorNode.parent && cursorNode.parent.type === "member_expression") {
    const objNode = cursorNode.parent.childForFieldName("object");
    const propNode = cursorNode.parent.childForFieldName("property");
    if (objNode && propNode && cursorNode.text === propNode.text) {
      const enumNode = findGlobalEnum(tree.rootNode, objNode.text);
      if (enumNode) {
        const variantNode = findEnumVariant(enumNode, propNode.text);
        if (variantNode) {
          const valStr = getEnumVariantValueText(variantNode);
          hoverText =
            `\`\`\`loom\n(enum variant) ${objNode.text}.${propNode.text} = ${valStr}\n\`\`\``;
        }
      }
    }
  }

  if (hoverText == "") {
    const declarationNode = getDeclarationNode(cursorNode, cursorNode.text);

    if (declarationNode) {
      if (declarationNode.type === "enum_definition") {
        const name = declarationNode.childForFieldName("name")?.text ||
          "unknown";
        hoverText = `\`\`\`loom\nenum ${name}\n\`\`\``;
      } else if (declarationNode.type === "enum_variant") {
        let p: Node | null = declarationNode.parent;
        while (p && p.type !== "enum_definition") p = p.parent;
        const enumName = p
          ? (p.childForFieldName("name")?.text || "enum")
          : "enum";
        const varName = declarationNode.childForFieldName("name")?.text ||
          "unknown";
        const valStr = getEnumVariantValueText(declarationNode);
        hoverText =
          `\`\`\`loom\n(enum variant) ${enumName}.${varName} = ${valStr}\n\`\`\``;
      } else if (
        declarationNode.type === "variable_declaration" ||
        declarationNode.type === "parameter"
      ) {
        const isParam = declarationNode.type === "parameter";
        const name = declarationNode.childForFieldName("name")?.text ||
          "unknown";
        const type = declarationNode.childForFieldName("type")?.text ||
          "unknown";
        const keyword = isParam
          ? "(parameter)"
          : (declarationNode.childForFieldName("keyword")?.text || "let");

        hoverText = `\`\`\`loom\n${keyword} ${name}: ${type}\n\`\`\``;
      } else if (declarationNode.type === "for") {
        const iteratorName =
          declarationNode.childForFieldName("iterator")?.text || "iterator";
        hoverText = `\`\`\`loom\n(loop variable) ${iteratorName}: int\n\`\`\``;
      } else if (declarationNode.type === "function_definition") {
        const name = declarationNode.childForFieldName("name")?.text ||
          "unknown";
        const paramsText =
          declarationNode.childForFieldName("parameters")?.text || "";
        const typeNode = declarationNode.childForFieldName("type");
        const returnType = typeNode ? `: ${typeNode.text}` : "";

        hoverText =
          `\`\`\`loom\nfunc ${name}(${paramsText})${returnType}\n\`\`\``;
      }
    }
  }

  if (hoverText != "") {
    return { contents: { kind: "markdown", value: hoverText } };
  }

  return null;
});

connection.onCompletion(
  (params: TextDocumentPositionParams): CompletionItem[] => {
    const document = documents.get(params.textDocument.uri);
    const tree = trees.get(params.textDocument.uri);

    if (!document || !tree) return [];

    const lineText = document.getText({
      start: { line: params.position.line, character: 0 },
      end: params.position,
    });

    const items: CompletionItem[] = [];

    if (lineText.match(/@[srpean]\[[^\]]*$/i)) return [];
    if (lineText.includes("--")) return [];

    const memberMatch = lineText.match(/([a-zA-Z_][a-zA-Z0-9_]*)\.$/);
    if (memberMatch) {
      const enumName = memberMatch[1]!;
      const enumNode = findGlobalEnum(tree.rootNode, enumName);
      if (enumNode) {
        const enumVariants: CompletionItem[] = [];
        const collectVariants = (node: Node) => {
          if (node.type === "enum_variant") {
            const nameNode = node.childForFieldName("name");
            if (nameNode) {
              const valStr = getEnumVariantValueText(node);
              enumVariants.push({
                label: nameNode.text,
                kind: CompletionItemKind.EnumMember,
                detail: `= ${valStr}`,
              });
            }
          }
          for (let i = 0; i < node.childCount; i++) {
            collectVariants(node.child(i)!);
          }
        };
        collectVariants(enumNode);
        return enumVariants;
      }
    }

    if (lineText.match(/:\s*[a-zA-Z_]*$/)) {
      return [
        { label: "int", kind: CompletionItemKind.TypeParameter },
        { label: "bool", kind: CompletionItemKind.TypeParameter },
        { label: "string", kind: CompletionItemKind.TypeParameter },
        { label: "void", kind: CompletionItemKind.TypeParameter },
      ];
    }
    if (lineText.match(/(let|const|func|for|enum)\s+[a-z_0-9]*$/i)) return [];
    if (lineText.match(/func\s+[a-z_0-9]+\s*\([^)]*$/i)) return [];

    if (lineText.match(/@$/)) {
      return [
        { label: "a", kind: CompletionItemKind.EnumMember },
        { label: "p", kind: CompletionItemKind.EnumMember },
        { label: "s", kind: CompletionItemKind.EnumMember },
        { label: "r", kind: CompletionItemKind.EnumMember },
        { label: "n", kind: CompletionItemKind.EnumMember },
        { label: "e", kind: CompletionItemKind.EnumMember },
      ];
    }

    if (lineText.match(/#$/)) {
      return [
        { label: "load", kind: CompletionItemKind.EnumMember },
        { label: "tick", kind: CompletionItemKind.EnumMember },
      ];
    }

    const cursorPoint = {
      row: params.position.line,
      column: Math.max(0, params.position.character - 1),
    };
    const cursorNode = tree.rootNode.descendantForPosition(cursorPoint);

    const variables = new Set<{ name: string; type: string; const: boolean }>();
    let inBlock = false;
    let inEnum = false;
    let curr: Node | null = cursorNode;

    while (curr) {
      if (curr.type === "block") {
        inBlock = true;
      }
      if (curr.type === "enum_definition") {
        inEnum = true;
      }

      if (curr.type === "for") {
        const iteratorNode = curr.childForFieldName("iterator");
        if (iteratorNode) {
          variables.add({ name: iteratorNode.text, type: "int", const: false });
        }
      }

      if (curr.type === "block" || curr.type === "source_file") {
        for (let i = 0; i < curr.childCount; i++) {
          const child = curr.child(i)!;
          if (child.endPosition.row <= params.position.line) {
            if (child.type === "variable_declaration") {
              const keywordNode = child.childForFieldName("keyword");
              const nameNode = child.childForFieldName("name");
              const typeNode = child.childForFieldName("type");
              if (nameNode) {
                variables.add({
                  name: nameNode.text,
                  type: typeNode ? typeNode.text : "unknown",
                  const: keywordNode != null && keywordNode.text == "const",
                });
              }
            }
          }
        }
      }

      curr = curr.parent;
    }

    if (inEnum) return [];

    const functions = new Set<
      { name: string; type: string; params: { name: string; type: string }[] }
    >();
    const findFunctions = (node: Node) => {
      if (node.type === "function_definition") {
        const nameNode = node.childForFieldName("name");
        const typeNode = node.childForFieldName("type");
        if (nameNode) {
          const params: { name: string; type: string }[] = [];

          const paramsNode = node.childForFieldName("parameters");
          if (paramsNode) {
            for (const paramNode of paramsNode.children) {
              const paramNameNode = paramNode.childForFieldName("name");
              const paramTypeNode = paramNode.childForFieldName("type");
              if (paramNameNode) {
                params.push({
                  name: paramNameNode.text,
                  type: paramTypeNode ? paramTypeNode.text : "unknown",
                });
              }
            }
          }

          functions.add({
            name: nameNode.text,
            params,
            type: typeNode ? typeNode.text : "void",
          });
        }
      }
      for (let i = 0; i < node.childCount; i++) findFunctions(node.child(i)!);
    };
    findFunctions(tree.rootNode);

    const enumsSet = new Set<{ name: string }>();
    const findEnums = (node: Node) => {
      if (node.type === "enum_definition") {
        const nameNode = node.childForFieldName("name");
        if (nameNode) {
          enumsSet.add({ name: nameNode.text });
        }
      }
      for (let i = 0; i < node.childCount; i++) findEnums(node.child(i)!);
    };
    findEnums(tree.rootNode);

    const addVariables = () => {
      for (const v of variables) {
        items.push({
          label: v.name,
          kind: CompletionItemKind.Variable,
          detail: `${v.const ? "const" : "let"} ${v.name}: ${v.type}`,
        });
      }
    };
    const addFunctions = () => {
      for (const fn of functions) {
        items.push({
          label: fn.name,
          kind: CompletionItemKind.Function,
          detail: `${fn.name}(${
            fn.params.map((param) => `${param.name}: ${param.type}`).join(", ")
          }): ${fn.type}`,
        });
      }
    };
    const addEnums = () => {
      for (const e of enumsSet) {
        items.push({
          label: e.name,
          kind: CompletionItemKind.Enum,
          detail: `enum ${e.name}`,
        });
      }
    };

    if (
      lineText.match(
        /(return|in|while)\s+|((let|const)\s+)?[a-z_0-9]+\s*=\s*|\${[^}]$/i,
      )
    ) {
      addVariables();
      addEnums();

      return [
        ...items,
        { label: "true", kind: CompletionItemKind.Constant },
        { label: "false", kind: CompletionItemKind.Constant },
        { label: "at", kind: CompletionItemKind.Operator },
        { label: "entity", kind: CompletionItemKind.Operator },
      ];
    }

    const keywords = [
      "let",
      "const",
      "func",
      "if",
      "else",
      "do",
      "while",
      "for",
      "return",
      "true",
      "false",
      "as",
      "at",
      "align",
      "anchored",
      "facing",
      "entity",
      "in",
      "on",
      "positioned",
      "over",
      "rotated",
      "enum",
    ];
    for (const kw of keywords) {
      items.push({ label: kw, kind: CompletionItemKind.Keyword });
    }

    if (inBlock) {
      addFunctions();
      addVariables();
    }

    return items;
  },
);

documents.listen(connection);
connection.listen();
