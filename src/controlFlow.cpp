#include "compiler.hpp"
#include "utils.hpp"
#include <format>
#include <stdexcept>

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

  if (expr.branchCondition.has_value()) {
    const std::string &bc = expr.branchCondition.value();
    auto invertBranch = [](const std::string &cond) -> std::string {
      if (cond.starts_with("if ")) return "unless " + cond.substr(3);
      if (cond.starts_with("unless ")) return "if " + cond.substr(7);
      return cond;
    };
    std::string setup;
    std::string bcLine = bc;
    auto lastNl = bc.rfind('\n');
    if (lastNl != std::string::npos) {
      setup = bc.substr(0, lastNl + 1);
      bcLine = bc.substr(lastNl + 1);
    }
    ret += setup;

    const std::string &trueData = compileBlock(blockNode);
    const size_t trueLineCount = std::count(trueData.begin(), trueData.end(), '\n');

    if (trueLineCount > 0) {
      if (trueLineCount == 1) {
        ret += std::format("execute {} run {}", bcLine, trueData);
      } else {
        compiledFunctions.push_back({.name = name + "_true", .data = trueData});
        ret += std::format("execute {} run function {}:internal/{}_true\n", bcLine, datapackNamespace, name);
      }
    }

    if (hasElse) {
      std::string altData;
      std::string altType = ts_node_type(altNode);
      if (altType == "block") altData = compileBlock(altNode);
      else if (altType == "if") altData = compileIf(altNode);

      const size_t altLineCount = std::count(altData.begin(), altData.end(), '\n');
      if (altLineCount > 0) {
        if (altLineCount == 1) {
          ret += std::format("execute {} run {}", invertBranch(bcLine), altData);
        } else {
          compiledFunctions.push_back({.name = name + "_false", .data = altData});
          ret += std::format("execute {} run function {}:internal/{}_false\n", invertBranch(bcLine), datapackNamespace, name);
        }
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
      ret += std::format("execute if score {} temp matches 1 run function {}:internal/{}_true\n", condScore, datapackNamespace, name);
    }
  }

  if (hasElse) {
    std::string altData;
    std::string altType = ts_node_type(altNode);
    if (altType == "block") altData = compileBlock(altNode);
    else if (altType == "if") altData = compileIf(altNode);

    const size_t altLineCount = std::count(altData.begin(), altData.end(), '\n');
    if (altLineCount > 0) {
      if (altLineCount == 1) {
        ret += std::format("execute unless score {} temp matches 1 run {}", condScore, altData);
      } else {
        compiledFunctions.push_back({.name = name + "_false", .data = altData});
        ret += std::format("execute unless score {} temp matches 1 run function {}:internal/{}_false\n", condScore, datapackNamespace, name);
      }
    }
  }

  return ret;
}

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
    if (condExpr.branchCondition.has_value()) {
      const std::string &bc = condExpr.branchCondition.value();
      auto lastNl = bc.rfind('\n');
      std::string setup, bcLine = bc;
      if (lastNl != std::string::npos) {
        setup = bc.substr(0, lastNl + 1);
        bcLine = bc.substr(lastNl + 1);
      }
      loopFuncBody += setup;
      loopFuncBody += std::format("execute {} run function {}:internal/{}\n", bcLine, datapackNamespace, loopName);
      ret += setup;
      ret += std::format("execute {} run function {}:internal/{}\n", bcLine, datapackNamespace, loopName);
    } else {
      loopFuncBody += condExpr.data + "\n";
      loopFuncBody += std::format("execute if score expr_output1 temp matches 1 run function {}:internal/{}\n", datapackNamespace, loopName);
      ret += condExpr.data + "\n";
      ret += std::format("execute if score expr_output1 temp matches 1 run function {}:internal/{}\n", datapackNamespace, loopName);
    }
  } else if (condExpr.data == "1") {
    loopFuncBody += std::format("function {}:internal/{}\n", datapackNamespace, loopName);
  }

  compiledFunctions.push_back({.name = loopName, .data = loopFuncBody});

  if (condExpr.precomputed) {
    ret += std::format("function {}:internal/{}\n", datapackNamespace, loopName);
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
    if (condExpr.branchCondition.has_value()) {
      const std::string &bc = condExpr.branchCondition.value();
      auto lastNl = bc.rfind('\n');
      std::string setup, bcLine = bc;
      if (lastNl != std::string::npos) {
        setup = bc.substr(0, lastNl + 1);
        bcLine = bc.substr(lastNl + 1);
      }
      loopFuncBody += setup;
      loopFuncBody += std::format("execute {} run function {}:internal/{}\n", bcLine, datapackNamespace, loopName);
    } else {
      loopFuncBody += condExpr.data + "\n";
      loopFuncBody += std::format("execute if score expr_output1 temp matches 1 run function {}:internal/{}\n", datapackNamespace, loopName);
    }
  } else if (condExpr.data == "1") {
    loopFuncBody += std::format("function {}:internal/{}\n", datapackNamespace, loopName);
  }

  compiledFunctions.push_back({.name = loopName, .data = loopFuncBody});

  ret += std::format("function {}:internal/{}\n", datapackNamespace, loopName);
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

  ret += std::format("execute if score {} vars < {} vars run function {}:internal/{}\n", iterMangled, endMangled, datapackNamespace, loopName);
  return ret;
}
