#include "../typeHandler.hpp"
#include "../utils.hpp"
#include <format>
#include <stdexcept>
#include <string>

class BooleanHandler : public TypeHandler {
public:
  bool handles(const Compiler::Type &type) const override { return type.isBoolean(); }

  std::optional<Compiler::ExpressionData>
  compileUnaryOp(Compiler &compiler, std::string_view op, const Compiler::ExpressionData &operand, unsigned int id, bool precompute, TSNode node) const override {

    if (op == "!") {
      if (!operand.type.isBoolean()) {
        throw std::runtime_error(formatError(node, "Logical NOT operator '!' can only be applied to booleans."));
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
    Compiler &compiler, std::string_view op, const Compiler::ExpressionData &left, const Compiler::ExpressionData &right, unsigned int id, bool precompute, TSNode node
  ) const override {

    const bool isLogical = (op == "&&" || op == "||");
    const bool isComparison = (op == "==" || op == "!=");

    if (!isLogical && !isComparison) return std::nullopt;

    if (!left.type.isBoolean() || !right.type.isBoolean()) {
      throw std::runtime_error(formatError(node, "Logical operators '&&', '||', '==' and '!=' require boolean operands."));
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

    std::string runtimeCommands = leftData + "\n" + rightData + "\n";

    if (isLogical) {
      if (op == "&&") {
        runtimeCommands += std::format("scoreboard players operation expr_output{} temp *= expr_output{} temp", id, id + 1);
      } else {
        runtimeCommands += std::format(
          "scoreboard players operation expr_output{} temp += expr_output{} temp\n"
          "execute if score expr_output{} temp matches 1.. run scoreboard players set expr_output{} temp 1",
          id,
          id + 1,
          id,
          id
        );
      }
    } else {
      std::string condType = (op == "==") ? "if" : "unless";
      runtimeCommands += std::format(
        "scoreboard players set internal1 temp 0\n"
        "execute {} score expr_output{} temp = expr_output{} temp run scoreboard players set internal1 temp 1\n"
        "scoreboard players operation expr_output{} temp = internal1 temp",
        condType,
        id,
        id + 1,
        id
      );
    }

    return Compiler::ExpressionData{.data = runtimeCommands, .precomputed = false, .type = Compiler::Type::BooleanType()};
  }
};

std::unique_ptr<TypeHandler> createBooleanHandler() { return std::make_unique<BooleanHandler>(); }
