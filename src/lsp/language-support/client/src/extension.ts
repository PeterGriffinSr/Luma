import { workspace, ExtensionContext, commands, window, Uri } from "vscode";
import * as path from "path";
import * as fs from "fs";

import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from "vscode-languageclient/node";

let client: LanguageClient;

const LOG_FILE = "/tmp/luma_lsp.log";

function findLumaBinary(context: ExtensionContext): string | null {
  // Search candidate paths from most to least specific.
  // __dirname = .../src/lsp/language-support/client/out
  const candidates = [
    // Settings override — highest priority
    workspace.getConfiguration("luma").get<string>("serverPath"),
    // Walk up from extension dir to find the binary
    path.resolve(__dirname, "..", "..", "..", "..", "..", "luma"),   // 5 up = project root
    path.resolve(__dirname, "..", "..", "..", "..", "luma"),         // 4 up
    path.resolve(__dirname, "..", "..", "..", "luma"),               // 3 up
    // Workspace root
    ...(workspace.workspaceFolders?.map(f => path.join(f.uri.fsPath, "luma")) ?? []),
    // System PATH fallback
    "luma",
  ];

  for (const candidate of candidates) {
    if (!candidate) continue;
    if (candidate === "luma") return candidate; // PATH fallback, can't fs.existsSync
    try {
      fs.accessSync(candidate, fs.constants.X_OK);
      return candidate;
    } catch {
      // not found or not executable, try next
    }
  }
  return null;
}

export function activate(context: ExtensionContext) {
  const outputChannel = window.createOutputChannel("Luma LSP");
  outputChannel.show(true);

  const lumaBin = findLumaBinary(context);

  if (!lumaBin) {
    outputChannel.appendLine("[LSP] ERROR: Could not find luma binary!");
    outputChannel.appendLine("[LSP] Set 'luma.serverPath' in settings.json to the full path of your luma binary.");
    window.showErrorMessage(
      "Luma LSP: binary not found. Set \"luma.serverPath\" in your settings.json.",
      "Open Settings"
    ).then(choice => {
      if (choice === "Open Settings") {
        commands.executeCommand("workbench.action.openSettings", "luma.serverPath");
      }
    });
    return;
  }

  outputChannel.appendLine(`LSP binary:  ${lumaBin}`);
  outputChannel.appendLine(`LSP log:     ${LOG_FILE}`);
  outputChannel.appendLine(`Tail with:   tail -f ${LOG_FILE}`);

  const serverOptions: ServerOptions = {
    command: "sh",
    transport: TransportKind.stdio,
    args: ["-c", `exec "${lumaBin}" -lsp 2>>"${LOG_FILE}"`],
  };

  const traceChannel = window.createOutputChannel("Luma LSP Trace");

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: "file", language: "luma" }],
    synchronize: {
      fileEvents: workspace.createFileSystemWatcher("**/.clientrc"),
    },
    outputChannel,
    traceOutputChannel: traceChannel,
    errorHandler: (() => {
      let errorCount = 0;
      return {
        error: (_error: any, _message: any, _count: any) => {
          errorCount++;
          outputChannel.appendLine(`[LSP] Protocol error #${errorCount}`);
          if (errorCount >= 10) {  // raise threshold
            outputChannel.appendLine("[LSP] Too many errors, shutting down.");
            return { action: 2 };
          }
          return { action: 1 };
        },
        closed: () => {
          outputChannel.appendLine("[LSP] Connection closed — restarting...");
          return { action: 1 };  // was 2 (Shutdown), change to 1 (Restart) so VS Code reconnects
        },
      };
    })(),
  };

  client = new LanguageClient("luma-lsp", "Luma LSP", serverOptions, clientOptions);

  client.start().then(() => {
    outputChannel.appendLine("[LSP] Client started successfully");
  }).catch((err) => {
    outputChannel.appendLine(`[LSP] Client failed to start: ${err}`);
    window.showErrorMessage(`Luma LSP failed to start: ${err.message}`);
  });

  context.subscriptions.push(
    commands.registerCommand("luma.openLog", () => {
      workspace.openTextDocument(Uri.file(LOG_FILE)).then(
        (doc) => window.showTextDocument(doc),
        () => window.showErrorMessage(`Could not open: ${LOG_FILE}`)
      );
    }),
    commands.registerCommand("luma.showBinaryPath", () => {
      window.showInformationMessage(`Luma binary: ${lumaBin}`);
    })
  );
}

export function deactivate(): Thenable<void> | undefined {
  if (!client) return undefined;
  return client.stop();
}