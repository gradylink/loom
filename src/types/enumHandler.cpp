#include "../typeHandler.hpp"
#include "../utils.hpp"
#include <format>
#include <stdexcept>
#include <string>

class EnumHandler : public TypeHandler {
public:
  bool handles(const Compiler::Type &type) const override { return type.kind == Compiler::Type::Enum; }

  std::optional<Compiler::ExpressionData> compileBinaryOp(
    Compiler &compiler, std::string_view op, const Compiler::ExpressionData &left, const Compiler::ExpressionData &right, unsigned int id, bool precompute, TSNode node
  ) const override {

    const bool isComparison = (op == "==" || op == "!=");
    if (!isComparison) return std::nullopt;

    if (left.type.kind != Compiler::Type::Enum || right.type.kind != Compiler::Type::Enum) {
      throw std::runtime_error(formatError(node, "Enum comparisons require both operands to be enum types."));
    }
    if (left.type.enumRef != right.type.enumRef) {
      throw std::runtime_error(formatError(node, "Cannot compare variants from different enums."));
    }

    const Compiler::Type retType = Compiler::Type::BooleanType();

    if (left.type.isInteger()) {
      if (left.precomputed && right.precomputed) {
        const int32_t lVal = std::stoi(left.data);
        const int32_t rVal = std::stoi(right.data);
        std::string result = ((op == "==") ? (lVal == rVal) : (lVal != rVal)) ? "1" : "0";
        if (precompute) return Compiler::ExpressionData{.data = result, .precomputed = true, .type = retType};
        return Compiler::ExpressionData{.data = std::format("scoreboard players set expr_output{} temp {}", id, result), .precomputed = false, .type = retType};
      }

      std::string leftData = left.data;
      std::string rightData = right.data;
      if (left.precomputed) leftData = std::format("scoreboard players set expr_output{} temp {}", id, left.data);
      if (right.precomputed) rightData = std::format("scoreboard players set expr_output{} temp {}", id + 1, right.data);

      std::string condType = (op == "==") ? "if" : "unless";
      std::string runtimeCmds = leftData + "\n" + rightData + "\n";
      runtimeCmds += std::format(
        "scoreboard players set internal1 temp 0\n"
        "execute {} score expr_output{} temp = expr_output{} temp run scoreboard players set internal1 temp 1\n"
        "scoreboard players operation expr_output{} temp = internal1 temp",
        condType,
        id,
        id + 1,
        id
      );

      return Compiler::ExpressionData{.data = runtimeCmds, .precomputed = false, .type = retType};
    }

    if (left.type.isString()) {
      if (left.precomputed && right.precomputed) {
        bool equal = (left.data == right.data);
        std::string result = ((op == "==") ? equal : !equal) ? "1" : "0";
        if (precompute) return Compiler::ExpressionData{.data = result, .precomputed = true, .type = retType};
        return Compiler::ExpressionData{.data = std::format("scoreboard players set expr_output{} temp {}", id, result), .precomputed = false, .type = retType};
      }

      std::string leftData = left.data;
      std::string rightData = right.data;
      if (left.precomputed) leftData = std::format("data modify storage {}:global expr_str{} set value {}", compiler.getDatapackNamespace(), id, left.data);
      if (right.precomputed) rightData = std::format("data modify storage {}:global expr_str{} set value {}", compiler.getDatapackNamespace(), id + 1, right.data);

      std::string condType = (op == "==") ? "if" : "unless";
      std::string runtimeCmds = leftData + "\n" + rightData + "\n";
      runtimeCmds += std::format(
        "scoreboard players set internal1 temp 0\n"
        "execute {0} data storage {1}:global {{expr_str{2}: $(expr_str{3})}} run scoreboard players set internal1 temp 1\n"
        "scoreboard players operation expr_output{2} temp = internal1 temp",
        condType,
        compiler.getDatapackNamespace(),
        id,
        id + 1
      );

      return Compiler::ExpressionData{.data = runtimeCmds, .precomputed = false, .type = retType};
    }

    if (left.type.isFloat()) {
      if (left.precomputed && right.precomputed) {
        const float lVal = std::stof(left.data);
        const float rVal = std::stof(right.data);
        std::string result = ((op == "==") ? (lVal == rVal) : (lVal != rVal)) ? "1" : "0";
        if (precompute) return Compiler::ExpressionData{.data = result, .precomputed = true, .type = retType};
        return Compiler::ExpressionData{.data = std::format("scoreboard players set expr_output{} temp {}", id, result), .precomputed = false, .type = retType};
      }

      std::string leftData =
        left.precomputed ? std::format("data modify storage {}:global expr_float{} set value {}f", compiler.getDatapackNamespace(), id, left.data) : left.data;
      std::string rightData =
        right.precomputed ? std::format("data modify storage {}:global expr_float{} set value {}f", compiler.getDatapackNamespace(), id + 1, right.data) : right.data;

      std::string runtimeCmds = leftData + "\n" + rightData + "\n";

      runtimeCmds += std::format(
        "data modify storage {0}:global _temp_cmp set from storage {0}:global expr_float{1}\n"
        "execute store success score internal1 temp run data modify storage {0}:global _temp_cmp set from storage {0}:global expr_float{2}\n",
        compiler.getDatapackNamespace(),
        id,
        id + 1
      );

      if (op == "==") {
        runtimeCmds += std::format(
          "scoreboard players set expr_output{0} temp 1\n"
          "execute if score internal1 temp matches 1 run scoreboard players set expr_output{0} temp 0",
          id
        );
      } else {
        runtimeCmds += std::format(
          "scoreboard players set expr_output{0} temp 0\n"
          "execute if score internal1 temp matches 1 run scoreboard players set expr_output{0} temp 1",
          id
        );
      }

      return Compiler::ExpressionData{.data = runtimeCmds, .precomputed = false, .type = retType};
    }

    return std::nullopt;
  }
  std::optional<Compiler::ExpressionData>
  compileCast(Compiler &compiler, const Compiler::ExpressionData &expr, const Compiler::Type &targetType, unsigned int id, bool precompute, TSNode node) const override {
    if (expr.type.isInteger() && targetType.isInteger()) {
      Compiler::ExpressionData ret = expr;
      ret.type = Compiler::Type::IntegerType();
      return ret;
    }
    if (expr.type.isFloat() && targetType.isFloat()) {
      Compiler::ExpressionData ret = expr;
      ret.type = Compiler::Type::FloatType();
      return ret;
    }
    if (expr.type.isString() && targetType.isString()) {
      Compiler::ExpressionData ret = expr;
      ret.type = Compiler::Type::StringType();
      return ret;
    }
    throw std::runtime_error(formatError(node, "Invalid cast from enum to target type. Enums can only be cast to their underlying type."));
  }
};

std::unique_ptr<TypeHandler> createEnumHandler() { return std::make_unique<EnumHandler>(); }
