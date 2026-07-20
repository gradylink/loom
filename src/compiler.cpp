#include "compiler.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tree_sitter/api.h>
#include <unordered_set>
#include <vector>

#include "typeHandler.hpp"

extern "C" const TSLanguage *tree_sitter_loom(void);

#include "utils.hpp"

Compiler::Compiler(const std::string_view &source, const std::string &datapackNamespace, std::filesystem::path currentDir)
    : source(source), datapackNamespace(datapackNamespace), currentDir(currentDir) {
  parser = ts_parser_new();
  ts_parser_set_language(parser, tree_sitter_loom());

  tree = ts_parser_parse_string(parser, nullptr, this->source.data(), source.length());
  root = ts_tree_root_node(tree);

  typeRegistry = std::make_unique<TypeRegistry>();
  registerDefaultTypeHandlers();
}

Compiler::~Compiler() {
  ts_tree_delete(tree);
  ts_parser_delete(parser);
}

void Compiler::registerDefaultTypeHandlers() {
  typeRegistry->registerHandler(*this, createIntegerHandler());
  typeRegistry->registerHandler(*this, createFloatHandler());
  typeRegistry->registerHandler(*this, createBooleanHandler());
  typeRegistry->registerHandler(*this, createStringHandler());
  typeRegistry->registerHandler(*this, createListHandler());
  typeRegistry->registerHandler(*this, createEnumHandler());
  typeRegistry->registerHandler(*this, createStructHandler());
}

TypeHandler *Compiler::getHandler(const Type &type) { return const_cast<TypeHandler *>(typeRegistry->findHandler(type)); }

std::string_view Compiler::getNodeText(TSNode node) {
  if (ts_node_is_null(node)) return "";
  const uint32_t start = ts_node_start_byte(node);
  const uint32_t end = ts_node_end_byte(node);
  return std::string_view(source.data() + start, end - start);
}

std::string_view Compiler::getFieldText(TSNode node, const std::string &field) { return getNodeText(ts_node_child_by_field_name(node, field.c_str(), field.length())); }

Compiler::Type Compiler::parseTypeFromString(const std::string &typeText) const {
  if (typeText.ends_with("[]")) {
    return Type::ListTypeOf(parseTypeFromString(typeText.substr(0, typeText.length() - 2)));
  }

  if (typeText == "int") return Type::IntegerType();
  if (typeText == "bool") return Type::BooleanType();
  if (typeText == "string") return Type::StringType();
  if (typeText == "float") return Type::FloatType();
  const auto &it = enums.find(typeText);
  if (it != enums.end()) return Type::EnumTypeOf(&it->second);
  const auto &itStruct = structs.find(typeText);
  if (itStruct != structs.end()) return Type::StructTypeOf(&itStruct->second);
  throw std::runtime_error(std::format("Unknown type: {}", typeText));
}

std::string Compiler::compileVariableDeclaration(TSNode child, TSNode scope, bool isGlobal) {
  TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
  const std::string name = std::string(getNodeText(nameNode));
  if (isBuiltin(name)) {
    throw std::runtime_error(formatError(nameNode, "Reserved name."));
  }

  const ExpressionData expr = compileExpression(ts_node_child_by_field_name(child, "value", 5));
  const bool constant = getFieldText(child, "keyword") == "const";
  TSNode varTypeNode = ts_node_child_by_field_name(child, "type", 4);
  Type varType;
  std::optional<std::string> value = std::nullopt;
  if (!ts_node_is_null(varTypeNode)) {
    const auto &typeText = getNodeText(varTypeNode);
    varType = parseTypeFromString(std::string(typeText));
  } else {
    varType = expr.type;
  }

  if (constant && expr.precomputed) {
    value = expr.data;
  }

  bool isExport = false;
  bool isExtern = false;
  if (isGlobal) {
    for (uint32_t j = 0; j < ts_node_child_count(child); j++) {
      TSNode subNode = ts_node_child(child, j);
      const std::string &nodeType = std::string(ts_node_type(subNode));
      if (nodeType == "export") {
        if (isExport) throw std::runtime_error(formatError(subNode, "Cannot use 'export' twice."));
        isExport = true;
      } else if (nodeType == "extern") {
        if (isExtern) throw std::runtime_error(formatError(subNode, "Cannot use 'extern' twice."));
        isExtern = true;
      }
    }
  }

  std::string mangled = name;
  if (!isExtern) mangled += "_" + randomMangleString();

  vars.emplace(name, VariableData{.name = name, .mangledName = mangled, .type = varType, .scope = scope, .value = value, .constant = constant, .exported = isExport});

  std::string ret = "";
  if (!value.has_value() || !constant || isExport) {
    if (expr.precomputed) {
      if (varType.isString() || varType.isList() || varType.isStruct() || varType.isFloat()) {
        ret += std::format("data modify storage {}:global vars.{} set value {}\n", datapackNamespace, mangled, expr.data);
      } else {
        ret += std::format("scoreboard players set {} vars {}\n", mangled, expr.data);
      }
    } else {
      if (varType.isString() || varType.isList() || varType.isStruct()) {
        ret += std::format("{}\ndata modify storage {}:global vars.{} set from storage {}:global expr_str1\n", expr.data, datapackNamespace, mangled, datapackNamespace);
      } else if (varType.isFloat()) {
        ret += std::format("{}\ndata modify storage {}:global vars.{} set from storage {}:global expr_float1\n", expr.data, datapackNamespace, mangled, datapackNamespace);
      } else {
        ret += std::format("{}\nscoreboard players operation {} vars = expr_output1 temp\n", expr.data, mangled);
      }
    }
  }
  return ret;
}

void Compiler::registerBuiltin(const std::string &name, BuiltinCompileCallback callback) {
  if (builtins.count(name)) {
    auto existing = builtins[name];
    builtins[name] = [existing, callback](Compiler &c, const std::vector<TSNode> &args, unsigned int id, bool precompute, TSNode node) -> std::optional<ExpressionData> {
      if (auto res = callback(c, args, id, precompute, node)) return res;
      return existing(c, args, id, precompute, node);
    };
  } else {
    builtins[name] = std::move(callback);
  }
}

std::vector<Compiler::CompiledFunction> Compiler::compile() {
  compiledFunctions.clear();
  internalFunctions.clear();

  internalFunctions.push_back(
    {.name = "internal_string_concat", .data = std::format("$data modify storage {}:global expr_str$(out_id) set value \"$(left)$(right)\"", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_string_slice",
     .data = std::format("$data modify storage {0}:global expr_str$(out_id) set string storage {0}:global expr_str$(target_id) $(start) $(end)", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_string_mutate_static",
     .data = std::format(
       "$data modify storage {0}:global macro_args.before set string storage {0}:global vars.$(var_name)$(path) 0 $(index)\n"
       "$data modify storage {0}:global macro_args.after set string storage {0}:global vars.$(var_name)$(path) $(index_plus_one)\n"
       "$data modify storage {0}:global vars.$(var_name)$(path) set value \"$(before)$(value)$(after)\"",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_string_mutate_dynamic",
     .data = std::format(
       "$data modify storage {0}:global macro_args.before set string storage {0}:global vars.$(var_name)$(path) 0 $(index)\n"
       "$data modify storage {0}:global macro_args.after set string storage {0}:global vars.$(var_name)$(path) $(index_plus_one)\n"
       "$data modify storage {0}:global vars.$(var_name)$(path) set value \"$(before)$(value)$(after)\"",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_string_append",
     .data = std::format(
       "$data modify storage {0}:global macro_args.left set from storage {0}:global expr_str$(target_id)\n"
       "$data modify storage {0}:global expr_str$(target_id) set value \"$(left)$(value)\"",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_string_append_str",
     .data = std::format(
       "$data modify storage {0}:global macro_args.left set from storage {0}:global expr_str$(target_id)\n"
       "$data modify storage {0}:global macro_args.right set from storage {0}:global expr_str$(elem_id)\n"
       "$data modify storage {0}:global expr_str$(target_id) set value \"$(left)$(right)\"",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_string_remove",
     .data = std::format(
       "$data modify storage {0}:global macro_args.before set string storage {0}:global expr_str$(target_id) 0 $(index)\n"
       "$data modify storage {0}:global macro_args.after set string storage {0}:global expr_str$(target_id) $(index_plus_one)\n"
       "$data modify storage {0}:global expr_str$(target_id) set value \"$(before)$(after)\"",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_string_insert_value",
     .data = std::format(
       "$data modify storage {0}:global macro_args.before set string storage {0}:global expr_str$(target_id) 0 $(index)\n"
       "$data modify storage {0}:global macro_args.after set string storage {0}:global expr_str$(target_id) $(index)\n"
       "$data modify storage {0}:global expr_str$(target_id) set value \"$(before)$(value)$(after)\"",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_string_insert_str",
     .data = std::format(
       "$data modify storage {0}:global macro_args.before set string storage {0}:global expr_str$(target_id) 0 $(index)\n"
       "$data modify storage {0}:global macro_args.after set string storage {0}:global expr_str$(target_id) $(index)\n"
       "$data modify storage {0}:global macro_args.right set from storage {0}:global expr_str$(elem_id)\n"
       "$data modify storage {0}:global expr_str$(target_id) set value \"$(before)$(right)$(after)\"",
       datapackNamespace
     )}
  );

  internalFunctions.push_back(
    {.name = "internal_list_get_primitive",
     .data = std::format("$execute store result score expr_output$(out_id) temp run data get storage {}:global expr_str$(target_id)[$(index)]", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_list_get_object",
     .data = std::format("$data modify storage {0}:global expr_str$(out_id) set from storage {0}:global expr_str$(target_id)[$(index)]", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_list_slice",
     .data = std::format(
       "$data modify storage {0}:global expr_str$(out_id) set value []\n"
       "$data modify storage {0}:global macro_args.current int $(start)\n"
       "function {0}:internal/loom/internal_list_slice_loop with storage {0}:global macro_args",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_list_slice_loop",
     .data = std::format(
       "execute store result score internal_current temp run data get storage {0}:global macro_args.current\n"
       "execute store result score internal_end temp run data get storage {0}:global macro_args.end\n"
       "$execute if score internal_current temp < internal_end temp run data modify storage {0}:global expr_str$(out_id) append from storage {0}:global "
       "expr_str$(target_id)[$(current)]\n"
       "execute if score internal_current temp < internal_end temp run scoreboard players add internal_current temp 1\n"
       "execute if score internal_current temp < internal_end temp store result storage {0}:global macro_args.current int 1 run scoreboard players get internal_current temp\n"
       "execute if score internal_current temp < internal_end temp run function {0}:internal/loom/internal_list_slice_loop with storage {0}:global macro_args",
       datapackNamespace
     )}
  );
  internalFunctions.push_back({.name = "internal_list_remove", .data = std::format("$data remove storage {0}:global expr_str$(target_id)[$(index)]", datapackNamespace)});
  internalFunctions.push_back(
    {.name = "internal_list_insert_value", .data = std::format("$data modify storage {0}:global expr_str$(target_id) insert $(index) value $(value)", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_list_insert_object",
     .data = std::format("$data modify storage {0}:global expr_str$(target_id) insert $(index) from storage {0}:global expr_str$(elem_id)", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_list_insert_primitive",
     .data = std::format(
       "$execute store result storage {0}:global expr_str$(target_id) insert $(index) int 1 run scoreboard players get expr_output$(elem_id) temp",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_path_append",
     .data = std::format("$data modify storage {0}:global macro_args.path set value \"$(string_before)[$(index_to_append)]\"", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_path_append_prop",
     .data = std::format("$data modify storage {0}:global macro_args.path set value \"$(string_before).$(prop_to_append)\"", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_list_nested_set_value", .data = std::format("$data modify storage {0}:global vars.$(var_name)$(path) set value $(value)", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_list_nested_set_object",
     .data = std::format("$data modify storage {0}:global vars.$(var_name)$(path) set from storage {0}:global expr_str1", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_list_nested_set_primitive",
     .data = std::format("$execute store result storage {0}:global vars.$(var_name)$(path) int 1 run scoreboard players get expr_output1 temp", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_float_sub_macro",
     .data = "$item modify block 18483211 -64 14504281 container.0 {function:set_custom_model_data,floats:{values:[{type:sum,summands:[{type:storage,storage:\"" +
             datapackNamespace +
             ":global\",path:\"macro_args.a\"},{type:score,target:{type:fixed,name:\"#-1\"},score:\"math\",scale:$(text)}],mode:replace_all}]}\n"
             "data modify storage " +
             datapackNamespace + ":global macro_args.out set from block 18483211 -64 14504281 Items[0].components.\"minecraft:custom_model_data\".floats[0]"}
  );

  internalFunctions.push_back(
    {.name = "internal_int_to_string", .data = std::format("$data modify storage {0}:global expr_str$(out_id) set value \"$(value)\"", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_float_to_string", .data = std::format("$data modify storage {0}:global expr_str$(out_id) set value \"$(value)\"", datapackNamespace)}
  );
  internalFunctions.push_back({.name = "internal_string_to_int", .data = std::format("$scoreboard players set expr_output$(out_id) temp $(value)", datapackNamespace)});
  internalFunctions.push_back(
    {.name = "internal_string_to_float", .data = std::format("$data modify storage {0}:global expr_float$(out_id) set value $(value)f", datapackNamespace)}
  );
  internalFunctions.push_back(
    {.name = "internal_string_to_charlist",
     .data = std::format(
       "$data modify storage {0}:global expr_str$(out_id) set value []\n"
       "$data modify storage {0}:global macro_args.index_plus_one set value 1\n"
       "function {0}:internal/loom/internal_string_to_charlist_loop with storage {0}:global macro_args",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_string_to_charlist_loop",
     .data = std::format(
       "execute store result score internal_charlist_idx temp run data get storage {0}:global macro_args.index\n"
       "execute store result score internal_charlist_len temp run data get storage {0}:global macro_args.length\n"
       "execute if score internal_charlist_idx temp < internal_charlist_len temp run "
       "function {0}:internal/loom/internal_string_to_charlist_step with storage {0}:global macro_args",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_string_to_charlist_step",
     .data = std::format(
       "$data modify storage {0}:global expr_str$(out_id) append string storage {0}:global expr_str$(target_id) $(index) $(index_plus_one)\n"
       "scoreboard players add internal_charlist_idx temp 1\n"
       "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get internal_charlist_idx temp\n"
       "scoreboard players add internal_charlist_idx temp 1\n"
       "execute store result storage {0}:global macro_args.index_plus_one int 1 run scoreboard players get internal_charlist_idx temp\n"
       "scoreboard players remove internal_charlist_idx temp 1\n"
       "function {0}:internal/loom/internal_string_to_charlist_loop with storage {0}:global macro_args",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_abs",
     .data = std::format(
       "$data modify storage {0}:global _temp_char set string storage {0}:global macro_args.value 0 1\n"
       "$execute if data storage {0}:global {{_temp_char:\"-\"}} run data modify storage {0}:global macro_args.value set string storage {0}:global macro_args.value 1\n"
       "$data modify storage {0}:global expr_float$(out_id) set from storage {0}:global macro_args.value",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_floor",
     .data = std::format(
       "$execute store result score _temp temp run data get storage {0}:global macro_args.value 1\n"
       "$execute store result storage {0}:global expr_float$(out_id) float 1 run scoreboard players get _temp temp",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_ceil",
     .data = std::format(
       "$execute store result score _temp temp run data get storage {0}:global macro_args.value 1\n"
       "$execute store result storage {0}:global expr_float$(out_id) float 1 run scoreboard players get _temp temp",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_round",
     .data = std::format(
       "$execute store result score _temp temp run data get storage {0}:global macro_args.value 1\n"
       "$execute store result storage {0}:global expr_float$(out_id) float 1 run scoreboard players get _temp temp",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_sqrt",
     .data = std::format(
       "data modify storage {0}:global _sqrt_S set from storage {0}:global macro_args.value\n"
       "data modify storage {0}:global _sqrt_x set value 1.0f\n"
       "data modify storage {0}:global _sqrt_iters set value 0\n"
       "function {0}:internal/loom/internal_float_sqrt_loop\n"
       "$data modify storage {0}:global expr_float$(out_id) set from storage {0}:global _sqrt_x",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_sqrt_loop",
     .data = std::format(
       "data modify storage {0}:global _temp_div set value [1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
       "data modify storage {0}:global _temp_div[3] set from storage {0}:global _sqrt_S\n"
       "data modify storage {0}:global _temp_div[15] set from storage {0}:global _sqrt_x\n"
       "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_div\n"
       "data modify storage {0}:global _sqrt_div_res set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"

       "item modify block 18483211 -64 14504281 container.0 "
       "{{function:set_custom_model_data,floats:{{mode:replace_all,values:[{{type:sum,summands:[{{type:storage,storage:\"{0}:global\",path:\"_sqrt_div_res\"}},{{type:storage,"
       "storage:\"{0}:global\",path:\"_sqrt_x\"}}]}}]}}}}\n"
       "data modify storage {0}:global _sqrt_add_res set from block 18483211 -64 14504281 Items[0].components.\"minecraft:custom_model_data\".floats[0]\n"

       "data modify storage {0}:global _temp_mul set value [1f,0f,0f,1f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
       "data modify storage {0}:global _temp_mul[15] set value 0.5f\n"
       "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_mul\n"
       "data modify storage {0}:global _temp_var1 set value [1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
       "data modify storage {0}:global _temp_var1[3] set from storage {0}:global _sqrt_add_res\n"
       "data modify storage {0}:global _temp_var1[15] set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"
       "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_var1\n"
       "data modify storage {0}:global _sqrt_x set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"

       "execute store result score _iters temp run data get storage {0}:global _sqrt_iters\n"
       "scoreboard players add _iters temp 1\n"
       "execute store result storage {0}:global _sqrt_iters int 1 run scoreboard players get _iters temp\n"
       "execute if score _iters temp < 5 run function {0}:internal/loom/internal_float_sqrt_loop",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_sin",
     .data = std::format(
       "data modify storage {0}:global _temp_mul set value [1f,0f,0f,1f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
       "data modify storage {0}:global _temp_mul[15] set value 57.29578f\n"
       "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_mul\n"
       "data modify storage {0}:global _temp_var1 set value [1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
       "data modify storage {0}:global _temp_var1[3] set from storage {0}:global macro_args.value\n"
       "data modify storage {0}:global _temp_var1[15] set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"
       "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_var1\n"
       "data modify storage {0}:global macro_args.yaw set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"
       "function {0}:internal/loom/internal_float_sin_tp with storage {0}:global macro_args",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_sin_tp",
     .data = std::format(
       "$teleport 6c6f6f6d-0-0-0-ffff 0.0 0.0 0.0 $(yaw) 0\n"
       "execute as 6c6f6f6d-0-0-0-ffff at @s run teleport @s ^ ^ ^1\n"
       "data modify storage {0}:global _temp_trans set value [1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,-1f]\n"
       "data modify storage {0}:global _temp_trans[3] set from entity 6c6f6f6d-0-0-0-ffff Pos[0]\n"
       "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_trans\n"
       "$data modify storage {0}:global expr_float$(out_id) set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_cos",
     .data = std::format(
       "data modify storage {0}:global _temp_mul set value [1f,0f,0f,1f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
       "data modify storage {0}:global _temp_mul[15] set value 57.29578f\n"
       "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_mul\n"
       "data modify storage {0}:global _temp_var1 set value [1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
       "data modify storage {0}:global _temp_var1[3] set from storage {0}:global macro_args.value\n"
       "data modify storage {0}:global _temp_var1[15] set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"
       "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_var1\n"
       "data modify storage {0}:global macro_args.yaw set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"
       "function {0}:internal/loom/internal_float_cos_tp with storage {0}:global macro_args",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_cos_tp",
     .data = std::format(
       "$teleport 6c6f6f6d-0-0-0-ffff 0.0 0.0 0.0 $(yaw) 0\n"
       "execute as 6c6f6f6d-0-0-0-ffff at @s run teleport @s ^ ^ ^1\n"
       "$data modify storage {0}:global expr_float$(out_id) set from entity 6c6f6f6d-0-0-0-ffff Pos[2]",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_tan",
     .data = std::format(
       "data modify storage {0}:global _tan_val set from storage {0}:global macro_args.value\n"
       "data modify storage {0}:global macro_args set value {{out_id: 9991}}\n"
       "data modify storage {0}:global macro_args.value set from storage {0}:global _tan_val\n"
       "function {0}:internal/loom/internal_float_sin with storage {0}:global macro_args\n"
       "data modify storage {0}:global _tan_sin set from storage {0}:global expr_float9991\n"
       "data modify storage {0}:global macro_args set value {{out_id: 9992}}\n"
       "data modify storage {0}:global macro_args.value set from storage {0}:global _tan_val\n"
       "function {0}:internal/loom/internal_float_cos with storage {0}:global macro_args\n"
       "data modify storage {0}:global _tan_cos set from storage {0}:global expr_float9992\n"

       "data modify storage {0}:global _temp_div set value [1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
       "data modify storage {0}:global _temp_div[3] set from storage {0}:global _tan_sin\n"
       "data modify storage {0}:global _temp_div[15] set from storage {0}:global _tan_cos\n"
       "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_div\n"
       "$data modify storage {0}:global expr_float$(out_id) set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_atan2",
     .data = std::format(
       "data modify entity 6c6f6f6d-0-0-0-aaaa Pos[0] set from storage {0}:global macro_args.x\n"
       "data modify entity 6c6f6f6d-0-0-0-aaaa Pos[1] set value 0.0d\n"
       "data modify entity 6c6f6f6d-0-0-0-aaaa Pos[2] set from storage {0}:global macro_args.y\n"
       "teleport 6c6f6f6d-0-0-0-ffff 0.0 0.0 0.0\n"
       "execute as 6c6f6f6d-0-0-0-ffff at @s facing entity 6c6f6f6d-0-0-0-aaaa run teleport @s ~ ~ ~ ~ ~\n"
       "data modify storage {0}:global _temp_degrees set from entity 6c6f6f6d-0-0-0-ffff Rotation[0]\n"

       "data modify storage {0}:global _temp_mul set value [1f,0f,0f,1f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
       "data modify storage {0}:global _temp_mul[15] set value 0.0174533f\n"
       "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_mul\n"
       "data modify storage {0}:global _temp_var1 set value [1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
       "data modify storage {0}:global _temp_var1[3] set from storage {0}:global _temp_degrees\n"
       "data modify storage {0}:global _temp_var1[15] set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"
       "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_var1\n"
       "$data modify storage {0}:global expr_float$(out_id) set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_atan",
     .data = std::format(
       "data modify storage {0}:global macro_args.y set from storage {0}:global macro_args.value\n"
       "data modify storage {0}:global macro_args.x set value 1.0f\n"
       "function {0}:internal/loom/internal_float_atan2 with storage {0}:global macro_args",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_asin",
     .data = std::format(
       "data modify storage {0}:global _asin_val set from storage {0}:global macro_args.value\n"

       "data modify storage {0}:global _temp_mul set value [1f,0f,0f,1f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
       "data modify storage {0}:global _temp_mul[15] set from storage {0}:global _asin_val\n"
       "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_mul\n"
       "data modify storage {0}:global _temp_var1 set value [1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
       "data modify storage {0}:global _temp_var1[3] set from storage {0}:global _asin_val\n"
       "data modify storage {0}:global _temp_var1[15] set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"
       "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_var1\n"
       "data modify storage {0}:global _asin_sq set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"

       "data modify storage {0}:global _temp_trans set value [1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,-1f]\n"
       "data modify storage {0}:global _temp_trans[3] set from storage {0}:global _asin_sq\n"
       "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_trans\n"
       "data modify storage {0}:global _asin_neg_sq set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"
       "data modify storage {0}:global _asin_one set value 1.0f\n"
       "item modify block 18483211 -64 14504281 container.0 "
       "{{function:set_custom_model_data,floats:{{mode:replace_all,values:[{{type:sum,summands:[{{type:storage,storage:\"{0}:global\",path:\"_asin_one\"}},{{type:storage,"
       "storage:\"{0}:global\",path:\"_asin_neg_sq\"}}]}}]}}}}\n"
       "data modify storage {0}:global _asin_sub set from block 18483211 -64 14504281 Items[0].components.\"minecraft:custom_model_data\".floats[0]\n"

       "data modify storage {0}:global macro_args set value {{out_id: 9993}}\n"
       "data modify storage {0}:global macro_args.value set from storage {0}:global _asin_sub\n"
       "function {0}:internal/loom/internal_float_sqrt with storage {0}:global macro_args\n"

       "$data modify storage {0}:global macro_args set value {{out_id: $(out_id)}}\n"
       "data modify storage {0}:global macro_args.y set from storage {0}:global _asin_val\n"
       "data modify storage {0}:global macro_args.x set from storage {0}:global expr_float9993\n"
       "function {0}:internal/loom/internal_float_atan2 with storage {0}:global macro_args",
       datapackNamespace
     )}
  );
  internalFunctions.push_back(
    {.name = "internal_float_acos",
     .data = std::format(
       "data modify storage {0}:global _acos_val set from storage {0}:global macro_args.value\n"

       "data modify storage {0}:global _temp_mul set value [1f,0f,0f,1f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
       "data modify storage {0}:global _temp_mul[15] set from storage {0}:global _acos_val\n"
       "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_mul\n"
       "data modify storage {0}:global _temp_var1 set value [1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
       "data modify storage {0}:global _temp_var1[3] set from storage {0}:global _acos_val\n"
       "data modify storage {0}:global _temp_var1[15] set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"
       "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_var1\n"
       "data modify storage {0}:global _acos_sq set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"

       "data modify storage {0}:global _temp_trans set value [1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,-1f]\n"
       "data modify storage {0}:global _temp_trans[3] set from storage {0}:global _acos_sq\n"
       "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_trans\n"
       "data modify storage {0}:global _acos_neg_sq set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"
       "data modify storage {0}:global _acos_one set value 1.0f\n"
       "item modify block 18483211 -64 14504281 container.0 "
       "{{function:set_custom_model_data,floats:{{mode:replace_all,values:[{{type:sum,summands:[{{type:storage,storage:\"{0}:global\",path:\"_acos_one\"}},{{type:storage,"
       "storage:\"{0}:global\",path:\"_acos_neg_sq\"}}]}}]}}}}\n"
       "data modify storage {0}:global _acos_sub set from block 18483211 -64 14504281 Items[0].components.\"minecraft:custom_model_data\".floats[0]\n"

       "data modify storage {0}:global macro_args set value {{out_id: 9994}}\n"
       "data modify storage {0}:global macro_args.value set from storage {0}:global _acos_sub\n"
       "function {0}:internal/loom/internal_float_sqrt with storage {0}:global macro_args\n"

       "$data modify storage {0}:global macro_args set value {{out_id: $(out_id)}}\n"
       "data modify storage {0}:global macro_args.y set from storage {0}:global expr_float9994\n"
       "data modify storage {0}:global macro_args.x set from storage {0}:global _acos_val\n"
       "function {0}:internal/loom/internal_float_atan2 with storage {0}:global macro_args",
       datapackNamespace
     )}
  );

  for (uint32_t i = 0; i < ts_node_named_child_count(root); i++) {
    TSNode child = ts_node_named_child(root, i);
    std::string type = ts_node_type(child);

    if (type == "import_statement") {
      std::string importPathStr = std::string(getFieldText(child, "path"));

      std::filesystem::path importPath(importPathStr);
      std::filesystem::path absPath = std::filesystem::absolute(currentDir / importPathStr);
      std::string absPathStr = absPath.string();

      static std::unordered_set<std::string> importedFiles;
      if (importedFiles.contains(absPathStr)) {
        continue;
      }
      importedFiles.insert(absPathStr);

      std::ifstream f(absPath);
      if (!f.is_open()) {
        throw std::runtime_error("Compilation Error: Could not open imported file: " + importPath.string());
      }

      std::ostringstream buf;
      buf << f.rdbuf();
      std::string importedSource = buf.str();

      Compiler importCompiler(importedSource, datapackNamespace, absPath.parent_path());
      std::vector<CompiledFunction> importedFuncs = importCompiler.compile();

      for (const auto &[name, funcData] : importCompiler.funcs) {
        if (funcData.exported) {
          funcs[name] = funcData;
          funcs[name].exported = false;
        }
      }

      for (const auto &[name, varData] : importCompiler.vars) {
        if (varData.exported) {
          vars[name] = varData;
          vars[name].scope = root;
          vars[name].exported = false;
        }
      }

      for (const auto &[name, enumData] : importCompiler.enums) {
        if (enumData.exported) {
          enums[name] = enumData;
          enums[name].exported = false;
        }
      }

      for (const auto &func : importedFuncs) {
        compiledFunctions.push_back(func);
      }

      std::string importedInit = importCompiler.globalInit;
      if (importedInit.starts_with(setupScoreboards)) {
        importedInit = importedInit.substr(std::strlen(setupScoreboards));
      }

      std::istringstream stream(importedInit);
      std::string line;
      while (std::getline(stream, line)) {
        if (line.starts_with("scoreboard") || line.starts_with("data")) {
          globalInit += line + "\n";
        }
      }
      continue;
    }

    if (type == "enum_definition") {
      TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
      const std::string &enumName = std::string(getNodeText(nameNode));
      if (isBuiltin(enumName)) throw std::runtime_error(formatError(nameNode, "Reserved name."));

      EnumData enumData = {.name = enumName};

      bool typeKnown = false;
      int32_t nextValue = 0;
      for (uint32_t j = 0; j < ts_node_named_child_count(child); j++) {
        TSNode variantNode = ts_node_named_child(child, j);
        if (std::string(ts_node_type(variantNode)) != "enum_variant") continue;

        const std::string &varName = std::string(getFieldText(variantNode, "name"));
        TSNode valueNode = ts_node_child_by_field_name(variantNode, "value", 5);

        EnumVariant variant;
        variant.name = varName;

        if (!ts_node_is_null(valueNode)) {
          const std::string &valType = ts_node_type(valueNode);
          const std::string &rawText = std::string(getNodeText(valueNode));

          if (valType == "string_literal") {
            if (typeKnown && enumData.type != EnumType::String) {
              throw std::runtime_error(formatError(valueNode, "Cannot mix enum types."));
            }

            typeKnown = true;
            enumData.type = EnumType::String;
            variant.value = rawText;
          } else if (valType == "integer") {
            if (typeKnown && enumData.type != EnumType::Integer) {
              throw std::runtime_error(formatError(valueNode, "Cannot mix enum types."));
            }

            typeKnown = true;
            enumData.type = EnumType::Integer;
            const int32_t parsedInt = std::stoi(rawText);
            variant.value = parsedInt;
            nextValue = parsedInt + 1;
          } else if (valType == "float") {
            if (typeKnown && enumData.type != EnumType::Float) {
              throw std::runtime_error(formatError(valueNode, "Cannot mix enum types."));
            }

            typeKnown = true;
            enumData.type = EnumType::Float;
            variant.value = std::stof(rawText);
          }
        } else {
          if (typeKnown && enumData.type != EnumType::Integer) {
            throw std::runtime_error(formatError(variantNode, "Enum variants must be explicit for non-integer enums."));
          }
          typeKnown = true;
          enumData.type = EnumType::Integer;
          variant.value = nextValue++;
        }

        enumData.variants[varName] = variant;
      }

      for (uint32_t j = 0; j < ts_node_child_count(child); j++) {
        TSNode node = ts_node_child(child, j);
        const std::string &nodeType = std::string(ts_node_type(node));
        if (nodeType == "export") {
          if (enumData.exported) throw std::runtime_error(formatError(node, "Cannot use 'export' twice."));
          enumData.exported = true;
        }
      }

      enums[enumName] = enumData;
      continue;
    }

    if (type == "struct_definition") {
      TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
      const std::string &structName = std::string(getNodeText(nameNode));
      if (isBuiltin(structName)) throw std::runtime_error(formatError(nameNode, "Reserved name."));

      StructData structData = {.name = structName};

      for (uint32_t j = 0; j < ts_node_named_child_count(child); j++) {
        TSNode fieldNode = ts_node_named_child(child, j);
        if (std::string(ts_node_type(fieldNode)) != "struct_field") continue;

        const std::string &fieldName = std::string(getFieldText(fieldNode, "name"));
        TSNode typeNode = ts_node_child_by_field_name(fieldNode, "type", 4);

        Type fieldType = parseTypeFromString(std::string(getNodeText(typeNode)));

        structData.fields.push_back({.name = fieldName, .type = std::make_unique<Type>(std::move(fieldType))});
      }

      for (uint32_t j = 0; j < ts_node_child_count(child); j++) {
        TSNode node = ts_node_child(child, j);
        const std::string &nodeType = std::string(ts_node_type(node));
        if (nodeType == "export") {
          if (structData.exported) throw std::runtime_error(formatError(node, "Cannot use 'export' twice."));
          structData.exported = true;
        }
      }

      structs[structName] = std::move(structData);
      continue;
    }

    if (type == "function_definition") {
      TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
      std::string name = std::string(getNodeText(nameNode));
      std::transform(name.begin(), name.end(), name.begin(), ::tolower);

      if (isBuiltin(name)) throw std::runtime_error(formatError(nameNode, "Reserved name."));

      std::optional<std::string> tag = std::nullopt;
      TSNode tagNode = ts_node_child_by_field_name(child, "tag", 3);
      if (!ts_node_is_null(tagNode)) {
        tag = getNodeText(tagNode);
      }

      std::optional<Type> retType = std::nullopt;
      TSNode typeNode = ts_node_child_by_field_name(child, "type", 4);
      if (!ts_node_is_null(typeNode)) {
        const auto &typeText = std::string(getNodeText(typeNode));
        retType = parseTypeFromString(typeText);
      }

      bool isExport = false;
      bool isExtern = false;

      std::vector<Type> paramTypes;
      for (uint32_t j = 0; j < ts_node_child_count(child); j++) {
        TSNode node = ts_node_child(child, j);
        const std::string &nodeType = std::string(ts_node_type(node));
        if (nodeType == "parameter") {
          TSNode paramTypeNode = ts_node_child_by_field_name(node, "type", 4);
          std::string typeStr = std::string(getNodeText(paramTypeNode));

          Type pType = parseTypeFromString(typeStr);
          paramTypes.push_back(pType);
        } else if (nodeType == "export") {
          if (isExport) throw std::runtime_error(formatError(node, "Cannot use 'export' twice."));
          isExport = true;
        } else if (nodeType == "extern") {
          if (isExtern) throw std::runtime_error(formatError(node, "Cannot use 'extern' twice."));
          isExtern = true;
        }
      }

      std::string mangledName = name;
      if (!isExtern) mangledName += "_" + randomFunctionMangleString();

      funcs[name] = {.name = name, .mangledName = mangledName, .returnType = retType, .params = paramTypes, .tag = tag, .exported = isExport, .internal = !isExtern};
    }
  }

  for (uint32_t i = 0; i < ts_node_named_child_count(root); i++) {
    TSNode child = ts_node_named_child(root, i);
    const std::string type = ts_node_type(child);

    if (type == "variable_declaration") {
      globalInit += compileVariableDeclaration(child, root, true);
      continue;
    }

    if (type == "function_definition") {
      std::string name = std::string(getFieldText(child, "name"));
      std::transform(name.begin(), name.end(), name.begin(), ::tolower);

      TSNode blockNode = ts_node_child_by_field_name(child, "block", 5);

      std::vector<TSNode> paramNodes;
      for (uint32_t j = 0; j < ts_node_named_child_count(child); j++) {
        TSNode pNode = ts_node_named_child(child, j);
        if (std::string(ts_node_type(pNode)) == "parameter") {
          paramNodes.push_back(pNode);
        }
      }

      std::string paramSetup = "";
      for (auto it = paramNodes.rbegin(); it != paramNodes.rend(); ++it) {
        TSNode pNode = *it;
        std::string pName = std::string(getFieldText(pNode, "name"));
        std::string pTypeStr = std::string(getFieldText(pNode, "type"));
        Type pType = parseTypeFromString(pTypeStr);

        std::string mangledName = pName + "_" + randomMangleString();

        vars.emplace(
          pName,
          VariableData{
            .name = pName,
            .mangledName = mangledName,
            .type = pType,
            .scope = blockNode,
            .value = std::nullopt,
            .constant = false,
          }
        );

        if (pType.isString() || pType.isList() || pType.isFloat()) {
          paramSetup += std::format("data modify storage {0}:global vars.{1} set from storage {0}:stack regs[-1]\n", datapackNamespace, mangledName);
        } else {
          paramSetup += std::format("execute store result score {1} vars run data get storage {0}:stack regs[-1]\n", datapackNamespace, mangledName);
        }

        paramSetup += std::format("data remove storage {}:stack regs[-1]\n", datapackNamespace);
      }

      compiledFunctions.push_back({.name = funcs[name].mangledName, .data = paramSetup + compileBlock(blockNode), .tag = funcs[name].tag, .internal = funcs[name].internal});
      continue;
    }

    if (type != "comment" && type != "enum_definition" && type != "struct_definition" && type != "import_statement")
      throw std::runtime_error(formatError(child, "Invalid global statement: " + type));
  }

  return compiledFunctions;
}

;
