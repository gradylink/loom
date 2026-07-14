#include "../typeHandler.hpp"
#include "../utils.hpp"
#include <cmath>
#include <format>

class FloatHandler : public TypeHandler {
public:
  bool handles(const Compiler::Type &type) const override { return type.isFloat(); }

  std::optional<Compiler::ExpressionData>
  compileUnaryOp(Compiler &compiler, std::string_view op, const Compiler::ExpressionData &operand, unsigned int id, bool precompute, TSNode node) const override {
    if (op == "-") {
      if (!operand.type.isFloat()) throw std::runtime_error(formatError(node, "Unary minus '-' on float requires a float operand."));

      if (operand.precomputed) {
        std::string finalVal = operand.data.starts_with('-') ? operand.data.substr(1) : "-" + operand.data;
        if (!precompute) {
          return Compiler::ExpressionData{
            .data = std::format("data modify storage {}:global expr_float{} set value {}f", compiler.getDatapackNamespace(), id, finalVal),
            .precomputed = false,
            .type = Compiler::Type::FloatType()
          };
        }
        return Compiler::ExpressionData{.data = finalVal, .precomputed = true, .type = Compiler::Type::FloatType()};
      }

      return Compiler::ExpressionData{
        .data = std::format(
          "data modify storage {0}:global _temp_trans set value [1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,-1f]\n"
          "data modify storage {0}:global _temp_trans[3] set from storage {0}:global expr_float{1}\n"
          "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_trans\n"
          "data modify storage {0}:global expr_float{1} set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]",
          compiler.getDatapackNamespace(),
          id
        ),
        .precomputed = false,
        .type = Compiler::Type::FloatType()
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

    const Compiler::Type retType = isMath ? Compiler::Type::FloatType() : Compiler::Type::BooleanType();

    if (left.precomputed && right.precomputed) {
      const float lVal = std::stof(left.data);
      const float rVal = std::stof(right.data);
      std::string result;

      if (op == "+") result = std::to_string(lVal + rVal);
      else if (op == "-") result = std::to_string(lVal - rVal);
      else if (op == "*") result = std::to_string(lVal * rVal);
      else if (op == "/") {
        if (rVal == 0.0f) throw std::runtime_error(formatError(node, "Float division by zero at compile-time."));
        result = std::to_string(lVal / rVal);
      } else if (op == "%") {
        if (rVal == 0.0f) throw std::runtime_error(formatError(node, "Float modulo by zero at compile-time."));
        result = std::to_string(std::fmod(lVal, rVal));
      } else if (op == "==") result = (lVal == rVal) ? "1" : "0";
      else if (op == "!=") result = (lVal != rVal) ? "1" : "0";
      else if (op == "<") result = (lVal < rVal) ? "1" : "0";
      else if (op == ">") result = (lVal > rVal) ? "1" : "0";
      else if (op == "<=") result = (lVal <= rVal) ? "1" : "0";
      else if (op == ">=") result = (lVal >= rVal) ? "1" : "0";

      if (precompute) return Compiler::ExpressionData{.data = result, .precomputed = true, .type = retType};

      if (isMath)
        return Compiler::ExpressionData{
          .data = std::format("data modify storage {}:global expr_float{} set value {}f", compiler.getDatapackNamespace(), id, result),
          .precomputed = false,
          .type = retType
        };
      return Compiler::ExpressionData{.data = std::format("scoreboard players set expr_output{} temp {}", id, result), .precomputed = false, .type = retType};
    }

    std::string leftData =
      left.precomputed ? std::format("data modify storage {}:global expr_float{} set value {}f", compiler.getDatapackNamespace(), id, left.data) : left.data;
    std::string rightData =
      right.precomputed ? std::format("data modify storage {}:global expr_float{} set value {}f", compiler.getDatapackNamespace(), id + 1, right.data) : right.data;
    std::string runtimeCommands = leftData + "\n" + rightData + "\n";

    if (isMath) {
      if (op == "+") {
        runtimeCommands += std::format(
          "item modify block 18483211 -64 14504281 container.0 "
          "{{function:set_custom_model_data,floats:{{mode:replace_all,values:[{{type:sum,summands:[{{type:storage,storage:\"{0}:global\",path:\"expr_float{1}\"}},{{type:"
          "storage,storage:\"{0}:global\",path:\"expr_float{2}\"}}]}}]}}}}\n"
          "data modify storage {0}:global expr_float{1} set from block 18483211 -64 14504281 Items[0].components.\"minecraft:custom_model_data\".floats[0]",
          compiler.getDatapackNamespace(),
          id,
          id + 1
        );
      } else if (op == "-") {
        runtimeCommands += std::format(
          "data modify storage {0}:global macro_args.a set from storage {0}:global expr_float{1}\n"
          "data modify storage {0}:global macro_args.b set from storage {0}:global expr_float{2}\n"
          "item modify block 18483211 -64 14504281 container.0 {{function:set_name,entity:this,name:{{storage:\"{0}:global\",nbt:\"macro_args.b\"}}}}\n"
          "function {0}:internal_float_sub_macro with block 18483211 -64 14504281 Items[0].components.\"minecraft:custom_name\".extra[0]\n"
          "data modify storage {0}:global expr_float{1} set from storage {0}:global macro_args.out",
          compiler.getDatapackNamespace(),
          id,
          id + 1
        );
      } else if (op == "*") {
        runtimeCommands += std::format(
          "data modify storage {0}:global _temp_mul set value [1f,0f,0f,1f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
          "data modify storage {0}:global _temp_mul[15] set from storage {0}:global expr_float{2}\n"
          "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_mul\n"
          "data modify storage {0}:global _temp_var1 set value [1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
          "data modify storage {0}:global _temp_var1[3] set from storage {0}:global expr_float{1}\n"
          "data modify storage {0}:global _temp_var1[15] set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"
          "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_var1\n"
          "data modify storage {0}:global expr_float{1} set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]",
          compiler.getDatapackNamespace(),
          id,
          id + 1
        );
      } else if (op == "/") {
        runtimeCommands += std::format(
          "data modify storage {0}:global _temp_div set value [1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
          "data modify storage {0}:global _temp_div[3] set from storage {0}:global expr_float{1}\n"
          "data modify storage {0}:global _temp_div[15] set from storage {0}:global expr_float{2}\n"
          "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_div\n"
          "data modify storage {0}:global expr_float{1} set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]",
          compiler.getDatapackNamespace(),
          id,
          id + 1
        );
      } else if (op == "%") {
        runtimeCommands += std::format(
          "data modify storage {0}:global _temp_div set value [1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
          "data modify storage {0}:global _temp_div[3] set from storage {0}:global expr_float{1}\n"
          "data modify storage {0}:global _temp_div[15] set from storage {0}:global expr_float{2}\n"
          "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_div\n"
          "execute store result score internal1 temp run data get entity 6c6f6f6d-0-0-0-ffff transformation.translation[0] 1\n"
          "data modify storage {0}:global _temp_mul set value [1f,0f,0f,1f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
          "data modify storage {0}:global _temp_mul[15] set from storage {0}:global expr_float{2}\n"
          "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_mul\n"
          "data modify storage {0}:global _temp_var1 set value [1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f]\n"
          "execute store result storage {0}:global _temp_var1[3] float 1 run scoreboard players get internal1 temp\n"
          "data modify storage {0}:global _temp_var1[15] set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"
          "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_var1\n"
          "data modify storage {0}:global macro_args.a set from storage {0}:global expr_float{1}\n"
          "data modify storage {0}:global macro_args.b set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"
          "item modify block 18483211 -64 14504281 container.0 {{function:set_name,entity:this,name:{{storage:\"{0}:global\",nbt:\"macro_args.b\"}}}}\n"
          "function {0}:internal_float_sub_macro with block 18483211 -64 14504281 Items[0].components.\"minecraft:custom_name\".extra[0]\n"
          "data modify storage {0}:global expr_float{1} set from storage {0}:global macro_args.out",
          compiler.getDatapackNamespace(),
          id,
          id + 1
        );
      }
    } else {
      runtimeCommands += std::format(
        "data modify storage {0}:global _temp_cmp set from storage {0}:global expr_float{1}\n"
        "execute store success score internal1 temp run data modify storage {0}:global _temp_cmp set from storage {0}:global expr_float{2}\n"
        "scoreboard players set is_eq temp 1\n"
        "execute if score internal1 temp matches 1 run scoreboard players set is_eq temp 0\n",
        compiler.getDatapackNamespace(),
        id,
        id + 1
      );

      if (op == "==") {
        runtimeCommands += std::format("scoreboard players operation expr_output{0} temp = is_eq temp", id);
      } else if (op == "!=") {
        runtimeCommands += std::format(
          "scoreboard players set expr_output{0} temp 1\n"
          "execute if score is_eq temp matches 1 run scoreboard players set expr_output{0} temp 0",
          id
        );
      } else {
        runtimeCommands += std::format(
          "data modify storage {0}:global _temp_trans set value [1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,1f,0f,0f,0f,0f,-1f]\n"
          "data modify storage {0}:global _temp_trans[3] set from storage {0}:global expr_float{2}\n"
          "data modify entity 6c6f6f6d-0-0-0-ffff transformation set from storage {0}:global _temp_trans\n"
          "data modify storage {0}:global _temp_neg set from entity 6c6f6f6d-0-0-0-ffff transformation.translation[0]\n"
          "item modify block 18483211 -64 14504281 container.0 "
          "{{function:set_custom_model_data,floats:{{mode:replace_all,values:[{{type:sum,summands:[{{type:storage,storage:\"{0}:global\",path:\"expr_float{1}\"}},{{type:"
          "storage,storage:\"{0}:global\",path:\"_temp_neg\"}}]}}]}}}}\n"
          "data modify storage {0}:global _temp_diff set from block 18483211 -64 14504281 Items[0].components.\"minecraft:custom_model_data\".floats[0]\n",
          compiler.getDatapackNamespace(),
          id,
          id + 1
        );

        runtimeCommands += std::format(
          "data modify storage {0}:global _temp_char set string storage {0}:global _temp_diff 0 1\n"
          "scoreboard players set is_neg temp 0\n"
          "execute if data storage {0}:global {{_temp_char:\"-\"}} run scoreboard players set is_neg temp 1\n"
          "scoreboard players set expr_output{1} temp 0\n",
          compiler.getDatapackNamespace(),
          id
        );

        if (op == "<") {
          runtimeCommands += std::format("execute if score is_neg temp matches 1 if score is_eq temp matches 0 run scoreboard players set expr_output{0} temp 1", id);
        } else if (op == ">") {
          runtimeCommands += std::format("execute if score is_neg temp matches 0 if score is_eq temp matches 0 run scoreboard players set expr_output{0} temp 1", id);
        } else if (op == "<=") {
          runtimeCommands += std::format(
            "execute if score is_neg temp matches 1 run scoreboard players set expr_output{0} temp 1\n"
            "execute if score is_eq temp matches 1 run scoreboard players set expr_output{0} temp 1",
            id
          );
        } else if (op == ">=") {
          runtimeCommands += std::format(
            "execute if score is_neg temp matches 0 run scoreboard players set expr_output{0} temp 1\n"
            "execute if score is_eq temp matches 1 run scoreboard players set expr_output{0} temp 1",
            id
          );
        }
      }
    }

    return Compiler::ExpressionData{.data = runtimeCommands, .precomputed = false, .type = retType};
  }
};

std::unique_ptr<TypeHandler> createFloatHandler() { return std::make_unique<FloatHandler>(); }
