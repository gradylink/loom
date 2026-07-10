import * as path from "path";
import * as fs from "fs";
import { commands, ExtensionContext, window, workspace } from "vscode";
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from "vscode-languageclient/node";

let client: LanguageClient | undefined;

export async function activate(context: ExtensionContext) {
  const config = workspace.getConfiguration("loom");
  const lspEnabled = config.get<boolean>("lspEnabled", true);

  if (lspEnabled) {
    await startLanguageServer(context);
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
          await startLanguageServer(context);
        }
      }
    }),
  );

  context.subscriptions.push(
    commands.registerCommand("loom.restartServer", async () => {
      await stopLanguageServer();
      const newConfig = workspace.getConfiguration("loom");
      if (newConfig.get<boolean>("lspEnabled", true)) {
        await startLanguageServer(context);
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

async function startLanguageServer(context: ExtensionContext): Promise<void> {
  const serverModule = context.asAbsolutePath(
    path.join("out", "server.js"),
  );

  const wasmPath = context.asAbsolutePath(
    path.join("out", "tree-sitter-loom.wasm"),
  );

  const coreWasmPath = context.asAbsolutePath(
    path.join("out", "web-tree-sitter.wasm"),
  );

  if (!fs.existsSync(serverModule)) {
    window.showErrorMessage(
      `Loom: LSP server not found at ${serverModule}. ` +
        "Please rebuild the extension.",
    );
    return;
  }

  if (!fs.existsSync(wasmPath)) {
    window.showWarningMessage(
      `Loom: WASM parser not found at ${wasmPath}. ` +
        "Tree-sitter diagnostics will be unavailable.",
    );
  }

  if (!fs.existsSync(coreWasmPath)) {
    window.showWarningMessage(
      `Loom: Internal WASM parser not found at ${coreWasmPath}. ` +
        "Tree-sitter diagnostics will be unavailable.",
    );
  }

  const config = workspace.getConfiguration("loom");
  const loomExecutable = config.get<string>("executablePath", "loom");

  const serverOptions: ServerOptions = {
    run: {
      module: serverModule,
      transport: TransportKind.ipc,
      options: {
        env: {
          ...process.env,
          LOOM_WASM_PATH: wasmPath,
          LOOM_EXECUTABLE: loomExecutable,
        },
      },
    },
    debug: {
      module: serverModule,
      transport: TransportKind.ipc,
      options: {
        execArgv: ["--nolazy", "--inspect=6009"],
        env: {
          ...process.env,
          LOOM_WASM_PATH: wasmPath,
          LOOM_EXECUTABLE: loomExecutable,
        },
      },
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
    initializationOptions: {
      wasmPath,
      loomExecutable,
      coreWasm: coreWasmPath,
    },
  };

  client = new LanguageClient(
    "loom",
    "Loom Language Server",
    serverOptions,
    clientOptions,
  );

  await client.start();
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
