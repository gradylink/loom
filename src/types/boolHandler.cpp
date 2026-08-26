#include "../typeHandler.hpp"
#include "../utils.hpp"
#include <format>
#include <stdexcept>
#include <string>

class BooleanHandler : public TypeHandler {
public:
  bool handles(const Compiler::Type &type) const override { return type.isBoolean(); }

  std::optional<Compiler::ExpressionData>
  compileUnaryOp(Compiler &compiler, std::string_view op, const Compiler::ExpressionData &operand, unsigned int id, bool precompute, SourceLoc loc) const override {

    if (op == "!") {
      if (!operand.type.isBoolean()) {
        throw std::runtime_error(formatError(loc, "Logical NOT operator '!' can only be applied to booleans."));
      }

      if (operand.precomputed) {
        std::string finalVal = (operand.data == "1") ? "0" : "1";
        if (!precompute)
          return Compiler::ExpressionData{
            .data = std::format("scoreboard players set expr_output{} temp {}", id, finalVal),
            .precomputed = false,
            .type = Compiler::Type::BooleanType()
          };
        return Compiler::ExpressionData{.data = finalVal, .precomputed = true, .type = Compiler::Type::BooleanType()};
      }

      return Compiler::ExpressionData{
        .data = std::format(
          "{}\n"
          "scoreboard players set internal1 temp 1\n"
          "scoreboard players operation internal1 temp -= expr_output{} temp\n"
          "scoreboard players operation expr_output{} temp = internal1 temp",
          operand.data,
          id,
          id
        ),
        .precomputed = false,
        .type = Compiler::Type::BooleanType()
      };
    }

    return std::nullopt;
  }

  std::optional<Compiler::ExpressionData> compileBinaryOp(
    Compiler &compiler, std::string_view op, const Compiler::ExpressionData &left, const Compiler::ExpressionData &right, unsigned int id, bool precompute, SourceLoc loc
  ) const override {

    const bool isLogical = (op == "&&" || op == "||");
    const bool isComparison = (op == "==" || op == "!=");

    if (!isLogical && !isComparison) return std::nullopt;

    if (!left.type.isBoolean() || !right.type.isBoolean()) {
      throw std::runtime_error(formatError(loc, "Logical operators '&&', '||', '==' and '!=' require boolean operands."));
    }

    if (left.precomputed && right.precomputed) {
      const int32_t lVal = std::stoi(left.data);
      const int32_t rVal = std::stoi(right.data);
      std::string result;

      if (op == "&&") result = (lVal && rVal) ? "1" : "0";
      else if (op == "||") result = (lVal || rVal) ? "1" : "0";
      else if (op == "==") result = (lVal == rVal) ? "1" : "0";
      else if (op == "!=") result = (lVal != rVal) ? "1" : "0";

      if (precompute) return Compiler::ExpressionData{.data = result, .precomputed = true, .type = Compiler::Type::BooleanType()};
      return Compiler::ExpressionData{
        .data = std::format("scoreboard players set expr_output{} temp {}", id, result),
        .precomputed = false,
        .type = Compiler::Type::BooleanType()
      };
    }

    std::string leftData = left.data;
    std::string rightData = right.data;
    if (left.precomputed) leftData = std::format("scoreboard players set expr_output{} temp {}", id, left.data);
    if (right.precomputed) rightData = std::format("scoreboard players set expr_output{} temp {}", id + 1, right.data);

    std::string runtimeCommands = leftData + "\n";

    if (isLogical) {
      const size_t rightLineCount = std::count(rightData.begin(), rightData.end(), '\n');

      std::string execCond;
      if (op == "&&") execCond = std::format("execute if score expr_output{} temp matches 1.. run ", id);
      else execCond = std::format("execute if score expr_output{} temp matches 0 run ", id);

      if (rightLineCount > 0) {
        std::string name = "short_circuit_" + randomFunctionMangleString();
        compiler.addCompiledFunction({.name = name, .data = rightData});
        runtimeCommands += execCond + std::format("function {}:internal/{}\n", compiler.getDatapackNamespace(), name);
      } else {
        runtimeCommands += execCond + rightData + "\n";
      }

      if (op == "&&") {
        runtimeCommands += std::format("scoreboard players operation expr_output{} temp *= expr_output{} temp", id, id + 1);
      } else {
        runtimeCommands += std::format(
          "execute if score expr_output{} temp matches 0 run scoreboard players operation expr_output{} temp += expr_output{} temp\n"
          "execute if score expr_output{} temp matches 1.. run scoreboard players set expr_output{} temp 1",
          id,
          id,
          id + 1,
          id,
          id
        );
      }
    } else {
      std::string setupCmds = leftData + "\n" + rightData;
      std::string condType = (op == "==") ? "if" : "unless";
      std::string branchCond = std::format("{} score expr_output{} temp = expr_output{} temp", condType, id, id + 1);
      std::string matCommands = setupCmds + "\n" +
                                std::format(
                                  "scoreboard players set internal1 temp 0\n"
                                  "execute {} score expr_output{} temp = expr_output{} temp run scoreboard players set internal1 temp 1\n"
                                  "scoreboard players operation expr_output{} temp = internal1 temp",
                                  condType,
                                  id,
                                  id + 1,
                                  id
                                );
      return Compiler::ExpressionData{
        .data = matCommands,
        .precomputed = false,
        .type = Compiler::Type::BooleanType(),
        .branchCondition = setupCmds.empty() ? branchCond : (setupCmds + "\n" + branchCond)
      };
    }
    return Compiler::ExpressionData{.data = runtimeCommands, .precomputed = false, .type = Compiler::Type::BooleanType()};
  }

  std::optional<Compiler::ExpressionData>
  compileCast(Compiler &compiler, const Compiler::ExpressionData &expr, const Compiler::Type &targetType, unsigned int id, bool precompute, SourceLoc loc) const override {
    if (targetType.isInteger()) {
      if (expr.precomputed) {
        std::string intStr = (expr.data != "0") ? "1" : "0";
        if (precompute) return Compiler::ExpressionData{.data = intStr, .precomputed = true, .type = Compiler::Type::IntegerType()};
        return Compiler::ExpressionData{
          .data = std::format("scoreboard players set expr_output{} temp {}", id, intStr),
          .precomputed = false,
          .type = Compiler::Type::IntegerType()
        };
      }
      return Compiler::ExpressionData{.data = expr.data, .precomputed = false, .type = Compiler::Type::IntegerType()};
    }

    if (targetType.isFloat()) {
      if (expr.precomputed) {
        std::string floatStr = (expr.data != "0") ? "1.0" : "0.0";
        if (precompute) return Compiler::ExpressionData{.data = floatStr, .precomputed = true, .type = Compiler::Type::FloatType()};
        return Compiler::ExpressionData{
          .data = std::format("data modify storage {}:global expr_float{} set value {}f", compiler.getDatapackNamespace(), id, floatStr),
          .precomputed = false,
          .type = Compiler::Type::FloatType()
        };
      }
      return Compiler::ExpressionData{
        .data = std::format(
          "{}\nexecute store result storage {}:global expr_float{} float 1 run scoreboard players get expr_output{} temp",
          expr.data,
          compiler.getDatapackNamespace(),
          id,
          id
        ),
        .precomputed = false,
        .type = Compiler::Type::FloatType()
      };
    }

    if (targetType.isString()) {
      if (expr.precomputed) {
        std::string strVal = (expr.data != "0") ? "\"true\"" : "\"false\"";
        if (precompute) return Compiler::ExpressionData{.data = strVal, .precomputed = true, .type = Compiler::Type::StringType()};
        return Compiler::ExpressionData{
          .data = std::format("data modify storage {}:global expr_str{} set value {}", compiler.getDatapackNamespace(), id, strVal),
          .precomputed = false,
          .type = Compiler::Type::StringType()
        };
      }
      return Compiler::ExpressionData{
        .data = std::format(
          "{}\n"
          "execute if score expr_output{} temp matches 1 run data modify storage {}:global expr_str{} set value \"true\"\n"
          "execute unless score expr_output{} temp matches 1 run data modify storage {}:global expr_str{} set value \"false\"",
          expr.data,
          id,
          compiler.getDatapackNamespace(),
          id,
          id,
          compiler.getDatapackNamespace(),
          id
        ),
        .precomputed = false,
        .type = Compiler::Type::StringType()
      };
    }

    return std::nullopt;
  }
};

std::unique_ptr<TypeHandler> createBooleanHandler() { return std::make_unique<BooleanHandler>(); }
