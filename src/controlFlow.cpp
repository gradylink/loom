#include "compiler.hpp"
#include "utils.hpp"
#include <format>
#include <stdexcept>

static constexpr const char *kReturnPropagateCheck = "execute if score _loom_returned temp matches 1 return run scoreboard players get _loom_ret_val temp\n";
static constexpr const char *kReturnFlagReset = "scoreboard players set _loom_returned temp 0\n";

std::string Compiler::compileIf(const IfStmt &ifStmt, SourceLoc loc) {
  std::string ret = "";

  const std::string &name = "if_" + randomFunctionMangleString();
  const ExpressionData expr = compileExpression(*ifStmt.condition);

  if (!expr.type.isBoolean()) throw std::runtime_error(formatError(loc, "Invalid type for if statement expression."));

  const bool hasElse = ifStmt.elseBranch.has_value();

  if (expr.precomputed) {
    if (expr.data == "1") {
      ret += compileBlock(*ifStmt.thenBlock);
    } else if (hasElse) {
      const Stmt &elseStmt = **ifStmt.elseBranch;
      if (auto *blockStmt = std::get_if<BlockStmt>(&elseStmt.data)) {
        ret += compileBlock(*blockStmt->block);
      } else if (auto *nestedIf = std::get_if<IfStmt>(&elseStmt.data)) {
        ret += compileIf(*nestedIf, elseStmt.loc);
      }
    }
    return ret;
  }

  const auto compileBody = [&](const Block &blk) -> std::string {
    controlFlowDepth++;
    std::string data = compileBlock(blk);
    controlFlowDepth--;
    return data;
  };

  const auto compileElseData = [&]() -> std::string {
    const Stmt &elseStmt = **ifStmt.elseBranch;
    if (auto *blockStmt = std::get_if<BlockStmt>(&elseStmt.data)) return compileBody(*blockStmt->block);
    if (auto *nestedIf = std::get_if<IfStmt>(&elseStmt.data)) return compileIf(*nestedIf, elseStmt.loc);
    return "";
  };

  const auto emitSubFuncCall = [&](const std::string &executePrefix, const std::string &funcSuffix) {
    ret += kReturnFlagReset;
    ret += std::format("{} run function {}:internal/{}\n", executePrefix, datapackNamespace, funcSuffix);
    ret += kReturnPropagateCheck;
  };

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

    const std::string trueData = compileBody(*ifStmt.thenBlock);
    const size_t trueLineCount = std::count(trueData.begin(), trueData.end(), '\n');

    if (trueLineCount > 0) {
      if (trueLineCount == 1) {
        ret += std::format("execute {} run {}", bcLine, trueData);
      } else {
        compiledFunctions.push_back({.name = name + "_true", .data = trueData});
        emitSubFuncCall(std::format("execute {}", bcLine), name + "_true");
      }
    }

    if (hasElse) {
      const std::string altData = compileElseData();

      const size_t altLineCount = std::count(altData.begin(), altData.end(), '\n');
      if (altLineCount > 0) {
        if (altLineCount == 1) {
          ret += std::format("execute {} run {}", invertBranch(bcLine), altData);
        } else {
          compiledFunctions.push_back({.name = name + "_false", .data = altData});
          emitSubFuncCall(std::format("execute {}", invertBranch(bcLine)), name + "_false");
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

  const std::string trueData = compileBody(*ifStmt.thenBlock);
  const size_t trueLineCount = std::count(trueData.begin(), trueData.end(), '\n');

  if (trueLineCount > 0) {
    if (trueLineCount == 1) {
      ret += std::format("execute if score {} temp matches 1 run {}", condScore, trueData);
    } else {
      compiledFunctions.push_back({.name = name + "_true", .data = trueData});
      emitSubFuncCall(std::format("execute if score {} temp matches 1", condScore), name + "_true");
    }
  }

  if (hasElse) {
    const std::string altData = compileElseData();

    const size_t altLineCount = std::count(altData.begin(), altData.end(), '\n');
    if (altLineCount > 0) {
      if (altLineCount == 1) {
        ret += std::format("execute unless score {} temp matches 1 run {}", condScore, altData);
      } else {
        compiledFunctions.push_back({.name = name + "_false", .data = altData});
        emitSubFuncCall(std::format("execute unless score {} temp matches 1", condScore), name + "_false");
      }
    }
  }

  return ret;
}

std::string Compiler::compileWhile(const WhileStmt &whileStmt, SourceLoc loc) {
  std::string ret = "";
  const std::string &loopName = "while_" + randomFunctionMangleString();

  const ExpressionData &condExpr = compileExpression(*whileStmt.condition);
  if (!condExpr.type.isBoolean()) {
    throw std::runtime_error(formatError(loc, "While loop condition must evaluate to a boolean."));
  }

  if (condExpr.precomputed && condExpr.data == "0") return "";

  controlFlowDepth++;
  std::string loopFuncBody = compileBlock(*whileStmt.body);
  controlFlowDepth--;

  auto appendLoopCall = [&](std::string &target, const std::string &callLine) {
    target += kReturnFlagReset;
    target += callLine + "\n";
    target += kReturnPropagateCheck;
  };

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
      appendLoopCall(loopFuncBody, std::format("execute {} run function {}:internal/{}", bcLine, datapackNamespace, loopName));
      ret += setup;
      appendLoopCall(ret, std::format("execute {} run function {}:internal/{}", bcLine, datapackNamespace, loopName));
    } else {
      loopFuncBody += condExpr.data + "\n";
      appendLoopCall(loopFuncBody, std::format("execute if score expr_output1 temp matches 1 run function {}:internal/{}", datapackNamespace, loopName));
      ret += condExpr.data + "\n";
      appendLoopCall(ret, std::format("execute if score expr_output1 temp matches 1 run function {}:internal/{}", datapackNamespace, loopName));
    }
  } else if (condExpr.data == "1") {
    appendLoopCall(loopFuncBody, std::format("function {}:internal/{}", datapackNamespace, loopName));
  }

  compiledFunctions.push_back({.name = loopName, .data = loopFuncBody});

  if (condExpr.precomputed) {
    appendLoopCall(ret, std::format("function {}:internal/{}", datapackNamespace, loopName));
  }

  return ret;
}

std::string Compiler::compileDoWhile(const DoWhileStmt &doWhileStmt, SourceLoc loc) {
  std::string ret = "";
  const std::string &loopName = "dowhile_" + randomFunctionMangleString();

  const ExpressionData &condExpr = compileExpression(*doWhileStmt.condition);
  if (!condExpr.type.isBoolean()) {
    throw std::runtime_error(formatError(loc, "Do-while loop condition must evaluate to a boolean."));
  }

  controlFlowDepth++;
  std::string loopFuncBody = compileBlock(*doWhileStmt.body);
  controlFlowDepth--;

  auto appendLoopCall = [&](std::string &target, const std::string &callLine) {
    target += kReturnFlagReset;
    target += callLine + "\n";
    target += kReturnPropagateCheck;
  };

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
      appendLoopCall(loopFuncBody, std::format("execute {} run function {}:internal/{}", bcLine, datapackNamespace, loopName));
    } else {
      loopFuncBody += condExpr.data + "\n";
      appendLoopCall(loopFuncBody, std::format("execute if score expr_output1 temp matches 1 run function {}:internal/{}", datapackNamespace, loopName));
    }
  } else if (condExpr.data == "1") {
    appendLoopCall(loopFuncBody, std::format("function {}:internal/{}", datapackNamespace, loopName));
  }

  compiledFunctions.push_back({.name = loopName, .data = loopFuncBody});

  appendLoopCall(ret, std::format("function {}:internal/{}", datapackNamespace, loopName));
  return ret;
}

std::string Compiler::compileFor(const ForStmt &forStmt, SourceLoc loc) {
  std::string ret = "";
  const std::string &loopName = "for_" + randomFunctionMangleString();

  const std::string &iterName = forStmt.iterator;
  const std::string &iterMangled = iterName + "_" + randomMangleString();
  const std::string &endMangled = "limit_" + randomMangleString();

  const ExpressionData &startExpr = compileExpression(*forStmt.start, 1, false);
  const ExpressionData &endExpr = compileExpression(*forStmt.end, 2, false);

  if (!startExpr.type.isInteger() && !startExpr.type.isBoolean()) {
    throw std::runtime_error(formatError(loc, "For loop start boundary must be an integer."));
  }
  if (!endExpr.type.isInteger() && !endExpr.type.isBoolean()) {
    throw std::runtime_error(formatError(loc, "For loop end boundary must be an integer."));
  }

  vars.emplace(
    iterName, VariableData{.name = iterName, .mangledName = iterMangled, .type = Type::IntegerType(), .scope = forStmt.body.get(), .value = std::nullopt, .constant = false}
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

  controlFlowDepth++;
  std::string loopFuncBody = compileBlock(*forStmt.body);
  controlFlowDepth--;

  loopFuncBody += std::format("scoreboard players add {} vars 1\n", iterMangled);
  loopFuncBody += kReturnFlagReset;
  loopFuncBody += std::format("execute if score {} vars < {} vars run function {}:{}\n", iterMangled, endMangled, datapackNamespace, loopName);
  loopFuncBody += kReturnPropagateCheck;

  compiledFunctions.push_back({.name = loopName, .data = loopFuncBody});

  ret += kReturnFlagReset;
  ret += std::format("execute if score {} vars < {} vars run function {}:internal/{}\n", iterMangled, endMangled, datapackNamespace, loopName);
  ret += kReturnPropagateCheck;
  return ret;
}
