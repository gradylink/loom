#include "../typeHandler.hpp"
#include "../utils.hpp"
#include <format>
#include <stdexcept>
#include <string>

class IntegerHandler : public TypeHandler {
public:
  bool handles(const Compiler::Type &type) const override { return type.isInteger(); }

  std::optional<Compiler::ExpressionData>
  compileUnaryOp(Compiler &compiler, std::string_view op, const Compiler::ExpressionData &operand, unsigned int id, bool precompute, TSNode node) const override {

    if (op == "-") {
      if (!operand.type.isInteger() && !operand.type.isBoolean()) {
        throw std::runtime_error(formatError(node, "Unary minus '-' can only be applied to integers and booleans."));
      }

      if (operand.precomputed) {
        std::string finalVal;
        if (operand.data.starts_with('-')) finalVal = operand.data.substr(1);
        else finalVal = "-" + operand.data;

        if (!precompute)
          return Compiler::ExpressionData{
            .data = std::format("scoreboard players set expr_output{} temp {}", id, finalVal),
            .precomputed = false,
            .type = Compiler::Type::IntegerType()
          };
        return Compiler::ExpressionData{.data = finalVal, .precomputed = true, .type = Compiler::Type::IntegerType()};
      }

      return Compiler::ExpressionData{
        .data = std::format("{}\nscoreboard players operation expr_output{} temp *= invert temp", operand.data, id),
        .precomputed = false,
        .type = Compiler::Type::IntegerType()
      };
    }

    return std::nullopt;
  }

  std::optional<Compiler::ExpressionData> compileBinaryOp(
    Compiler &compiler, std::string_view op, const Compiler::ExpressionData &left, const Compiler::ExpressionData &right, unsigned int id, bool precompute, TSNode node
  ) const override {

    const bool isMath = (op == "+" || op == "-" || op == "*" || op == "/" || op == "%");
    const bool isComparison = (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=");

    if (!isMath && !isComparison) return std::nullopt;

    if (!left.type.isInteger() || !right.type.isInteger()) {
      throw std::runtime_error(formatError(node, "Invalid operand types for integer operation: " + std::string(op)));
    }

    const Compiler::Type retType = isMath ? Compiler::Type::IntegerType() : Compiler::Type::BooleanType();

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

      if (precompute) return Compiler::ExpressionData{.data = result, .precomputed = true, .type = retType};
      return Compiler::ExpressionData{.data = std::format("scoreboard players set expr_output{} temp {}", id, result), .precomputed = false, .type = retType};
    }

    std::string leftData = left.data;
    std::string rightData = right.data;
    if (left.precomputed) leftData = std::format("scoreboard players set expr_output{} temp {}", id, left.data);
    if (right.precomputed) rightData = std::format("scoreboard players set expr_output{} temp {}", id + 1, right.data);

    std::string runtimeCommands = leftData + "\n" + rightData;

    if (isMath) {
      runtimeCommands += std::format("\nscoreboard players operation expr_output{} temp {}= expr_output{} temp", id, op, id + 1);
      return Compiler::ExpressionData{.data = runtimeCommands, .precomputed = false, .type = retType};
    }

    std::string mcOp = std::string(op);
    std::string condType = "if";
    if (op == "==") {
      mcOp = "=";
    } else if (op == "!=") {
      mcOp = "=";
      condType = "unless";
    }
    std::string branchCond = std::format("{} score expr_output{} temp {} expr_output{} temp", condType, id, mcOp, id + 1);
    std::string matCommands = runtimeCommands + "\n" +
                              std::format(
                                "scoreboard players set internal1 temp 0\n"
                                "execute {} score expr_output{} temp {} expr_output{} temp run scoreboard players set internal1 temp 1\n"
                                "scoreboard players operation expr_output{} temp = internal1 temp",
                                condType,
                                id,
                                mcOp,
                                id + 1,
                                id
                              );
    return Compiler::ExpressionData{
      .data = matCommands,
      .precomputed = false,
      .type = retType,
      .branchCondition = runtimeCommands.empty() ? branchCond : (runtimeCommands + "\n" + branchCond)
    };
  }
  std::optional<Compiler::ExpressionData>
  compileCast(Compiler &compiler, const Compiler::ExpressionData &expr, const Compiler::Type &targetType, unsigned int id, bool precompute, TSNode node) const override {
    if (targetType.isFloat()) {
      if (expr.precomputed) {
        std::string floatStr = std::to_string(static_cast<float>(std::stoi(expr.data)));
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

    if (targetType.isBoolean()) {
      if (expr.precomputed) {
        std::string finalVal = (std::stoi(expr.data) != 0) ? "1" : "0";
        if (precompute) return Compiler::ExpressionData{.data = finalVal, .precomputed = true, .type = Compiler::Type::BooleanType()};
        return Compiler::ExpressionData{
          .data = std::format("scoreboard players set expr_output{} temp {}", id, finalVal),
          .precomputed = false,
          .type = Compiler::Type::BooleanType()
        };
      }
      return Compiler::ExpressionData{
        .data = std::format(
          "{}\n"
          "scoreboard players set internal1 temp 0\n"
          "execute unless score expr_output{} temp matches 0 run scoreboard players set internal1 temp 1\n"
          "scoreboard players operation expr_output{} temp = internal1 temp",
          expr.data,
          id,
          id
        ),
        .precomputed = false,
        .type = Compiler::Type::BooleanType()
      };
    }

    if (targetType.isString()) {
      if (expr.precomputed) {
        std::string strVal = "\"" + expr.data + "\"";
        if (precompute) return Compiler::ExpressionData{.data = strVal, .precomputed = true, .type = Compiler::Type::StringType()};
        return Compiler::ExpressionData{
          .data = std::format("data modify storage {}:global expr_str{} set value {}", compiler.getDatapackNamespace(), id, strVal),
          .precomputed = false,
          .type = Compiler::Type::StringType()
        };
      }
      compiler.useInternalFunction("internal_int_to_string");
      return Compiler::ExpressionData{
        .data = std::format(
          "{}\n"
          "execute store result storage {}:global macro_args.value int 1 run scoreboard players get expr_output{} temp\n"
          "data modify storage {}:global macro_args.out_id set value {}\n"
          "function {}:internal/loom/internal_int_to_string with storage {}:global macro_args",
          expr.data,
          compiler.getDatapackNamespace(),
          id,
          compiler.getDatapackNamespace(),
          id,
          compiler.getDatapackNamespace(),
          compiler.getDatapackNamespace()
        ),
        .precomputed = false,
        .type = Compiler::Type::StringType()
      };
    }

    return std::nullopt;
  }
};

std::unique_ptr<TypeHandler> createIntegerHandler() { return std::make_unique<IntegerHandler>(); }
