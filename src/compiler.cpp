#include "compiler.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tree_sitter/api.h>
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

Compiler::Compiler(const std::string_view &source) : source(source) {
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

Compiler::ExpressionData Compiler::compileExpression(TSNode node, unsigned int id, bool precompute) {
  if (ts_node_is_null(node)) {
    throw std::runtime_error("Malformed Expression");
  }

  const std::string type = ts_node_type(node);

  if (type == "parenthesized_expression") {
    return compileExpression(ts_node_named_child(node, 0), id, precompute);
  }

  if (type == "integer") {
    if (precompute) {
      return {.data = std::string(getNodeText(node)), .precomputed = true, .type = Type::Integer};
    }
    return {.data = std::format("scoreboard players set expr_output{} temp {}", id, getNodeText(node)), .precomputed = false, .type = Type::Integer};
  }

  if (type == "boolean") {
    const std::string &numericVal = (getNodeText(node) == "true") ? "1" : "0";
    if (precompute) {
      return {.data = numericVal, .precomputed = true, .type = Type::Boolean};
    }
    return {.data = std::format("scoreboard players set expr_output{} temp {}", id, numericVal), .precomputed = false, .type = Type::Boolean};
  }

  if (type == "unary_expression") {
    TSNode opNode = ts_node_child_by_field_name(node, "operator", 8);
    TSNode argNode = ts_node_child_by_field_name(node, "argument", 8);

    const ExpressionData subExpr = compileExpression(argNode, id, true);
    const std::string_view op = getNodeText(opNode);

    Type expType;
    if (op == "-") {
      expType = Type::Integer;
      if (subExpr.type != Type::Integer && subExpr.type != Type::Boolean) {
        throw std::runtime_error(formatError(node, "Unary minus '-' can only be applied to integers and booleans."));
      }
    } else if (op == "!") {
      expType = Type::Boolean;
      if (subExpr.type != Type::Boolean) {
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
      return {.data = std::format("{}\nscoreboard players operation expr_output{} temp *= invert temp", subExpr.data, id), .precomputed = false, .type = Type::Integer};
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
        .type = Type::Boolean
      };
    }
  }

  if (type == "function_call") {
    std::string targetFunc = std::string(getFieldText(node, "name"));
    std::transform(targetFunc.begin(), targetFunc.end(), targetFunc.begin(), ::tolower);

    if (funcs.find(targetFunc) == funcs.end()) {
      throw std::runtime_error(formatError(node, "Unknown function: " + targetFunc));
    }

    std::vector<TSNode> argNodes;
    for (uint32_t i = 1; i < ts_node_named_child_count(node); i++) {
      argNodes.push_back(ts_node_named_child(node, i));
    }

    if (argNodes.size() != funcs[targetFunc].params.size()) {
      throw std::runtime_error(formatError(node, std::format("Function '{}' expects {} arguments, got {}", targetFunc, funcs[targetFunc].params.size(), argNodes.size())));
    }

    std::string argPushData = "";
    for (size_t i = 0; i < argNodes.size(); i++) {
      const ExpressionData argExpr = compileExpression(argNodes[i]);
      if (argExpr.type != funcs[targetFunc].params[i]) {
        throw std::runtime_error(formatError(argNodes[i], std::format("Argument {} type mismatch for function '{}'", i + 1, targetFunc)));
      }

      if (argExpr.precomputed) {
        argPushData += std::format("data modify storage loom:stack regs append value {}\n", argExpr.data);
      } else {
        argPushData += argExpr.data + "\n";
        argPushData += "execute store result storage loom:stack regs append int 1 run scoreboard players get expr_output1 temp\n";
      }
    }

    std::string push;
    std::string pop;
    for (unsigned int i = 1; i < id; i++) {
      push += std::format("execute store result storage loom:stack regs append int 1 run scoreboard players get expr_output{} temp\n", i);
      pop = std::format("\nexecute store result score expr_output{} temp run data get storage loom:stack regs[-1]\ndata remove storage loom:stack regs[-1]", i) + pop;
    }

    Type funcType = Type::Integer;
    if (funcs[targetFunc].returnType == ReturnType::Boolean) funcType = Type::Boolean;

    std::string callCommand;
    if (funcs[targetFunc].returnType == ReturnType::Void) {
      callCommand = std::format("function loom:{}", funcs[targetFunc].name);
    } else {
      callCommand = std::format("execute store result score expr_output{} temp run function loom:{}", id, funcs[targetFunc].name);
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

  if (type == "binary_expression") {
    TSNode leftNode = ts_node_child_by_field_name(node, "left", 4);
    TSNode rightNode = ts_node_child_by_field_name(node, "right", 5);
    ExpressionData left = compileExpression(leftNode, id, true);
    ExpressionData right = compileExpression(rightNode, id + 1, true);
    const std::string_view op = getFieldText(node, "operator");

    const bool isMath = (op == "+" || op == "-" || op == "*" || op == "/" || op == "%");
    const bool isComparison = (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=");
    const bool isLogical = (op == "&&" || op == "||");

    if (!isMath && !isComparison && !isLogical) {
      throw std::runtime_error(formatError(node, "Unknown binary operation: " + std::string(op)));
    }

    if (isLogical) {
      if (left.type != Type::Boolean || right.type != Type::Boolean) {
        throw std::runtime_error(formatError(node, "Logical operators '&&' and '||' require boolean operands."));
      }
    } else {
      if ((left.type != Type::Integer && left.type != Type::Boolean) || (right.type != Type::Integer && right.type != Type::Boolean)) {
        throw std::runtime_error(formatError(node, "Invalid operand types for binary operation: " + std::string(op)));
      }
    }

    const Type expectedReturnType = isMath ? Type::Integer : Type::Boolean;

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
  bool hasVars = false;
  for (TSNode arg : args) {
    if (std::string(ts_node_type(arg)) == "variable_ref") {
      hasVars = true;
      break;
    }
  }
  if (!hasVars) return std::nullopt;

  auto buildJsonTextArray = [&](size_t startIdx) -> std::string {
    if (startIdx >= args.size()) return "[]";
    std::string out = "[";
    for (size_t i = startIdx; i < args.size(); ++i) {
      TSNode arg = args[i];
      std::string argType = ts_node_type(arg);

      if (argType == "variable_ref") {
        std::string varName = std::string(getFieldText(arg, "name"));
        out += std::format(R"({{"score":{{"name":"{}","objective":"vars"}},"color":"white"}})", vars[varName].mangledName);
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
      R"(tellraw @a [{{"text":"[","color":"white"}},{{"selector":"@s","color":"white"}},{{"text":"] ","color":"white"}},{}])",
      buildJsonTextArray(0).substr(1)
    );
  }

  if (commandName == "tellraw" && args.size() >= 2) {
    std::string target = std::string(getNodeText(args[0]));
    return std::format("tellraw {} {}", target, buildJsonTextArray(1));
  }

  if (commandName == "title" && args.size() >= 3) {
    std::string target = std::string(getNodeText(args[0]));
    std::string position = std::string(getNodeText(args[1]));
    return std::format("title {} {} {}", target, position, buildJsonTextArray(2));
  }

  if ((commandName == "msg" || commandName == "tell" || commandName == "w") && args.size() >= 2) {
    std::string target = std::string(getNodeText(args[0]));
    return std::format(
      R"(tellraw {} [{{"text":"[","color":"gray"}},{{"selector":"@s"}},{{"text":" -> ","color":"gray"}},{{"text":"{}"}},{{"text":"] ","color":"gray"}},{}])",
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
      return std::format("bossbar set {} name {}", id, buildJsonTextArray(4));
    }
  }

  return std::nullopt;
}

std::vector<Compiler::CompiledFunction> Compiler::compile() {
  compiledFunctions.clear();
  std::string globalInit = setupScoreboards;

  for (uint32_t i = 0; i < ts_node_named_child_count(root); i++) {
    TSNode child = ts_node_named_child(root, i);
    std::string type = ts_node_type(child);
    if (type == "function_definition") {
      std::string name = std::string(getFieldText(child, "name"));
      std::transform(name.begin(), name.end(), name.begin(), ::tolower);

      ReturnType retType = ReturnType::Void;
      TSNode typeNode = ts_node_child_by_field_name(child, "type", 4);
      if (!ts_node_is_null(typeNode)) {
        const auto &typeText = getNodeText(typeNode);
        if (typeText == "int" || typeText == "integer") {
          retType = ReturnType::Integer;
        } else if (typeText == "bool" || typeText == "boolean") {
          retType = ReturnType::Boolean;
        } else {
          throw std::runtime_error(formatError(typeNode, "Invalid type in function definition: " + std::string(typeText)));
        }
      }

      std::vector<Type> paramTypes;
      for (uint32_t j = 0; j < ts_node_named_child_count(child); j++) {
        TSNode paramNode = ts_node_named_child(child, j);
        if (std::string(ts_node_type(paramNode)) == "parameter") {
          TSNode paramTypeNode = ts_node_child_by_field_name(paramNode, "type", 4);
          std::string typeStr = std::string(getNodeText(paramTypeNode));

          if (typeStr == "int" || typeStr == "integer") {
            paramTypes.push_back(Type::Integer);
          } else if (typeStr == "bool" || typeStr == "boolean") {
            paramTypes.push_back(Type::Boolean);
          } else {
            throw std::runtime_error(formatError(paramTypeNode, "Invalid parameter type: " + typeStr));
          }
        }
      }
      funcs[name] = {name, retType, paramTypes};
    }
  }

  for (uint32_t i = 0; i < ts_node_named_child_count(root); i++) {
    TSNode child = ts_node_named_child(root, i);
    const std::string type = ts_node_type(child);

    if (type == "variable_declaration") {
      const std::string name = std::string(getFieldText(child, "name"));
      const ExpressionData expr = compileExpression(ts_node_child_by_field_name(child, "value", 5));
      const bool constant = getFieldText(child, "keyword") == "const";
      TSNode varTypeNode = ts_node_child_by_field_name(child, "type", 4);
      const auto &typeText = getNodeText(varTypeNode);
      std::optional<int32_t> value = std::nullopt;

      Type varType;
      if (typeText == "int" || typeText == "integer") {
        varType = Type::Integer;
      } else if (typeText == "bool" || typeText == "boolean") {
        varType = Type::Boolean;
      } else {
        throw std::runtime_error(formatError(varTypeNode, "Invalid type in variable declaration: " + std::string(typeText)));
      }

      if (constant && expr.precomputed) {
        value = std::stoi(expr.data);
      }

      std::string mangled = name + "_" + randomMangleString();
      vars.emplace(
        name,
        VariableData{
          .name = name,
          .mangledName = mangled,
          .type = varType,
          .scope = root,
          .constant = constant,
          .value = value,
        }
      );

      if (!value.has_value() || !constant) {
        if (expr.precomputed) {
          globalInit += std::format("scoreboard players set {} vars {}\n", mangled, expr.data);
        } else {
          globalInit += std::format("{}\nscoreboard players operation {} vars = expr_output1 temp\n", expr.data, mangled);
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
        Type pType = (pTypeStr == "int" || pTypeStr == "integer") ? Type::Integer : Type::Boolean;

        std::string mangledName = pName + "_" + randomMangleString();

        vars.emplace(
          pName,
          VariableData{
            .name = pName,
            .mangledName = mangledName,
            .type = pType,
            .scope = blockNode,
            .constant = false,
            .value = std::nullopt,
          }
        );

        paramSetup += std::format("execute store result score {} vars run data get storage loom:stack regs[-1]\n", mangledName);
        paramSetup += "data remove storage loom:stack regs[-1]\n";
      }

      compiledFunctions.push_back({.name = name, .data = paramSetup + compileBlock(blockNode)});
      continue;
    }

    if (type != "comment") throw std::runtime_error(formatError(child, "Invalid global statement: " + type));
  }

  const auto &it = std::find_if(compiledFunctions.begin(), compiledFunctions.end(), [](CompiledFunction func) { return func.name == "load"; });
  if (it != compiledFunctions.end()) {
    it->data = globalInit + it->data;
  } else {
    compiledFunctions.push_back({.name = "load", .data = globalInit});
  }

  return compiledFunctions;
}

std::string Compiler::compileIf(TSNode ifRoot) {
  std::string ret = "";

  const std::string &name = "if_" + randomFunctionMangleString();
  const ExpressionData expr = compileExpression(ts_node_child_by_field_name(ifRoot, "expression", 10));

  if (expr.type != Type::Boolean) throw std::runtime_error(formatError(ifRoot, "Invalid type for if statement expression."));

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
      ret += std::format("execute if score {} temp matches 1 run {}\n", condScore, trueData);
    } else {
      compiledFunctions.push_back({.name = name + "_true", .data = trueData});
      ret += std::format("execute if score {} temp matches 1 run function loom:{}_true\n", condScore, name);
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
        ret += std::format("execute unless score {} temp matches 1 run {}\n", condScore, altData);
      } else {
        compiledFunctions.push_back({.name = name + "_false", .data = altData});
        ret += std::format("execute unless score {} temp matches 1 run function loom:{}_false\n", condScore, name);
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
  if (condExpr.type != Type::Boolean) {
    throw std::runtime_error(formatError(condNode, "While loop condition must evaluate to a boolean."));
  }

  if (condExpr.precomputed && condExpr.data == "0") return "";

  std::string loopFuncBody = compileBlock(blockNode);
  if (!condExpr.precomputed) {
    loopFuncBody += condExpr.data + "\n";
    loopFuncBody += std::format("execute if score expr_output1 temp matches 1 run function loom:{}\n", loopName);
  } else if (condExpr.data == "1") {
    loopFuncBody += std::format("function loom:{}\n", loopName);
  }

  compiledFunctions.push_back({.name = loopName, .data = loopFuncBody});

  if (condExpr.precomputed) {
    ret += std::format("function loom:{}\n", loopName);
  } else {
    ret += condExpr.data + "\n";
    ret += std::format("execute if score expr_output1 temp matches 1 run function loom:{}\n", loopName);
  }

  return ret;
}

std::string Compiler::compileDoWhile(TSNode doWhileNode) {
  std::string ret = "";
  const std::string &loopName = "dowhile_" + randomFunctionMangleString();

  TSNode blockNode = ts_node_child_by_field_name(doWhileNode, "block", 5);
  TSNode condNode = ts_node_child_by_field_name(doWhileNode, "condition", 9);

  const ExpressionData &condExpr = compileExpression(condNode);
  if (condExpr.type != Type::Boolean) {
    throw std::runtime_error(formatError(condNode, "Do-while loop condition must evaluate to a boolean."));
  }

  std::string loopFuncBody = compileBlock(blockNode);
  if (!condExpr.precomputed) {
    loopFuncBody += condExpr.data + "\n";
    loopFuncBody += std::format("execute if score expr_output1 temp matches 1 run function loom:{}\n", loopName);
  } else if (condExpr.data == "1") {
    loopFuncBody += std::format("function loom:{}\n", loopName);
  }

  compiledFunctions.push_back({.name = loopName, .data = loopFuncBody});

  ret += std::format("function loom:{}\n", loopName);
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

  if (startExpr.type != Type::Integer && startExpr.type != Type::Boolean) {
    throw std::runtime_error(formatError(startNode, "For loop start boundary must be an integer."));
  }
  if (endExpr.type != Type::Integer && endExpr.type != Type::Boolean) {
    throw std::runtime_error(formatError(endNode, "For loop end boundary must be an integer."));
  }

  vars.emplace(iterName, VariableData{.name = iterName, .mangledName = iterMangled, .type = Type::Integer, .scope = blockNode, .constant = false, .value = std::nullopt});

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
  loopFuncBody += std::format("execute if score {} vars < {} vars run function loom:{}\n", iterMangled, endMangled, loopName);

  compiledFunctions.push_back({.name = loopName, .data = loopFuncBody});

  ret += std::format("execute if score {} vars < {} vars run function loom:{}\n", iterMangled, endMangled, loopName);
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
        ret += std::format("{} run function loom:{}\n", contextChain, macroFuncName);
      }
      continue;
    }

    if (type == "variable_declaration") {
      const std::string name = std::string(getFieldText(child, "name"));
      const std::string mangledName = name + "_" + randomMangleString();
      const ExpressionData expr = compileExpression(ts_node_child_by_field_name(child, "value", 5));
      const bool constant = getFieldText(child, "keyword") == "const";
      TSNode varTypeNode = ts_node_child_by_field_name(child, "type", 4);
      const auto &typeText = getNodeText(varTypeNode);

      Type varType;
      if (typeText == "int" || typeText == "integer") {
        varType = Type::Integer;
      } else if (typeText == "bool" || typeText == "boolean") {
        varType = Type::Boolean;
      } else {
        throw std::runtime_error(formatError(varTypeNode, "Invalid type in variable declaration: " + std::string(typeText)));
      }

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
          .constant = constant,
          .value = value,
        }
      );

      if (!value.has_value()) {
        if (expr.precomputed) {
          ret += std::format("scoreboard players set {} vars {}\n", mangledName, expr.data);
          continue;
        }
        ret += std::format("{}\nscoreboard players operation {} vars = expr_output1 temp\n", expr.data, mangledName);
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
      const ExpressionData expr = compileExpression(expNode);

      if (vars[name].type == Type::Boolean && expr.type == Type::Integer) {
        throw std::runtime_error(formatError(expNode, "Cannot assign an 'int' to 'bool' variable: " + name));
      }

      if (expr.precomputed) {
        ret += std::format("scoreboard players set {} vars {}\n", vars[name].mangledName, expr.data);
      } else {
        if (std::string(ts_node_type(expNode)) == "binary_expression") {
          const std::string_view op = getFieldText(expNode, "operator");

          if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
            TSNode leftNode = ts_node_child_by_field_name(expNode, "left", 4);
            TSNode rightNode = ts_node_child_by_field_name(expNode, "right", 5);

            if (std::string(ts_node_type(leftNode)) == "variable_ref") {
              if (std::string(getFieldText(leftNode, "name")) == name) {
                const ExpressionData rightExpr = compileExpression(rightNode, 1, false);
                ret += std::format("{}\nscoreboard players operation {} vars {}= expr_output1 temp\n", rightExpr.data, vars[name].mangledName, op);
                continue;
              }
            }

            if (std::string(ts_node_type(rightNode)) == "variable_ref") {
              if (std::string(getFieldText(rightNode, "name")) == name) {
                const ExpressionData leftExpr = compileExpression(leftNode, 1, false);
                ret += std::format("{}\nscoreboard players operation {} vars {}= expr_output1 temp\n", leftExpr.data, vars[name].mangledName, op);
                continue;
              }
            }
          }
        }

        ret += std::format("{}\nscoreboard players operation {} vars = expr_output1 temp\n", expr.data, vars[name].mangledName);
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
      bool requiresMacro = false;

      for (uint32_t j = 1; j < ts_node_named_child_count(child); j++) {
        TSNode argNode = ts_node_named_child(child, j);
        std::string argType = ts_node_type(argNode);
        if (argType == "command_arg" || argType == "variable_ref" || argType == "integer") {
          args.push_back(argNode);
          if (argType == "variable_ref") requiresMacro = true;
        }
      }

      std::optional<std::string> optimized = optimizeCommand(cmdName, args);
      if (optimized.has_value()) {
        ret += optimized.value() + "\n";
        continue;
      }

      if (!requiresMacro) {
        ret += cmdName;
        for (TSNode arg : args) ret += " " + std::string(getNodeText(arg));
        ret += "\n";
        continue;
      }

      std::string macroBody = "$" + cmdName;
      std::string macroSetup = "";

      for (TSNode arg : args) {
        if (std::string(ts_node_type(arg)) == "variable_ref") {
          std::string varName = std::string(getFieldText(arg, "name"));
          macroBody += std::format(" $(var_{})", varName);
          macroSetup += std::format("execute store result storage loom:function_input var_{} int 1 run scoreboard players get {} vars\n", varName, vars[varName].mangledName);
        } else {
          macroBody += " " + std::string(getNodeText(arg));
        }
      }

      std::string macroFuncName = std::format("_generated_function_{}", currentGeneratedFunction++);
      compiledFunctions.push_back({.name = macroFuncName, .data = macroBody + "\n"});

      ret += macroSetup;
      ret += std::format("function loom:{} with storage loom:function_input\n", macroFuncName);
      continue;
    }

    if (type != "comment") throw std::runtime_error(formatError(child, "Invalid block statement: " + type));
  }

  std::erase_if(vars, [&node](const auto &pair) { return ts_node_eq(pair.second.scope, node); });

  return ret;
}
