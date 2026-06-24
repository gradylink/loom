#include "compiler.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tree_sitter/api.h>
#include <unordered_set>
#include <vector>

extern "C" const TSLanguage *tree_sitter_loom(void);

static std::string randomMangleString() {
  static constexpr std::string_view chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  static constexpr unsigned int len = 8;
  static std::random_device rd;
  static std::mt19937 generator(rd());
  std::uniform_int_distribution<> distribution(0, chars.length() - 1);

  std::string ret;
  ret.reserve(len);
  for (unsigned int i = 0; i < len; i++) ret += chars[distribution(generator)];
  return ret;
}

static std::string randomFunctionMangleString() {
  static constexpr std::string_view chars = "abcdefghijklmnopqrstuvwxyz";
  static constexpr unsigned int len = 12;
  static std::random_device rd;
  static std::mt19937 generator(rd());
  std::uniform_int_distribution<> distribution(0, chars.length() - 1);

  std::string ret;
  ret.reserve(len);
  for (unsigned int i = 0; i < len; i++) ret += chars[distribution(generator)];
  return ret;
}

static std::string formatError(TSNode node, const std::string &message) {
  if (ts_node_is_null(node)) return message;
  TSPoint start = ts_node_start_point(node);
  return std::format("line {}, col {}: {}", start.row + 1, start.column + 1, message);
}

Compiler::Compiler(const std::string_view &source, const std::string &datapackNamespace, std::filesystem::path currentDir)
    : source(source), datapackNamespace(datapackNamespace), currentDir(currentDir) {
  parser = ts_parser_new();
  ts_parser_set_language(parser, tree_sitter_loom());

  tree = ts_parser_parse_string(parser, nullptr, this->source.data(), source.length());
  root = ts_tree_root_node(tree);
}

Compiler::~Compiler() {
  ts_tree_delete(tree);
  ts_parser_delete(parser);
}

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
  const auto &it = enums.find(typeText);
  if (it == enums.end()) throw std::runtime_error(std::format("Unknown type: {}", typeText));
  return Type::EnumTypeOf(&it->second);
}

Compiler::ExpressionData Compiler::compileExpression(TSNode node, unsigned int id, bool precompute) {
  if (ts_node_is_null(node)) {
    throw std::runtime_error(formatError(node, "Malformed Expression"));
  }

  const std::string type = ts_node_type(node);

  if (type == "parenthesized_expression") {
    return compileExpression(ts_node_named_child(node, 0), id, precompute);
  }

  if (type == "integer") {
    if (precompute) {
      return {.data = std::string(getNodeText(node)), .precomputed = true, .type = Type::IntegerType()};
    }
    return {.data = std::format("scoreboard players set expr_output{} temp {}", id, getNodeText(node)), .precomputed = false, .type = Type::IntegerType()};
  }

  if (type == "boolean") {
    const std::string &numericVal = (getNodeText(node) == "true") ? "1" : "0";
    if (precompute) {
      return {.data = numericVal, .precomputed = true, .type = Type::BooleanType()};
    }
    return {.data = std::format("scoreboard players set expr_output{} temp {}", id, numericVal), .precomputed = false, .type = Type::BooleanType()};
  }

  if (type == "string_literal") {
    const std::string &text = std::string(getNodeText(node));
    if (precompute) {
      return {.data = text, .precomputed = true, .type = Type::StringType()};
    }
    return {.data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, text), .precomputed = false, .type = Type::StringType()};
  }

  if (type == "list_expression") {
    uint32_t childCount = ts_node_named_child_count(node);

    Type elementType = Type::IntegerType();
    bool allPrecomputed = true;
    std::vector<ExpressionData> compiledElements;
    compiledElements.reserve(childCount);

    for (uint32_t i = 0; i < childCount; ++i) {
      TSNode child = ts_node_named_child(node, i);
      ExpressionData elemData = compileExpression(child, id + 1, true);

      if (i == 0) {
        elementType = elemData.type;
      } else if (elemData.type != elementType) {
        throw std::runtime_error(formatError(child, "Mismatched types inside list expression."));
      }

      if (!elemData.precomputed) {
        allPrecomputed = false;
      }
      compiledElements.push_back(elemData);
    }

    Type listType = Type::ListTypeOf(elementType);

    if (allPrecomputed) {
      std::string jsonArray = "[";
      for (size_t i = 0; i < compiledElements.size(); ++i) {
        jsonArray += compiledElements[i].data;
        if (i + 1 < compiledElements.size()) jsonArray += ",";
      }
      jsonArray += "]";

      if (precompute) {
        return {.data = jsonArray, .precomputed = true, .type = listType};
      }
      return {.data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, jsonArray), .precomputed = false, .type = listType};
    }

    // TODO: optimize begging of list being precomputed
    std::string runtimeCmds = std::format("data modify storage {}:global expr_str{} set value []\n", datapackNamespace, id);

    for (const auto &elemData : compiledElements) {
      if (elemData.precomputed) {
        runtimeCmds += std::format("data modify storage {}:global expr_str{} append value {}\n", datapackNamespace, id, elemData.data);
      } else {
        runtimeCmds += elemData.data + "\n";

        if (elemData.type.isString() || elemData.type.kind == Type::List) {
          runtimeCmds += std::format("data modify storage {}:global expr_str{} append from storage {}:global expr_str{}\n", datapackNamespace, id, datapackNamespace, id + 1);
        } else {
          runtimeCmds +=
            std::format("execute store result storage {}:global expr_str{} append int 1 run scoreboard players get expr_output{} temp\n", datapackNamespace, id, id + 1);
        }
      }
    }

    return {.data = runtimeCmds, .precomputed = false, .type = listType};
  }

  if (type == "ternary_expression") {
    TSNode conditionNode = ts_node_child_by_field_name(node, "condition", 9);
    TSNode leftNode = ts_node_child_by_field_name(node, "left", 4);
    TSNode rightNode = ts_node_child_by_field_name(node, "right", 5);
    ExpressionData condition = compileExpression(conditionNode, id + 2, true);

    if (!condition.type.isBoolean()) throw std::runtime_error(formatError(conditionNode, "Condition of ternary must be a boolean."));

    if (condition.precomputed) {
      return compileExpression(condition.data == "1" ? leftNode : rightNode, id, precompute);
    }
    ExpressionData left = compileExpression(leftNode, id + 1, false);
    ExpressionData right = compileExpression(rightNode, id, false);

    if (left.type != right.type) throw std::runtime_error(formatError(rightNode, "Both possible ternary outputs must match."));

    return {
      .data = std::format(
        "{0}\n{1}\nexecute if score expr_output{2} temp matches 1 run scoreboard players operation expr_output{3} temp = expr_output{2} temp",
        condition.data,
        right.data,
        id + 2,
        id
      ),
      .precomputed = false,
      .type = left.type
    };
  }

  if (type == "member_expression") {
    const std::string &obj = std::string(getFieldText(node, "object"));
    const std::string &prop = std::string(getFieldText(node, "property"));

    const auto &enumIt = enums.find(obj);
    if (enumIt != enums.end()) {
      const auto &enumRef = enumIt->second;

      auto varIt = enumRef.variants.find(prop);
      if (varIt == enumRef.variants.end()) {
        throw std::runtime_error(formatError(ts_node_child_by_field_name(node, "property", 8), std::format("Enum '{}' has no variant named '{}'", obj, prop)));
      }

      const EnumVariant &var = varIt->second;

      ExpressionData exprResult = std::visit(
        [&](auto &&arg) -> ExpressionData {
          using T = std::decay_t<decltype(arg)>;

          if constexpr (std::is_same_v<T, int32_t>) {
            if (precompute) {
              return {.data = std::to_string(arg), .precomputed = true, .type = Type::EnumTypeOf(&enumIt->second)};
            }
            return {.data = std::format("scoreboard players set expr_output{} temp {}", id, arg), .precomputed = false, .type = Type::EnumTypeOf(&enumIt->second)};
          } else if constexpr (std::is_same_v<T, std::string>) {
            if (precompute) {
              return {.data = arg, .precomputed = true, .type = Type::EnumTypeOf(&enumIt->second)};
            }
            return {
              .data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, arg),
              .precomputed = false,
              .type = Type::EnumTypeOf(&enumIt->second)
            };
          }
        },
        var.value
      );

      return exprResult;
    }
    throw std::runtime_error(formatError(ts_node_child_by_field_name(node, "object", 0), std::format("Unknown identifier: '{}'", obj)));
  }

  if (type == "unary_expression") {
    TSNode opNode = ts_node_child_by_field_name(node, "operator", 8);
    TSNode argNode = ts_node_child_by_field_name(node, "argument", 8);

    const std::string_view op = getNodeText(opNode);

    if (op == "entity") {
      return {
        .data = std::format("scoreboard players set expr_output{} temp 0\nexecute if entity {} run scoreboard players set expr_output{} temp 1", id, getNodeText(argNode), id),
        .precomputed = false,
        .type = Type::BooleanType()
      };
    }

    const ExpressionData subExpr = compileExpression(argNode, id, true);

    Type expType;
    if (op == "-") {
      expType = Type::IntegerType();
      if (!subExpr.type.isInteger() && !subExpr.type.isBoolean()) {
        throw std::runtime_error(formatError(node, "Unary minus '-' can only be applied to integers and booleans."));
      }
    } else if (op == "!") {
      expType = Type::BooleanType();
      if (!subExpr.type.isBoolean()) {
        throw std::runtime_error(formatError(node, "Logical NOT operator '!' can only be applied to booleans."));
      }
    } else {
      throw std::runtime_error(formatError(node, "Unknown unary operation: " + std::string(op)));
    }

    if (std::string(ts_node_type(argNode)) == "unary_expression" && op == getFieldText(argNode, "operator")) {
      return compileExpression(ts_node_child_by_field_name(argNode, "argument", 8), id, precompute);
    }

    if (subExpr.precomputed) {
      std::string finalVal;
      if (op == "-") {
        if (subExpr.data.starts_with('-')) {
          finalVal = subExpr.data.substr(1);
        } else {
          finalVal = std::string(op) + subExpr.data;
        }
      } else if (op == "!") {
        finalVal = (subExpr.data == "1") ? "0" : "1";
      }

      if (!precompute) {
        return {.data = std::format("scoreboard players set expr_output{} temp {}", id, finalVal), .precomputed = false, .type = expType};
      }
      return {.data = finalVal, .precomputed = true, .type = expType};
    }

    if (op == "-") {
      return {.data = std::format("{}\nscoreboard players operation expr_output{} temp *= invert temp", subExpr.data, id), .precomputed = false, .type = Type::IntegerType()};
    }

    if (op == "!") {
      return {
        .data = std::format(
          "{}\n"
          "scoreboard players set internal1 temp 1\n"
          "scoreboard players operation internal1 temp -= expr_output{} temp\n"
          "scoreboard players operation expr_output{} temp = internal1 temp",
          subExpr.data,
          id,
          id
        ),
        .precomputed = false,
        .type = Type::BooleanType()
      };
    }
  }

  if (type == "function_call") {
    std::string targetFunc = std::string(getFieldText(node, "name"));
    std::transform(targetFunc.begin(), targetFunc.end(), targetFunc.begin(), ::tolower);

    if (funcs.find(targetFunc) == funcs.end() && targetFunc != "append" && targetFunc != "remove" && targetFunc != "insert" && targetFunc != "len") {
      throw std::runtime_error(formatError(node, "Unknown function: " + targetFunc));
    }

    std::vector<TSNode> argNodes;
    for (uint32_t i = 1; i < ts_node_named_child_count(node); i++) {
      argNodes.push_back(ts_node_named_child(node, i));
    }

    unsigned int targetSize;
    if (targetFunc == "append" || targetFunc == "remove") targetSize = 2;
    else if (targetFunc == "insert") targetSize = 3;
    else if (targetFunc == "len") targetSize = 1;
    else targetSize = funcs[targetFunc].params.size();
    if (argNodes.size() != targetSize) {
      throw std::runtime_error(formatError(node, std::format("Function '{}' expects {} arguments, got {}", targetFunc, funcs[targetFunc].params.size(), argNodes.size())));
    }

    if (targetFunc == "len") {
      ExpressionData objExpr = compileExpression(argNodes[0], id, true);

      if (!objExpr.type.isList() && !objExpr.type.isString()) {
        throw std::runtime_error(formatError(argNodes[0], "Argument to 'len' must be a list or a string."));
      }

      if (objExpr.precomputed) {
        size_t length = 0;
        if (objExpr.type.isString()) {
          std::string rawStr = objExpr.data;
          if (rawStr.size() >= 2 && rawStr.front() == '"' && rawStr.back() == '"') {
            length = rawStr.size() - 2;
          } else {
            length = rawStr.size();
          }
        } else {
          length = std::count(objExpr.data.begin(), objExpr.data.end(), ',') + 1;
        }
        if (!precompute) return {.data = std::format("scoreboard players set expr_output{} temp {}", id, length), .precomputed = false, .type = Type::IntegerType()};
        return {.data = std::to_string(length), .precomputed = true, .type = Type::IntegerType()};
      }

      return {
        .data = std::format("{2}\nexecute store result score expr_output{0} temp run data get storage {1}:global expr_str{0}", id, datapackNamespace, objExpr.data),
        .precomputed = false,
        .type = Type::IntegerType()
      };
    }

    if (targetFunc == "append" || targetFunc == "remove" || targetFunc == "insert") {
      ExpressionData listExpr = compileExpression(argNodes[0], id, true);
      if (!listExpr.type.isList() && !listExpr.type.isString()) {
        throw std::runtime_error(formatError(argNodes[0], "First argument to '" + targetFunc + "' must be a list or a string."));
      }

      const bool &isStringMode = listExpr.type.isString();
      std::string cmds = listExpr.data + "\n";
      if (listExpr.precomputed) {
        cmds += std::format("data modify storage {0}:global expr_str{1} set value {2}\n", datapackNamespace, id, listExpr.data);
      }

      if (targetFunc == "append") {
        ExpressionData elemExpr = compileExpression(argNodes[1], id + 1, true);
        cmds += elemExpr.data + "\n";

        if (isStringMode) {
          if (!elemExpr.type.isString()) {
            throw std::runtime_error(formatError(argNodes[1], "Type mismatch: cannot append non-string to a string."));
          }
          if (elemExpr.precomputed) {
            cmds += std::format(
              "data modify storage {0}:global macro_args set value {{target_id: {1}, value: {2}}}\n"
              "function {0}:internal_string_append with storage {0}:global macro_args\n",
              datapackNamespace,
              id,
              elemExpr.data
            );
          } else {
            cmds += std::format(
              "data modify storage {0}:global macro_args set value {{target_id: {1}, elem_id: {2}}}\n"
              "function {0}:internal_string_append_str with storage {0}:global macro_args\n",
              datapackNamespace,
              id,
              id + 1
            );
          }
        } else {
          if (elemExpr.type != *listExpr.type.baseType) {
            throw std::runtime_error(formatError(argNodes[1], "Type mismatch: cannot append element to this list type."));
          }
          if (elemExpr.precomputed) {
            cmds += std::format("data modify storage {0}:global expr_str{1} append value {2}\n", datapackNamespace, id, elemExpr.data);
          } else if (elemExpr.type.isString() || elemExpr.type.isList()) {
            cmds += std::format("data modify storage {0}:global expr_str{1} append from storage {0}:global expr_str{2}\n", datapackNamespace, id, id + 1);
          } else {
            cmds +=
              std::format("execute store result storage {0}:global expr_str{1} append int 1 run scoreboard players get expr_output{2} temp\n", datapackNamespace, id, id + 1);
          }
        }
      } else if (targetFunc == "remove") {
        ExpressionData idxExpr = compileExpression(argNodes[1], id + 1, true);
        if (!idxExpr.type.isInteger()) {
          throw std::runtime_error(formatError(argNodes[1], "Index must evaluate to an integer."));
        }
        cmds += idxExpr.data + "\n";

        if (isStringMode) {
          if (idxExpr.precomputed) {
            int idxVal = std::stoi(idxExpr.data);
            cmds += std::format(
              "data modify storage {0}:global macro_args set value {{target_id: {1}, index: {2}, index_plus_one: {3}}}\n"
              "function {0}:internal_string_remove with storage {0}:global macro_args\n",
              datapackNamespace,
              id,
              idxVal,
              idxVal + 1
            );
          } else {
            cmds += std::format(
              "data modify storage {0}:global macro_args set value {{target_id: {1}}}\n"
              "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get expr_output{2} temp\n"
              "scoreboard players operation expr_output3 temp = expr_output{2} temp\n"
              "scoreboard players add expr_output3 temp 1\n"
              "execute store result storage {0}:global macro_args.index_plus_one int 1 run scoreboard players get expr_output3 temp\n"
              "function {0}:internal_string_remove with storage {0}:global macro_args\n",
              datapackNamespace,
              id,
              id + 1
            );
          }
        } else {
          if (idxExpr.precomputed) {
            cmds += std::format("data remove storage {0}:global expr_str{1}[{2}]\n", datapackNamespace, id, idxExpr.data);
          } else {
            cmds += std::format(
              "data modify storage {0}:global macro_args set value {{target_id: {1}}}\n"
              "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get expr_output{2} temp\n"
              "function {0}:internal_list_remove with storage {0}:global macro_args\n",
              datapackNamespace,
              id,
              id + 1
            );
          }
        }
      } else if (targetFunc == "insert") {
        ExpressionData idxExpr = compileExpression(argNodes[1], id + 1, true);
        ExpressionData elemExpr = compileExpression(argNodes[2], id + 2, true);
        if (!idxExpr.type.isInteger()) {
          throw std::runtime_error(formatError(argNodes[1], "Index must evaluate to an integer."));
        }

        if (isStringMode) {
          if (!elemExpr.type.isString()) {
            throw std::runtime_error(formatError(argNodes[2], "Type mismatch: cannot insert non-string into a string."));
          }
          cmds += idxExpr.data + "\n" + elemExpr.data + "\n";

          if (idxExpr.precomputed) {
            if (elemExpr.precomputed) {
              cmds += std::format(
                "data modify storage {0}:global macro_args set value {{target_id: {1}, index: {2}, value: {3}}}\n"
                "function {0}:internal_string_insert_value with storage {0}:global macro_args\n",
                datapackNamespace,
                id,
                idxExpr.data,
                elemExpr.data
              );
            } else {
              cmds += std::format(
                "data modify storage {0}:global macro_args set value {{target_id: {1}, index: {2}, elem_id: {3}}}\n"
                "function {0}:internal_string_insert_str with storage {0}:global macro_args\n",
                datapackNamespace,
                id,
                idxExpr.data,
                id + 2
              );
            }
          } else {
            cmds += std::format(
              "data modify storage {0}:global macro_args set value {{target_id: {1}, elem_id: {2}}}\n"
              "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get expr_output{3} temp\n",
              datapackNamespace,
              id,
              id + 2,
              id + 1
            );
            if (elemExpr.precomputed) {
              cmds += std::format(
                "data modify storage {0}:global macro_args.value set value {1}\n"
                "function {0}:internal_string_insert_value with storage {0}:global macro_args\n",
                datapackNamespace,
                elemExpr.data
              );
            } else {
              cmds += std::format("function {0}:internal_string_insert_str with storage {0}:global macro_args\n", datapackNamespace);
            }
          }
        } else {
          if (elemExpr.type != *listExpr.type.baseType) {
            throw std::runtime_error(formatError(argNodes[2], "Type mismatch: cannot insert element into this list type."));
          }
          cmds += idxExpr.data + "\n" + elemExpr.data + "\n";

          if (idxExpr.precomputed) {
            if (elemExpr.precomputed) {
              cmds += std::format("data modify storage {0}:global expr_str{1} insert {2} value {3}\n", datapackNamespace, id, idxExpr.data, elemExpr.data);
            } else if (elemExpr.type.isString() || elemExpr.type.isList()) {
              cmds += std::format("data modify storage {0}:global expr_str{1} insert {2} from storage {0}:global expr_str{3}\n", datapackNamespace, id, idxExpr.data, id + 2);
            } else {
              cmds += std::format(
                "execute store result storage {0}:global expr_str{1} insert {2} int 1 run scoreboard players get expr_output{3} temp\n",
                datapackNamespace,
                id,
                idxExpr.data,
                id + 2
              );
            }
          } else {
            if (elemExpr.precomputed) {
              cmds += std::format(
                "data modify storage {0}:global macro_args set value {{target_id: {1}, value: {2}}}\n"
                "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get expr_output{3} temp\n"
                "function {0}:internal_list_insert_value with storage {0}:global macro_args\n",
                datapackNamespace,
                id,
                elemExpr.data,
                id + 1
              );
            } else if (elemExpr.type.isString() || elemExpr.type.isList()) {
              cmds += std::format(
                "data modify storage {0}:global macro_args set value {{target_id: {1}, elem_id: {2}}}\n"
                "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get expr_output{3} temp\n"
                "function {0}:internal_list_insert_object with storage {0}:global macro_args\n",
                datapackNamespace,
                id,
                id + 2,
                id + 1
              );
            } else {
              cmds += std::format(
                "data modify storage {0}:global macro_args set value {{target_id: {1}, elem_id: {2}}}\n"
                "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get expr_output{3} temp\n"
                "function {0}:internal_list_insert_primitive with storage {0}:global macro_args\n",
                datapackNamespace,
                id,
                id + 2,
                id + 1
              );
            }
          }
        }
      }

      return {.data = cmds, .precomputed = false, .type = listExpr.type};
    }

    std::string argPushData = "";
    for (size_t i = 0; i < argNodes.size(); i++) {
      const ExpressionData argExpr = compileExpression(argNodes[i]);
      if (argExpr.type != funcs[targetFunc].params[i]) {
        throw std::runtime_error(formatError(argNodes[i], std::format("Argument {} type mismatch for function '{}'", i + 1, targetFunc)));
      }

      if (argExpr.precomputed) {
        argPushData += std::format("data modify storage {}:stack regs append value {}\n", datapackNamespace, argExpr.data);
      } else {
        argPushData += argExpr.data + "\n";
        if (argExpr.type.isString()) {
          argPushData += std::format("data modify storage {}:stack regs append from storage {}:global expr_str{}\n", datapackNamespace, datapackNamespace, id);
        } else {
          argPushData += std::format("execute store result storage {}:stack regs append int 1 run scoreboard players get expr_output1 temp\n", datapackNamespace);
        }
      }
    }

    std::string push;
    std::string pop;
    for (unsigned int i = 1; i < id; i++) {
      push += std::format("execute store result storage {}:stack regs append int 1 run scoreboard players get expr_output{} temp\n", datapackNamespace, i);
      pop = std::format(
              "\nexecute store result score expr_output{} temp run data get storage {}:stack regs[-1]\ndata remove storage {}:stack regs[-1]",
              i,
              datapackNamespace,
              datapackNamespace
            ) +
            pop;
    }

    Type funcType = Type::IntegerType();
    if (funcs[targetFunc].returnType.has_value()) funcType = funcs[targetFunc].returnType.value();

    std::string callCommand;
    if (!funcs[targetFunc].returnType.has_value()) {
      callCommand = std::format("function {}:{}", datapackNamespace, funcs[targetFunc].name);
    } else {
      callCommand = std::format("execute store result score expr_output{} temp run function {}:{}", id, datapackNamespace, funcs[targetFunc].mangledName);
    }

    return {.data = std::format("{}{}{}{}", push, argPushData, callCommand, pop), .precomputed = false, .type = funcType};
  }

  if (type == "variable_ref") {
    const std::string targetVar = std::string(getFieldText(node, "name"));

    if (vars.find(targetVar) == vars.end()) {
      throw std::runtime_error(formatError(node, "Unknown variable used in expression: " + targetVar));
    }

    if (vars[targetVar].type.isString()) {
      return {
        .data =
          std::format("data modify storage {}:global expr_str{} set from storage {}:global vars.{}", datapackNamespace, id, datapackNamespace, vars[targetVar].mangledName),
        .precomputed = false,
        .type = vars[targetVar].type
      };
    }

    if (vars[targetVar].value.has_value()) {
      if (precompute) {
        return {.data = std::to_string(vars[targetVar].value.value()), .precomputed = true, .type = vars[targetVar].type};
      }
      return {.data = std::format("scoreboard players set expr_output{} temp {}", id, vars[targetVar].value.value()), .precomputed = false, .type = vars[targetVar].type};
    }

    return {
      .data = std::format("scoreboard players operation expr_output{} temp = {} vars", id, vars[targetVar].mangledName),
      .precomputed = false,
      .type = vars[targetVar].type
    };
  }

  if (type == "slice_expression") {
    TSNode targetNode = ts_node_child_by_field_name(node, "target", 6);
    TSNode startNode = ts_node_child_by_field_name(node, "start", 5);
    TSNode endNode = ts_node_child_by_field_name(node, "end", 3);

    ExpressionData target = compileExpression(targetNode, id, true);
    ExpressionData start = compileExpression(startNode, id + 1, true);
    ExpressionData end = compileExpression(endNode, id + 2, true);

    if (!target.type.isString() && !target.type.isList()) {
      throw std::runtime_error(formatError(node, "Slice parameters can only be used on strings or lists."));
    }
    if (!start.type.isInteger() || !end.type.isInteger()) {
      throw std::runtime_error(formatError(node, "Slice ranges must evaluate to integer bounds."));
    }

    if (target.type.isString() && target.precomputed && start.precomputed && end.precomputed) {
      std::string rawStr = target.data;
      if (rawStr.size() >= 2 && (rawStr.front() == '"' || rawStr.front() == '\'')) {
        rawStr = rawStr.substr(1, rawStr.size() - 2);
      }
      int32_t sIdx = std::clamp(std::stoi(start.data), 0, (int32_t)rawStr.size());
      int32_t eIdx = std::clamp(std::stoi(end.data), sIdx, (int32_t)rawStr.size());
      const std::string &sliced = "\"" + rawStr.substr(sIdx, eIdx - sIdx) + "\"";

      if (precompute) return {.data = sliced, .precomputed = true, .type = Type::StringType()};
      return {.data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, sliced), .precomputed = false, .type = Type::StringType()};
    }

    if (target.precomputed) target.data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, target.data);
    if (start.precomputed) start.data = std::format("scoreboard players set expr_output{} temp {}", id + 1, start.data);
    if (end.precomputed) end.data = std::format("scoreboard players set expr_output{} temp {}", id + 2, end.data);

    std::string runtimeCmds = target.data + "\n" + start.data + "\n" + end.data + "\n";
    runtimeCmds += std::format(
      "data modify storage {}:global macro_args set value {{out_id: {}, target_id: {}}}\n"
      "execute store result storage {}:global macro_args.start int 1 run scoreboard players get expr_output{} temp\n"
      "execute store result storage {}:global macro_args.end int 1 run scoreboard players get expr_output{} temp\n",
      datapackNamespace,
      id,
      id,
      datapackNamespace,
      id + 1,
      datapackNamespace,
      id + 2
    );

    if (target.type.isString()) {
      runtimeCmds += std::format("function {}:internal_string_slice with storage {}:global macro_args", datapackNamespace, datapackNamespace);
    } else {
      runtimeCmds += std::format("function {}:internal_list_slice with storage {}:global macro_args", datapackNamespace, datapackNamespace);
    }

    return {.data = runtimeCmds, .precomputed = false, .type = target.type};
  }

  if (type == "element_expression") {
    TSNode targetNode = ts_node_child_by_field_name(node, "target", 6);
    TSNode indexNode = ts_node_child_by_field_name(node, "index", 5);

    ExpressionData target = compileExpression(targetNode, id, true);
    ExpressionData index = compileExpression(indexNode, id + 1, true);

    if (!index.type.isInteger()) {
      throw std::runtime_error(formatError(node, "Index must evaluate to an integer."));
    }
    if (!target.type.isString() && !target.type.isList()) {
      throw std::runtime_error(formatError(node, "Only strings and lists can be queried."));
    }

    Type resultType = target.type.isString() ? Type::StringType() : *target.type.baseType;

    if (target.type.isString() && target.precomputed && index.precomputed) {
      std::string rawStr = target.data;
      if (rawStr.size() >= 2 && (rawStr.front() == '"' || rawStr.front() == '\'')) {
        rawStr = rawStr.substr(1, rawStr.size() - 2);
      }
      int32_t idx = std::stoi(index.data);
      std::string singleChar = (idx >= 0 && idx < (int32_t)rawStr.size()) ? std::string(1, rawStr[idx]) : "";

      if (precompute) return {.data = "\"" + singleChar + "\"", .precomputed = true, .type = Type::StringType()};
      return {
        .data = std::format("data modify storage {}:global expr_str{} set value \"{}\"", datapackNamespace, id, singleChar),
        .precomputed = false,
        .type = Type::StringType()
      };
    }

    if (target.precomputed) target.data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, target.data);
    if (index.precomputed) index.data = std::format("scoreboard players set expr_output{} temp {}", id + 1, index.data);

    std::string runtimeCmds = target.data + "\n" + index.data + "\n";

    if (target.type.isString()) {
      runtimeCmds += std::format(
        "scoreboard players operation expr_output{} temp = expr_output{} temp\n"
        "scoreboard players add expr_output{} temp 1\n"
        "data modify storage {}:global macro_args set value {{out_id: {}, target_id: {}}}\n"
        "execute store result storage {}:global macro_args.start int 1 run scoreboard players get expr_output{} temp\n"
        "execute store result storage {}:global macro_args.end int 1 run scoreboard players get expr_output{} temp\n"
        "function {}:internal_string_slice with storage {}:global macro_args",
        id + 2,
        id + 1,
        id + 2,
        datapackNamespace,
        id,
        id,
        datapackNamespace,
        id + 1,
        datapackNamespace,
        id + 2,
        datapackNamespace,
        datapackNamespace
      );
    } else {
      runtimeCmds += std::format(
        "data modify storage {}:global macro_args set value {{out_id: {}, target_id: {}}}\n"
        "execute store result storage {}:global macro_args.index int 1 run scoreboard players get expr_output{} temp\n",
        datapackNamespace,
        id,
        id,
        datapackNamespace,
        id + 1
      );

      if (resultType.isString() || resultType.isList()) {
        runtimeCmds += std::format("function {}:internal_list_get_object with storage {}:global macro_args", datapackNamespace, datapackNamespace);
      } else {
        runtimeCmds += std::format("function {}:internal_list_get_primitive with storage {}:global macro_args", datapackNamespace, datapackNamespace);
      }
    }

    return {.data = runtimeCmds, .precomputed = false, .type = resultType};
  }

  if (type == "binary_expression") {
    TSNode leftNode = ts_node_child_by_field_name(node, "left", 4);
    TSNode rightNode = ts_node_child_by_field_name(node, "right", 5);

    const std::string_view op = getFieldText(node, "operator");

    ExpressionData left;
    ExpressionData right;

    if (op == "at") { // TODO: optimize in "!" unary operation
      return {
        .data = std::format(
          "scoreboard players set expr_output{} temp 0\nexecute if block {} {} run scoreboard players set expr_output{} temp 1",
          id,
          getNodeText(rightNode),
          getNodeText(leftNode),
          id
        ),
        .precomputed = false,
        .type = Type::BooleanType()
      };
    } else {
      left = compileExpression(leftNode, id, true);
      right = compileExpression(rightNode, id + 1, true);
    }

    if (op == "+" && (left.type.isString() || right.type.isString())) {
      if (!left.type.isString() || !right.type.isString()) {
        throw std::runtime_error(formatError(node, "Implicit concatenation coercion between strings and numeric primitives is not allowed."));
      }

      if (left.precomputed && right.precomputed) {
        std::string lStr = left.data;
        std::string rStr = right.data;
        if (lStr.size() >= 2 && (lStr.front() == '"' || lStr.front() == '\'')) lStr = lStr.substr(1, lStr.size() - 2);
        if (rStr.size() >= 2 && (rStr.front() == '"' || rStr.front() == '\'')) rStr = rStr.substr(1, rStr.size() - 2);
        std::string joined = "\"" + lStr + rStr + "\"";

        if (precompute) return {.data = joined, .precomputed = true, .type = Type::StringType()};
        return {.data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, joined), .precomputed = false, .type = Type::StringType()};
      }

      if (left.precomputed) left.data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, left.data);
      if (right.precomputed) right.data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id + 1, right.data);

      std::string runtimeCmds = left.data + "\n" + right.data + "\n";
      runtimeCmds += std::format(
        "data modify storage {}:global macro_args set value {{out_id: {}}}\n"
        "data modify storage {}:global macro_args.left set from storage {}:global expr_str{}\n"
        "data modify storage {}:global macro_args.right set from storage {}:global expr_str{}\n"
        "function {}:internal_string_concat with storage {}:global macro_args",
        datapackNamespace,
        id,
        datapackNamespace,
        datapackNamespace,
        id,
        datapackNamespace,
        datapackNamespace,
        id + 1,
        datapackNamespace,
        datapackNamespace
      );
      return {.data = runtimeCmds, .precomputed = false, .type = Type::StringType()};
    }

    const bool isMath = (op == "+" || op == "-" || op == "*" || op == "/" || op == "%");
    const bool isComparison = (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=");
    const bool isLogical = (op == "&&" || op == "||");

    if (!isMath && !isComparison && !isLogical) {
      throw std::runtime_error(formatError(node, "Unknown binary operation: " + std::string(op)));
    }

    if (isLogical) {
      if (!left.type.isBoolean() || !right.type.isBoolean()) {
        throw std::runtime_error(formatError(node, "Logical operators '&&' and '||' require boolean operands."));
      }
    } else {
      if ((!left.type.isInteger() && !left.type.isBoolean()) || (!right.type.isInteger() && !right.type.isBoolean())) {
        throw std::runtime_error(formatError(node, "Invalid operand types for binary operation: " + std::string(op)));
      }
    }

    const Type expectedReturnType = isMath ? Type::IntegerType() : Type::BooleanType();

    if (left.precomputed && right.precomputed) {
      const int32_t lVal = std::stoi(left.data);
      const int32_t rVal = std::stoi(right.data);
      std::string result;

      if (op == "+") result = std::to_string(lVal + rVal);
      else if (op == "-") result = std::to_string(lVal - rVal);
      else if (op == "*") result = std::to_string(lVal * rVal);
      else if (op == "/") {
        if (rVal == 0) throw std::runtime_error(formatError(node, "Division by zero at compile-time."));
        result = std::to_string(lVal / rVal);
      } else if (op == "%") {
        if (rVal == 0) throw std::runtime_error(formatError(node, "Modulo by zero at compile-time."));
        result = std::to_string(lVal % rVal);
      } else if (op == "==") result = (lVal == rVal) ? "1" : "0";
      else if (op == "!=") result = (lVal != rVal) ? "1" : "0";
      else if (op == "<") result = (lVal < rVal) ? "1" : "0";
      else if (op == ">") result = (lVal > rVal) ? "1" : "0";
      else if (op == "<=") result = (lVal <= rVal) ? "1" : "0";
      else if (op == ">=") result = (lVal >= rVal) ? "1" : "0";
      else if (op == "&&") result = (lVal && rVal) ? "1" : "0";
      else if (op == "||") result = (lVal || rVal) ? "1" : "0";

      if (precompute) return {.data = result, .precomputed = true, .type = expectedReturnType};
      return {.data = std::format("scoreboard players set expr_output{} temp {}", id, result), .precomputed = false, .type = expectedReturnType};
    }

    if (left.precomputed) {
      left.data = std::format("scoreboard players set expr_output{} temp {}", id, left.data);
    } else if (right.precomputed) {
      right.data = std::format("scoreboard players set expr_output{} temp {}", id + 1, right.data);
    }

    std::string runtimeCommands = left.data + "\n" + right.data + "\n";

    if (isMath) {
      runtimeCommands += std::format("scoreboard players operation expr_output{} temp {}= expr_output{} temp", id, op, id + 1);
    } else if (isComparison) {
      std::string mcOp = std::string(op);
      std::string condType = "if";

      if (op == "==") {
        mcOp = "=";
      } else if (op == "!=") {
        mcOp = "=";
        condType = "unless";
      }

      runtimeCommands += std::format(
        "scoreboard players set internal1 temp 0\n"
        "execute {} score expr_output{} temp {} expr_output{} temp run scoreboard players set internal1 temp 1\n"
        "scoreboard players operation expr_output{} temp = internal1 temp",
        condType,
        id,
        mcOp,
        id + 1,
        id
      );
    } else if (isLogical) {
      if (op == "&&") {
        runtimeCommands += std::format("scoreboard players operation expr_output{} temp *= expr_output{} temp", id, id + 1);
      } else if (op == "||") {
        runtimeCommands += std::format(
          "scoreboard players operation expr_output{} temp += expr_output{} temp\n"
          "execute if score expr_output{} temp matches 1.. run scoreboard players set expr_output{} temp 1",
          id,
          id + 1,
          id,
          id
        );
      }
    }

    return {.data = runtimeCommands, .precomputed = false, .type = expectedReturnType};
  }

  throw std::runtime_error(formatError(node, "Unexpected type while compiling expression: " + type));
}

std::optional<std::string> Compiler::optimizeCommand(const std::string &commandName, const std::vector<TSNode> &args) {
  bool hasInterpolation = false;

  for (TSNode arg : args) {
    if (std::string(ts_node_type(arg)) == "interpolation") {
      hasInterpolation = true;
      break;
    }
  }

  if (!hasInterpolation) return std::nullopt;

  std::string setup = "";

  auto buildJsonTextArray = [&](size_t startIdx) -> std::string {
    if (startIdx >= args.size()) return "[]";
    std::string out = "[";
    for (size_t i = startIdx; i < args.size(); ++i) {
      TSNode arg = args[i];
      std::string argType = ts_node_type(arg);

      if (argType == "interpolation") {
        TSNode expNode = ts_node_child_by_field_name(arg, "expression", 10);
        if (std::string(ts_node_type(expNode)) == "variable_ref") {
          const std::string mangledName = vars[std::string(getFieldText(expNode, "name"))].mangledName;
          out += std::format(R"({{"score":{{"name":"{}","objective":"vars"}},"color":"white"}})", mangledName);
        } else {
          setup += compileExpression(expNode, 1, false).data + "\n";
          out += R"({{"score":{{"name":"expr_output1","objective":"temp"}},"color":"white"}})";
        }
      } else {
        std::string val = std::string(getNodeText(arg));
        out += std::format(R"({{"text":"{}","color":"white"}})", val);
      }

      if (i < args.size() - 1) out += R"(,{"text":" ","color":"white"},)";
    }
    out += "]";
    return out;
  };

  if (commandName == "say") {
    return std::format(
      R"({}tellraw @a [{{"text":"[","color":"white"}},{{"selector":"@s","color":"white"}},{{"text":"] ","color":"white"}},{}])",
      setup,
      buildJsonTextArray(0).substr(1)
    );
  }

  if (commandName == "tellraw" && args.size() >= 2) {
    std::string target = std::string(getNodeText(args[0]));
    return std::format("{}tellraw {} {}", setup, target, buildJsonTextArray(1));
  }

  if (commandName == "title" && args.size() >= 3) {
    std::string target = std::string(getNodeText(args[0]));
    std::string position = std::string(getNodeText(args[1]));
    return std::format("{}title {} {} {}", setup, target, position, buildJsonTextArray(2));
  }

  if ((commandName == "msg" || commandName == "tell" || commandName == "w") && args.size() >= 2) {
    std::string target = std::string(getNodeText(args[0]));
    return std::format(
      R"({}tellraw {} [{{"text":"[","color":"gray"}},{{"selector":"@s"}},{{"text":" -> ","color":"gray"}},{{"text":"{}"}},{{"text":"] ","color":"gray"}},{}])",
      setup,
      target,
      target,
      target,
      buildJsonTextArray(1).substr(1)
    );
  }

  if (commandName == "bossbar" && args.size() >= 4) {
    std::string action = std::string(getNodeText(args[1]));
    std::string id = std::string(getNodeText(args[2]));
    std::string property = std::string(getNodeText(args[3]));
    if (action == "set" && property == "name") {
      return std::format("{}bossbar set {} name {}", setup, id, buildJsonTextArray(4));
    }
  }

  return std::nullopt;
}

std::vector<Compiler::CompiledFunction> Compiler::compile() {
  compiledFunctions.clear();

  compiledFunctions.push_back(
    {.name = "internal_string_concat", .data = std::format("$data modify storage {}:global expr_str$(out_id) set value \"$(left)$(right)\"", datapackNamespace)}
  );
  compiledFunctions.push_back(
    {.name = "internal_string_slice",
     .data = std::format("$data modify storage {0}:global expr_str$(out_id) set string storage {0}:global expr_str$(target_id) $(start) $(end)", datapackNamespace)}
  );
  compiledFunctions.push_back(
    {.name = "internal_string_mutate_static",
     .data = std::format(
       "$data modify storage {0}:global macro_args.before set string storage {0}:global vars.$(var_name)$(path) 0 $(index)\n"
       "$data modify storage {0}:global macro_args.after set string storage {0}:global vars.$(var_name)$(path) $(index_plus_one)\n"
       "$data modify storage {0}:global vars.$(var_name)$(path) set value \"$(before)$(value)$(after)\"",
       datapackNamespace
     )}
  );
  compiledFunctions.push_back(
    {.name = "internal_string_mutate_dynamic",
     .data = std::format(
       "$data modify storage {0}:global macro_args.before set string storage {0}:global vars.$(var_name)$(path) 0 $(index)\n"
       "$data modify storage {0}:global macro_args.after set string storage {0}:global vars.$(var_name)$(path) $(index_plus_one)\n"
       "$data modify storage {0}:global vars.$(var_name)$(path) set value \"$(before)$(value)$(after)\"",
       datapackNamespace
     )}
  );
  compiledFunctions.push_back(
    {.name = "internal_string_append",
     .data = std::format(
       "$data modify storage {0}:global macro_args.left set from storage {0}:global expr_str$(target_id)\n"
       "$data modify storage {0}:global expr_str$(target_id) set value \"$(left)$(value)\"",
       datapackNamespace
     )}
  );
  compiledFunctions.push_back(
    {.name = "internal_string_append_str",
     .data = std::format(
       "$data modify storage {0}:global macro_args.left set from storage {0}:global expr_str$(target_id)\n"
       "$data modify storage {0}:global macro_args.right set from storage {0}:global expr_str$(elem_id)\n"
       "$data modify storage {0}:global expr_str$(target_id) set value \"$(left)$(right)\"",
       datapackNamespace
     )}
  );
  compiledFunctions.push_back(
    {.name = "internal_string_remove",
     .data = std::format(
       "$data modify storage {0}:global macro_args.before set string storage {0}:global expr_str$(target_id) 0 $(index)\n"
       "$data modify storage {0}:global macro_args.after set string storage {0}:global expr_str$(target_id) $(index_plus_one)\n"
       "$data modify storage {0}:global expr_str$(target_id) set value \"$(before)$(after)\"",
       datapackNamespace
     )}
  );
  compiledFunctions.push_back(
    {.name = "internal_string_insert_value",
     .data = std::format(
       "$data modify storage {0}:global macro_args.before set string storage {0}:global expr_str$(target_id) 0 $(index)\n"
       "$data modify storage {0}:global macro_args.after set string storage {0}:global expr_str$(target_id) $(index)\n"
       "$data modify storage {0}:global expr_str$(target_id) set value \"$(before)$(value)$(after)\"",
       datapackNamespace
     )}
  );
  compiledFunctions.push_back(
    {.name = "internal_string_insert_str",
     .data = std::format(
       "$data modify storage {0}:global macro_args.before set string storage {0}:global expr_str$(target_id) 0 $(index)\n"
       "$data modify storage {0}:global macro_args.after set string storage {0}:global expr_str$(target_id) $(index)\n"
       "$data modify storage {0}:global macro_args.right set from storage {0}:global expr_str$(elem_id)\n"
       "$data modify storage {0}:global expr_str$(target_id) set value \"$(before)$(right)$(after)\"",
       datapackNamespace
     )}
  );

  compiledFunctions.push_back(
    {.name = "internal_list_get_primitive",
     .data = std::format("$execute store result score expr_output$(out_id) temp run data get storage {}:global expr_str$(target_id)[$(index)]", datapackNamespace)}
  );
  compiledFunctions.push_back(
    {.name = "internal_list_get_object",
     .data = std::format("$data modify storage {0}:global expr_str$(out_id) set from storage {0}:global expr_str$(target_id)[$(index)]", datapackNamespace)}
  );
  compiledFunctions.push_back(
    {.name = "internal_list_slice",
     .data = std::format(
       "$data modify storage {0}:global expr_str$(out_id) set value []\n"
       "$data modify storage {0}:global macro_args.current int $(start)\n"
       "function {0}:internal_list_slice_loop with storage {0}:global macro_args",
       datapackNamespace
     )}
  );
  compiledFunctions.push_back(
    {.name = "internal_list_slice_loop",
     .data = std::format(
       "execute store result score internal_current temp run data get storage {0}:global macro_args.current\n"
       "execute store result score internal_end temp run data get storage {0}:global macro_args.end\n"
       "$execute if score internal_current temp < internal_end temp run data modify storage {0}:global expr_str$(out_id) append from storage {0}:global "
       "expr_str$(target_id)[$(current)]\n"
       "execute if score internal_current temp < internal_end temp run data modify storage {0}:global macro_args.current add value 1\n"
       "execute if score internal_current temp < internal_end temp run function {0}:internal_list_slice_loop with storage {0}:global macro_args",
       datapackNamespace
     )}
  );
  compiledFunctions.push_back({.name = "internal_list_remove", .data = std::format("$data remove storage {0}:global expr_str$(target_id)[$(index)]", datapackNamespace)});
  compiledFunctions.push_back(
    {.name = "internal_list_insert_value", .data = std::format("$data modify storage {0}:global expr_str$(target_id) insert $(index) value $(value)", datapackNamespace)}
  );
  compiledFunctions.push_back(
    {.name = "internal_list_insert_object",
     .data = std::format("$data modify storage {0}:global expr_str$(target_id) insert $(index) from storage {0}:global expr_str$(elem_id)", datapackNamespace)}
  );
  compiledFunctions.push_back(
    {.name = "internal_list_insert_primitive",
     .data = std::format(
       "$execute store result storage {0}:global expr_str$(target_id) insert $(index) int 1 run scoreboard players get expr_output$(elem_id) temp",
       datapackNamespace
     )}
  );
  compiledFunctions.push_back(
    {.name = "internal_path_append",
     .data = std::format("$data modify storage {0}:global macro_args.path set value \"$(string_before)[$(index_to_append)]\"", datapackNamespace)}
  );
  compiledFunctions.push_back(
    {.name = "internal_list_nested_set_value", .data = std::format("$data modify storage {0}:global vars.$(var_name)$(path) set value $(value)", datapackNamespace)}
  );
  compiledFunctions.push_back(
    {.name = "internal_list_nested_set_object",
     .data = std::format("$data modify storage {0}:global vars.$(var_name)$(path) set from storage {0}:global expr_str1", datapackNamespace)}
  );
  compiledFunctions.push_back(
    {.name = "internal_list_nested_set_primitive",
     .data = std::format("$execute store result storage {0}:global vars.$(var_name)$(path) int 1 run scoreboard players get expr_output1 temp", datapackNamespace)}
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
        if (func.name != "internal_string_concat" && func.name != "internal_string_slice") {
          compiledFunctions.push_back(func);
        }
      }

      std::string importedInit = importCompiler.globalInit;
      if (importedInit.starts_with(setupScoreboards)) {
        importedInit = importedInit.substr(std::strlen(setupScoreboards));
      }
      globalInit += importedInit;

      continue;
    }

    if (type == "enum_definition") {
      TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
      const std::string &enumName = std::string(getNodeText(nameNode));
      if (enumName == "append" || enumName == "remove" || enumName == "insert" || enumName == "len") throw std::runtime_error(formatError(nameNode, "Reserved name."));

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
            if (typeKnown && enumData.type == EnumType::Integer) {
              throw std::runtime_error(formatError(valueNode, "Cannot mix enum types."));
            }

            typeKnown = true;
            enumData.type = EnumType::String;
            variant.value = rawText;
          } else if (valType == "integer") {
            if (typeKnown && enumData.type == EnumType::String) {
              throw std::runtime_error(formatError(valueNode, "Cannot mix enum types."));
            }

            typeKnown = true;
            enumData.type = EnumType::Integer;
            const int32_t parsedInt = std::stoi(rawText);
            variant.value = parsedInt;
            nextValue = parsedInt + 1;
          }
        } else {
          if (typeKnown && enumData.type == EnumType::String) {
            throw std::runtime_error(formatError(variantNode, "Enum variants must be explicit for string enums."));
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

    if (type == "function_definition") {
      TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
      std::string name = std::string(getNodeText(nameNode));
      std::transform(name.begin(), name.end(), name.begin(), ::tolower);

      if (name == "append" || name == "remove" || name == "insert" || name == "len") throw std::runtime_error(formatError(nameNode, "Reserved name."));

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

      funcs[name] = {.name = name, .mangledName = mangledName, .returnType = retType, .params = paramTypes, .tag = tag, .exported = isExport};
    }
  }

  for (uint32_t i = 0; i < ts_node_named_child_count(root); i++) {
    TSNode child = ts_node_named_child(root, i);
    const std::string type = ts_node_type(child);

    if (type == "variable_declaration") {
      TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
      const std::string name = std::string(getNodeText(nameNode));
      if (name == "append" || name == "remove" || name == "insert" || name == "len") throw std::runtime_error(formatError(nameNode, "Reserved name."));

      const ExpressionData expr = compileExpression(ts_node_child_by_field_name(child, "value", 5));
      const bool constant = getFieldText(child, "keyword") == "const";
      TSNode varTypeNode = ts_node_child_by_field_name(child, "type", 4);
      const auto &typeText = getNodeText(varTypeNode);
      std::optional<int32_t> value = std::nullopt;

      Type varType = parseTypeFromString(std::string(typeText));

      if (constant && expr.precomputed) {
        value = std::stoi(expr.data);
      }

      bool isExport = false;
      bool isExtern = false;
      for (uint32_t j = 0; j < ts_node_child_count(child); j++) {
        TSNode node = ts_node_child(child, j);
        const std::string &nodeType = std::string(ts_node_type(node));
        if (nodeType == "export") {
          if (isExport) throw std::runtime_error(formatError(node, "Cannot use 'export' twice."));
          isExport = true;
        } else if (nodeType == "extern") {
          if (isExtern) throw std::runtime_error(formatError(node, "Cannot use 'extern' twice."));
          isExtern = true;
        }
      }

      std::string mangled = name;
      if (!isExtern) mangled += "_" + randomMangleString();

      vars.emplace(name, VariableData{.name = name, .mangledName = mangled, .type = varType, .scope = root, .value = value, .constant = constant, .exported = isExport});

      if (!value.has_value() || !constant || true /* temp, const vars aren't inlined yet */) {
        if (expr.precomputed) {
          if (varType.isString()) {
            globalInit += std::format("data modify storage {}:global vars.{} set value {}\n", datapackNamespace, mangled, expr.data);
          } else {
            globalInit += std::format("scoreboard players set {} vars {}\n", mangled, expr.data);
          }
        } else {
          if (varType.isString()) {
            globalInit +=
              std::format("{}\ndata modify storage {}:global vars.{} set from storage {}:global expr_str1\n", expr.data, datapackNamespace, mangled, datapackNamespace);
          } else {
            globalInit += std::format("{}\nscoreboard players operation {} vars = expr_output1 temp\n", expr.data, mangled);
          }
        }
      }
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

        paramSetup += std::format("execute store result score {} vars run data get storage {}:stack regs[-1]\n", mangledName, datapackNamespace);
        paramSetup += std::format("data remove storage {}:stack regs[-1]\n", datapackNamespace);
      }

      compiledFunctions.push_back({.name = funcs[name].mangledName, .data = paramSetup + compileBlock(blockNode), .tag = funcs[name].tag});
      continue;
    }

    if (type != "comment" && type != "enum_definition" && type != "import_statement") throw std::runtime_error(formatError(child, "Invalid global statement: " + type));
  }

  return compiledFunctions;
}

std::string Compiler::compileIf(TSNode ifRoot) {
  std::string ret = "";

  const std::string &name = "if_" + randomFunctionMangleString();
  const ExpressionData expr = compileExpression(ts_node_child_by_field_name(ifRoot, "expression", 10));

  if (!expr.type.isBoolean()) throw std::runtime_error(formatError(ifRoot, "Invalid type for if statement expression."));

  TSNode blockNode = ts_node_child_by_field_name(ifRoot, "block", 5);

  const bool hasElse = ts_node_named_child_count(ifRoot) > 2;
  TSNode altNode;
  if (hasElse) altNode = ts_node_named_child(ifRoot, 2);

  if (expr.precomputed) {
    if (expr.data == "1") {
      ret += compileBlock(blockNode);
    } else if (hasElse) {
      std::string altType = ts_node_type(altNode);
      if (altType == "block") {
        ret += compileBlock(altNode);
      } else if (altType == "if") {
        ret += compileIf(altNode);
      }
    }
    return ret;
  }

  ret += expr.data + "\n";

  std::string condScore = "expr_output1";
  if (hasElse) {
    condScore = name + "_condition";
    ret += std::format("scoreboard players operation {} temp = expr_output1 temp\n", condScore);
  }

  const std::string &trueData = compileBlock(blockNode);
  const size_t trueLineCount = std::count(trueData.begin(), trueData.end(), '\n');

  if (trueLineCount > 0) {
    if (trueLineCount == 1) {
      ret += std::format("execute if score {} temp matches 1 run {}", condScore, trueData);
    } else {
      compiledFunctions.push_back({.name = name + "_true", .data = trueData});
      ret += std::format("execute if score {} temp matches 1 run function {}:{}_true\n", condScore, datapackNamespace, name);
    }
  }

  if (hasElse) {
    std::string altData;
    std::string altType = ts_node_type(altNode);

    if (altType == "block") {
      altData = compileBlock(altNode);
    } else if (altType == "if") {
      altData = compileIf(altNode);
    }

    const size_t altLineCount = std::count(altData.begin(), altData.end(), '\n');

    if (altLineCount > 0) {
      if (altLineCount == 1) {
        ret += std::format("execute unless score {} temp matches 1 run {}", condScore, altData);
      } else {
        compiledFunctions.push_back({.name = name + "_false", .data = altData});
        ret += std::format("execute unless score {} temp matches 1 run function {}:{}_false\n", condScore, datapackNamespace, name);
      }
    }
  }

  return ret;
};

std::string Compiler::compileWhile(TSNode whileNode) {
  std::string ret = "";
  const std::string &loopName = "while_" + randomFunctionMangleString();

  TSNode condNode = ts_node_child_by_field_name(whileNode, "condition", 9);
  TSNode blockNode = ts_node_child_by_field_name(whileNode, "block", 5);

  const ExpressionData &condExpr = compileExpression(condNode);
  if (!condExpr.type.isBoolean()) {
    throw std::runtime_error(formatError(condNode, "While loop condition must evaluate to a boolean."));
  }

  if (condExpr.precomputed && condExpr.data == "0") return "";

  std::string loopFuncBody = compileBlock(blockNode);
  if (!condExpr.precomputed) {
    loopFuncBody += condExpr.data + "\n";
    loopFuncBody += std::format("execute if score expr_output1 temp matches 1 run function {}:{}\n", datapackNamespace, loopName);
  } else if (condExpr.data == "1") {
    loopFuncBody += std::format("function {}:{}\n", datapackNamespace, loopName);
  }

  compiledFunctions.push_back({.name = loopName, .data = loopFuncBody});

  if (condExpr.precomputed) {
    ret += std::format("function {}:{}\n", datapackNamespace, loopName);
  } else {
    ret += condExpr.data + "\n";
    ret += std::format("execute if score expr_output1 temp matches 1 run function {}:{}\n", datapackNamespace, loopName);
  }

  return ret;
}

std::string Compiler::compileDoWhile(TSNode doWhileNode) {
  std::string ret = "";
  const std::string &loopName = "dowhile_" + randomFunctionMangleString();

  TSNode blockNode = ts_node_child_by_field_name(doWhileNode, "block", 5);
  TSNode condNode = ts_node_child_by_field_name(doWhileNode, "condition", 9);

  const ExpressionData &condExpr = compileExpression(condNode);
  if (!condExpr.type.isBoolean()) {
    throw std::runtime_error(formatError(condNode, "Do-while loop condition must evaluate to a boolean."));
  }

  std::string loopFuncBody = compileBlock(blockNode);
  if (!condExpr.precomputed) {
    loopFuncBody += condExpr.data + "\n";
    loopFuncBody += std::format("execute if score expr_output1 temp matches 1 run function {}:{}\n", datapackNamespace, loopName);
  } else if (condExpr.data == "1") {
    loopFuncBody += std::format("function {}:{}\n", datapackNamespace, loopName);
  }

  compiledFunctions.push_back({.name = loopName, .data = loopFuncBody});

  ret += std::format("function {}:{}\n", datapackNamespace, loopName);
  return ret;
}

std::string Compiler::compileFor(TSNode forNode) {
  std::string ret = "";
  const std::string &loopName = "for_" + randomFunctionMangleString();

  TSNode iterNode = ts_node_child_by_field_name(forNode, "iterator", 8);
  TSNode startNode = ts_node_child_by_field_name(forNode, "start", 5);
  TSNode endNode = ts_node_child_by_field_name(forNode, "end", 3);
  TSNode blockNode = ts_node_child_by_field_name(forNode, "block", 5);

  const std::string &iterName = std::string(getNodeText(iterNode));
  const std::string &iterMangled = iterName + "_" + randomMangleString();
  const std::string &endMangled = "limit_" + randomMangleString();

  const ExpressionData &startExpr = compileExpression(startNode, 1, false);
  const ExpressionData &endExpr = compileExpression(endNode, 2, false);

  if (!startExpr.type.isInteger() && !startExpr.type.isBoolean()) {
    throw std::runtime_error(formatError(startNode, "For loop start boundary must be an integer."));
  }
  if (!endExpr.type.isInteger() && !endExpr.type.isBoolean()) {
    throw std::runtime_error(formatError(endNode, "For loop end boundary must be an integer."));
  }

  vars.emplace(
    iterName,
    VariableData{.name = iterName, .mangledName = iterMangled, .type = Type::IntegerType(), .scope = blockNode, .value = std::nullopt, .constant = false}
  );

  if (startExpr.precomputed) {
    ret += std::format("scoreboard players set {} vars {}\n", iterMangled, startExpr.data);
  } else {
    ret += startExpr.data + "\n";
    ret += std::format("scoreboard players operation {} vars = expr_output1 temp\n", iterMangled);
  }

  if (endExpr.precomputed) {
    ret += std::format("scoreboard players set {} vars {}\n", endMangled, endExpr.data);
  } else {
    ret += endExpr.data + "\n";
    ret += std::format("scoreboard players operation {} vars = expr_output2 temp\n", endMangled);
  }

  std::string loopFuncBody = compileBlock(blockNode);
  loopFuncBody += std::format("scoreboard players add {} vars 1\n", iterMangled);
  loopFuncBody += std::format("execute if score {} vars < {} vars run function {}:{}\n", iterMangled, endMangled, datapackNamespace, loopName);

  compiledFunctions.push_back({.name = loopName, .data = loopFuncBody});

  ret += std::format("execute if score {} vars < {} vars run function {}:{}\n", iterMangled, endMangled, datapackNamespace, loopName);
  return ret;
}

std::string Compiler::compileBlock(TSNode node) {
  std::string ret = "";

  for (uint32_t i = 0; i < ts_node_named_child_count(node); i++) {
    TSNode child = ts_node_named_child(node, i);
    const std::string type = ts_node_type(child);

    if (type == "if") {
      ret += compileIf(child);
      continue;
    }

    if (type == "while") {
      ret += compileWhile(child);
      continue;
    }

    if (type == "do_while") {
      ret += compileDoWhile(child);
      continue;
    }

    if (type == "for") {
      ret += compileFor(child);
      continue;
    }

    if (type == "context_statement") {
      std::string contextChain = "execute";

      const uint32_t childCount = ts_node_child_count(child);

      TSNode blockNode;

      for (uint32_t i = 0; i < childCount; i++) {
        TSNode subNode = ts_node_child(child, i);
        const std::string &subType = ts_node_type(subNode);

        if (subType == "block") {
          blockNode = subNode;
          break;
        }

        if (subType == "contextModifier") {
          TSNode keywordNode = ts_node_child(subNode, 0);
          const std::string keyword = std::string(getNodeText(keywordNode));

          if (keyword == "as" || keyword == "at") {
            const std::string selector = std::string(getNodeText(ts_node_child_by_field_name(subNode, "selector", 8)));
            contextChain += std::format(" {} {}", keyword, selector);
          } else if (keyword == "align") {
            const std::string axes = std::string(getNodeText(ts_node_child_by_field_name(subNode, "axes", 4)));
            contextChain += std::format(" align {}", axes);
          } else if (keyword == "anchored") {
            const std::string anchor = std::string(getNodeText(ts_node_child_by_field_name(subNode, "anchor", 6)));
            contextChain += std::format(" anchored {}", anchor);
          } else if (keyword == "facing") {
            const std::string secondText = std::string(getNodeText(ts_node_child(subNode, 1)));
            if (secondText == "entity") {
              const std::string selector = std::string(getNodeText(ts_node_child_by_field_name(subNode, "selector", 8)));
              const std::string anchor = std::string(getNodeText(ts_node_child_by_field_name(subNode, "anchor", 6)));
              contextChain += std::format(" facing entity {} {}", selector, anchor);
            } else {
              const std::string pos = std::string(getNodeText(ts_node_child_by_field_name(subNode, "pos", 3)));
              contextChain += std::format(" facing {}", pos);
            }
          } else if (keyword == "in") {
            const std::string dim = std::string(getNodeText(ts_node_child_by_field_name(subNode, "dim", 3)));
            contextChain += std::format(" in {}", dim);
          } else if (keyword == "on") {
            const std::string relation = std::string(getNodeText(ts_node_child_by_field_name(subNode, "relation", 8)));
            contextChain += std::format(" on {}", relation);
          } else if (keyword == "positioned") {
            const std::string secondText = std::string(getNodeText(ts_node_child(subNode, 1)));
            if (secondText == "as") {
              const std::string selector = std::string(getNodeText(ts_node_child_by_field_name(subNode, "selector", 8)));
              contextChain += std::format(" positioned as {}", selector);
            } else if (secondText == "over") {
              const std::string heightmap = std::string(getNodeText(ts_node_child_by_field_name(subNode, "heightmap", 9)));
              contextChain += std::format(" positioned over {}", heightmap);
            } else {
              const std::string pos = std::string(getNodeText(ts_node_child_by_field_name(subNode, "pos", 3)));
              contextChain += std::format(" positioned {}", pos);
            }
          } else if (keyword == "rotated") {
            const std::string secondText = std::string(getNodeText(ts_node_child(subNode, 1)));
            if (secondText == "as") {
              const std::string selector = std::string(getNodeText(ts_node_child_by_field_name(subNode, "selector", 8)));
              contextChain += std::format(" rotated as {}", selector);
            } else {
              const std::string rot = std::string(getNodeText(ts_node_child_by_field_name(subNode, "rot", 3)));
              contextChain += std::format(" rotated {}", rot);
            }
          }
        }
      }

      if (ts_node_is_null(blockNode)) {
        continue;
      }

      const std::string innerBlockData = compileBlock(blockNode);
      std::vector<std::string> validLines;
      std::stringstream ss(innerBlockData);
      std::string currentLine;
      while (std::getline(ss, currentLine)) {
        if (!currentLine.empty() && currentLine.back() == '\r') currentLine.pop_back();
        if (!currentLine.empty()) validLines.push_back(currentLine);
      }

      if (validLines.size() == 1) {
        ret += std::format("{} run {}\n", contextChain, validLines[0]);
      } else if (validLines.size() > 1) {
        const std::string &macroFuncName = std::format("_generated_function_{}", currentGeneratedFunction++);
        std::string macroBody = "";
        for (const auto &line : validLines) {
          macroBody += line + "\n";
        }
        compiledFunctions.push_back({.name = macroFuncName, .data = macroBody});
        ret += std::format("{} run function {}:{}\n", contextChain, datapackNamespace, macroFuncName);
      }
      continue;
    }

    if (type == "variable_declaration") {
      TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
      const std::string name = std::string(getNodeText(nameNode));
      if (name == "append" || name == "remove" || name == "insert" || name == "len") throw std::runtime_error(formatError(nameNode, "Reserved name."));

      const std::string mangledName = name + "_" + randomMangleString();
      const ExpressionData expr = compileExpression(ts_node_child_by_field_name(child, "value", 5));
      const bool constant = getFieldText(child, "keyword") == "const";
      TSNode varTypeNode = ts_node_child_by_field_name(child, "type", 4);
      const auto &typeText = getNodeText(varTypeNode);

      Type varType = parseTypeFromString(std::string(typeText));

      std::optional<int32_t> value = std::nullopt;
      if (constant && expr.precomputed) {
        value = std::stoi(expr.data);
      }

      vars.emplace(
        name,
        VariableData{
          .name = name,
          .mangledName = mangledName,
          .type = varType,
          .scope = node,
          .value = value,
          .constant = constant,
        }
      );

      if (!value.has_value() || !constant || true /* temp, const vars aren't inlined yet */) {
        if (expr.precomputed) {
          if (varType.isString()) {
            ret += std::format("data modify storage {}:global vars.{} set value {}\n", datapackNamespace, mangledName, expr.data);
          } else {
            ret += std::format("scoreboard players set {} vars {}\n", mangledName, expr.data);
          }
        } else {
          if (varType.isString()) {
            ret +=
              std::format("{}\ndata modify storage {}:global vars.{} set from storage {}:global expr_str1\n", expr.data, datapackNamespace, mangledName, datapackNamespace);
          } else {
            ret += std::format("{}\nscoreboard players operation {} vars = expr_output1 temp\n", expr.data, mangledName);
          }
        }
      }
      continue;
    }

    if (type == "assignment") {
      const std::string name = std::string(getFieldText(child, "name"));
      if (vars.find(name) == vars.end()) {
        throw std::runtime_error(formatError(child, "Assignment to undefined variable: " + name));
      }
      if (vars[name].constant) {
        throw std::runtime_error(formatError(child, "Cannot reassign constant variable: " + name));
      }

      TSNode expNode = ts_node_child_by_field_name(child, "value", 5);

      std::vector<TSNode> indexNodes;
      uint32_t namedCount = ts_node_named_child_count(child);
      for (uint32_t i = 1; i < namedCount - 1; i++) {
        indexNodes.push_back(ts_node_named_child(child, i));
      }
      const bool &isIndexed = !indexNodes.empty();

      if (isIndexed) {
        if (!vars[name].type.isList() && !vars[name].type.isString()) {
          throw std::runtime_error(formatError(child, "Cannot use index assignment on non-container variable: " + name));
        }

        const ExpressionData expr = compileExpression(expNode, 1, true);
        ret += expr.data + "\n";

        Compiler::Type expectedType = vars[name].type;
        bool endsInStringSubscript = false;

        if (expectedType.isString()) {
          if (indexNodes.size() != 1) {
            throw std::runtime_error(formatError(child, "Flat strings can only take a single index descriptor."));
          }
          endsInStringSubscript = true;
        } else {
          for (size_t i = 0; i < indexNodes.size(); i++) {
            if (!expectedType.isList()) {
              throw std::runtime_error(formatError(indexNodes[i], "Too many indices provided for this list depth."));
            }
            expectedType = *expectedType.baseType;
          }
          if (expectedType.isString()) {
            endsInStringSubscript = true;
          }
        }

        if (endsInStringSubscript) {
          if (!expr.type.isString()) {
            throw std::runtime_error(formatError(expNode, "Type mismatch: cannot assign non-string to a string character index."));
          }
        } else {
          if (expr.type != expectedType) {
            throw std::runtime_error(formatError(expNode, "Type mismatch: cannot assign value to this nested list element type."));
          }
        }

        std::vector<ExpressionData> compiledIndices;
        bool allIndicesPrecomputed = true;
        for (size_t i = 0; i < indexNodes.size(); i++) {
          ExpressionData idxExpr = compileExpression(indexNodes[i], 2, true);
          if (!idxExpr.type.isInteger()) {
            throw std::runtime_error(formatError(indexNodes[i], "Indices must evaluate to integers."));
          }
          if (!idxExpr.precomputed) {
            allIndicesPrecomputed = false;
          }
          compiledIndices.push_back(idxExpr);
        }

        std::string pathSuffix = "";
        size_t listDimensions = endsInStringSubscript ? indexNodes.size() - 1 : indexNodes.size();
        for (size_t i = 0; i < listDimensions; i++) {
          pathSuffix += "[" + compiledIndices[i].data + "]";
        }

        if (endsInStringSubscript) {
          ExpressionData stringCharIdx = compiledIndices.back();

          ret += std::format("data modify storage {0}:global macro_args.var_name set value \"{1}\"\n", datapackNamespace, vars[name].mangledName);

          if (expr.precomputed) {
            ret += std::format("data modify storage {0}:global macro_args.value set value {1}\n", datapackNamespace, expr.data);
          } else {
            ret += std::format("data modify storage {0}:global macro_args.value set from storage {0}:global expr_str1\n", datapackNamespace);
          }

          if (allIndicesPrecomputed) {
            ret += std::format("data modify storage {0}:global macro_args.path set value \"{1}\"\n", datapackNamespace, pathSuffix);
          } else {
            ret += std::format("data modify storage {0}:global macro_args.path set value \"\"\n", datapackNamespace);
            for (size_t i = 0; i < listDimensions; i++) {
              if (!compiledIndices[i].precomputed) ret += compiledIndices[i].data + "\n";
              ret += std::format("data modify storage {0}:global macro_args.string_before set from storage {0}:global macro_args.path\n", datapackNamespace);
              if (compiledIndices[i].precomputed) {
                ret += std::format("data modify storage {0}:global macro_args.index_to_append set value \"{1}\"\n", datapackNamespace, compiledIndices[i].data);
              } else {
                ret +=
                  std::format("execute store result storage {0}:global macro_args.index_to_append int 1 run scoreboard players get expr_output2 temp\n", datapackNamespace);
              }
              ret += std::format("function {0}:internal_path_append with storage {0}:global macro_args\n", datapackNamespace);
            }
          }

          if (stringCharIdx.precomputed) {
            int idxVal = std::stoi(stringCharIdx.data);
            ret += std::format(
              "data modify storage {0}:global macro_args.index set value {1}\n"
              "data modify storage {0}:global macro_args.index_plus_one set value {2}\n"
              "function {0}:internal_string_mutate_static with storage {0}:global macro_args\n",
              datapackNamespace,
              idxVal,
              idxVal + 1
            );
          } else {
            ret += stringCharIdx.data + "\n";
            ret += std::format(
              "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get expr_output2 temp\n"
              "scoreboard players operation expr_output3 temp = expr_output2 temp\n"
              "scoreboard players add expr_output3 temp 1\n"
              "execute store result storage {0}:global macro_args.index_plus_one int 1 run scoreboard players get expr_output3 temp\n"
              "function {0}:internal_string_mutate_dynamic with storage {0}:global macro_args\n",
              datapackNamespace
            );
          }
          continue;
        }

        if (allIndicesPrecomputed) {
          if (expr.precomputed) {
            ret += std::format("data modify storage {0}:global vars.{1}{2} set value {3}\n", datapackNamespace, vars[name].mangledName, pathSuffix, expr.data);
          } else if (expr.type.isString() || expr.type.isList()) {
            ret += std::format("data modify storage {0}:global vars.{1}{2} set from storage {0}:global expr_str1\n", datapackNamespace, vars[name].mangledName, pathSuffix);
          } else {
            ret += std::format(
              "execute store result storage {0}:global vars.{1}{2} int 1 run scoreboard players get expr_output1 temp\n",
              datapackNamespace,
              vars[name].mangledName,
              pathSuffix
            );
          }
        } else {
          ret += std::format("data modify storage {0}:global macro_args.path set value \"\"\n", datapackNamespace);
          for (size_t i = 0; i < indexNodes.size(); i++) {
            if (!compiledIndices[i].precomputed) ret += compiledIndices[i].data + "\n";
            ret += std::format("data modify storage {0}:global macro_args.string_before set from storage {0}:global macro_args.path\n", datapackNamespace);
            if (compiledIndices[i].precomputed) {
              ret += std::format("data modify storage {0}:global macro_args.index_to_append set value \"{1}\"\n", datapackNamespace, compiledIndices[i].data);
            } else {
              ret += std::format("execute store result storage {0}:global macro_args.index_to_append int 1 run scoreboard players get expr_output2 temp\n", datapackNamespace);
            }
            ret += std::format("function {0}:internal_path_append with storage {0}:global macro_args\n", datapackNamespace);
          }

          ret += std::format("data modify storage {0}:global macro_args.var_name set value \"{1}\"\n", datapackNamespace, vars[name].mangledName);
          if (expr.precomputed) {
            ret += std::format(
              "data modify storage {0}:global macro_args.value set value {1}\nfunction {0}:internal_list_nested_set_value with storage {0}:global macro_args\n",
              datapackNamespace,
              expr.data
            );
          } else if (expr.type.isString() || expr.type.isList()) {
            ret += std::format("function {0}:internal_list_nested_set_object with storage {0}:global macro_args\n", datapackNamespace);
          } else {
            ret += std::format("function {0}:internal_list_nested_set_primitive with storage {0}:global macro_args\n", datapackNamespace);
          }
        }
        continue;
      }

      const ExpressionData expr = compileExpression(expNode);

      if (vars[name].type.kind == Compiler::Type::Enum) {
        if (expr.type.kind != Compiler::Type::Enum || expr.type.enumRef != vars[name].type.enumRef) {
          throw std::runtime_error(formatError(expNode, "Assignment to enum variable requires a variant of the same enum: " + name));
        }
      } else {
        if (vars[name].type.isBoolean() && expr.type.isInteger()) {
          throw std::runtime_error(formatError(expNode, "Cannot assign an 'int' to 'bool' variable: " + name));
        }
        if (vars[name].type.isString() && !expr.type.isString()) {
          throw std::runtime_error(formatError(expNode, "Invalid type for 'string' variable: " + name));
        }
      }

      if (expr.precomputed) {
        if (vars[name].type.isString()) {
          ret += std::format("data modify storage {0}:global vars.{1} set value {2}\n", datapackNamespace, vars[name].mangledName, expr.data);
        } else {
          ret += std::format("scoreboard players set {} vars {}\n", vars[name].mangledName, expr.data);
        }
      } else {
        if (std::string(ts_node_type(expNode)) == "binary_expression" && !vars[name].type.isString()) {
          const std::string_view op = getFieldText(expNode, "operator");

          if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
            TSNode leftNode = ts_node_child_by_field_name(expNode, "left", 4);
            TSNode rightNode = ts_node_child_by_field_name(expNode, "right", 5);

            if (std::string(ts_node_type(leftNode)) == "variable_ref" && std::string(getFieldText(leftNode, "name")) == name) {
              const ExpressionData rightExpr = compileExpression(rightNode, 1, false);
              ret += std::format("{}\nscoreboard players operation {} vars {}= expr_output1 temp\n", rightExpr.data, vars[name].mangledName, op);
              continue;
            }
            if (std::string(ts_node_type(rightNode)) == "variable_ref" && std::string(getFieldText(rightNode, "name")) == name) {
              const ExpressionData leftExpr = compileExpression(leftNode, 1, false);
              ret += std::format("{}\nscoreboard players operation {} vars {}= expr_output1 temp\n", leftExpr.data, vars[name].mangledName, op);
              continue;
            }
          }
        }

        if (vars[name].type.isString()) {
          ret += std::format("{0}\ndata modify storage {1}:global vars.{2} set from storage {1}:global expr_str1\n", expr.data, datapackNamespace, vars[name].mangledName);
        } else {
          ret += std::format("{}\nscoreboard players operation {} vars = expr_output1 temp\n", expr.data, vars[name].mangledName);
        }
      }
      continue;
    }

    if (type == "function_call") {
      ExpressionData expr = compileExpression(child, 1, false);
      ret += expr.data + "\n";
      continue;
    }

    if (type == "return_statement") {
      TSNode exprNode = ts_node_named_child(child, 0);
      if (!ts_node_is_null(exprNode)) {
        ExpressionData expr = compileExpression(exprNode);
        if (expr.precomputed) {
          ret += std::format("return {}\n", expr.data);
        } else {
          ret += std::format("{}\nreturn run scoreboard players get expr_output1 temp\n", expr.data);
        }
      } else {
        ret += "return 0\n";
      }
      break;
    }

    if (type == "command_statement") {
      std::string cmdName = std::string(getNodeText(ts_node_named_child(child, 0)));
      std::vector<TSNode> args;
      std::vector<std::optional<Compiler::ExpressionData>> compiledArgs;
      bool requiresMacro = false;

      for (uint32_t j = 1; j < ts_node_named_child_count(child); j++) {
        TSNode argNode = ts_node_named_child(child, j);
        std::string argType = ts_node_type(argNode);

        if (argType == "command_arg" || argType == "integer") {
          args.push_back(argNode);
          compiledArgs.push_back(std::nullopt);
        } else if (argType == "interpolation") {
          args.push_back(argNode);

          TSNode exprNode = ts_node_child_by_field_name(argNode, "expression", 10);

          Compiler::ExpressionData expr = compileExpression(exprNode);
          compiledArgs.push_back(expr);

          if (!expr.precomputed) {
            requiresMacro = true;
          }
        }
      }

      std::optional<std::string> optimized = optimizeCommand(cmdName, args);
      if (optimized.has_value()) {
        ret += optimized.value() + "\n";
        continue;
      }

      if (!requiresMacro) {
        ret += cmdName;
        for (size_t i = 0; i < args.size(); i++) {
          if (compiledArgs[i].has_value()) {
            ret += " " + compiledArgs[i].value().data;
          } else {
            ret += " " + std::string(getNodeText(args[i]));
          }
        }
        ret += "\n";
        continue;
      }

      std::string macroBody = "$" + cmdName;
      std::string macroSetup = "";
      int macroVarId = 0;

      for (size_t i = 0; i < args.size(); i++) {
        if (compiledArgs[i].has_value()) {
          Compiler::ExpressionData &expr = compiledArgs[i].value();

          if (expr.precomputed) {
            macroBody += " " + expr.data;
          } else {
            macroBody += std::format(" $(var_{})", macroVarId);

            macroSetup += expr.data + "\n";

            macroSetup +=
              std::format("execute store result storage {}:function_input var_{} int 1 run scoreboard players get expr_output1 temp\n", datapackNamespace, macroVarId);
            macroVarId++;
          }
        } else {
          macroBody += " " + std::string(getNodeText(args[i]));
        }
      }

      const std::string macroFuncName = std::format("_generated_function_{}", currentGeneratedFunction++);
      compiledFunctions.push_back({.name = macroFuncName, .data = macroBody + "\n"});

      ret += macroSetup;
      ret += std::format("function {}:{} with storage {}:function_input\n", datapackNamespace, macroFuncName, datapackNamespace);
      continue;
    }

    if (type != "comment") throw std::runtime_error(formatError(child, "Invalid block statement: " + type));
  }

  std::erase_if(vars, [&node](const auto &pair) { return ts_node_eq(pair.second.scope, node); });

  return ret;
}
