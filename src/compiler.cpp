#include "compiler.hpp"
#include <algorithm>
#include <format>
#include <stdexcept>
#include <string_view>
#include <tree_sitter/api.h>

extern "C" const TSLanguage *tree_sitter_loom(void);

Compiler::Compiler(const std::string_view &source) : source(source) {
  parser = ts_parser_new();
  ts_parser_set_language(parser, tree_sitter_loom());

  tree = ts_parser_parse_string(parser, nullptr, this->source.c_str(), source.length());
  root = ts_tree_root_node(tree);
}

Compiler::~Compiler() {
  ts_tree_delete(tree);
  ts_parser_delete(parser);
}

std::string_view Compiler::getNodeText(TSNode node) {
  const uint32_t start = ts_node_start_byte(node);
  const uint32_t end = ts_node_end_byte(node);
  return std::string_view(source.data() + start, end - start);
}

Compiler::ExpressionData Compiler::compileExpression(TSNode node, unsigned int id, bool precompute) {
  if (ts_node_is_null(node)) {
    throw std::runtime_error("Malformed Expression");
  }

  const std::string type = ts_node_type(node);

  if (type == "integer") {
    if (precompute) {
      return {
          .data = std::string(getNodeText(node)),
          .precomputed = true
      };
    }
    return {
        .data = std::format("scoreboard players set expr_output{} temp {}", id, getNodeText(node)),
        .precomputed = false
    };
  }
  if (type == "unary_expression") {
    TSNode opNode = ts_node_child_by_field_name(node, "operator", 8);
    TSNode argNode = ts_node_child_by_field_name(node, "argument", 8);

    const ExpressionData subExpr = compileExpression(argNode, id, true);
    const std::string_view op = getNodeText(opNode);

    if (op == "-" && std::string_view(ts_node_type(argNode)) == "unary_expression" && getNodeText(ts_node_child_by_field_name(argNode, "operator", 8)) == "-") {
      return compileExpression(ts_node_child_by_field_name(argNode, "argument", 8), id, precompute);
    }

    if (subExpr.precomputed) {
      if (!precompute) {
        return {
            .data = std::format("scoreboard players set expr_output{} temp {}", id, std::string(op) + subExpr.data),
            .precomputed = false
        };
      }
      return {
          .data = std::string(op) + subExpr.data,
          .precomputed = true
      };
    }
    if (op == "-") {
      return {
          .data = std::format("{}\nscoreboard players operation expr_output{} temp *= invert temp", subExpr.data, id),
          .precomputed = false
      };
    }
    throw std::runtime_error("Unknown unary operation: " + std::string(op));
  }
  if (type == "function_call") {
    std::string targetFunc = std::string(getNodeText(ts_node_child_by_field_name(node, "name", 4)));
    std::transform(targetFunc.begin(), targetFunc.end(), targetFunc.begin(), ::tolower);
    if (funcs[targetFunc].returnType != ReturnType::Integer) {
      throw std::runtime_error("Attempted to use a function that doesn't return an integer in expression.");
    }

    std::string push;
    std::string pop;
    for (unsigned int i = 0; i < id; i++) {
      push += std::format("execute store result storage loom:stack regs append int 1 run scoreboard players get expr_output{} temp\n", i);
      pop = std::format("\nexecute store result score expr_output{} temp run data get storage loom:stack regs[-1]\ndata remove storage loom:stack regs[-1]", i) + pop;
    }

    return {
        .data = std::format("{}execute store result score expr_output{} temp run function loom:{}{}", push, id, funcs[targetFunc].name, pop),
        .precomputed = false
    };
  }
  if (type == "variable_ref") {
    const std::string targetVar = std::string(getNodeText(ts_node_child_by_field_name(node, "name", 4)));

    if (vars[targetVar].type != Type::Integer) {
      throw std::runtime_error("Attempted to use a non-integer variable in expression.");
    }

    if (vars[targetVar].value.has_value() && precompute) {
      return {
          .data = std::to_string(vars[targetVar].value.value()),
          .precomputed = true
      };
    }

    return {
        .data = std::format("scoreboard players operation expr_output{} temp = {} vars", id, vars[targetVar].mangledName),
        .precomputed = false
    };
  }
  if (type == "binary_expression") {
    TSNode leftNode = ts_node_child_by_field_name(node, "left", 4);
    TSNode rightNode = ts_node_child_by_field_name(node, "right", 5);
    ExpressionData left = compileExpression(leftNode, id, true);
    ExpressionData right = compileExpression(rightNode, id + 1, true);
    const std::string_view op = getNodeText(ts_node_child_by_field_name(node, "operator", 8));

    if (left.precomputed && right.precomputed) {
      std::string result;
      if (op == "+") result = std::to_string(std::stoi(left.data) + std::stoi(right.data));
      if (op == "-") result = std::to_string(std::stoi(left.data) - std::stoi(right.data));
      if (op == "*") result = std::to_string(std::stoi(left.data) * std::stoi(right.data));
      if (op == "/") result = std::to_string(std::stoi(left.data) / std::stoi(right.data));
      if (op == "%") result = std::to_string(std::stoi(left.data) % std::stoi(right.data));

      if (precompute) return {
          .data = result,
          .precomputed = true
      };
      return {
          .data = std::format("scoreboard players set expr_output{} temp {}", id, result),
          .precomputed = false
      };
    }

    if (left.precomputed) {
      left.data = std::format("scoreboard players set expr_output{} temp {}", id, left.data);
    } else if (right.precomputed) {
      right.data = std::format("scoreboard players set expr_output{} temp {}", id + 1, right.data);
    }

    return {
        .data = std::format("{}\n{}\nscoreboard players operation expr_output{} temp {}= expr_output{} temp", left.data, right.data, id, op, id + 1),
        .precomputed = false
    };
  }

  throw std::runtime_error("Unexpected type while compiling expression: " + type);
}
