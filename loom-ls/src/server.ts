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
const { Language, Node, Parser, Tree } = require("web-tree-sitter");
import path from "node:path";
import fs from "node:fs";
import os from "node:os";
import { exec } from "child_process";
import { fileURLToPath } from "node:url";
import { spawn } from "node:child_process";

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments(TextDocument);

let parser: typeof Parser;
const trees: Map<string, any> = new Map();
const importCache: Map<string, { tree: typeof Tree; path: string }> = new Map();

const COMPILER_PATH = process.env.LOOM_EXECUTABLE || "loom";
const WASM_PATH = process.env.LOOM_WASM_PATH ||
  "/home/grady.link/loom/tree-sitter-loom/tree-sitter-loom.wasm";

function resolveImportPath(
  importPath: string,
  sourceDir: string,
): string | null {
  const resolved = path.resolve(sourceDir, importPath);
  if (fs.existsSync(resolved)) {
    return resolved;
  }
  return null;
}

function getOrParseFile(filePath: string, sourceDir: string): any {
  const resolved = resolveImportPath(filePath, sourceDir);
  if (!resolved) return null;

  if (importCache.has(resolved)) {
    return importCache.get(resolved)!.tree;
  }

  try {
    const content = fs.readFileSync(resolved, "utf8");
    const tree = parser.parse(content);
    if (!tree) return null;
    importCache.set(resolved, { tree, path: resolved });
    return tree;
  } catch {
    return null;
  }
}

function getImports(tree: typeof Tree): string[] {
  const imports: string[] = [];
  const traverse = (node: typeof Node) => {
    if (node.type === "import_statement") {
      const pathNode = node.childForFieldName("path");
      if (pathNode) {
        imports.push(pathNode.text);
      }
    }
    for (let i = 0; i < node.childCount; i++) {
      traverse(node.child(i)!);
    }
  };
  traverse(tree.rootNode);
  return imports;
}

function findExportedSymbols(tree: typeof Tree): {
  enums: Map<string, typeof Node>;
  functions: Map<string, typeof Node>;
  variables: Map<string, typeof Node>;
} {
  const enums = new Map<string, typeof Node>();
  const functions = new Map<string, typeof Node>();
  const variables = new Map<string, typeof Node>();

  const traverse = (node: typeof Node) => {
    if (node.type === "enum_definition") {
      const exportNode = node.children.find((n: typeof Node) =>
        n.text === "export"
      );
      if (exportNode) {
        const nameNode = node.childForFieldName("name");
        if (nameNode) enums.set(nameNode.text, node);
      }
    } else if (node.type === "function_definition") {
      const exportNode = node.children.find((n: typeof Node) =>
        n.text === "export"
      );
      if (exportNode) {
        const nameNode = node.childForFieldName("name");
        if (nameNode) functions.set(nameNode.text, node);
      }
    } else if (node.type === "variable_declaration") {
      const exportNode = node.children.find((n: typeof Node) =>
        n.text === "export"
      );
      if (exportNode) {
        const nameNode = node.childForFieldName("name");
        if (nameNode) variables.set(nameNode.text, node);
      }
    }

    for (let i = 0; i < node.childCount; i++) {
      traverse(node.child(i)!);
    }
  };

  traverse(tree.rootNode);
  return { enums, functions, variables };
}

function collectAllSymbols(
  tree: typeof Tree,
  docUri: string,
  visited = new Set<string>(),
): {
  enums: Map<string, { node: typeof Node; file: string }>;
  functions: Map<string, { node: typeof Node; file: string }>;
  variables: Map<string, { node: typeof Node; file: string }>;
} {
  const enums = new Map<string, { node: typeof Node; file: string }>();
  const functions = new Map<string, { node: typeof Node; file: string }>();
  const variables = new Map<string, { node: typeof Node; file: string }>();

  const sourceDir = path.dirname(docUri.replace("file://", ""));

  const traverse = (node: typeof Node) => {
    if (node.type === "enum_definition") {
      const nameNode = node.childForFieldName("name");
      if (nameNode) enums.set(nameNode.text, { node, file: docUri });
    } else if (node.type === "function_definition") {
      const nameNode = node.childForFieldName("name");
      if (nameNode) functions.set(nameNode.text, { node, file: docUri });
    } else if (node.type === "variable_declaration") {
      const nameNode = node.childForFieldName("name");
      if (nameNode) variables.set(nameNode.text, { node, file: docUri });
    }

    for (let i = 0; i < node.childCount; i++) {
      traverse(node.child(i)!);
    }
  };
  traverse(tree.rootNode);

  const imports = getImports(tree);
  for (const importPath of imports) {
    const resolved = resolveImportPath(importPath, sourceDir);
    if (resolved && !visited.has(resolved)) {
      visited.add(resolved);
      const importedTree = getOrParseFile(importPath, sourceDir);
      if (importedTree) {
        const exported = findExportedSymbols(importedTree);
        exported.enums.forEach((node, name) => {
          if (!enums.has(name)) {
            enums.set(name, { node, file: `file://${resolved}` });
          }
        });
        exported.functions.forEach((node, name) => {
          if (!functions.has(name)) {
            functions.set(name, { node, file: `file://${resolved}` });
          }
        });
        exported.variables.forEach((node, name) => {
          if (!variables.has(name)) {
            variables.set(name, { node, file: `file://${resolved}` });
          }
        });
      }
    }
  }

  return { enums, functions, variables };
}

connection.onInitialize(
  async (params: InitializeParams): Promise<InitializeResult> => {
    let locateFile: ((scriptName: string) => string) | null = null;
    if (params.initializationOptions && params.initializationOptions.coreWasm) {
      locateFile = () => params.initializationOptions.coreWasm;
    }

    await Parser.init({
      locateFile,
    });
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

  const traverse = (node: any) => {
    if (node.type === "ERROR" || node.isMissing) {
      const endChar = node.isMissing
        ? node.endPosition.column + 1
        : node.endPosition.column;

      diagnostics.push({
        severity: DiagnosticSeverity.Error,
        range: {
          start: {
            line: node.startPosition.row,
            character: node.startPosition.column,
          },
          end: { line: node.endPosition.row, character: endChar },
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

  let actualDir = process.cwd();
  if (document.uri.startsWith("file://")) {
    actualDir = path.dirname(fileURLToPath(document.uri));
  }

  const tempOutDir = path.join(os.tmpdir(), "loom-lsp-out");
  if (!fs.existsSync(tempOutDir)) fs.mkdirSync(tempOutDir, { recursive: true });

  connection.console.info(
    `[LSP] Spawning compiler: ${COMPILER_PATH} --stdin --base-dir "${actualDir}"`,
  );

  const child = spawn(
    COMPILER_PATH,
    ["--stdin", "--base-dir", actualDir, "-o", tempOutDir],
    { cwd: actualDir },
  );

  let stdout = "";
  let stderr = "";

  child.stdout.on("data", (data) => {
    stdout += data;
  });
  child.stderr.on("data", (data) => {
    stderr += data;
  });

  child.on("error", (error) => {
    connection.console.error(`[LSP] Spawn error: ${error.message}`);
    connection.sendDiagnostics({ uri: document.uri, diagnostics });
  });

  child.on("close", (code) => {
    connection.console.info(`[LSP] Exited with code ${code}`);
    if (stdout) connection.console.info(`[LSP] STDOUT: ${stdout.trim()}`);
    if (stderr) connection.console.info(`[LSP] STDERR: ${stderr.trim()}`);

    const errorStream = stderr || stdout || "";

    if (errorStream) {
      const errorRegex = /line\s+(\d+),\s+col\s+(\d+):\s+(.*)/gi;
      let match;
      let foundLineMatch = false;

      while ((match = errorRegex.exec(errorStream)) !== null) {
        foundLineMatch = true;
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

      if (!foundLineMatch && errorStream.toLowerCase().includes("error")) {
        diagnostics.push({
          severity: DiagnosticSeverity.Error,
          range: {
            start: { line: 0, character: 0 },
            end: { line: 0, character: 50 },
          },
          message: errorStream.trim(),
          source: "loom-compiler",
        });
      }
    }

    connection.sendDiagnostics({ uri: document.uri, diagnostics });
  });

  child.stdin.write(text);
  child.stdin.end();
});

const findGlobalEnum = (
  root: typeof Node,
  targetName: string,
): typeof Node | null => {
  let found: typeof Node | null = null;
  const traverse = (node: typeof Node) => {
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

const findEnumVariant = (
  enumNode: typeof Node,
  variantName: string,
): typeof Node | null => {
  let found: typeof Node | null = null;
  const traverse = (node: typeof Node) => {
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

const getEnumVariantValueText = (variantNode: typeof Node): string => {
  const valueNode = variantNode.childForFieldName("value");
  if (valueNode) return valueNode.text;

  let enumNode: typeof Node | null = variantNode.parent;
  while (enumNode && enumNode.type !== "enum_definition") {
    enumNode = enumNode.parent;
  }
  if (!enumNode) return "unknown";

  let isStringEnum = false;
  const variants: typeof Node[] = [];

  const traverse = (n: typeof Node) => {
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

function getDeclarationNode(
  cursorNode: typeof Node,
  targetName: string,
  docUri?: string,
): { node: typeof Node; file: string } | null {
  let curr: typeof Node | null = cursorNode;

  while (curr) {
    if (curr.type === "for") {
      const iteratorNode = curr.childForFieldName("iterator");
      if (iteratorNode && iteratorNode.text === targetName) {
        return { node: curr, file: docUri || "" };
      }
    }

    if (curr.type === "block" || curr.type === "source_file") {
      for (let i = 0; i < curr.childCount; i++) {
        const child = curr.child(i)!;

        if (child.startPosition.row > cursorNode.startPosition.row) break;

        if (child.type === "variable_declaration") {
          const nameNode = child.childForFieldName("name");
          if (nameNode && nameNode.text === targetName) {
            return { node: child, file: docUri || "" };
          }
        }
      }
    }

    if (curr.type === "function_definition") {
      const funcNameNode = curr.childForFieldName("name");
      if (funcNameNode && funcNameNode.text === targetName) {
        return { node: curr, file: docUri || "" };
      }

      const paramsNode = curr.childForFieldName("parameters");
      if (paramsNode) {
        let foundParam: typeof Node | null = null;
        const checkParam = (n: typeof Node) => {
          if (n.type === "parameter") {
            const pName = n.childForFieldName("name");
            if (pName && pName.text === targetName) {
              foundParam = n;
            }
          }
          for (let i = 0; i < n.childCount; i++) checkParam(n.child(i)!);
        };
        checkParam(paramsNode);
        if (foundParam) return { node: foundParam, file: docUri || "" };
      }
    }

    curr = curr.parent;
  }

  let root = cursorNode;
  while (root.parent) root = root.parent;

  const globalEnum = findGlobalEnum(root, targetName);
  if (globalEnum) return { node: globalEnum, file: docUri || "" };

  let globalFunc: typeof Node | null = null;
  const findGlobalFunc = (node: typeof Node) => {
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

  if (globalFunc) return { node: globalFunc, file: docUri || "" };

  if (docUri) {
    const tree = trees.get(docUri);
    if (tree) {
      const allSymbols = collectAllSymbols(tree, docUri);
      if (allSymbols.enums.has(targetName)) {
        const sym = allSymbols.enums.get(targetName)!;
        return { node: sym.node, file: sym.file };
      }
      if (allSymbols.functions.has(targetName)) {
        const sym = allSymbols.functions.get(targetName)!;
        return { node: sym.node, file: sym.file };
      }
      if (allSymbols.variables.has(targetName)) {
        const sym = allSymbols.variables.get(targetName)!;
        return { node: sym.node, file: sym.file };
      }
    }
  }

  return null;
}

connection.onDefinition((params: DefinitionParams): Location | null => {
  const tree = trees.get(params.textDocument.uri);
  if (!tree) return null;

  const cursorPoint = {
    row: params.position.line,
    column: params.position.character,
  };
  const cursorNode: typeof Node = tree.rootNode.namedDescendantForPosition(
    cursorPoint,
  );
  if (!cursorNode || cursorNode.type !== "identifier") return null;

  let targetNode: { node: typeof Node; file: string } | null = null;

  if (cursorNode.parent && cursorNode.parent.type === "member_expression") {
    const objNode = cursorNode.parent.childForFieldName("object");
    const propNode = cursorNode.parent.childForFieldName("property");
    if (objNode && propNode && cursorNode.text === propNode.text) {
      const enumNode = findGlobalEnum(tree.rootNode, objNode.text);
      if (enumNode) {
        const variantNode = findEnumVariant(enumNode, propNode.text);
        if (variantNode) {
          targetNode = {
            node: variantNode.childForFieldName("name") || variantNode,
            file: params.textDocument.uri,
          };
        }
      } else {
        const allSymbols = collectAllSymbols(tree, params.textDocument.uri);
        if (allSymbols.enums.has(objNode.text)) {
          const importedEnum = allSymbols.enums.get(objNode.text)!;
          const importedTree = trees.get(importedEnum.file);
          if (importedTree) {
            const variantNode = findEnumVariant(
              importedEnum.node,
              propNode.text,
            );
            if (variantNode) {
              targetNode = {
                node: variantNode.childForFieldName("name") || variantNode,
                file: importedEnum.file,
              };
            }
          }
        }
      }
    }
  }

  if (!targetNode) {
    const declarationData = getDeclarationNode(
      cursorNode,
      cursorNode.text,
      params.textDocument.uri,
    );
    if (declarationData) {
      const nameFieldNode = declarationData.node.childForFieldName("name") ||
        declarationData.node.childForFieldName("iterator") ||
        declarationData.node.namedChild(0) ||
        declarationData.node;
      targetNode = { node: nameFieldNode, file: declarationData.file };
    }
  }

  if (targetNode) {
    return {
      uri: targetNode.file,
      range: {
        start: {
          line: targetNode.node.startPosition.row,
          character: targetNode.node.startPosition.column,
        },
        end: {
          line: targetNode.node.endPosition.row,
          character: targetNode.node.endPosition.column,
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
  const cursorNode: typeof Node = tree.rootNode.namedDescendantForPosition(
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
      } else {
        const allSymbols = collectAllSymbols(tree, params.textDocument.uri);
        if (allSymbols.enums.has(objNode.text)) {
          const importedEnum = allSymbols.enums.get(objNode.text)!;
          const variantNode = findEnumVariant(
            importedEnum.node,
            propNode.text,
          );
          if (variantNode) {
            const valStr = getEnumVariantValueText(variantNode);
            hoverText =
              `\`\`\`loom\n(enum variant) ${objNode.text}.${propNode.text} = ${valStr}\n\`\`\``;
          }
        }
      }
    }
  }

  if (hoverText == "") {
    const declarationData = getDeclarationNode(
      cursorNode,
      cursorNode.text,
      params.textDocument.uri,
    );

    if (declarationData) {
      const declarationNode = declarationData.node;
      if (declarationNode.type === "enum_definition") {
        const name = declarationNode.childForFieldName("name")?.text ||
          "unknown";
        hoverText = `\`\`\`loom\nenum ${name}\n\`\`\``;
      } else if (declarationNode.type === "enum_variant") {
        let p: typeof Node | null = declarationNode.parent;
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
        const collectVariants = (node: typeof Node) => {
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
      } else {
        const allSymbols = collectAllSymbols(tree, params.textDocument.uri);
        if (allSymbols.enums.has(enumName)) {
          const importedEnum = allSymbols.enums.get(enumName)!;
          const enumVariants: CompletionItem[] = [];
          const collectVariants = (node: typeof Node) => {
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
          collectVariants(importedEnum.node);
          return enumVariants;
        }
      }
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
    const findEnums = (node: typeof Node) => {
      if (node.type === "enum_definition") {
        const nameNode = node.childForFieldName("name");
        if (nameNode) {
          enumsSet.add({ name: nameNode.text });
        }
      }
      for (let i = 0; i < node.childCount; i++) findEnums(node.child(i)!);
    };
    findEnums(tree.rootNode);

    const allSymbols = collectAllSymbols(tree, params.textDocument.uri);
    for (const [varName, varData] of allSymbols.variables) {
      variables.add({
        name: varName,
        type: varData.node.childForFieldName("type")?.text || "unknown",
        const: varData.node.childForFieldName("keyword")?.text === "const",
      });
    }
    for (const [funcName, funcData] of allSymbols.functions) {
      const params: { name: string; type: string }[] = [];
      const paramsNode = funcData.node.childForFieldName("parameters");
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
        name: funcName,
        params,
        type: funcData.node.childForFieldName("type")?.text || "void",
      });
    }
    for (const [enumName] of allSymbols.enums) {
      enumsSet.add({ name: enumName });
    }

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
          detail: `func ${fn.name}(${
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

    if (lineText.match(/:\s*[a-zA-Z_]*$/)) {
      addEnums();
      return [
        ...items,
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
        {
          label: "append",
          kind: CompletionItemKind.Function,
          detail: "func append(list: T[], value: T): T[]",
        },
        {
          label: "insert",
          kind: CompletionItemKind.Function,
          detail: "func insert(list: T[], index: int, value: T): T[]",
        },
        {
          label: "remove",
          kind: CompletionItemKind.Function,
          detail: "func remove(list: T[], index: int): T[]",
        },
        {
          label: "len",
          kind: CompletionItemKind.Function,
          detail: "func len(list: string | any[]): int",
        },
      ];
    }

    if (lineText.match(/(?:export|extern)\s+$/)) {
      return ["let", "const", "enum", "func"].map((keyword) => ({
        label: keyword,
        kind: CompletionItemKind.Keyword,
      }));
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
      "import",
      "export",
      "extern",
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
