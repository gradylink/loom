#include "../typeHandler.hpp"
#include "../utils.hpp"
#include <format>
#include <stdexcept>
#include <string>

class MapHandler : public TypeHandler {
public:
  bool handles(const Compiler::Type &type) const override { return type.isMap(); }

  void registerBuiltins(Compiler &compiler) const override {
    compiler.registerBuiltin(
      "contains",
      [](Compiler &c, const std::vector<const Expr *> &args, unsigned int id, bool precompute, SourceLoc loc) -> std::optional<Compiler::ExpressionData> {
        Compiler::ExpressionData mapExpr = c.compileExpression(*args[0], id, true);
        if (!mapExpr.type.isMap()) return std::nullopt;

        const Compiler::Type &keyType = *mapExpr.type.baseType;
        const std::string &ns = c.getDatapackNamespace();

        Compiler::ExpressionData keyExpr = c.compileExpression(*args[1], id + 1, true);
        if (keyExpr.type != keyType) throw std::runtime_error(formatError(args[1]->loc, "Key type does not match this map's key type."));

        std::string cmds = mapExpr.precomputed ? std::format("data modify storage {}:global expr_str{} set value {}\n", ns, id, mapExpr.data) : mapExpr.data + "\n";

        if (keyExpr.precomputed) {
          std::string keyLit = keyType.isString() ? keyExpr.data : ("\"" + keyExpr.data + "\"");
          cmds += std::format("execute store success score expr_output{0} temp if data storage {1}:global expr_str{0}.{2}\n", id, ns, keyLit);
        } else {
          cmds += keyExpr.data + "\n";
          cmds += std::format("data modify storage {}:global macro_args set value {{}}\n", ns);
          cmds += std::format("data modify storage {0}:global macro_args.path set value \"expr_str{1}\"\n", ns, id);
          cmds += c.copyExprInto(keyExpr, "macro_args.key", id + 1);
          cmds += std::format("data modify storage {}:global macro_args.out_id set value {}\n", ns, id);
          c.useInternalFunction("internal_map_contains");
          cmds += std::format("function {0}:internal/loom/internal_map_contains with storage {0}:global macro_args\n", ns);
        }

        return Compiler::ExpressionData{.data = cmds, .precomputed = false, .type = Compiler::Type::BooleanType()};
      }
    );
  }
};

std::unique_ptr<TypeHandler> createMapHandler() { return std::make_unique<MapHandler>(); }
