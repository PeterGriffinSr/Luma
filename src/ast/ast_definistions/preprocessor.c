#include "../ast.h"
#include <stdio.h>
#include <string.h>

AstNode *create_module_node(ArenaAllocator *arena, const char *name,
                            const char *doc_comment, int potions,
                            AstNode **body, size_t body_count, size_t line,
                            size_t column) {
  AstNode *node = create_preprocessor_node(
      arena, AST_PREPROCESSOR_MODULE, Node_Category_PREPROCESSOR, line, column);
  node->preprocessor.module.name = (char *)name;
  node->preprocessor.module.doc_comment = (char *)doc_comment;
  node->preprocessor.module.potions = potions;
  node->preprocessor.module.body = body;
  node->preprocessor.module.body_count = body_count;
  node->preprocessor.module.file_path = NULL;
  node->preprocessor.module.tokens = NULL;
  node->preprocessor.module.token_count = 0;
  node->preprocessor.module.scope = NULL;
  return node;
}

AstNode *create_use_node(ArenaAllocator *arena, const char *module_name,
                         const char *alias, size_t line, size_t column) {
  AstNode *node = create_preprocessor_node(
      arena, AST_PREPROCESSOR_USE, Node_Category_PREPROCESSOR, line, column);
  node->preprocessor.use.module_name = module_name;
  node->preprocessor.use.alias = alias;
  return node;
}

AstNode *create_os_node(ArenaAllocator *arena, char **platforms,
                        AstNode **bodies, size_t arm_count, bool has_default,
                        AstNode *default_body, size_t line, size_t column) {
  AstNode *node = create_preprocessor_node(
      arena, AST_PREPROCESSOR_OS, Node_Category_PREPROCESSOR, line, column);
  node->preprocessor.os.platforms = platforms;
  node->preprocessor.os.bodies = bodies;
  node->preprocessor.os.arm_count = arm_count;
  node->preprocessor.os.has_default = has_default;
  node->preprocessor.os.default_body = default_body;
  return node;
}

AstNode *create_link_node(ArenaAllocator *arena, const char *lib_name,
                          size_t line, size_t column) {
  AstNode *node = create_preprocessor_node(
      arena, AST_PREPROCESSOR_LINK, Node_Category_PREPROCESSOR, line, column);
  node->preprocessor.link.lib_name = lib_name;
  return node;
}

void apply_dll_import(AstNode *func_node, const char *dll_name,
                      const char *callconv) {
  func_node->stmt.func_decl.is_dll_import = true;
  func_node->stmt.func_decl.dll_name = dll_name;
  func_node->stmt.func_decl.dll_callconv = callconv; // Can be null
}

void apply_lib_import(AstNode *func_node, const char *lib_name) {
  func_node->stmt.func_decl.is_lib_import = true;
  func_node->stmt.func_decl.lib_name = lib_name;
  // no callconv needed, always cdecl
}
