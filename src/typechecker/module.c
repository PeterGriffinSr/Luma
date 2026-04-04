#include "type.h"
#include <stdio.h>
#include <string.h>

/**
 * @brief Register a module scope in the global scope
 */
bool register_module(Scope *global_scope, const char *module_name,
                     Scope *module_scope, ArenaAllocator *arena) {
  (void)module_scope;
  size_t prefixed_len = strlen("__module_") + strlen(module_name) + 1;
  char *prefixed_name = arena_alloc(arena, prefixed_len, 1);
  snprintf(prefixed_name, prefixed_len, "__module_%s", module_name);

  AstNode *module_type = create_basic_type(arena, "module", 0, 0);

  return scope_add_symbol(global_scope, prefixed_name, module_type, true, false,
                          arena);
}

/**
 * @brief Find a module scope by name
 */
Scope *find_module_scope(Scope *global_scope, const char *module_name) {
  for (size_t i = 0; i < global_scope->children.count; i++) {
    Scope **child_ptr =
        (Scope **)((char *)global_scope->children.data + i * sizeof(Scope *));
    Scope *child = *child_ptr;

    if (child->is_module_scope &&
        strcmp(child->module_name, module_name) == 0) {
      return child;
    }
  }
  return NULL;
}

/**
 * @brief Add a module import to a scope
 */
bool add_module_import(Scope *importing_scope, const char *module_name,
                       const char *alias, Scope *module_scope,
                       ArenaAllocator *arena) {
  ModuleImport *import =
      (ModuleImport *)growable_array_push(&importing_scope->imported_modules);
  if (!import) {
    return false;
  }

  import->module_name = arena_strdup(arena, module_name);
  import->alias = arena_strdup(arena, alias);
  import->module_scope = module_scope;

  return true;
}

/**
 * @brief Look up a qualified symbol (module_alias.symbol_name) with visibility
 * rules
 */
Symbol *lookup_qualified_symbol(Scope *scope, const char *module_alias,
                                const char *symbol_name) {
  Scope *current = scope;
  while (current) {
    for (size_t i = 0; i < current->imported_modules.count; i++) {
      ModuleImport *import =
          (ModuleImport *)((char *)current->imported_modules.data +
                           i * sizeof(ModuleImport));

      if (strcmp(import->alias, module_alias) == 0) {
        // Check what symbols exist in the module
        for (size_t j = 0; j < import->module_scope->symbols.count; j++) {
          Symbol *s = (Symbol *)((char *)import->module_scope->symbols.data +
                                 j * sizeof(Symbol));
        }

        Scope *requesting_module = find_containing_module(scope);
        Symbol *result = scope_lookup_current_only_with_visibility(
            import->module_scope, symbol_name, requesting_module);

        return result;
      }
    }
    current = current->parent;
  }

  return NULL;
}

/**
 * @brief Create a new module scope
 */
Scope *create_module_scope(Scope *global_scope, const char *module_name,
                           ArenaAllocator *arena) {
  Scope *module_scope = create_child_scope(global_scope, module_name, arena);
  module_scope->is_module_scope = true;
  module_scope->module_name = arena_strdup(arena, module_name);

  growable_array_init(&module_scope->imported_modules, arena, 4,
                      sizeof(ModuleImport));

  return module_scope;
}

void build_dependency_graph(AstNode **modules, size_t module_count,
                            GrowableArray *dep_graph, ArenaAllocator *arena) {
  for (size_t i = 0; i < module_count; i++) {
    AstNode *module = modules[i];
    if (!module || module->type != AST_PREPROCESSOR_MODULE)
      continue;

    const char *module_name = module->preprocessor.module.name;

    ModuleDependency *dep = (ModuleDependency *)growable_array_push(dep_graph);
    dep->module_name = module_name;
    dep->processed = false;
    growable_array_init(&dep->dependencies, arena, 4, sizeof(const char *));

    // Scan for @use statements to find dependencies
    AstNode **body = module->preprocessor.module.body;
    int body_count = module->preprocessor.module.body_count;

    for (int j = 0; j < body_count; j++) {
      if (body[j] && body[j]->type == AST_PREPROCESSOR_USE) {
        const char *imported_module = body[j]->preprocessor.use.module_name;
        const char **dep_slot =
            (const char **)growable_array_push(&dep->dependencies);
        *dep_slot = imported_module;
      }
    }
  }
}

/**
 * @brief Process modules in dependency order (topological sort)
 */
bool process_module_in_order(const char *module_name, GrowableArray *dep_graph,
                             AstNode **modules, size_t module_count,
                             Scope *global_scope, ArenaAllocator *arena) {
  ModuleDependency *current_dep = NULL;
  for (size_t i = 0; i < dep_graph->count; i++) {
    ModuleDependency *dep = (ModuleDependency *)((char *)dep_graph->data +
                                                 i * sizeof(ModuleDependency));
    if (strcmp(dep->module_name, module_name) == 0) {
      current_dep = dep;
      break;
    }
  }

  if (!current_dep)
    return true;
  if (current_dep->processed)
    return true;

  for (size_t i = 0; i < current_dep->dependencies.count; i++) {
    const char **dep_name =
        (const char **)((char *)current_dep->dependencies.data +
                        i * sizeof(const char *));
    if (!process_module_in_order(*dep_name, dep_graph, modules, module_count,
                                 global_scope, arena)) {
      return false;
    }
  }

  AstNode *module = NULL;
  for (size_t i = 0; i < module_count; i++) {
    if (modules[i] && modules[i]->type == AST_PREPROCESSOR_MODULE &&
        strcmp(modules[i]->preprocessor.module.name, module_name) == 0) {
      module = modules[i];
      break;
    }
  }

  if (!module) {
    tc_error(modules[0], "Internal Error", "Module '%s' not found",
             module_name);
    return false;
  }

  g_tokens = module->preprocessor.module.tokens;
  g_token_count = module->preprocessor.module.token_count;
  g_file_path = module->preprocessor.module.file_path;
  tc_error_init(g_tokens, g_token_count, g_file_path, arena);

  Scope *module_scope = find_module_scope(global_scope, module_name);
  if (!module_scope) {
    tc_error(module, "Internal Error", "Module scope not found for '%s'",
             module_name);
    return false;
  }

  AstNode **body = module->preprocessor.module.body;
  int body_count = module->preprocessor.module.body_count;

  // ===== PASS 1: Forward declarations =====
  for (int j = 0; j < body_count; j++) {
    if (!body[j])
      continue;
    if (body[j]->type == AST_PREPROCESSOR_USE)
      continue;

    if (body[j]->type == AST_STMT_FUNCTION &&
        body[j]->stmt.func_decl.forward_declared) {
      if (!typecheck_func_decl(body[j], module_scope, arena)) {
        tc_error(body[j], "Prototype Error",
                 "Failed to typecheck function prototype in module '%s'",
                 module_name);
        return false;
      }
    }
  }

  // ===== PASS 1b: Pre-register @os symbols (functions AND constants) =====
  for (int j = 0; j < body_count; j++) {
    if (!body[j] || body[j]->type != AST_PREPROCESSOR_OS)
      continue;

    AstNode *os_node = body[j];
    const char *target_os =
        module_scope->config ? module_scope->config->target_os : NULL;
    if (!target_os)
      continue;

    AstNode *matched_body = NULL;
    for (size_t k = 0; k < os_node->preprocessor.os.arm_count; k++) {
      if (strcmp(os_node->preprocessor.os.platforms[k], target_os) == 0) {
        matched_body = os_node->preprocessor.os.bodies[k];
        break;
      }
    }
    if (!matched_body && os_node->preprocessor.os.has_default)
      matched_body = os_node->preprocessor.os.default_body;
    if (!matched_body || matched_body->type != AST_STMT_BLOCK)
      continue;

    for (size_t k = 0; k < matched_body->stmt.block.stmt_count; k++) {
      AstNode *inner = matched_body->stmt.block.statements[k];
      if (!inner)
        continue;

      if (inner->type == AST_STMT_FUNCTION) {
        AstNode *func_type = create_function_type(
            arena, inner->stmt.func_decl.param_types,
            inner->stmt.func_decl.param_count,
            inner->stmt.func_decl.return_type, inner->line, inner->column);

        if (!scope_lookup_current_only(module_scope,
                                       inner->stmt.func_decl.name)) {
          scope_add_symbol_with_ownership(
              module_scope, inner->stmt.func_decl.name, func_type,
              inner->stmt.func_decl.is_public, false,
              inner->stmt.func_decl.returns_ownership,
              inner->stmt.func_decl.takes_ownership, arena);
        }
      } else {
        // For constants and everything else, just typecheck them directly
        // into the module scope so they're visible to the rest of the module
        if (!scope_lookup_current_only(module_scope,
                                       inner->type == AST_STMT_VAR_DECL
                                           ? inner->stmt.var_decl.name
                                           : NULL)) {
          typecheck(inner, module_scope, arena, module_scope->config);
        }
      }
    }
  }

  // ===== PASS 2: Everything except @os and forward decls =====
  for (int j = 0; j < body_count; j++) {
    if (!body[j])
      continue;
    if (body[j]->type == AST_PREPROCESSOR_USE)
      continue;
    if (body[j]->type == AST_PREPROCESSOR_OS)
      continue;
    if (body[j]->type == AST_STMT_FUNCTION &&
        body[j]->stmt.func_decl.forward_declared)
      continue;

    if (!typecheck(body[j], module_scope, arena, global_scope->config)) {
      tc_error(body[j], "Module Error",
               "Failed to typecheck statement in module '%s'", module_name);
      return false;
    }
  }

  // ===== PASS 3: Typecheck @os blocks =====
  for (int j = 0; j < body_count; j++) {
    if (!body[j] || body[j]->type != AST_PREPROCESSOR_OS)
      continue;

    if (!typecheck_os_stmt(body[j], module_scope, arena)) {
      tc_error(body[j], "Module Error",
               "Failed to typecheck @os block in module '%s'", module_name);
      return false;
    }
  }

  module->preprocessor.module.scope = (void *)module_scope;

  StaticMemoryAnalyzer *analyzer = get_static_analyzer(module_scope);
  if (analyzer && g_tokens && g_token_count > 0 && g_file_path &&
      global_scope->config->check_mem) {
    static_memory_check_and_report(analyzer, arena);
  }

  current_dep->processed = true;
  return true;
}

bool typecheck_os_stmt(AstNode *node, Scope *scope, ArenaAllocator *arena) {
  if (!node || node->type != AST_PREPROCESSOR_OS) {
    tc_error(node, "Internal Error", "Expected @os node");
    return false;
  }

  // Walk up to find config if not set on this scope
  BuildConfig *config = scope->config;
  if (!config) {
    Scope *current = scope->parent;
    while (current) {
      if (current->config) {
        config = current->config;
        break;
      }
      current = current->parent;
    }
  }

  const char *target_os = config ? config->target_os : NULL;

  if (!target_os) {
    tc_error(node, "@os Error",
             "@os block requires a target OS to be set (--target-os flag)");
    return false;
  }

  AstNode *matched_body = NULL;
  for (size_t i = 0; i < node->preprocessor.os.arm_count; i++) {
    if (strcmp(node->preprocessor.os.platforms[i], target_os) == 0) {
      matched_body = node->preprocessor.os.bodies[i];
      break;
    }
  }

  if (!matched_body && node->preprocessor.os.has_default)
    matched_body = node->preprocessor.os.default_body;

  if (!matched_body)
    return true;

  if (matched_body->type != AST_STMT_BLOCK) {
    tc_error(matched_body, "Internal Error",
             "@os arm body must be a block statement");
    return false;
  }

  for (size_t i = 0; i < matched_body->stmt.block.stmt_count; i++) {
    AstNode *stmt = matched_body->stmt.block.statements[i];
    if (!stmt)
      continue;

    if (stmt->type == AST_STMT_FUNCTION) {
      if (!typecheck_func_decl(stmt, scope, arena)) {
        tc_error(stmt, "@os Error",
                 "Failed to typecheck function in @os \"%s\" arm", target_os);
        return false;
      }
      continue;
    }

    if (!typecheck(stmt, scope, arena, config)) {
      tc_error(stmt, "@os Error",
               "Failed to typecheck statement in @os \"%s\" arm", target_os);
      return false;
    }
  }

  return true;
}