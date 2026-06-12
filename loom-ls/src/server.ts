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

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments(TextDocument);

let parser: Parser;
const trees: Map<string, any> = new Map();

connection.onInitialize(
  async (params: InitializeParams): Promise<InitializeResult> => {
    await Parser.init();
    parser = new Parser();

    const LoomLanguage = await Language.load(
      "/home/grady.link/loom/tree-sitter-loom/tree-sitter-loom.wasm", // TODO: find a better way to provide this path
    );
    parser.setLanguage(LoomLanguage);

    return {
      capabilities: {
        textDocumentSync: TextDocumentSyncKind.Incremental,
        definitionProvider: true,
        hoverProvider: true, // Enable Hover
        completionProvider: {
          resolveProvider: false,
          triggerCharacters: ["$"],
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
  connection.sendDiagnostics({ uri: document.uri, diagnostics });
});

function getDeclarationNode(rootNode: Node, targetName: string): Node | null {
  let definitionNode: Node | null = null;

  const find = (node: Node) => {
    if (definitionNode) return;

    const isAssignment = node.type === "variable_declaration" ||
      node.type === "function_definition" ||
      node.type === "parameter";

    if (isAssignment) {
      const nameNode = node.childForFieldName("name") || node.namedChild(0);
      if (nameNode && nameNode.text === targetName) {
        definitionNode = node;
        return;
      }
    }

    for (let i = 0; i < node.childCount; i++) {
      find(node.child(i)!);
    }
  };

  find(rootNode);
  return definitionNode;
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

  const declarationNode = getDeclarationNode(tree.rootNode, cursorNode.text);

  if (declarationNode) {
    const nameNode = declarationNode.childForFieldName("name") ||
      declarationNode.namedChild(0);
    const targetNode = nameNode || declarationNode;

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

  const declarationNode = getDeclarationNode(tree.rootNode, cursorNode.text);

  if (declarationNode) {
    let hoverText = "";

    if (
      declarationNode.type === "variable_declaration" ||
      declarationNode.type === "parameter"
    ) {
      const isParam = declarationNode.type === "parameter";
      const name = declarationNode.childForFieldName("name")?.text || "unknown";
      const type = declarationNode.childForFieldName("type")?.text || "unknown";

      const keyword = isParam
        ? "(parameter)"
        : (declarationNode.childForFieldName("keyword")?.text || "let");

      hoverText = `\`\`\`loom\n${keyword} ${name}: ${type}\n\`\`\``;
    } else if (declarationNode.type === "function_definition") {
      const name = declarationNode.childForFieldName("name")?.text || "unknown";
      const paramsText =
        declarationNode.childForFieldName("parameters")?.text || "";
      const typeNode = declarationNode.childForFieldName("type");
      const returnType = typeNode ? `: ${typeNode.text}` : "";

      hoverText =
        `\`\`\`loom\nfunc ${name}(${paramsText})${returnType}\n\`\`\``;
    }

    if (hoverText) {
      return {
        contents: {
          kind: "markdown",
          value: hoverText,
        },
      };
    }
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

    // type
    if (lineText.match(/:\s*[a-zA-Z_]*$/)) {
      return [
        { label: "int", kind: CompletionItemKind.TypeParameter },
        { label: "bool", kind: CompletionItemKind.TypeParameter },
        { label: "void", kind: CompletionItemKind.TypeParameter },
      ];
    }

    // var/function name
    if (lineText.match(/(let|const|func)\s+[a-z_0-9]*$/i)) {
      return [];
    }

    // param name
    if (lineText.match(/func\s+[a-z_0-9]+\s*\([^)]*$/i)) {
      return [];
    }

    // comment
    if (lineText.includes("--")) {
      return [];
    }

    const variables = new Set<string>();
    const functions = new Set<string>();

    function findSymbols(node: Node) {
      if (node.type === "variable_declaration" || node.type === "parameter") {
        const nameNode = node.childForFieldName("name");
        if (nameNode) variables.add(nameNode.text);
      } else if (node.type === "function_definition") {
        const nameNode = node.childForFieldName("name");
        if (nameNode) functions.add(nameNode.text);
      }
      for (let i = 0; i < node.childCount; i++) findSymbols(node.child(i)!);
    }
    findSymbols(tree.rootNode);

    // variable
    if (lineText.match(/\$[a-z_0-9]*$/i)) {
      for (const v of variables) {
        items.push({
          label: v,
          kind: CompletionItemKind.Variable,
          detail: "variable",
        });
      }
      return items;
    }

    // expression
    if (lineText.match(/return\s+|(?:(?:let|const)\s+)?[a-z_0-9]+\s*=\s*/i)) {
      return [
        { label: "true", kind: CompletionItemKind.Keyword },
        { label: "false", kind: CompletionItemKind.Keyword },
      ];
    }

    const cursorPoint = {
      row: params.position.line,
      column: Math.max(0, params.position.character - 1),
    };
    const cursorNode = tree.rootNode.descendantForPosition(cursorPoint);

    let inBlock = false;
    let curr: Node | null = cursorNode;
    while (curr) {
      if (curr.type === "block") {
        inBlock = true;
        break;
      }
      curr = curr.parent;
    }

    const keywords = [
      "let",
      "const",
      "func",
      "if",
      "else",
      "return",
      "true",
      "false",
    ];
    for (const kw of keywords) {
      items.push({ label: kw, kind: CompletionItemKind.Keyword });
    }

    if (inBlock) {
      for (const fn of functions) {
        items.push({
          label: fn,
          kind: CompletionItemKind.Function,
          detail: "function",
        });
      }
    }

    return items;
  },
);

documents.listen(connection);
connection.listen();
