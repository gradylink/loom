#include "../typeHandler.hpp"
#include "../utils.hpp"
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>

class StringHandler : public TypeHandler {
public:
  bool handles(const Compiler::Type &type) const override { return type.isString(); }

  std::optional<Compiler::ExpressionData> compileBinaryOp(
    Compiler &compiler, std::string_view op, const Compiler::ExpressionData &left, const Compiler::ExpressionData &right, unsigned int id, bool precompute, SourceLoc loc
  ) const override {

    if (op != "+") return std::nullopt;

    if (!left.type.isString() || !right.type.isString()) {
      throw std::runtime_error(formatError(loc, "Implicit concatenation coercion between strings and numeric primitives is not allowed."));
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

  void registerBuiltins(Compiler &compiler) const override {
    compiler.registerBuiltin(
      "len",
      [](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<Compiler::ExpressionData> {
        Compiler::ExpressionData objExpr = c.compileExpression(*args[0], id, true);
        if (!objExpr.type.isString()) return std::nullopt; // Let next handler try

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
          .data = std::format("{}\nexecute store result score expr_output{} temp run data get storage {}:global expr_str{}", objExpr.data, id, c.getDatapackNamespace(), id),
          .precomputed = false,
          .type = Compiler::Type::IntegerType()
        };
      }
    );

    compiler.registerBuiltin(
      "append",
      [](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<Compiler::ExpressionData> {
        Compiler::ExpressionData strExpr = c.compileExpression(*args[0], id, true);
        if (!strExpr.type.isString()) return std::nullopt;

        std::string cmds = strExpr.data + "\n";
        if (strExpr.precomputed) cmds += std::format("data modify storage {}:global expr_str{} set value {}\n", c.getDatapackNamespace(), id, strExpr.data);

        Compiler::ExpressionData elemExpr = c.compileExpression(*args[1], id + 1, true);
        if (!elemExpr.type.isString()) throw std::runtime_error(formatError(args[1]->loc, "Type mismatch: cannot append non-string to a string."));

        cmds += elemExpr.data + "\n";
        if (elemExpr.precomputed) {
          c.useInternalFunction("internal_string_append");
          cmds += std::format(
            "data modify storage {0}:global macro_args set value {{target_id: {1}, value: {2}}}\n"
            "function {0}:internal/loom/internal_string_append with storage {0}:global macro_args\n",
            c.getDatapackNamespace(),
            id,
            elemExpr.data
          );
        } else {
          c.useInternalFunction("internal_string_append_str");
          cmds += std::format(
            "data modify storage {0}:global macro_args set value {{target_id: {1}, elem_id: {2}}}\n"
            "function {0}:internal/loom/internal_string_append_str with storage {0}:global macro_args\n",
            c.getDatapackNamespace(),
            id,
            id + 1
          );
        }

        return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = strExpr.type};
      }
    );

    compiler.registerBuiltin(
      "remove",
      [](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<Compiler::ExpressionData> {
        Compiler::ExpressionData strExpr = c.compileExpression(*args[0], id, true);
        if (!strExpr.type.isString()) return std::nullopt;

        std::string cmds = strExpr.data + "\n";
        if (strExpr.precomputed) cmds += std::format("data modify storage {}:global expr_str{} set value {}\n", c.getDatapackNamespace(), id, strExpr.data);

        Compiler::ExpressionData idxExpr = c.compileExpression(*args[1], id + 1, true);
        if (!idxExpr.type.isInteger()) throw std::runtime_error(formatError(args[1]->loc, "Index must evaluate to an integer."));

        cmds += idxExpr.data + "\n";
        if (idxExpr.precomputed) {
          int idxVal = std::stoi(idxExpr.data);
          c.useInternalFunction("internal_string_remove");
          cmds += std::format(
            "data modify storage {0}:global macro_args set value {{target_id: {1}, index: {2}, index_plus_one: {3}}}\n"
            "function {0}:internal/loom/internal_string_remove with storage {0}:global macro_args\n",
            c.getDatapackNamespace(),
            id,
            idxVal,
            idxVal + 1
          );
        } else {
          c.useInternalFunction("internal_string_remove");
          cmds += std::format(
            "data modify storage {0}:global macro_args set value {{target_id: {1}}}\n"
            "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get expr_output{2} temp\n"
            "scoreboard players operation expr_output3 temp = expr_output{2} temp\n"
            "scoreboard players add expr_output3 temp 1\n"
            "execute store result storage {0}:global macro_args.index_plus_one int 1 run scoreboard players get expr_output3 temp\n"
            "function {0}:internal/loom/internal_string_remove with storage {0}:global macro_args\n",
            c.getDatapackNamespace(),
            id,
            id + 1
          );
        }

        return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = strExpr.type};
      }
    );

    compiler.registerBuiltin(
      "insert",
      [](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<Compiler::ExpressionData> {
        Compiler::ExpressionData strExpr = c.compileExpression(*args[0], id, true);
        if (!strExpr.type.isString()) return std::nullopt;

        std::string cmds = strExpr.data + "\n";
        if (strExpr.precomputed) cmds += std::format("data modify storage {}:global expr_str{} set value {}\n", c.getDatapackNamespace(), id, strExpr.data);

        Compiler::ExpressionData idxExpr = c.compileExpression(*args[1], id + 1, true);
        Compiler::ExpressionData elemExpr = c.compileExpression(*args[2], id + 2, true);

        if (!idxExpr.type.isInteger()) throw std::runtime_error(formatError(args[1]->loc, "Index must evaluate to an integer."));
        if (!elemExpr.type.isString()) throw std::runtime_error(formatError(args[2]->loc, "Type mismatch: cannot insert non-string into a string."));

        cmds += idxExpr.data + "\n" + elemExpr.data + "\n";

        if (idxExpr.precomputed) {
          if (elemExpr.precomputed) {
            c.useInternalFunction("internal_string_insert_value");
            cmds += std::format(
              "data modify storage {0}:global macro_args set value {{target_id: {1}, index: {2}, value: {3}}}\n"
              "function {0}:internal/loom/internal_string_insert_value with storage {0}:global macro_args\n",
              c.getDatapackNamespace(),
              id,
              idxExpr.data,
              elemExpr.data
            );
          } else {
            c.useInternalFunction("internal_string_insert_str");
            cmds += std::format(
              "data modify storage {0}:global macro_args set value {{target_id: {1}, index: {2}, elem_id: {3}}}\n"
              "function {0}:internal/loom/internal_string_insert_str with storage {0}:global macro_args\n",
              c.getDatapackNamespace(),
              id,
              idxExpr.data,
              id + 2
            );
          }
        } else {
          cmds += std::format(
            "data modify storage {0}:global macro_args set value {{target_id: {1}, elem_id: {2}}}\n"
            "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get expr_output{3} temp\n",
            c.getDatapackNamespace(),
            id,
            id + 2,
            id + 1
          );
          if (elemExpr.precomputed) {
            c.useInternalFunction("internal_string_insert_value");
            cmds += std::format(
              "data modify storage {0}:global macro_args.value set value {1}\n"
              "function {0}:internal/loom/internal_string_insert_value with storage {0}:global macro_args\n",
              c.getDatapackNamespace(),
              elemExpr.data
            );
          } else {
            c.useInternalFunction("internal_string_insert_str");
            cmds += std::format("function {0}:internal/loom/internal_string_insert_str with storage {0}:global macro_args\n", c.getDatapackNamespace());
          }
        }

        return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = strExpr.type};
      }
    );
  }

  std::optional<Compiler::ExpressionData>
  compileCast(Compiler &compiler, const Compiler::ExpressionData &expr, const Compiler::Type &targetType, unsigned int id, bool precompute, SourceLoc loc) const override {
    if (targetType.isInteger()) {
      if (expr.precomputed) {
        std::string raw = expr.data;
        if (raw.size() >= 2 && (raw.front() == '"' || raw.front() == '\'')) raw = raw.substr(1, raw.size() - 2);
        int32_t val = std::stoi(raw);
        if (precompute) return Compiler::ExpressionData{.data = std::to_string(val), .precomputed = true, .type = Compiler::Type::IntegerType()};
        return Compiler::ExpressionData{
          .data = std::format("scoreboard players set expr_output{} temp {}", id, val),
          .precomputed = false,
          .type = Compiler::Type::IntegerType()
        };
      }
      compiler.useInternalFunction("internal_string_to_int");
      return Compiler::ExpressionData{
        .data = std::format(
          "{}\n"
          "data modify storage {}:global macro_args.value set from storage {}:global expr_str{}\n"
          "data modify storage {}:global macro_args.out_id set value {}\n"
          "function {}:internal/loom/internal_string_to_int with storage {}:global macro_args",
          expr.data,
          compiler.getDatapackNamespace(),
          compiler.getDatapackNamespace(),
          id,
          compiler.getDatapackNamespace(),
          id,
          compiler.getDatapackNamespace(),
          compiler.getDatapackNamespace()
        ),
        .precomputed = false,
        .type = Compiler::Type::IntegerType()
      };
    }

    if (targetType.isFloat()) {
      if (expr.precomputed) {
        std::string raw = expr.data;
        if (raw.size() >= 2 && (raw.front() == '"' || raw.front() == '\'')) raw = raw.substr(1, raw.size() - 2);
        float val = std::stof(raw);
        if (precompute) return Compiler::ExpressionData{.data = std::to_string(val), .precomputed = true, .type = Compiler::Type::FloatType()};
        return Compiler::ExpressionData{
          .data = std::format("data modify storage {}:global expr_float{} set value {}f", compiler.getDatapackNamespace(), id, val),
          .precomputed = false,
          .type = Compiler::Type::FloatType()
        };
      }
      compiler.useInternalFunction("internal_string_to_float");
      return Compiler::ExpressionData{
        .data = std::format(
          "{}\n"
          "data modify storage {}:global macro_args.value set from storage {}:global expr_str{}\n"
          "data modify storage {}:global macro_args.out_id set value {}\n"
          "function {}:internal/loom/internal_string_to_float with storage {}:global macro_args",
          expr.data,
          compiler.getDatapackNamespace(),
          compiler.getDatapackNamespace(),
          id,
          compiler.getDatapackNamespace(),
          id,
          compiler.getDatapackNamespace(),
          compiler.getDatapackNamespace()
        ),
        .precomputed = false,
        .type = Compiler::Type::FloatType()
      };
    }

    if (targetType.isList() && targetType.baseType && targetType.baseType->isString()) {
      if (expr.precomputed) {
        std::string raw = expr.data;
        if (raw.size() >= 2 && (raw.front() == '"' || raw.front() == '\'')) raw = raw.substr(1, raw.size() - 2);
        std::string listLiteral = "[";
        for (size_t i = 0; i < raw.size(); ++i) {
          if (i > 0) listLiteral += ',';
          char c = raw[i];
          if (c == '"') listLiteral += "\\\"\"\\\"\"";
          else if (c == '\\') listLiteral += "\"\\\\\\\\\"";
          else listLiteral += '"', listLiteral += c, listLiteral += '"';
        }
        listLiteral += "]";
        if (precompute) return Compiler::ExpressionData{.data = listLiteral, .precomputed = true, .type = targetType};
        return Compiler::ExpressionData{
          .data = std::format("data modify storage {}:global expr_str{} set value {}", compiler.getDatapackNamespace(), id, listLiteral),
          .precomputed = false,
          .type = targetType
        };
      }
      compiler.useInternalFunction("internal_string_to_charlist");
      compiler.useInternalFunction("internal_string_to_charlist_loop");
      compiler.useInternalFunction("internal_string_to_charlist_step");
      return Compiler::ExpressionData{
        .data = std::format(
          "{}\n"
          "execute store result score expr_output{} temp run data get storage {}:global expr_str{}\n"
          "data modify storage {}:global macro_args set value {{target_id: {}, out_id: {}, index: 0, index_plus_one: 1}}\n"
          "execute store result storage {}:global macro_args.length int 1 run scoreboard players get expr_output{} temp\n"
          "function {}:internal/loom/internal_string_to_charlist with storage {}:global macro_args",
          expr.data,
          id,
          compiler.getDatapackNamespace(),
          id,
          compiler.getDatapackNamespace(),
          id,
          id,
          compiler.getDatapackNamespace(),
          id,
          compiler.getDatapackNamespace(),
          compiler.getDatapackNamespace()
        ),
        .precomputed = false,
        .type = targetType
      };
    }

    return std::nullopt;
  }
};

std::unique_ptr<TypeHandler> createStringHandler() { return std::make_unique<StringHandler>(); }
