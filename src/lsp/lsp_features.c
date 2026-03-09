#include "lsp.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Internal: JSON-escape a string into a static arena buffer.
// Double-backslash and double-quote so the result is safe inside a JSON string.
// ---------------------------------------------------------------------------
static const char *json_escape_hover(const char *src, ArenaAllocator *arena) {
  if (!src) return "";
  size_t len = strlen(src);
  // Worst case: every char doubles
  char *dst = arena_alloc(arena, len * 2 + 1, 1);
  if (!dst) return src;
  char *out = dst;
  while (*src) {
    switch (*src) {
    case '"':  *out++ = '\\'; *out++ = '"';  break;
    case '\\': *out++ = '\\'; *out++ = '\\'; break;
    case '\n': *out++ = '\\'; *out++ = 'n';  break;
    case '\r': *out++ = '\\'; *out++ = 'r';  break;
    case '\t': *out++ = '\\'; *out++ = 't';  break;
    default:   *out++ = *src; break;
    }
    src++;
  }
  *out = '\0';
  return dst;
}

// ---------------------------------------------------------------------------
// Internal: find token at a 0-based LSP position.
// Tokens store 1-based line/col — convert before comparing.
// ---------------------------------------------------------------------------
static Token *find_token_at(LSPDocument *doc, LSPPosition pos) {
  if (!doc || !doc->tokens) return NULL;
  for (size_t i = 0; i < doc->token_count; i++) {
    Token *tok = &doc->tokens[i];
    // Convert 1-based token coords to 0-based LSP coords
    int tok_line = (int)tok->line - 1;
    int tok_col  = (int)tok->col  - 1;
    int tok_end  = tok_col + (int)tok->length;
    if (tok_line == pos.line &&
        tok_col <= pos.character && pos.character < tok_end) {
      return tok;
    }
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// Internal: build a markdown hover string for a symbol.
// ---------------------------------------------------------------------------
static const char *hover_for_symbol(Symbol *sym, const char *module_alias,
                                     ArenaAllocator *arena) {
  if (!sym || !sym->name) return NULL;

  const char *type_str = sym->type ? type_to_string(sym->type, arena) : "unknown";

  // Determine declaration keyword
  const char *kw = "let";
  if (sym->type) {
    switch (sym->type->type) {
    case AST_TYPE_FUNCTION: kw = "fn";     break;
    case AST_TYPE_STRUCT:   kw = "struct"; break;
    default:
      kw = sym->is_mutable ? "let" : "const";
      break;
    }
  }

  // Build the code line shown in the hover
  char code[512];
  if (module_alias) {
    snprintf(code, sizeof(code), "%s::%s: %s", module_alias, sym->name, type_str);
  } else {
    snprintf(code, sizeof(code), "%s %s: %s", kw, sym->name, type_str);
  }

  // Build visibility/mutability tags
  char tags[128] = "";
  if (sym->is_public)   strncat(tags, "public ", sizeof(tags) - strlen(tags) - 1);
  if (sym->is_mutable)  strncat(tags, "mutable ", sizeof(tags) - strlen(tags) - 1);

  // Final markdown
  size_t buf_size = strlen(code) + strlen(tags) + 128;
  char *result = arena_alloc(arena, buf_size, 1);
  if (!result) return NULL;

  if (strlen(tags) > 0) {
    snprintf(result, buf_size, "```luma\\n%s\\n```\\n*%s*", code, tags);
  } else {
    snprintf(result, buf_size, "```luma\\n%s\\n```", code);
  }

  return json_escape_hover(result, arena);
}

// ---------------------------------------------------------------------------
// lsp_hover
// ---------------------------------------------------------------------------
const char *lsp_hover(LSPDocument *doc, LSPPosition position,
                      ArenaAllocator *arena) {
  if (!doc) return NULL;

  // 1. Find the token at the cursor
  Token *tok = find_token_at(doc, position);
  if (!tok || tok->type_ != TOK_IDENTIFIER) return NULL;

  // Extract token text
  char name[256];
  size_t len = tok->length < 255 ? tok->length : 255;
  memcpy(name, tok->value, len);
  name[len] = '\0';

  // 2. Look up in local scope chain first
  if (doc->scope) {
    Symbol *sym = scope_lookup(doc->scope, name);
    if (sym) {
      return hover_for_symbol(sym, NULL, arena);
    }
  }

  // 3. Check module-qualified names: look for "ALIAS::name" pattern by
  //    checking if the token to the left is "::" and further left is an alias.
  //    For now, search all imported module scopes for this bare name.
  if (doc->imports) {
    for (size_t i = 0; i < doc->import_count; i++) {
      ImportedModule *imp = &doc->imports[i];
      if (!imp->scope || !imp->scope->symbols.data) continue;

      for (size_t j = 0; j < imp->scope->symbols.count; j++) {
        Symbol *sym = (Symbol *)((char *)imp->scope->symbols.data +
                                  j * sizeof(Symbol));
        if (!sym || !sym->name) continue;
        if (strcmp(sym->name, name) == 0) {
          const char *alias = imp->alias ? imp->alias : imp->module_path;
          return hover_for_symbol(sym, alias, arena);
        }
      }
    }
  }

  // 4. If it's a keyword or built-in, show a brief description
  static const struct { const char *kw; const char *desc; } keywords[] = {
    {"if",       "```luma\\nif (condition) { ... }\\n```\\nConditional branch"},
    {"elif",     "```luma\\nelif (condition) { ... }\\n```\\nAdditional branch"},
    {"else",     "Else branch of an if statement"},
    {"loop",     "```luma\\nloop { ... }\\n```\\nInfinite or conditional loop"},
    {"switch",   "```luma\\nswitch (value) { case -> result; }\\n```\\nPattern match"},
    {"return",   "Return a value from a function"},
    {"break",    "Exit the current loop"},
    {"continue", "Skip to the next loop iteration"},
    {"let",      "```luma\\nlet name: Type = value;\\n```\\nMutable variable binding"},
    {"const",    "```luma\\nconst name -> fn (...) Type { ... }\\n```\\nImmutable binding"},
    {"pub",      "Mark a declaration as publicly exported"},
    {"fn",       "Function type or declaration"},
    {"struct",   "Struct type declaration"},
    {"enum",     "Enum type declaration"},
    {"cast",     "```luma\\ncast<Type>(value)\\n```\\nType cast"},
    {"sizeof",   "```luma\\nsizeof<Type>\\n```\\nSize of a type in bytes"},
    {"alloc",    "```luma\\nalloc(size)\\n```\\nAllocate heap memory"},
    {"free",     "```luma\\nfree(ptr)\\n```\\nFree heap memory"},
    {"output",   "```luma\\noutput(value)\\n```\\nPrint without newline"},
    {"outputln", "```luma\\noutputln(value)\\n```\\nPrint with newline"},
    {"input",    "```luma\\ninput<Type>(\\\"prompt\\\")\\n```\\nRead typed input"},
    {"defer",    "```luma\\ndefer { ... }\\n```\\nRun block when scope exits"},
  };

  for (size_t i = 0; i < sizeof(keywords)/sizeof(keywords[0]); i++) {
    if (strcmp(name, keywords[i].kw) == 0) {
      char *result = arena_alloc(arena, strlen(keywords[i].desc) + 1, 1);
      if (result) {
        strcpy(result, keywords[i].desc);
        return result;
      }
    }
  }

  return NULL;
}

// ---------------------------------------------------------------------------
// lsp_definition
// ---------------------------------------------------------------------------
LSPLocation *lsp_definition(LSPDocument *doc, LSPPosition position,
                            ArenaAllocator *arena) {
  if (!doc) return NULL;

  Symbol *symbol = lsp_symbol_at_position(doc, position);
  if (!symbol) return NULL;

  LSPLocation *loc = arena_alloc(arena, sizeof(LSPLocation), alignof(LSPLocation));
  loc->uri = doc->uri;
  loc->range.start.line = position.line;
  loc->range.start.character = 0;
  loc->range.end.line = position.line;
  loc->range.end.character = 100;

  return loc;
}

// ---------------------------------------------------------------------------
// lsp_completion
// ---------------------------------------------------------------------------
LSPCompletionItem *lsp_completion(LSPDocument *doc, LSPPosition position,
                                  size_t *completion_count,
                                  ArenaAllocator *arena) {
  (void)position;
  if (!doc || !completion_count) {
    return NULL;
  }

  GrowableArray completions;
  growable_array_init(&completions, arena, 32, sizeof(LSPCompletionItem));

  const struct {
    const char *label;
    const char *snippet;
    const char *detail;
  } keywords[] = {
      {"const fn", "const ${1:name} -> fn (${2:params}) ${3:Type} {\n\t$0\n}",
       "Function declaration"},
      {"pub const fn",
       "pub const ${1:name} -> fn (${2:params}) ${3:Type} {\n\t$0\n}",
       "Public function"},
      {"const fn<T>",
       "const ${1:name} = fn<${2:T}>(${3:params}) ${4:Type} {\n\t$0\n}",
       "Generic function"},
      {"pub const fn<T>",
       "pub const ${1:name} = fn<${2:T}>(${3:params}) ${4:Type} {\n\t$0\n}",
       "Public generic function"},
      {"const struct",
       "const ${1:Name} -> struct {\n\t${2:field}: ${3:Type}$0,\n};",
       "Struct definition"},
      {"const struct<T>",
       "const ${1:Name} -> struct<${2:T}> {\n\t${3:field}: ${4:Type}$0,\n};",
       "Generic struct"},
      {"const enum", "const ${1:Name} -> enum {\n\t${2:Variant}$0,\n};",
       "Enum definition"},
      {"const var", "const ${1:name}: ${2:Type} = ${3:value};$0",
       "Top-level constant"},
      {"if", "if (${1:condition}) {\n\t$0\n}", "If statement"},
      {"if else", "if (${1:condition}) {\n\t${2}\n} else {\n\t$0\n}",
       "If-else statement"},
      {"elif", "elif (${1:condition}) {\n\t$0\n}", "Elif clause"},
      {"loop", "loop {\n\t$0\n}", "Infinite loop"},
      {"loop while", "loop (${1:condition}) {\n\t$0\n}", "While-style loop"},
      {"loop for",
       "loop [${1:i}: int = 0](${1:i} < ${2:10}) : (++${1:i}) {\n\t$0\n}",
       "For-style loop"},
      {"loop for multi",
       "loop [${1:i}: int = 0, ${2:j}: int = 0](${1:i} < ${3:10}) : (++${1:i}) "
       "{\n\t$0\n}",
       "Multi-variable for loop"},
      {"switch", "switch (${1:value}) {\n\t${2:case} -> ${3:result};$0\n}",
       "Switch statement"},
      {"switch default",
       "switch (${1:value}) {\n\t${2:case} -> ${3:result};\n\t_ -> "
       "${4:default};$0\n}",
       "Switch with default case"},
      {"let", "let ${1:name}: ${2:Type} = ${3:value};$0",
       "Variable declaration"},
      {"defer block", "defer {\n\t${1:cleanup()};$0\n}", "Defer block"},
      {"@module", "@module \"${1:name}\"$0", "Module declaration"},
      {"@use", "@use \"${1:module}\" as ${2:alias}$0", "Import module"},
      {"return", "return ${1:value};$0", "Return statement"},
      {"break", "break;$0", "Break statement"},
      {"continue", "continue;$0", "Continue statement"},
      {"main", "const main -> fn () int {\n\t$0\n\treturn 0;\n};",
       "Main function"},
      {"outputln", "outputln(${1:message});$0", "Output with newline"},
      {"output", "output(${1:message});$0", "Output without newline"},
      {"input", "input<${1:Type}>(\"${2:prompt}\")$0", "Read typed input"},
      {"system", "system(\"${1:command}\");$0", "Execute system command"},
      {"cast", "cast<${1:Type}>(${2:value})$0", "Type cast"},
      {"sizeof", "sizeof<${1:Type}>$0", "Size of type"},
      {"alloc", "cast<${1:*Type}>(alloc(${2:size} * sizeof<${3:Type}>))$0",
       "Allocate memory"},
      {"alloc defer",
       "let ${1:ptr}: ${2:*Type} = cast<${2:*Type}>(alloc(${3:size} * "
       "sizeof<${4:Type}>));\ndefer free(${1:ptr});$0",
       "Allocate with defer cleanup"},
      {"struct method", "${1:name} -> fn (${2:params}) ${3:Type} {\n\t$0\n}",
       "Struct method"},
      {"struct pub", "pub:\n\t${1:field}: ${2:Type},$0",
       "Public struct fields"},
      {"struct priv", "priv:\n\t${1:field}: ${2:Type},$0",
       "Private struct fields"},
      {"array", "[${1:Type}; ${2:size}]$0", "Array type"},
      {"array init", "let ${1:arr}: [${2:Type}; ${3:size}] = [${4:values}];$0",
       "Array initialization"},
      {"pointer", "*${1:Type}$0", "Pointer type"},
      {"address of", "&${1:variable}$0", "Address-of operator"},
      {"dereference", "*${1:pointer}$0", "Dereference pointer"},
      {"#returns_ownership",
       "#returns_ownership\nconst ${1:name} -> fn (${2:params}) ${3:*Type} "
       "{\n\t$0\n}",
       "Function returns owned pointer"},
      {"#takes_ownership",
       "#takes_ownership\nconst ${1:name} -> fn (${2:ptr}: ${3:*Type}) void "
       "{\n\t$0\n}",
       "Function takes ownership"},
  };

  for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
    LSPCompletionItem *item =
        (LSPCompletionItem *)growable_array_push(&completions);
    if (item) {
      item->label = arena_strdup(arena, keywords[i].label);
      item->kind = LSP_COMPLETION_SNIPPET;
      item->insert_text = arena_strdup(arena, keywords[i].snippet);
      item->format = LSP_INSERT_FORMAT_SNIPPET;
      item->detail = arena_strdup(arena, keywords[i].detail);
      item->documentation = NULL;
      item->sort_text = NULL;
      item->filter_text = NULL;
    }
  }

  Scope *local_scope = doc->scope;

  if (local_scope) {
    Scope *current_scope = local_scope;
    int scope_depth = 0;

    while (current_scope) {
      if (current_scope->symbols.data && current_scope->symbols.count > 0) {
        for (size_t i = 0; i < current_scope->symbols.count; i++) {
          Symbol *sym = (Symbol *)((char *)current_scope->symbols.data +
                                   i * sizeof(Symbol));

          if (!sym || !sym->name || !sym->type) continue;

          LSPCompletionItem *item =
              (LSPCompletionItem *)growable_array_push(&completions);
          if (item) {
            item->label = arena_strdup(arena, sym->name);

            if (sym->type->type == AST_TYPE_FUNCTION) {
              item->kind = LSP_COMPLETION_FUNCTION;
              char snippet[512];
              snprintf(snippet, sizeof(snippet), "%s()$0", sym->name);
              item->insert_text = arena_strdup(arena, snippet);
              item->format = LSP_INSERT_FORMAT_SNIPPET;
              item->detail = type_to_string(sym->type, arena);
            } else if (sym->type->type == AST_TYPE_STRUCT) {
              item->kind = LSP_COMPLETION_STRUCT;
              item->insert_text = arena_strdup(arena, sym->name);
              item->format = LSP_INSERT_FORMAT_PLAIN_TEXT;
              item->detail = type_to_string(sym->type, arena);
            } else {
              item->kind = LSP_COMPLETION_VARIABLE;
              item->insert_text = arena_strdup(arena, sym->name);
              item->format = LSP_INSERT_FORMAT_PLAIN_TEXT;
              item->detail = type_to_string(sym->type, arena);
            }

            char sort[8];
            snprintf(sort, sizeof(sort), "%d", scope_depth);
            item->sort_text = arena_strdup(arena, sort);
            item->documentation = NULL;
            item->filter_text = NULL;
          }
        }
      }

      current_scope = current_scope->parent;
      scope_depth++;
    }
  }

  if (doc->imports && doc->import_count > 0) {
    for (size_t i = 0; i < doc->import_count; i++) {
      ImportedModule *import = &doc->imports[i];

      if (!import->scope || !import->scope->symbols.data) continue;

      const char *prefix = import->alias ? import->alias : "module";

      for (size_t j = 0; j < import->scope->symbols.count; j++) {
        Symbol *sym = (Symbol *)((char *)import->scope->symbols.data +
                                 j * sizeof(Symbol));

        if (!sym || !sym->name || !sym->type || !sym->is_public) continue;
        if (strncmp(sym->name, "__", 2) == 0) continue;

        LSPCompletionItem *item =
            (LSPCompletionItem *)growable_array_push(&completions);
        if (item) {
          size_t label_len = strlen(prefix) + strlen(sym->name) + 3;
          char *label = arena_alloc(arena, label_len, 1);
          snprintf(label, label_len, "%s::%s", prefix, sym->name);

          item->label = label;
          item->kind = (sym->type->type == AST_TYPE_FUNCTION)
                           ? LSP_COMPLETION_FUNCTION
                           : LSP_COMPLETION_VARIABLE;
          item->insert_text = arena_strdup(arena, label);
          item->format = LSP_INSERT_FORMAT_PLAIN_TEXT;
          item->detail = type_to_string(sym->type, arena);
          item->documentation = NULL;
          item->sort_text = arena_strdup(arena, "9");
          item->filter_text = NULL;
        }
      }
    }
  }

  *completion_count = completions.count;
  return (LSPCompletionItem *)completions.data;
}