#include <stdlib.h>
#include "lsp.h"

// ---------------------------------------------------------------------------
// Helper: serialize diagnostics to a heap-allocated string.
// Caller must free() the returned pointer.
// ---------------------------------------------------------------------------
static char *build_diagnostics_params(const char *uri,
                                      LSPDiagnostic *diagnostics,
                                      size_t diag_count) {
  size_t buf_size = 512 + diag_count * 1024;
  char *params = (char *)malloc(buf_size);
  if (!params)
    return NULL;
  serialize_diagnostics_to_json(uri, diagnostics, diag_count,
                                params, buf_size);
  return params;
}

// ---------------------------------------------------------------------------
// Helper: run analysis on a document and publish diagnostics.
// Uses its own arena so it never touches the caller's temp_arena.
// ---------------------------------------------------------------------------
static void analyze_and_publish(LSPServer *server, LSPDocument *doc,
                                const char *uri) {
  BuildConfig config = {0};
  config.check_mem = true;
  lsp_document_analyze(doc, server, &config);

  ArenaAllocator diag_arena;
  arena_allocator_init(&diag_arena, 64 * 1024);

  size_t diag_count = 0;
  LSPDiagnostic *diagnostics = lsp_diagnostics(doc, &diag_count, &diag_arena);

  fprintf(stderr, "[LSP] Sending %zu diagnostics\n", diag_count);

  char *params = build_diagnostics_params(uri, diagnostics, diag_count);
  if (params) {
    lsp_send_notification("textDocument/publishDiagnostics", params);
    free(params);
  }

  arena_destroy(&diag_arena);
}

// ---------------------------------------------------------------------------
// Main message dispatcher
// ---------------------------------------------------------------------------
void lsp_handle_message(LSPServer *server, const char *message) {
  if (!server || !message)
    return;

  fprintf(stderr, "[LSP] Received message: %.300s\n", message);

  LSPMethod method     = lsp_parse_method(message);
  int       request_id = extract_int(message, "id");

  fprintf(stderr, "[LSP] method=%d request_id=%d\n", (int)method, request_id);

  // Small arena for string extraction only.
  // All heavy allocations use their own dedicated arenas below.
  ArenaAllocator temp_arena;
  arena_allocator_init(&temp_arena, 32 * 1024);

  switch (method) {

  // --------------------------------------------------------------------------
  case LSP_METHOD_INITIALIZE: {
    fprintf(stderr, "[LSP] Handling initialize\n");

    if (request_id < 0) {
      fprintf(stderr, "[LSP] ERROR: No valid request_id for initialize\n");
      break;
    }

    // rootUri may be JSON null — only extract when it is a real string
    const char *raw_root = find_json_value(message, "rootUri");
    if (raw_root && *raw_root == '"') {
      const char *root_uri = extract_string(message, "rootUri", &temp_arena);
      if (root_uri)
        build_module_registry(server, root_uri);
    }

    server->initialized = true;

    const char *capabilities =
        "{"
        "\"capabilities\":{"
        "\"textDocumentSync\":1,"
        "\"hoverProvider\":true,"
        "\"definitionProvider\":true,"
        "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\"]},"
        "\"documentSymbolProvider\":true"
        "},"
        "\"serverInfo\":{\"name\":\"Luma LSP\",\"version\":\"0.1.0\"}"
        "}";
    lsp_send_response(request_id, capabilities);
    break;
  }

  // --------------------------------------------------------------------------
  case LSP_METHOD_INITIALIZED:
    fprintf(stderr, "[LSP] Client initialized (notification)\n");
    break;

  // --------------------------------------------------------------------------
  case LSP_METHOD_TEXT_DOCUMENT_DID_OPEN: {
    fprintf(stderr, "[LSP] Handling didOpen\n");

    const char *uri = extract_string(message, "uri", &temp_arena);
    int version     = extract_int(message, "version");

    // "text" comes after "languageId" in the textDocument object.
    const char *after_langid = find_json_value(message, "languageId");
    const char *text = extract_string(
        after_langid ? after_langid : message, "text", &temp_arena);

    if (!uri)  { fprintf(stderr, "[LSP] didOpen: missing uri\n");  break; }
    if (!text) { fprintf(stderr, "[LSP] didOpen: missing text\n"); break; }

    fprintf(stderr, "[LSP] Opening document: %s (version %d, %zu bytes)\n",
            uri, version, strlen(text));

    // Clear the module negative cache — the user may have just created a
    // new .lx file that was previously not found.
    lsp_negative_cache_clear();

    LSPDocument *doc = lsp_document_open(server, uri, text, version);
    if (doc)
      analyze_and_publish(server, doc, uri);
    break;
  }

  // --------------------------------------------------------------------------
  case LSP_METHOD_TEXT_DOCUMENT_DID_CHANGE: {
    fprintf(stderr, "[LSP] Handling didChange\n");

    const char *uri = extract_string(message, "uri", &temp_arena);
    int version     = extract_int(message, "version");

    // "text" lives inside contentChanges[0].
    const char *changes_val = find_json_value(message, "contentChanges");
    const char *text = extract_string(
        changes_val ? changes_val : message, "text", &temp_arena);

    fprintf(stderr, "[LSP] didChange: uri=%s version=%d text_len=%zu\n",
            uri ? uri : "NULL", version, text ? strlen(text) : 0);

    if (!uri)  { fprintf(stderr, "[LSP] didChange: missing uri\n");  break; }
    if (!text) { fprintf(stderr, "[LSP] didChange: missing text\n"); break; }

    lsp_document_update(server, uri, text, version);

    LSPDocument *doc = lsp_document_find(server, uri);
    if (doc)
      analyze_and_publish(server, doc, uri);
    break;
  }

  // --------------------------------------------------------------------------
  case LSP_METHOD_TEXT_DOCUMENT_DID_CLOSE: {
    fprintf(stderr, "[LSP] Handling didClose\n");
    const char *uri = extract_string(message, "uri", &temp_arena);
    if (uri)
      lsp_document_close(server, uri);
    break;
  }

  // --------------------------------------------------------------------------
  case LSP_METHOD_TEXT_DOCUMENT_HOVER: {
    fprintf(stderr, "[LSP] Handling hover\n");

    const char *uri      = extract_string(message, "uri", &temp_arena);
    LSPPosition position = extract_position(message);

    if (!uri) { lsp_send_response(request_id, "null"); break; }

    LSPDocument *doc = lsp_document_find(server, uri);
    if (!doc) { lsp_send_response(request_id, "null"); break; }

    ArenaAllocator hover_arena;
    arena_allocator_init(&hover_arena, 16 * 1024);

    const char *hover_text = lsp_hover(doc, position, &hover_arena);
    if (hover_text) {
      size_t ht_len  = strlen(hover_text);
      size_t buf_len = ht_len * 2 + 128;
      char *escaped  = (char *)malloc(buf_len);
      char *result   = (char *)malloc(buf_len + 64);
      if (escaped && result) {
        char *d = escaped;
        for (const char *s = hover_text; *s; s++) {
          if      (*s == '\\') { *d++ = '\\'; *d++ = '\\'; }
          else if (*s == '"')  { *d++ = '\\'; *d++ = '"';  }
          else if (*s == '\n') { *d++ = '\\'; *d++ = 'n';  }
          else if (*s == '\r') { *d++ = '\\'; *d++ = 'r';  }
          else                 { *d++ = *s; }
        }
        *d = '\0';
        snprintf(result, buf_len + 64,
                 "{\"contents\":{\"kind\":\"markdown\",\"value\":\"%s\"}}",
                 escaped);
        lsp_send_response(request_id, result);
      } else {
        lsp_send_response(request_id, "null");
      }
      free(escaped);
      free(result);
    } else {
      lsp_send_response(request_id, "null");
    }

    arena_destroy(&hover_arena);
    break;
  }

  // --------------------------------------------------------------------------
  case LSP_METHOD_TEXT_DOCUMENT_DEFINITION: {
    fprintf(stderr, "[LSP] Handling definition\n");

    const char *uri      = extract_string(message, "uri", &temp_arena);
    LSPPosition position = extract_position(message);

    if (!uri) { lsp_send_response(request_id, "null"); break; }

    LSPDocument *doc = lsp_document_find(server, uri);
    if (!doc) { lsp_send_response(request_id, "null"); break; }

    ArenaAllocator def_arena;
    arena_allocator_init(&def_arena, 16 * 1024);

    LSPLocation *loc = lsp_definition(doc, position, &def_arena);
    if (loc) {
      size_t result_len = strlen(loc->uri) + 256;
      char *result = (char *)malloc(result_len);
      if (result) {
        snprintf(result, result_len,
                 "{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%d,"
                 "\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}",
                 loc->uri,
                 loc->range.start.line, loc->range.start.character,
                 loc->range.end.line,   loc->range.end.character);
        lsp_send_response(request_id, result);
        free(result);
      } else {
        lsp_send_response(request_id, "null");
      }
    } else {
      lsp_send_response(request_id, "null");
    }

    arena_destroy(&def_arena);
    break;
  }

  // --------------------------------------------------------------------------
  case LSP_METHOD_TEXT_DOCUMENT_COMPLETION: {
    fprintf(stderr, "[LSP] Handling completion\n");

    const char *uri      = extract_string(message, "uri", &temp_arena);
    LSPPosition position = extract_position(message);

    fprintf(stderr, "[LSP] completion: uri=%s line=%d char=%d\n",
            uri ? uri : "NULL", position.line, position.character);

    if (!uri) { lsp_send_response(request_id, "{\"items\":[]}"); break; }

    LSPDocument *doc = lsp_document_find(server, uri);
    if (!doc) {
      fprintf(stderr, "[LSP] completion: document not found\n");
      lsp_send_response(request_id, "{\"items\":[]}");
      break;
    }

    // Dedicated 512KB arena — enough for all snippet strings + scope symbols
    ArenaAllocator comp_arena;
    arena_allocator_init(&comp_arena, 512 * 1024);

    size_t count = 0;
    LSPCompletionItem *items =
        lsp_completion(doc, position, &count, &comp_arena);

    fprintf(stderr, "[LSP] completion: %zu items\n", count);

    if (items && count > 0) {
      // 640 bytes per item covers the longest snippets with JSON escaping
      size_t out_size = count * 640 + 64;
      char *result = (char *)malloc(out_size);
      if (result) {
        serialize_completion_items(items, count, result, out_size);
        lsp_send_response(request_id, result);
        free(result);
      } else {
        lsp_send_response(request_id, "{\"items\":[]}");
      }
    } else {
      lsp_send_response(request_id, "{\"items\":[]}");
    }

    arena_destroy(&comp_arena);
    break;
  }

  // --------------------------------------------------------------------------
  case LSP_METHOD_TEXT_DOCUMENT_DOCUMENT_SYMBOL: {
    fprintf(stderr, "[LSP] Handling documentSymbol\n");

    const char *uri = extract_string(message, "uri", &temp_arena);
    if (!uri) { lsp_send_response(request_id, "[]"); break; }

    LSPDocument *doc = lsp_document_find(server, uri);
    if (!doc) { lsp_send_response(request_id, "[]"); break; }

    ArenaAllocator sym_arena;
    arena_allocator_init(&sym_arena, 64 * 1024);

    size_t sym_count = 0;
    LSPDocumentSymbol **symbols =
        lsp_document_symbols(doc, &sym_count, &sym_arena);

    if (!symbols || sym_count == 0) {
      arena_destroy(&sym_arena);
      lsp_send_response(request_id, "[]");
      break;
    }

    size_t buf_size = sym_count * 512 + 16;
    char *result = (char *)malloc(buf_size);
    if (!result) {
      arena_destroy(&sym_arena);
      lsp_send_response(request_id, "[]");
      break;
    }

    size_t off = 0;
    off += snprintf(result + off, buf_size - off, "[");
    for (size_t i = 0; i < sym_count && off < buf_size - 2; i++) {
      LSPDocumentSymbol *sym = symbols[i];
      if (i > 0)
        off += snprintf(result + off, buf_size - off, ",");
      off += snprintf(result + off, buf_size - off,
                      "{\"name\":\"%s\",\"kind\":%d,"
                      "\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                      "\"end\":{\"line\":%d,\"character\":%d}},"
                      "\"selectionRange\":{\"start\":{\"line\":%d,"
                      "\"character\":%d},\"end\":{\"line\":%d,"
                      "\"character\":%d}}}",
                      sym->name ? sym->name : "", (int)sym->kind,
                      sym->range.start.line, sym->range.start.character,
                      sym->range.end.line,   sym->range.end.character,
                      sym->selection_range.start.line,
                      sym->selection_range.start.character,
                      sym->selection_range.end.line,
                      sym->selection_range.end.character);
    }
    snprintf(result + off, buf_size - off, "]");

    lsp_send_response(request_id, result);
    free(result);
    arena_destroy(&sym_arena);
    break;
  }

  // --------------------------------------------------------------------------
  case LSP_METHOD_SHUTDOWN:
    fprintf(stderr, "[LSP] Handling shutdown\n");
    lsp_send_response(request_id, "null");
    break;

  case LSP_METHOD_EXIT:
    fprintf(stderr, "[LSP] Exiting\n");
    arena_destroy(&temp_arena);
    exit(0);
    break;

  default:
    fprintf(stderr, "[LSP] Unhandled method %d\n", (int)method);
    if (request_id >= 0)
      lsp_send_error(request_id, -32601, "Method not found");
    break;
  }

  arena_destroy(&temp_arena);
}