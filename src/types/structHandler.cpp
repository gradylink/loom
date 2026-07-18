#include "../typeHandler.hpp"
#include <format>
#include <ryml.hpp>
#include <stdexcept>
#include <string>

class StructHandler : public TypeHandler {
public:
  bool handles(const Compiler::Type &type) const override { return type.kind == Compiler::Type::Struct; }

  std::optional<Compiler::ExpressionData> compileMemberExpression(
    Compiler &compiler, const Compiler::ExpressionData &object, std::string_view property, unsigned int id, bool precompute, TSNode node
  ) const override {
    std::string prop(property);
    const Compiler::StructData *structRef = object.type.structRef;

    if (!structRef) return std::nullopt;

    Compiler::Type fieldType;
    bool found = false;
    for (const auto &field : structRef->fields) {
      if (field.name == prop) {
        fieldType = *field.type;
        found = true;
        break;
      }
    }

    if (!found) {
      throw std::runtime_error(std::format("Struct '{}' has no field named '{}'", structRef->name, prop));
    }

    if (object.precomputed) {
      ryml::Tree tree = ryml::parse_in_arena(ryml::to_csubstr(object.data));
      ryml::ConstNodeRef root = tree.rootref();

      if (root.is_map() && root.has_child(ryml::to_csubstr(prop))) {
        ryml::ConstNodeRef fieldNode = root[ryml::to_csubstr(prop)];
        std::string extractedValue;

        if (fieldNode.has_val() && !fieldType.isString()) {
          extractedValue = std::string(fieldNode.val().data(), fieldNode.val().size());
        } else {
          extractedValue = ryml::emitrs_json<std::string>(tree, fieldNode.id());
        }

        if (!extractedValue.empty() && extractedValue.back() == '\n') {
          extractedValue.pop_back();
        }

        if (precompute) {
          return Compiler::ExpressionData{.data = extractedValue, .precomputed = true, .type = fieldType};
        }

        std::string runtimeCmds;
        if (fieldType.isString() || fieldType.isList() || fieldType.kind == Compiler::Type::Struct) {
          runtimeCmds = std::format("data modify storage {}:global expr_str{} set value {}\n", compiler.getDatapackNamespace(), id, extractedValue);
        } else if (fieldType.isFloat()) {
          runtimeCmds = std::format("data modify storage {}:global expr_float{} set value {}\n", compiler.getDatapackNamespace(), id, extractedValue);
        } else {
          runtimeCmds = std::format("scoreboard players set expr_output{} temp {}\n", id, extractedValue);
        }
        return Compiler::ExpressionData{.data = runtimeCmds, .precomputed = false, .type = fieldType};
      }

      std::string runtimeCmds = std::format("data modify storage {}:global expr_str{} set value {}\n", compiler.getDatapackNamespace(), id, object.data);

      if (fieldType.isString() || fieldType.isList() || fieldType.kind == Compiler::Type::Struct) {
        runtimeCmds += std::format(
          "data modify storage {}:global expr_str{} set from storage {}:global expr_str{}.{}\n",
          compiler.getDatapackNamespace(),
          id,
          compiler.getDatapackNamespace(),
          id,
          prop
        );
      } else if (fieldType.isFloat()) {
        runtimeCmds += std::format(
          "data modify storage {}:global expr_float{} set from storage {}:global expr_str{}.{}\n",
          compiler.getDatapackNamespace(),
          id,
          compiler.getDatapackNamespace(),
          id,
          prop
        );
      } else {
        runtimeCmds +=
          std::format("execute store result score expr_output{} temp run data get storage {}:global expr_str{}.{}\n", id, compiler.getDatapackNamespace(), id, prop);
      }
      return Compiler::ExpressionData{.data = runtimeCmds, .precomputed = false, .type = fieldType};
    }

    std::string runtimeCmds = object.data + "\n";

    if (fieldType.isString() || fieldType.isList() || fieldType.kind == Compiler::Type::Struct) {
      runtimeCmds += std::format(
        "data modify storage {}:global expr_str{} set from storage {}:global expr_str{}.{}\n",
        compiler.getDatapackNamespace(),
        id,
        compiler.getDatapackNamespace(),
        id,
        prop
      );
    } else if (fieldType.isFloat()) {
      runtimeCmds += std::format(
        "data modify storage {}:global expr_float{} set from storage {}:global expr_str{}.{}\n",
        compiler.getDatapackNamespace(),
        id,
        compiler.getDatapackNamespace(),
        id,
        prop
      );
    } else {
      runtimeCmds +=
        std::format("execute store result score expr_output{} temp run data get storage {}:global expr_str{}.{}\n", id, compiler.getDatapackNamespace(), id, prop);
    }

    return Compiler::ExpressionData{.data = runtimeCmds, .precomputed = false, .type = fieldType};
  }
};
std::unique_ptr<TypeHandler> createStructHandler() { return std::make_unique<StructHandler>(); }
