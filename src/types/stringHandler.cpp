#include "../typeHandler.hpp"
#include "../utils.hpp"
#include <format>
#include <stdexcept>
#include <string>

class StringHandler : public TypeHandler {
public:
  bool handles(const Compiler::Type &type) const override { return type.isString(); }

  std::optional<Compiler::ExpressionData> compileBinaryOp(
    Compiler &compiler, std::string_view op, const Compiler::ExpressionData &left, const Compiler::ExpressionData &right, unsigned int id, bool precompute, TSNode node
  ) const override {

    if (op != "+") return std::nullopt;

    if (!left.type.isString() || !right.type.isString()) {
      throw std::runtime_error(formatError(node, "Implicit concatenation coercion between strings and numeric primitives is not allowed."));
    }

    if (left.precomputed && right.precomputed) {
      std::string lStr = left.data;
      std::string rStr = right.data;
      if (lStr.size() >= 2 && (lStr.front() == '"' || lStr.front() == '\'')) lStr = lStr.substr(1, lStr.size() - 2);
      if (rStr.size() >= 2 && (rStr.front() == '"' || rStr.front() == '\'')) rStr = rStr.substr(1, rStr.size() - 2);
      std::string joined = "\"" + lStr + rStr + "\"";

      if (precompute) return Compiler::ExpressionData{.data = joined, .precomputed = true, .type = Compiler::Type::StringType()};
      return Compiler::ExpressionData{
        .data = std::format("data modify storage {}:global expr_str{} set value {}", compiler.getDatapackNamespace(), id, joined),
        .precomputed = false,
        .type = Compiler::Type::StringType()
      };
    }

    std::string leftData = left.data;
    std::string rightData = right.data;
    if (left.precomputed) leftData = std::format("data modify storage {}:global expr_str{} set value {}", compiler.getDatapackNamespace(), id, left.data);
    if (right.precomputed) rightData = std::format("data modify storage {}:global expr_str{} set value {}", compiler.getDatapackNamespace(), id + 1, right.data);

    std::string runtimeCmds = leftData + "\n" + rightData + "\n";
    compiler.useInternalFunction("internal_string_concat");
    runtimeCmds += std::format(
      "data modify storage {}:global macro_args set value {{out_id: {}}}\n"
      "data modify storage {}:global macro_args.left set from storage {}:global expr_str{}\n"
      "data modify storage {}:global macro_args.right set from storage {}:global expr_str{}\n"
      "function {}:internal/loom/internal_string_concat with storage {}:global macro_args",
      compiler.getDatapackNamespace(),
      id,
      compiler.getDatapackNamespace(),
      compiler.getDatapackNamespace(),
      id,
      compiler.getDatapackNamespace(),
      compiler.getDatapackNamespace(),
      id + 1,
      compiler.getDatapackNamespace(),
      compiler.getDatapackNamespace()
    );

    return Compiler::ExpressionData{.data = runtimeCmds, .precomputed = false, .type = Compiler::Type::StringType()};
  }

  std::optional<Compiler::ExpressionData>
  compileBuiltinFunction(Compiler &compiler, std::string_view funcName, const std::vector<TSNode> &argNodes, unsigned int id, bool precompute, TSNode node) const override {

    if (funcName == "len") {
      Compiler::ExpressionData objExpr = compiler.compileExpression(argNodes[0], id, true);
      if (!objExpr.type.isString()) return std::nullopt;

      if (objExpr.precomputed) {
        std::string rawStr = objExpr.data;
        size_t length = (rawStr.size() >= 2 && rawStr.front() == '"' && rawStr.back() == '"') ? rawStr.size() - 2 : rawStr.size();
        if (!precompute)
          return Compiler::ExpressionData{
            .data = std::format("scoreboard players set expr_output{} temp {}", id, length),
            .precomputed = false,
            .type = Compiler::Type::IntegerType()
          };
        return Compiler::ExpressionData{.data = std::to_string(length), .precomputed = true, .type = Compiler::Type::IntegerType()};
      }

      return Compiler::ExpressionData{
        .data =
          std::format("{}\nexecute store result score expr_output{} temp run data get storage {}:global expr_str{}", objExpr.data, id, compiler.getDatapackNamespace(), id),
        .precomputed = false,
        .type = Compiler::Type::IntegerType()
      };
    }

    if (funcName == "append" || funcName == "remove" || funcName == "insert") {
      Compiler::ExpressionData strExpr = compiler.compileExpression(argNodes[0], id, true);
      if (!strExpr.type.isString()) return std::nullopt;

      std::string cmds = strExpr.data + "\n";
      if (strExpr.precomputed) cmds += std::format("data modify storage {}:global expr_str{} set value {}\n", compiler.getDatapackNamespace(), id, strExpr.data);

      if (funcName == "append") {
        Compiler::ExpressionData elemExpr = compiler.compileExpression(argNodes[1], id + 1, true);
        if (!elemExpr.type.isString()) throw std::runtime_error(formatError(argNodes[1], "Type mismatch: cannot append non-string to a string."));
        cmds += elemExpr.data + "\n";
        if (elemExpr.precomputed) {
          compiler.useInternalFunction("internal_string_append");
          cmds += std::format(
            "data modify storage {0}:global macro_args set value {{target_id: {1}, value: {2}}}\n"
            "function {0}:internal/loom/internal_string_append with storage {0}:global macro_args\n",
            compiler.getDatapackNamespace(),
            id,
            elemExpr.data
          );
        } else {
          compiler.useInternalFunction("internal_string_append_str");
          cmds += std::format(
            "data modify storage {0}:global macro_args set value {{target_id: {1}, elem_id: {2}}}\n"
            "function {0}:internal/loom/internal_string_append_str with storage {0}:global macro_args\n",
            compiler.getDatapackNamespace(),
            id,
            id + 1
          );
        }
      } else if (funcName == "remove") {
        Compiler::ExpressionData idxExpr = compiler.compileExpression(argNodes[1], id + 1, true);
        if (!idxExpr.type.isInteger()) throw std::runtime_error(formatError(argNodes[1], "Index must evaluate to an integer."));
        cmds += idxExpr.data + "\n";
        if (idxExpr.precomputed) {
          int idxVal = std::stoi(idxExpr.data);
          compiler.useInternalFunction("internal_string_remove");
          cmds += std::format(
            "data modify storage {0}:global macro_args set value {{target_id: {1}, index: {2}, index_plus_one: {3}}}\n"
            "function {0}:internal/loom/internal_string_remove with storage {0}:global macro_args\n",
            compiler.getDatapackNamespace(),
            id,
            idxVal,
            idxVal + 1
          );
        } else {
          compiler.useInternalFunction("internal_string_remove");
          cmds += std::format(
            "data modify storage {0}:global macro_args set value {{target_id: {1}}}\n"
            "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get expr_output{2} temp\n"
            "scoreboard players operation expr_output3 temp = expr_output{2} temp\n"
            "scoreboard players add expr_output3 temp 1\n"
            "execute store result storage {0}:global macro_args.index_plus_one int 1 run scoreboard players get expr_output3 temp\n"
            "function {0}:internal/loom/internal_string_remove with storage {0}:global macro_args\n",
            compiler.getDatapackNamespace(),
            id,
            id + 1
          );
        }
      } else { // insert
        Compiler::ExpressionData idxExpr = compiler.compileExpression(argNodes[1], id + 1, true);
        Compiler::ExpressionData elemExpr = compiler.compileExpression(argNodes[2], id + 2, true);
        if (!idxExpr.type.isInteger()) throw std::runtime_error(formatError(argNodes[1], "Index must evaluate to an integer."));
        if (!elemExpr.type.isString()) throw std::runtime_error(formatError(argNodes[2], "Type mismatch: cannot insert non-string into a string."));
        cmds += idxExpr.data + "\n" + elemExpr.data + "\n";

        if (idxExpr.precomputed) {
          if (elemExpr.precomputed) {
            compiler.useInternalFunction("internal_string_insert_value");
            cmds += std::format(
              "data modify storage {0}:global macro_args set value {{target_id: {1}, index: {2}, value: {3}}}\n"
              "function {0}:internal/loom/internal_string_insert_value with storage {0}:global macro_args\n",
              compiler.getDatapackNamespace(),
              id,
              idxExpr.data,
              elemExpr.data
            );
          } else {
            compiler.useInternalFunction("internal_string_insert_str");
            cmds += std::format(
              "data modify storage {0}:global macro_args set value {{target_id: {1}, index: {2}, elem_id: {3}}}\n"
              "function {0}:internal/loom/internal_string_insert_str with storage {0}:global macro_args\n",
              compiler.getDatapackNamespace(),
              id,
              idxExpr.data,
              id + 2
            );
          }
        } else {
          cmds += std::format(
            "data modify storage {0}:global macro_args set value {{target_id: {1}, elem_id: {2}}}\n"
            "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get expr_output{3} temp\n",
            compiler.getDatapackNamespace(),
            id,
            id + 2,
            id + 1
          );
          if (elemExpr.precomputed) {
            compiler.useInternalFunction("internal_string_insert_value");
            cmds += std::format(
              "data modify storage {0}:global macro_args.value set value {1}\n"
              "function {0}:internal/loom/internal_string_insert_value with storage {0}:global macro_args\n",
              compiler.getDatapackNamespace(),
              elemExpr.data
            );
          } else {
            compiler.useInternalFunction("internal_string_insert_str");
            cmds += std::format("function {0}:internal/loom/internal_string_insert_str with storage {0}:global macro_args\n", compiler.getDatapackNamespace());
          }
        }
      }

      return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = strExpr.type};
    }

    return std::nullopt;
  }
};

std::unique_ptr<TypeHandler> createStringHandler() { return std::make_unique<StringHandler>(); }
