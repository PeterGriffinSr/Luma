#include <stdio.h>

#include "../helper/help.h"
#include "type.h"

bool typecheck(AstNode *node, Scope *scope, ArenaAllocator *arena,
               BuildConfig *config) {
  scope->config = config;

  // Propagate to all child scopes that don't have config yet
  for (size_t i = 0; i < scope->children.count; i++) {
    Scope **child_ptr = (Scope **)((char *)scope->children.data +
                                   i * sizeof(Scope *));
    if (*child_ptr && !(*child_ptr)->config) {
      (*child_ptr)->config = config;
    }
  }

  switch (node->category) {
  case Node_Category_STMT:
    return typecheck_statement(node, scope, arena);

  case Node_Category_EXPR: {
    AstNode *result_type = typecheck_expression(node, scope, arena);
    return result_type != NULL;
  }

  case Node_Category_TYPE:
    return true;

  case Node_Category_PREPROCESSOR:
    switch (node->type) {
    case AST_PREPROCESSOR_MODULE:
      return typecheck_module_stmt(node, scope, arena);
    case AST_PREPROCESSOR_USE:
      return typecheck_use_stmt(node, scope, scope, arena);
    case AST_PREPROCESSOR_OS:
      return typecheck_os_stmt(node, scope, arena);
    default:
      tc_error(node, "SyntaxError", "Unknown preprocessor node type %d",
               node->type);
      return false;
    }

  default:
    tc_error(node, "Unsupported Node", "Unsupported node category %d",
             node->category);
    return false;
  }
}
