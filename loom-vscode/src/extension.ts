import {
  commands,
  ExtensionContext,
  languages,
  window,
  workspace,
} from "vscode";
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from "vscode-languageclient/node";
import { loadLoomGrammar } from "./parser";
import { createSemanticTokensProvider, legend } from "./semanticTokens";

let client: LanguageClient | undefined;

export async function activate(context: ExtensionContext) {
  const config = workspace.getConfiguration("loom");
  const lspEnabled = config.get<boolean>("lspEnabled", true);

  if (lspEnabled) {
    await startLanguageServer();
  }

  try {
    const { parser, query } = await loadLoomGrammar(
      context.extensionUri.fsPath,
    );
    context.subscriptions.push(
      languages.registerDocumentSemanticTokensProvider(
        { language: "loom" },
        createSemanticTokensProvider(parser, query),
        legend,
      ),
    );
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    console.error(
      "Loom: failed to load tree-sitter grammar for semantic highlighting.",
      err,
    );
    window.showErrorMessage(
      `Loom: failed to load tree-sitter grammar for semantic highlighting. ${message}`,
    );
  }

  context.subscriptions.push(
    workspace.onDidChangeConfiguration(async (event) => {
      if (
        event.affectsConfiguration("loom.lspEnabled") ||
        event.affectsConfiguration("loom.executablePath")
      ) {
        await stopLanguageServer();
        const newConfig = workspace.getConfiguration("loom");
        if (newConfig.get<boolean>("lspEnabled", true)) {
          await startLanguageServer();
        }
      }
    }),
  );

  context.subscriptions.push(
    commands.registerCommand("loom.restartServer", async () => {
      await stopLanguageServer();
      const newConfig = workspace.getConfiguration("loom");
      if (newConfig.get<boolean>("lspEnabled", true)) {
        await startLanguageServer();
        window.showInformationMessage("Loom language server restarted.");
      }
    }),
  );

  context.subscriptions.push(
    commands.registerCommand("loom.showOutput", () => {
      client?.outputChannel.show();
    }),
  );
}

async function startLanguageServer(): Promise<void> {
  const config = workspace.getConfiguration("loom");
  const loomExecutable = config.get<string>("executablePath", "loom");

  const serverOptions: ServerOptions = {
    run: {
      command: loomExecutable,
      args: ["--lsp"],
      transport: TransportKind.stdio,
    },
    debug: {
      command: loomExecutable,
      args: ["--lsp"],
      transport: TransportKind.stdio,
    },
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: "file", language: "loom" }],
    synchronize: {
      fileEvents: workspace.createFileSystemWatcher("**/*.loom"),
    },
    traceOutputChannel: window.createOutputChannel(
      "Loom Language Server Trace",
    ),
    outputChannelName: "Loom Language Server",
  };

  client = new LanguageClient(
    "loom",
    "Loom Language Server",
    serverOptions,
    clientOptions,
  );

  try {
    await client.start();
  } catch (err) {
    window.showErrorMessage(
      `Loom: failed to start '${loomExecutable} --lsp'. ` +
        "Make sure the loom compiler is installed and on your PATH, or set loom.executablePath. " +
        `(${err instanceof Error ? err.message : String(err)})`,
    );
  }
}

async function stopLanguageServer(): Promise<void> {
  if (client) {
    await client.stop();
    client = undefined;
  }
}

export async function deactivate(): Promise<void> {
  await stopLanguageServer();
}
