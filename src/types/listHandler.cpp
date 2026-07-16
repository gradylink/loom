#include "../typeHandler.hpp"
#include "../utils.hpp"
#include <format>
#include <stdexcept>
#include <string>

class ListHandler : public TypeHandler {
public:
  bool handles(const Compiler::Type &type) const override { return type.isList(); }

  std::optional<Compiler::ExpressionData>
  compileBuiltinFunction(Compiler &compiler, std::string_view funcName, const std::vector<TSNode> &argNodes, unsigned int id, bool precompute, TSNode node) const override {

    if (funcName == "len") {
      Compiler::ExpressionData listExpr = compiler.compileExpression(argNodes[0], id, true);
      if (!listExpr.type.isList()) return std::nullopt;

      if (listExpr.precomputed) {
        size_t length = std::count(listExpr.data.begin(), listExpr.data.end(), ',') + 1;
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
          std::format("{}\nexecute store result score expr_output{} temp run data get storage {}:global expr_str{}", listExpr.data, id, compiler.getDatapackNamespace(), id),
        .precomputed = false,
        .type = Compiler::Type::IntegerType()
      };
    }

    if (funcName == "append" || funcName == "remove" || funcName == "insert") {
      Compiler::ExpressionData listExpr = compiler.compileExpression(argNodes[0], id, true);
      if (!listExpr.type.isList()) return std::nullopt;

      std::string cmds = listExpr.data + "\n";
      if (listExpr.precomputed) cmds += std::format("data modify storage {}:global expr_str{} set value {}\n", compiler.getDatapackNamespace(), id, listExpr.data);

      if (funcName == "append") {
        Compiler::ExpressionData elemExpr = compiler.compileExpression(argNodes[1], id + 1, true);
        if (elemExpr.type != *listExpr.type.baseType) throw std::runtime_error(formatError(argNodes[1], "Type mismatch: cannot append element to this list type."));
        cmds += elemExpr.data + "\n";
        if (elemExpr.precomputed) {
          cmds += std::format("data modify storage {0}:global expr_str{1} append value {2}\n", compiler.getDatapackNamespace(), id, elemExpr.data);
        } else if (elemExpr.type.isString() || elemExpr.type.isList()) {
          cmds += std::format("data modify storage {0}:global expr_str{1} append from storage {0}:global expr_str{2}\n", compiler.getDatapackNamespace(), id, id + 1);
        } else {
          cmds += std::format(
            "execute store result storage {0}:global expr_str{1} append int 1 run scoreboard players get expr_output{2} temp\n",
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
          cmds += std::format("data remove storage {0}:global expr_str{1}[{2}]\n", compiler.getDatapackNamespace(), id, idxExpr.data);
        } else {
          compiler.useInternalFunction("internal_list_remove");
          cmds += std::format(
            "data modify storage {0}:global macro_args set value {{target_id: {1}}}\n"
            "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get expr_output{2} temp\n"
            "function {0}:internal/loom/internal_list_remove with storage {0}:global macro_args\n",
            compiler.getDatapackNamespace(),
            id,
            id + 1
          );
        }
      } else { // insert
        Compiler::ExpressionData idxExpr = compiler.compileExpression(argNodes[1], id + 1, true);
        Compiler::ExpressionData elemExpr = compiler.compileExpression(argNodes[2], id + 2, true);
        if (!idxExpr.type.isInteger()) throw std::runtime_error(formatError(argNodes[1], "Index must evaluate to an integer."));
        if (elemExpr.type != *listExpr.type.baseType) throw std::runtime_error(formatError(argNodes[2], "Type mismatch: cannot insert element into this list type."));
        cmds += idxExpr.data + "\n" + elemExpr.data + "\n";

        if (idxExpr.precomputed) {
          if (elemExpr.precomputed) {
            cmds += std::format("data modify storage {0}:global expr_str{1} insert {2} value {3}\n", compiler.getDatapackNamespace(), id, idxExpr.data, elemExpr.data);
          } else if (elemExpr.type.isString() || elemExpr.type.isList()) {
            cmds += std::format(
              "data modify storage {0}:global expr_str{1} insert {2} from storage {0}:global expr_str{3}\n",
              compiler.getDatapackNamespace(),
              id,
              idxExpr.data,
              id + 2
            );
          } else {
            cmds += std::format(
              "execute store result storage {0}:global expr_str{1} insert {2} int 1 run scoreboard players get expr_output{3} temp\n",
              compiler.getDatapackNamespace(),
              id,
              idxExpr.data,
              id + 2
            );
          }
        } else {
          if (elemExpr.precomputed) {
            compiler.useInternalFunction("internal_list_insert_value");
            cmds += std::format(
              "data modify storage {0}:global macro_args set value {{target_id: {1}, value: {2}}}\n"
              "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get expr_output{3} temp\n"
              "function {0}:internal/loom/internal_list_insert_value with storage {0}:global macro_args\n",
              compiler.getDatapackNamespace(),
              id,
              elemExpr.data,
              id + 1
            );
          } else if (elemExpr.type.isString() || elemExpr.type.isList()) {
            compiler.useInternalFunction("internal_list_insert_object");
            cmds += std::format(
              "data modify storage {0}:global macro_args set value {{target_id: {1}, elem_id: {2}}}\n"
              "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get expr_output{3} temp\n"
              "function {0}:internal/loom/internal_list_insert_object with storage {0}:global macro_args\n",
              compiler.getDatapackNamespace(),
              id,
              id + 2,
              id + 1
            );
          } else {
            compiler.useInternalFunction("internal_list_insert_primitive");
            cmds += std::format(
              "data modify storage {0}:global macro_args set value {{target_id: {1}, elem_id: {2}}}\n"
              "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get expr_output{3} temp\n"
              "function {0}:internal/loom/internal_list_insert_primitive with storage {0}:global macro_args\n",
              compiler.getDatapackNamespace(),
              id,
              id + 2,
              id + 1
            );
          }
        }
      }

      return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = listExpr.type};
    }

    return std::nullopt;
  }
};

std::unique_ptr<TypeHandler> createListHandler() { return std::make_unique<ListHandler>(); }
