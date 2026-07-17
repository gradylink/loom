#include "compiler.hpp"
#include "typeHandler.hpp"
#include "utils.hpp"
#include <algorithm>
#include <format>
#include <stdexcept>

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

  if (type == "float") {
    if (precompute) {
      return {.data = std::string(getNodeText(node)), .precomputed = true, .type = Type::FloatType()};
    }
    return {
      .data = std::format("data modify storage {}:global expr_float{} set value {}f", datapackNamespace, id, getNodeText(node)),
      .precomputed = false,
      .type = Type::FloatType()
    };
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
    TSNode objNode = ts_node_child_by_field_name(node, "object", 6);

    try {
      Compiler::ExpressionData objExpr = compileExpression(objNode, id, precompute);
      if (TypeHandler *handler = getHandler(objExpr.type)) {
        if (auto optResult = handler->compileMemberExpression(*this, objExpr, prop, id, precompute, node)) {
          return optResult.value();
        }
      }
    } catch (...) {
      // enum
    }

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
          } else if constexpr (std::is_same_v<T, float>) {
            if (precompute) {
              return {.data = std::to_string(arg), .precomputed = true, .type = Type::EnumTypeOf(&enumIt->second)};
            }
            return {
              .data = std::format("data modify storage {}:global expr_float{} set value {}", datapackNamespace, id, arg),
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

    if (std::string(ts_node_type(argNode)) == "unary_expression" && op == getFieldText(argNode, "operator")) {
      return compileExpression(ts_node_child_by_field_name(argNode, "argument", 8), id, precompute);
    }

    if (TypeHandler *handler = getHandler(subExpr.type)) {
      if (auto optResult = handler->compileUnaryOp(*this, op, subExpr, id, precompute, node)) {
        return optResult.value();
      }
    }

    throw std::runtime_error(formatError(node, "Unknown unary operation: " + std::string(op)));
  }

  if (type == "function_call") {
    std::string targetFunc = std::string(getFieldText(node, "name"));
    std::transform(targetFunc.begin(), targetFunc.end(), targetFunc.begin(), ::tolower);

    std::vector<TSNode> argNodes;
    for (uint32_t i = 1; i < ts_node_named_child_count(node); i++) {
      argNodes.push_back(ts_node_named_child(node, i));
    }

    if (!argNodes.empty()) {
      Compiler::ExpressionData firstArg = compileExpression(argNodes[0], id, true);
      if (TypeHandler *handler = getHandler(firstArg.type)) {
        if (auto optResult = handler->compileBuiltinFunction(*this, targetFunc, argNodes, id, precompute, node)) {
          return optResult.value();
        }
      }
    }

    if (funcs.find(targetFunc) == funcs.end()) {
      throw std::runtime_error(formatError(node, "Unknown function: " + targetFunc));
    }

    const unsigned int targetSize = funcs[targetFunc].params.size();
    if (argNodes.size() != targetSize) {
      throw std::runtime_error(formatError(node, std::format("Function '{}' expects {} arguments, got {}", targetFunc, targetSize, argNodes.size())));
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
        if (argExpr.type.isString() || argExpr.type.isList()) {
          argPushData += std::format("data modify storage {}:stack regs append from storage {}:global expr_str{}\n", datapackNamespace, datapackNamespace, id);
        } else if (argExpr.type.isFloat()) {
          argPushData += std::format("data modify storage {}:stack regs append from storage {}:global expr_float{}\n", datapackNamespace, datapackNamespace, id);
        } else {
          argPushData += std::format(
            "execute store result storage {0}:global stack_temp int 1 run scoreboard players get expr_output1 temp\ndata modify storage {0}:stack regs append from storage "
            "{0}:global stack_temp\n",
            datapackNamespace
          );
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
      callCommand = std::format("function {}:{}{}", datapackNamespace, funcs[targetFunc].internal ? "internal/" : "", funcs[targetFunc].mangledName);
    } else {
      callCommand = std::format(
        "execute store result score expr_output{} temp run function {}:{}{}",
        id,
        datapackNamespace,
        funcs[targetFunc].internal ? "internal/" : "",
        funcs[targetFunc].mangledName
      );
    }

    return {.data = std::format("{}{}{}{}", push, argPushData, callCommand, pop), .precomputed = false, .type = funcType};
  }

  if (type == "variable_ref") {
    const std::string targetVar = std::string(getFieldText(node, "name"));

    if (vars.find(targetVar) == vars.end()) {
      throw std::runtime_error(formatError(node, "Unknown variable used in expression: " + targetVar));
    }

    if (vars[targetVar].value.has_value()) {
      if (precompute) {
        return {.data = vars[targetVar].value.value(), .precomputed = true, .type = vars[targetVar].type};
      }
      if (vars[targetVar].type.isString() || vars[targetVar].type.isList()) {
        return {
          .data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, vars[targetVar].value.value()),
          .precomputed = false,
          .type = vars[targetVar].type
        };
      }
      if (vars[targetVar].type.isFloat()) {
        return {
          .data = std::format("data modify storage {}:global expr_float{} set value {}f", datapackNamespace, id, vars[targetVar].value.value()),
          .precomputed = false,
          .type = vars[targetVar].type
        };
      }
      return {.data = std::format("scoreboard players set expr_output{} temp {}", id, vars[targetVar].value.value()), .precomputed = false, .type = vars[targetVar].type};
    }

    if (vars[targetVar].type.isString() || vars[targetVar].type.isList()) {
      return {
        .data =
          std::format("data modify storage {}:global expr_str{} set from storage {}:global vars.{}", datapackNamespace, id, datapackNamespace, vars[targetVar].mangledName),
        .precomputed = false,
        .type = vars[targetVar].type
      };
    }

    if (vars[targetVar].type.isFloat()) {
      return {
        .data =
          std::format("data modify storage {}:global expr_float{} set from storage {}:global vars.{}", datapackNamespace, id, datapackNamespace, vars[targetVar].mangledName),
        .precomputed = false,
        .type = vars[targetVar].type
      };
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
      useInternalFunction("internal_string_slice");
      runtimeCmds += std::format("function {}:internal/loom/internal_string_slice with storage {}:global macro_args", datapackNamespace, datapackNamespace);
    } else {
      useInternalFunction("internal_list_slice");
      runtimeCmds += std::format("function {}:internal/loom/internal_list_slice with storage {}:global macro_args", datapackNamespace, datapackNamespace);
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
      useInternalFunction("internal_string_slice");
      runtimeCmds += std::format(
        "scoreboard players operation expr_output{} temp = expr_output{} temp\n"
        "scoreboard players add expr_output{} temp 1\n"
        "data modify storage {}:global macro_args set value {{out_id: {}, target_id: {}}}\n"
        "execute store result storage {}:global macro_args.start int 1 run scoreboard players get expr_output{} temp\n"
        "execute store result storage {}:global macro_args.end int 1 run scoreboard players get expr_output{} temp\n"
        "function {}:internal/loom/internal_string_slice with storage {}:global macro_args",
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
        useInternalFunction("internal_list_slice");
        runtimeCmds += std::format("function {}:internal/loom/internal_list_get_object with storage {}:global macro_args", datapackNamespace, datapackNamespace);
      } else {
        useInternalFunction("internal_list_get_primitive");
        runtimeCmds += std::format("function {}:internal/loom/internal_list_get_primitive with storage {}:global macro_args", datapackNamespace, datapackNamespace);
      }
    }

    return {.data = runtimeCmds, .precomputed = false, .type = resultType};
  }

  if (type == "binary_expression") {
    TSNode leftNode = ts_node_child_by_field_name(node, "left", 4);
    TSNode rightNode = ts_node_child_by_field_name(node, "right", 5);

    const std::string_view op = getFieldText(node, "operator");

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
    }

    ExpressionData left = compileExpression(leftNode, id, true);
    ExpressionData right = compileExpression(rightNode, id + 1, true);

    if (left.type.isInteger() && right.type.isFloat()) {
      if (left.precomputed) {
        left.data = std::to_string(static_cast<float>(std::stoi(left.data)));
        left.type = Type::FloatType();
      } else {
        std::string promoteCmd = left.data + "\n";
        promoteCmd += std::format("execute store result storage {0}:global expr_float{1} float 1 run scoreboard players get expr_output{1} temp", datapackNamespace, id);
        left.data = promoteCmd;
        left.type = Type::FloatType();
      }
    } else if (left.type.isFloat() && right.type.isInteger()) {
      if (right.precomputed) {
        right.data = std::to_string(static_cast<float>(std::stoi(right.data)));
        right.type = Type::FloatType();
      } else {
        std::string promoteCmd = right.data + "\n";
        promoteCmd += std::format("execute store result storage {0}:global expr_float{1} float 1 run scoreboard players get expr_output{1} temp", datapackNamespace, id + 1);
        right.data = promoteCmd;
        right.type = Type::FloatType();
      }
    }

    if (TypeHandler *handler = getHandler(left.type)) {
      if (auto optResult = handler->compileBinaryOp(*this, op, left, right, id, precompute, node)) {
        return optResult.value();
      }
    }

    throw std::runtime_error(formatError(node, "No handler found for binary operation '" + std::string(op) + "' on the given types."));
  }

  throw std::runtime_error(formatError(node, "Unexpected type while compiling expression: " + type));
}
