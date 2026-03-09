import { workspace, ExtensionContext, commands, window, Uri } from "vscode";

import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from "vscode-languageclient/node";

let client: LanguageClient;

// ← Update these two paths for your machine
const LUMA_BIN = "/home/connor/projects/lsp_servers/Luma/luma";
const LOG_FILE = "/tmp/luma_lsp.log";

export function activate(context: ExtensionContext) {
  // We wrap the binary in a shell one-liner so stderr goes to the log file
  // without touching the stdio streams the LSP client uses for JSON-RPC.
  // stdout (JSON-RPC) is untouched; only stderr is redirected.
  const serverOptions: ServerOptions = {
    command: "sh",
    transport: TransportKind.stdio,
    args: [
      "-c",
      // The exec replaces the shell so the PID is the luma process itself.
      // stderr goes to the log file; stdout stays connected to the pipe.
      `exec "${LUMA_BIN}" -lsp 2>>"${LOG_FILE}"`,
    ],
  };

  const outputChannel = window.createOutputChannel("Luma LSP");
  outputChannel.appendLine(`LSP binary:  ${LUMA_BIN}`);
  outputChannel.appendLine(`LSP log:     ${LOG_FILE}`);
  outputChannel.appendLine(`Tail with:   tail -f ${LOG_FILE}`);
  outputChannel.show(true);

  const traceChannel = window.createOutputChannel("Luma LSP Trace");

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: "file", language: "luma" }],
    synchronize: {
      fileEvents: workspace.createFileSystemWatcher("**/.clientrc"),
    },
    outputChannel,
    traceOutputChannel: traceChannel,
  };

  client = new LanguageClient(
    "luma-lsp",
    "Luma LSP",
    serverOptions,
    clientOptions
  );

  client.start();

  // "Luma: Open LSP Log" in Ctrl+Shift+P
  context.subscriptions.push(
    commands.registerCommand("luma.openLog", () => {
      workspace.openTextDocument(Uri.file(LOG_FILE)).then(
        (doc) => window.showTextDocument(doc),
        () => window.showErrorMessage(`Could not open: ${LOG_FILE}`)
      );
    })
  );
}

export function deactivate(): Thenable<void> | undefined {
  if (!client) return undefined;
  return client.stop();
}