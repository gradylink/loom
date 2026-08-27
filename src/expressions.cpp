#include "compiler.hpp"
#include "parser.hpp"
#include "typeHandler.hpp"
#include "utils.hpp"
#include <algorithm>
#include <format>
#include <stdexcept>

Compiler::ExpressionData Compiler::compileExpression(const Expr &node, unsigned int id, bool precompute) {
  Compiler::ExpressionData expr = compileExpressionImpl(node, id, precompute);

  if (expr.type.isRef() && !std::holds_alternative<ReferenceExpr>(node.data)) {
    const Type innerType = *expr.type.baseType;
    if (expr.precomputed) {
      std::string mangledName = expr.data;
      if (mangledName.size() >= 2 && (mangledName.front() == '"' || mangledName.front() == '\'')) {
        mangledName = mangledName.substr(1, mangledName.size() - 2);
      }

      const VariableData *targetVar = nullptr;
      for (const auto &[name, var] : vars) {
        if (var.mangledName == mangledName) {
          targetVar = &var;
          break;
        }
      }

      if (targetVar) {
        if (targetVar->value.has_value()) {
          if (precompute) {
            return {.data = targetVar->value.value(), .precomputed = true, .type = innerType};
          }
          if (innerType.isString() || innerType.isList() || innerType.isMap() || innerType.isStruct()) {
            return {
              .data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, targetVar->value.value()),
              .precomputed = false,
              .type = innerType
            };
          }
          if (innerType.isFloat()) {
            return {
              .data = std::format("data modify storage {}:global expr_float{} set value {}f", datapackNamespace, id, targetVar->value.value()),
              .precomputed = false,
              .type = innerType
            };
          }
          return {.data = std::format("scoreboard players set expr_output{} temp {}", id, targetVar->value.value()), .precomputed = false, .type = innerType};
        }

        if (innerType.isString() || innerType.isList() || innerType.isMap() || innerType.isStruct()) {
          return {
            .data = std::format("data modify storage {0}:global expr_str{1} set from storage {0}:global vars.{2}", datapackNamespace, id, targetVar->getStorageName()),
            .precomputed = false,
            .type = innerType
          };
        }
        if (innerType.isFloat()) {
          return {
            .data = std::format("data modify storage {0}:global expr_float{1} set from storage {0}:global vars.{2}", datapackNamespace, id, targetVar->getStorageName()),
            .precomputed = false,
            .type = innerType
          };
        }
        return {.data = std::format("scoreboard players operation expr_output{} temp = {} vars", id, targetVar->getStorageName()), .precomputed = false, .type = innerType};
      }
    }

    std::string derefCmds = expr.data + "\n";
    derefCmds += std::format("data modify storage {0}:global macro_args set value {{out_id: {1}}}\n", datapackNamespace, id);
    derefCmds += std::format("data modify storage {0}:global macro_args.refname set from storage {0}:global expr_str{1}\n", datapackNamespace, id);

    if (innerType.isString() || innerType.isList() || innerType.isMap() || innerType.isStruct()) {
      useInternalFunction("internal_deref_object");
      derefCmds += std::format("function {0}:internal/loom/internal_deref_object with storage {0}:global macro_args", datapackNamespace);
    } else if (innerType.isFloat()) {
      useInternalFunction("internal_deref_float");
      derefCmds += std::format("function {0}:internal/loom/internal_deref_float with storage {0}:global macro_args", datapackNamespace);
    } else {
      useInternalFunction("internal_deref_int");
      derefCmds += std::format("function {0}:internal/loom/internal_deref_int with storage {0}:global macro_args", datapackNamespace);
    }

    return {.data = derefCmds, .precomputed = false, .type = innerType};
  }

  return expr;
}

Compiler::ExpressionData Compiler::compileMethodCallOnVariable(
  const std::string &objVarName, const std::string &methodName, const std::vector<const Expr *> &argNodes, unsigned int id, bool precompute, SourceLoc loc
) {
  auto objVarIt = findInMap(vars, objVarName);
  if (objVarIt == vars.end()) {
    throw std::runtime_error(formatError(loc, "Unknown variable used in expression: " + objVarName));
  }
  const VariableData &objVar = objVarIt->second;
  const Type &objActualType = objVar.type.isRef() ? *objVar.type.baseType : objVar.type;
  if (!objActualType.isStruct()) {
    throw std::runtime_error(formatError(loc, "Cannot call methods on a non-struct value."));
  }
  const StructData *structRef = objActualType.structRef;

  std::string methodKey = structRef->name + "::" + methodName;
  std::transform(methodKey.begin(), methodKey.end(), methodKey.begin(), ::tolower);
  auto methodIt = funcs.find(methodKey);
  if (methodIt == funcs.end() || methodIt->second.empty()) {
    throw std::runtime_error(formatError(loc, std::format("Struct '{}' has no method named '{}'", structRef->name, methodName)));
  }
  const FunctionData &rep = methodIt->second.front();
  if (rep.isStatic) {
    throw std::runtime_error(formatError(loc, std::format("'{}' is a static struct function; call it as {}::{}(...)", methodName, structRef->name, methodName)));
  }
  if (rep.isPrivate && currentStructContext != rep.ownerStruct) {
    throw std::runtime_error(formatError(loc, std::format("'{}' is private to struct '{}'.", methodName, structRef->name)));
  }

  ExpressionData implicitSelf;
  if (objVar.type.isRef()) {
    implicitSelf = {.data = std::format("\"{}\"", objVar.mangledName), .precomputed = true, .type = objVar.type};
  } else {
    implicitSelf = {.data = std::format("\"{}\"", objVar.mangledName), .precomputed = true, .type = Type::RefTypeOf(objVar.type)};
  }

  return compileFunctionInvocation(methodIt->second, methodName, argNodes, implicitSelf, id, precompute, loc);
}

Compiler::ExpressionData Compiler::compileFunctionInvocation(
  const std::vector<FunctionData> &overloads,
  const std::string &displayName,
  const std::vector<const Expr *> &argNodes,
  std::optional<ExpressionData> implicitSelf,
  unsigned int id,
  bool precompute,
  SourceLoc loc
) {
  std::vector<ExpressionData> compiledArgs;
  std::vector<Type> argTypes;
  for (const auto &argNode : argNodes) {
    ExpressionData argExpr = compileExpression(*argNode);
    compiledArgs.push_back(argExpr);
    argTypes.push_back(argExpr.type);
  }

  const FunctionData *selectedOverload = nullptr;

  for (const auto &overload : overloads) {
    if (overload.params.size() == argTypes.size()) {
      bool exactMatch = true;
      for (size_t i = 0; i < argTypes.size(); i++) {
        if (argTypes[i] != overload.params[i]) {
          exactMatch = false;
          break;
        }
      }
      if (exactMatch) {
        selectedOverload = &overload;
        break;
      }
    }
  }

  if (!selectedOverload) {
    std::vector<const FunctionData *> bestMatches;
    int minPromotions = 999999;

    for (const auto &overload : overloads) {
      if (overload.params.size() == argTypes.size()) {
        bool canPromote = true;
        int promotionCount = 0;

        for (size_t i = 0; i < argTypes.size(); i++) {
          if (argTypes[i] != overload.params[i]) {
            if (argTypes[i].isInteger() && overload.params[i].isFloat()) {
              promotionCount++;
            } else {
              canPromote = false;
              break;
            }
          }
        }

        if (canPromote) {
          if (promotionCount < minPromotions) {
            minPromotions = promotionCount;
            bestMatches.clear();
            bestMatches.push_back(&overload);
          } else if (promotionCount == minPromotions) {
            bestMatches.push_back(&overload);
          }
        }
      }
    }

    if (bestMatches.size() == 1) {
      selectedOverload = bestMatches[0];
    } else if (bestMatches.size() > 1) {
      throw std::runtime_error(formatError(loc, std::format("Ambiguous function call: multiple overloads match for '{}' with equally valid type promotions.", displayName)));
    }
  }

  if (!selectedOverload) {
    throw std::runtime_error(formatError(loc, std::format("No matching overload found for function '{}' with the provided argument types.", displayName)));
  }

  std::string argPushData = "";

  if (implicitSelf.has_value()) {
    argPushData += std::format("data modify storage {}:stack regs append value {}\n", datapackNamespace, implicitSelf->data);
  }

  for (size_t i = 0; i < compiledArgs.size(); i++) {
    ExpressionData argExpr = compiledArgs[i];

    const Type &expectedType = selectedOverload->params[i];

    if (!expectedType.isRef()) {
      if (argExpr.type != expectedType) {
        std::optional<ExpressionData> casted = std::nullopt;
        if (TypeHandler *handler = getHandler(argExpr.type)) {
          casted = handler->compileCast(*this, argExpr, expectedType, id + 1, false, argNodes[i]->loc);
        }

        if (casted.has_value()) {
          argExpr = casted.value();
        } else {
          throw std::runtime_error(formatError(argNodes[i]->loc, std::format("Failed to promote argument {} for function '{}'", i + 1, displayName)));
        }
      }
    } else {
      bool isValidRefArg = false;
      if (std::holds_alternative<ReferenceExpr>(argNodes[i]->data)) {
        if (argExpr.type.isRef() && *argExpr.type.baseType == *expectedType.baseType) {
          isValidRefArg = true;
        }
      } else if (auto *vr = std::get_if<VarRefExpr>(&argNodes[i]->data)) {
        auto varIt = findInMap(vars, vr->name);
        if (varIt != vars.end() && varIt->second.type.isRef()) {
          if (*varIt->second.type.baseType == *expectedType.baseType) {
            isValidRefArg = true;
            if (varIt->second.refTargetMangledName.has_value()) {
              argExpr = {.data = std::format("\"{}\"", varIt->second.refTargetMangledName.value()), .precomputed = true, .type = expectedType};
            } else {
              argExpr = {
                .data = std::format(
                  "data modify storage {}:global expr_str{} set from storage {}:global vars.{}_refargs.refname",
                  datapackNamespace,
                  id,
                  datapackNamespace,
                  varIt->second.mangledName
                ),
                .precomputed = false,
                .type = expectedType
              };
            }
          }
        }
      }
      if (!isValidRefArg) {
        throw std::runtime_error(formatError(argNodes[i]->loc, std::format("Argument {} for function '{}' requires a reference.", i + 1, displayName)));
      }
    }

    if (argExpr.precomputed) {
      argPushData += std::format("data modify storage {}:stack regs append value {}\n", datapackNamespace, argExpr.data);
    } else {
      argPushData += argExpr.data + "\n";
      if (argExpr.type.isString() || argExpr.type.isList() || argExpr.type.isMap() || argExpr.type.isRef()) {
        argPushData += std::format("data modify storage {}:stack regs append from storage {}:global expr_str{}\n", datapackNamespace, datapackNamespace, id);
      } else if (argExpr.type.isFloat()) {
        argPushData += std::format("data modify storage {}:stack regs append from storage {}:global expr_float{}\n", datapackNamespace, datapackNamespace, id);
      } else {
        argPushData += std::format(
          "execute store result storage {0}:global stack_temp int 1 run scoreboard players get expr_output1 temp\ndata modify storage "
          "{0}:stack regs append from storage "
          "{0}:global stack_temp\n",
          datapackNamespace
        );
      }
    }
  }

  std::string push;
  std::string pop;
  for (unsigned int i = 1; i < id; i++) {
    push += std::format(
      "data modify storage {0}:stack regs append value {{}}\n"
      "data modify storage {0}:stack regs[-1].str set from storage {0}:global expr_str{1}\n"
      "data modify storage {0}:stack regs[-1].flt set from storage {0}:global expr_float{1}\n"
      "execute store result storage {0}:stack regs[-1].int int 1 run scoreboard players get expr_output{1} temp\n",
      datapackNamespace,
      i
    );
    pop = std::format(
            "\ndata modify storage {0}:global expr_str{1} set from storage {0}:stack regs[-1].str\n"
            "data modify storage {0}:global expr_float{1} set from storage {0}:stack regs[-1].flt\n"
            "execute store result score expr_output{1} temp run data get storage {0}:stack regs[-1].int\n"
            "data remove storage {0}:stack regs[-1]",
            datapackNamespace,
            i
          ) +
          pop;
  }

  Type funcType = Type::IntegerType();
  if (selectedOverload->returnType.has_value()) funcType = selectedOverload->returnType.value();

  std::string callCommand;
  if (funcType.isInteger() || funcType.isBoolean()) {
    callCommand = std::format(
      "execute store result score expr_output{} temp run function {}:{}{}",
      id,
      datapackNamespace,
      selectedOverload->internal ? "internal/" : "",
      selectedOverload->mangledName
    );
  } else {
    callCommand = std::format("function {}:{}{}", datapackNamespace, selectedOverload->internal ? "internal/" : "", selectedOverload->mangledName);
  }

  std::string captureReturn = "";
  if (id != 1) {
    if (funcType.isString() || funcType.isList() || funcType.isMap() || funcType.isStruct() || funcType.isRef()) {
      captureReturn = std::format("\ndata modify storage {0}:global expr_str{1} set from storage {0}:global expr_str1", datapackNamespace, id);
    } else if (funcType.isFloat()) {
      captureReturn = std::format("\ndata modify storage {0}:global expr_float{1} set from storage {0}:global expr_float1", datapackNamespace, id);
    }
  }

  return {.data = std::format("{}{}{}{}{}", push, argPushData, callCommand, captureReturn, pop), .precomputed = false, .type = funcType};
}

std::string Compiler::copyExprInto(const ExpressionData &expr, const std::string &destPath, unsigned int computedAtId) const {
  if (expr.precomputed) {
    return std::format("data modify storage {}:global {} set value {}\n", datapackNamespace, destPath, expr.data);
  }
  if (expr.type.isString() || expr.type.isList() || expr.type.isMap() || expr.type.isStruct() || expr.type.isRef()) {
    return std::format("data modify storage {0}:global {1} set from storage {0}:global expr_str{2}\n", datapackNamespace, destPath, computedAtId);
  }
  if (expr.type.isFloat()) {
    return std::format("data modify storage {0}:global {1} set from storage {0}:global expr_float{2}\n", datapackNamespace, destPath, computedAtId);
  }
  return std::format("execute store result storage {0}:global {1} int 1 run scoreboard players get expr_output{2} temp\n", datapackNamespace, destPath, computedAtId);
}

Compiler::ExpressionData Compiler::compileMapGet(ExpressionData target, ExpressionData index, unsigned int id, SourceLoc loc) {
  const Type &keyType = *target.type.baseType;
  Type resultType = *target.type.mapValueType;

  if (index.type != keyType) throw std::runtime_error(formatError(loc, "Key type does not match this map's key type."));

  if (target.precomputed) target.data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, target.data);
  std::string cmds = target.data + "\n";

  if (index.precomputed) {
    std::string keyLit = keyType.isString() ? index.data : ("\"" + index.data + "\"");
    std::string sourcePath = std::format("expr_str{}.{}", id, keyLit);

    if (resultType.isFloat()) {
      cmds += std::format("data modify storage {0}:global expr_float{1} set from storage {0}:global {2}", datapackNamespace, id, sourcePath);
    } else if (resultType.isString() || resultType.isList() || resultType.isMap() || resultType.isStruct() || resultType.isRef()) {
      cmds += std::format("data modify storage {0}:global expr_str{1} set from storage {0}:global {2}\n", datapackNamespace, id + 2, sourcePath);
      cmds += std::format("data modify storage {0}:global expr_str{1} set from storage {0}:global expr_str{2}", datapackNamespace, id, id + 2);
    } else {
      cmds += std::format("execute store result score expr_output{0} temp run data get storage {1}:global {2}", id, datapackNamespace, sourcePath);
    }
    return {.data = cmds, .precomputed = false, .type = resultType};
  }

  cmds += index.data + "\n";
  cmds += std::format("data modify storage {}:global macro_args set value {{}}\n", datapackNamespace);
  cmds += std::format("data modify storage {0}:global macro_args.path set value \"expr_str{1}\"\n", datapackNamespace, id);
  cmds += copyExprInto(index, "macro_args.key", id + 1);

  if (resultType.isFloat()) {
    cmds += std::format("data modify storage {}:global macro_args.out_id set value {}\n", datapackNamespace, id);
    useInternalFunction("internal_map_get_float");
    cmds += std::format("function {0}:internal/loom/internal_map_get_float with storage {0}:global macro_args", datapackNamespace);
  } else if (resultType.isString() || resultType.isList() || resultType.isMap() || resultType.isStruct() || resultType.isRef()) {
    cmds += std::format("data modify storage {}:global macro_args.out_id set value {}\n", datapackNamespace, id + 2);
    useInternalFunction("internal_map_get_object");
    cmds += std::format("function {0}:internal/loom/internal_map_get_object with storage {0}:global macro_args\n", datapackNamespace);
    cmds += std::format("data modify storage {0}:global expr_str{1} set from storage {0}:global expr_str{2}", datapackNamespace, id, id + 2);
  } else {
    cmds += std::format("data modify storage {}:global macro_args.out_id set value {}\n", datapackNamespace, id);
    useInternalFunction("internal_map_get_primitive");
    cmds += std::format("function {0}:internal/loom/internal_map_get_primitive with storage {0}:global macro_args", datapackNamespace);
  }

  return {.data = cmds, .precomputed = false, .type = resultType};
}

Compiler::ExpressionData Compiler::compileExpressionImpl(const Expr &node, unsigned int id, bool precompute) {
  return std::visit(
    [&](auto &&n) -> ExpressionData {
      using T = std::decay_t<decltype(n)>;

      if constexpr (std::is_same_v<T, IntLit>) {
        if (precompute) {
          return {.data = n.text, .precomputed = true, .type = Type::IntegerType()};
        }
        return {.data = std::format("scoreboard players set expr_output{} temp {}", id, n.text), .precomputed = false, .type = Type::IntegerType()};
      }

      else if constexpr (std::is_same_v<T, FloatLit>) {
        if (precompute) {
          return {.data = n.text, .precomputed = true, .type = Type::FloatType()};
        }
        return {
          .data = std::format("data modify storage {}:global expr_float{} set value {}f", datapackNamespace, id, n.text),
          .precomputed = false,
          .type = Type::FloatType()
        };
      }

      else if constexpr (std::is_same_v<T, BoolLit>) {
        const std::string &numericVal = n.value ? "1" : "0";
        if (precompute) {
          return {.data = numericVal, .precomputed = true, .type = Type::BooleanType()};
        }
        return {.data = std::format("scoreboard players set expr_output{} temp {}", id, numericVal), .precomputed = false, .type = Type::BooleanType()};
      }

      else if constexpr (std::is_same_v<T, StringLit>) {
        if (precompute) {
          return {.data = n.text, .precomputed = true, .type = Type::StringType()};
        }
        return {.data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, n.text), .precomputed = false, .type = Type::StringType()};
      }

      else if constexpr (std::is_same_v<T, ListExpr>) {
        uint32_t childCount = static_cast<uint32_t>(n.elements.size());

        Type elementType = Type::IntegerType();
        bool allPrecomputed = true;
        std::vector<ExpressionData> compiledElements;
        compiledElements.reserve(childCount);

        for (uint32_t i = 0; i < childCount; ++i) {
          ExpressionData elemData = compileExpression(*n.elements[i], id + 1, true);

          if (i == 0) {
            elementType = elemData.type;
          } else if (elemData.type != elementType) {
            throw std::runtime_error(formatError(n.elements[i]->loc, "Mismatched types inside list expression."));
          }

          if (!elemData.precomputed) {
            allPrecomputed = false;
          }
          compiledElements.push_back(elemData);
        }

        Type listType = Type::ListTypeOf(elementType);

        if (allPrecomputed) {
          std::string jsonArray = "[";
          for (size_t i = 0; i < compiledElements.size(); ++i) {
            jsonArray += compiledElements[i].data;
            if (i + 1 < compiledElements.size()) jsonArray += ",";
          }
          jsonArray += "]";

          if (precompute) {
            return {.data = jsonArray, .precomputed = true, .type = listType};
          }
          return {.data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, jsonArray), .precomputed = false, .type = listType};
        }

        std::string runtimeCmds = std::format("data modify storage {}:global expr_str{} set value []\n", datapackNamespace, id);

        for (const auto &elemData : compiledElements) {
          if (elemData.precomputed) {
            runtimeCmds += std::format("data modify storage {}:global expr_str{} append value {}\n", datapackNamespace, id, elemData.data);
          } else {
            runtimeCmds += elemData.data + "\n";

            if (elemData.type.isString() || elemData.type.kind == Type::List || elemData.type.kind == Type::Map) {
              runtimeCmds +=
                std::format("data modify storage {}:global expr_str{} append from storage {}:global expr_str{}\n", datapackNamespace, id, datapackNamespace, id + 1);
            } else {
              runtimeCmds += std::format(
                "execute store result storage {0}:global expr_int{2} int 1 run scoreboard players get expr_output{2} temp\n"
                "data modify storage {0}:global expr_str{1} append from storage {0}:global expr_int{2}\n",
                datapackNamespace,
                id,
                id + 1
              );
            }
          }
        }

        return {.data = runtimeCmds, .precomputed = false, .type = listType};
      }

      else if constexpr (std::is_same_v<T, StructExpr>) {
        auto it = findInMap(structs, n.name);
        if (it == structs.end()) {
          throw std::runtime_error(formatError(node.loc, "Unknown struct type: " + n.name));
        }
        const StructData &structData = it->second;
        if (structData.hasConstructor) {
          throw std::runtime_error(
            formatError(node.loc, std::format("Struct '{}' has a constructor; use {}(...) instead of struct-literal syntax.", n.name, structData.name))
          );
        }
        Type targetType = Type::StructTypeOf(&structData);

        bool allPrecomputed = true;
        std::vector<std::pair<std::string, ExpressionData>> compiledFields;

        for (const auto &field : n.fields) {
          ExpressionData elemData = compileExpression(*field.value, id + 1, true);

          bool found = false;
          for (const auto &sf : structData.fields) {
            if (sf.name == field.name) {
              found = true;
              if (*sf.type != elemData.type) {
                throw std::runtime_error(formatError(node.loc, "Type mismatch for field '" + field.name + "'"));
              }
              break;
            }
          }
          if (!found) {
            throw std::runtime_error(formatError(node.loc, "Unknown field '" + field.name + "' in struct " + n.name));
          }

          if (!elemData.precomputed) allPrecomputed = false;
          compiledFields.push_back({field.name, elemData});
        }

        if (allPrecomputed) {
          std::string jsonObj = "{";
          for (size_t i = 0; i < compiledFields.size(); ++i) {
            jsonObj += compiledFields[i].first + ":" + compiledFields[i].second.data;
            if (i + 1 < compiledFields.size()) jsonObj += ",";
          }
          jsonObj += "}";

          if (precompute) {
            return {.data = jsonObj, .precomputed = true, .type = targetType};
          }
          return {.data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, jsonObj), .precomputed = false, .type = targetType};
        }

        std::string runtimeCmds = std::format("data modify storage {}:global expr_str{} set value {{}}\n", datapackNamespace, id);

        for (const auto &pair : compiledFields) {
          const auto &fieldName = pair.first;
          const auto &elemData = pair.second;
          if (elemData.precomputed) {
            runtimeCmds += std::format("data modify storage {}:global expr_str{}.{} set value {}\n", datapackNamespace, id, fieldName, elemData.data);
          } else {
            runtimeCmds += elemData.data + "\n";
            if (elemData.type.isString() || elemData.type.isList() || elemData.type.isMap() || elemData.type.isStruct()) {
              runtimeCmds += std::format(
                "data modify storage {}:global expr_str{}.{} set from storage {}:global expr_str{}\n",
                datapackNamespace,
                id,
                fieldName,
                datapackNamespace,
                id + 1
              );
            } else if (elemData.type.isFloat()) {
              runtimeCmds += std::format(
                "data modify storage {}:global expr_str{}.{} set from storage {}:global expr_float{}\n",
                datapackNamespace,
                id,
                fieldName,
                datapackNamespace,
                id + 1
              );
            } else {
              runtimeCmds += std::format(
                "execute store result storage {}:global expr_str{}.{} int 1 run scoreboard players get expr_output{} temp\n",
                datapackNamespace,
                id,
                fieldName,
                id + 1
              );
            }
          }
        }

        return {.data = runtimeCmds, .precomputed = false, .type = targetType};
      }

      else if constexpr (std::is_same_v<T, TernaryExpr>) {
        ExpressionData condition = compileExpression(*n.condition, id + 2, true);

        if (!condition.type.isBoolean()) throw std::runtime_error(formatError(n.condition->loc, "Condition of ternary must be a boolean."));

        if (condition.precomputed) {
          return compileExpression(condition.data == "1" ? *n.ifTrue : *n.ifFalse, id, precompute);
        }
        ExpressionData left = compileExpression(*n.ifTrue, id + 1, false);
        ExpressionData right = compileExpression(*n.ifFalse, id, false);

        if (left.type != right.type) throw std::runtime_error(formatError(n.ifFalse->loc, "Both possible ternary outputs must match."));

        return {
          .data = std::format(
            "{0}\n{1}\nexecute if score expr_output{2} temp matches 1 run scoreboard players operation expr_output{3} temp = expr_output{2} temp",
            condition.data,
            right.data,
            id + 2,
            id
          ),
          .precomputed = false,
          .type = left.type
        };
      }

      else if constexpr (std::is_same_v<T, MemberExpr>) {
        std::string objText = exprToString(*n.object);

        std::optional<Compiler::ExpressionData> objExpr;
        try {
          objExpr = compileExpression(*n.object, id, precompute);
        } catch (...) {
        }

        if (objExpr.has_value()) {
          if (TypeHandler *handler = getHandler(objExpr->type)) {
            if (auto optResult = handler->compileMemberExpression(*this, objExpr.value(), n.property, id, precompute, node.loc)) {
              return optResult.value();
            }
          }
        }

        const auto enumIt = findInMap(enums, objText);
        if (enumIt != enums.end()) {
          const auto &enumRef = enumIt->second;

          auto varIt = enumRef.variants.find(n.property);
          if (varIt == enumRef.variants.end()) {
            throw std::runtime_error(formatError(node.loc, std::format("Enum '{}' has no variant named '{}'", objText, n.property)));
          }

          const EnumVariant &var = varIt->second;

          return std::visit(
            [&](auto &&arg) -> ExpressionData {
              using VT = std::decay_t<decltype(arg)>;

              if constexpr (std::is_same_v<VT, int32_t>) {
                if (precompute) {
                  return {.data = std::to_string(arg), .precomputed = true, .type = Type::EnumTypeOf(&enumIt->second)};
                }
                return {.data = std::format("scoreboard players set expr_output{} temp {}", id, arg), .precomputed = false, .type = Type::EnumTypeOf(&enumIt->second)};
              } else if constexpr (std::is_same_v<VT, std::string>) {
                if (precompute) {
                  return {.data = arg, .precomputed = true, .type = Type::EnumTypeOf(&enumIt->second)};
                }
                return {
                  .data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, arg),
                  .precomputed = false,
                  .type = Type::EnumTypeOf(&enumIt->second)
                };
              } else if constexpr (std::is_same_v<VT, float>) {
                if (precompute) {
                  return {.data = std::to_string(arg), .precomputed = true, .type = Type::EnumTypeOf(&enumIt->second)};
                }
                return {
                  .data = std::format("data modify storage {}:global expr_float{} set value {}", datapackNamespace, id, arg),
                  .precomputed = false,
                  .type = Type::EnumTypeOf(&enumIt->second)
                };
              }
            },
            var.value
          );
        }
        throw std::runtime_error(formatError(node.loc, std::format("Unknown identifier: '{}'", objText)));
      }

      else if constexpr (std::is_same_v<T, EntityTestExpr>) {
        return {
          .data = std::format("scoreboard players set expr_output{} temp 0\nexecute if entity {} run scoreboard players set expr_output{} temp 1", id, n.selectorText, id),
          .precomputed = false,
          .type = Type::BooleanType()
        };
      }

      else if constexpr (std::is_same_v<T, UnaryExpr>) {
        const ExpressionData subExpr = compileExpression(*n.operand, id, true);

        if (auto *innerUnary = std::get_if<UnaryExpr>(&n.operand->data)) {
          if (n.op == innerUnary->op) {
            return compileExpression(*innerUnary->operand, id, precompute);
          }
        }

        if (TypeHandler *handler = getHandler(subExpr.type)) {
          if (auto optResult = handler->compileUnaryOp(*this, n.op, subExpr, id, precompute, node.loc)) {
            return optResult.value();
          }
        }

        throw std::runtime_error(formatError(node.loc, "Unknown unary operation: " + n.op));
      }

      else if constexpr (std::is_same_v<T, MethodCallExpr>) {
        std::string objVarName;
        if (auto *vr = std::get_if<VarRefExpr>(&n.object->data)) {
          objVarName = vr->name;
        } else {
          objVarName = exprToString(*n.object);
        }

        std::vector<const Expr *> argNodes;
        for (const auto &a : n.arguments) argNodes.push_back(a.get());

        return compileMethodCallOnVariable(objVarName, n.method, argNodes, id, precompute, node.loc);
      }

      else if constexpr (std::is_same_v<T, CallExpr>) {
        if (n.name.size() > 4 && n.name.compare(0, 4, "map<") == 0 && n.name.back() == '>') {
          if (!n.arguments.empty()) throw std::runtime_error(formatError(node.loc, "map<K, V>() takes no arguments — it always constructs an empty map."));
          Type mapType = parseTypeFromString(n.name);
          if (!(mapType.baseType->isInteger() || mapType.baseType->isFloat() || mapType.baseType->isBoolean() || mapType.baseType->isString())) {
            throw std::runtime_error(formatError(node.loc, "Map keys must be int, float, bool, or string."));
          }
          return ExpressionData{.data = "{}", .precomputed = true, .type = std::move(mapType)};
        }

        std::string targetFunc = n.name;
        std::transform(targetFunc.begin(), targetFunc.end(), targetFunc.begin(), ::tolower);

        std::vector<const Expr *> argNodes;
        for (const auto &a : n.arguments) argNodes.push_back(a.get());

        if (const auto &it = builtins.find(targetFunc); it != builtins.end()) {
          if (auto optResult = it->second(*this, argNodes, id, precompute, node.loc)) return optResult.value();
        }

        if (currentStructContext && targetFunc.find("::") == std::string::npos) {
          std::string implicitKey = currentStructContext->name + "::" + targetFunc;
          std::transform(implicitKey.begin(), implicitKey.end(), implicitKey.begin(), ::tolower);
          auto implicitIt = funcs.find(implicitKey);
          if (implicitIt != funcs.end() && !implicitIt->second.empty() && !implicitIt->second.front().isStatic) {
            auto thisIt = vars.find("this");
            if (thisIt != vars.end()) {
              ExpressionData implicitSelf = {.data = std::format("\"{}\"", thisIt->second.mangledName), .precomputed = true, .type = thisIt->second.type};
              return compileFunctionInvocation(implicitIt->second, targetFunc, argNodes, implicitSelf, id, precompute, node.loc);
            }
          }
        }

        auto funcIt = findInMap(funcs, targetFunc, true);
        if (funcIt == funcs.end()) {
          throw std::runtime_error(formatError(node.loc, "Unknown function: " + targetFunc));
        }

        const auto &overloads = funcIt->second;
        if (!overloads.empty() && overloads.front().ownerStruct) {
          const FunctionData &rep = overloads.front();
          if (!rep.isConstructor && !rep.isStatic) {
            throw std::runtime_error(formatError(node.loc, std::format("'{}' is an instance method; call it as <object>.{}(...)", targetFunc, targetFunc)));
          }
          if (rep.isPrivate && currentStructContext != rep.ownerStruct) {
            throw std::runtime_error(formatError(node.loc, std::format("'{}' is private to struct '{}'.", targetFunc, rep.ownerStruct->name)));
          }
        }

        return compileFunctionInvocation(overloads, targetFunc, argNodes, std::nullopt, id, precompute, node.loc);
      }

      else if constexpr (std::is_same_v<T, VarRefExpr>) {
        const std::string &targetVar = n.name;

        auto varIt = findInMap(vars, targetVar);
        if (varIt == vars.end() && currentStructContext) {
          for (const auto &field : currentStructContext->fields) {
            if (field.name != targetVar) continue;
            auto thisIt = vars.find("this");
            if (thisIt == vars.end()) break;

            ExpressionData thisObj{
              .data = std::format(
                "data modify storage {}:global expr_str{} set from storage {}:global vars.{}",
                datapackNamespace,
                id,
                datapackNamespace,
                thisIt->second.mangledName
              ),
              .precomputed = false,
              .type = Type::StructTypeOf(currentStructContext)
            };
            if (TypeHandler *handler = getHandler(thisObj.type)) {
              if (auto res = handler->compileMemberExpression(*this, thisObj, targetVar, id, precompute, node.loc)) return res.value();
            }
          }
        }
        if (varIt == vars.end()) {
          throw std::runtime_error(formatError(node.loc, "Unknown variable used in expression: " + targetVar));
        }

        const auto &varData = varIt->second;
        const Type &actualType = varData.type.isRef() ? *varData.type.baseType : varData.type;

        if (varData.value.has_value()) {
          if (precompute) {
            return {.data = varData.value.value(), .precomputed = true, .type = actualType};
          }
          if (actualType.isString() || actualType.isList() || actualType.isMap() || actualType.isStruct()) {
            return {
              .data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, varData.value.value()),
              .precomputed = false,
              .type = actualType
            };
          }
          if (actualType.isFloat()) {
            return {
              .data = std::format("data modify storage {}:global expr_float{} set value {}f", datapackNamespace, id, varData.value.value()),
              .precomputed = false,
              .type = actualType
            };
          }
          return {.data = std::format("scoreboard players set expr_output{} temp {}", id, varData.value.value()), .precomputed = false, .type = actualType};
        }

        if (actualType.isString() || actualType.isList() || actualType.isMap() || actualType.isStruct()) {
          return {
            .data =
              std::format("data modify storage {}:global expr_str{} set from storage {}:global vars.{}", datapackNamespace, id, datapackNamespace, varData.getStorageName()),
            .precomputed = false,
            .type = actualType
          };
        }

        if (actualType.isFloat()) {
          return {
            .data =
              std::format("data modify storage {}:global expr_float{} set from storage {}:global vars.{}", datapackNamespace, id, datapackNamespace, varData.getStorageName()),
            .precomputed = false,
            .type = actualType
          };
        }

        std::string branchCond;
        if (actualType.isBoolean() || actualType.isInteger()) {
          branchCond = std::format("if score {} vars matches 1..", varData.getStorageName());
        }
        return {
          .data = std::format("scoreboard players operation expr_output{} temp = {} vars", id, varData.getStorageName()),
          .precomputed = false,
          .type = actualType,
          .branchCondition = branchCond.empty() ? std::optional<std::string>{} : branchCond
        };
      }

      else if constexpr (std::is_same_v<T, SliceExpr>) {
        ExpressionData target = compileExpression(*n.target, id, true);
        ExpressionData start = compileExpression(*n.start, id + 1, true);
        ExpressionData end = compileExpression(*n.end, id + 2, true);

        if (!target.type.isString() && !target.type.isList()) {
          throw std::runtime_error(formatError(node.loc, "Slice parameters can only be used on strings or lists."));
        }
        if (!start.type.isInteger() || !end.type.isInteger()) {
          throw std::runtime_error(formatError(node.loc, "Slice ranges must evaluate to integer bounds."));
        }

        if (target.type.isString() && target.precomputed && start.precomputed && end.precomputed) {
          std::string rawStr = target.data;
          if (rawStr.size() >= 2 && (rawStr.front() == '"' || rawStr.front() == '\'')) {
            rawStr = rawStr.substr(1, rawStr.size() - 2);
          }
          int32_t sIdx = std::clamp(std::stoi(start.data), 0, (int32_t)rawStr.size());
          int32_t eIdx = std::clamp(std::stoi(end.data), sIdx, (int32_t)rawStr.size());
          const std::string &sliced = "\"" + rawStr.substr(sIdx, eIdx - sIdx) + "\"";

          if (precompute) return {.data = sliced, .precomputed = true, .type = Type::StringType()};
          return {
            .data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, sliced),
            .precomputed = false,
            .type = Type::StringType()
          };
        }

        if (target.precomputed) target.data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, target.data);
        if (start.precomputed) start.data = std::format("scoreboard players set expr_output{} temp {}", id + 1, start.data);
        if (end.precomputed) end.data = std::format("scoreboard players set expr_output{} temp {}", id + 2, end.data);

        std::string runtimeCmds = target.data + "\n" + start.data + "\n" + end.data + "\n";
        runtimeCmds += std::format(
          "data modify storage {}:global macro_args set value {{out_id: {}, target_id: {}}}\n"
          "execute store result storage {}:global macro_args.start int 1 run scoreboard players get expr_output{} temp\n"
          "execute store result storage {}:global macro_args.end int 1 run scoreboard players get expr_output{} temp\n",
          datapackNamespace,
          id,
          id,
          datapackNamespace,
          id + 1,
          datapackNamespace,
          id + 2
        );

        if (target.type.isString()) {
          useInternalFunction("internal_string_slice");
          runtimeCmds += std::format("function {}:internal/loom/internal_string_slice with storage {}:global macro_args", datapackNamespace, datapackNamespace);
        } else {
          useInternalFunction("internal_list_slice");
          runtimeCmds += std::format("function {}:internal/loom/internal_list_slice with storage {}:global macro_args", datapackNamespace, datapackNamespace);
        }

        return {.data = runtimeCmds, .precomputed = false, .type = target.type};
      }

      else if constexpr (std::is_same_v<T, ElementExpr>) {
        ExpressionData target = compileExpression(*n.target, id, true);
        ExpressionData index = compileExpression(*n.index, id + 1, true);

        if (target.type.isMap()) return compileMapGet(target, index, id, node.loc);

        if (!index.type.isInteger()) {
          throw std::runtime_error(formatError(node.loc, "Index must evaluate to an integer."));
        }
        if (!target.type.isString() && !target.type.isList()) {
          throw std::runtime_error(formatError(node.loc, "Only strings, lists, and maps can be queried."));
        }

        Type resultType = target.type.isString() ? Type::StringType() : *target.type.baseType;

        if (target.type.isString() && target.precomputed && index.precomputed) {
          std::string rawStr = target.data;
          if (rawStr.size() >= 2 && (rawStr.front() == '"' || rawStr.front() == '\'')) {
            rawStr = rawStr.substr(1, rawStr.size() - 2);
          }
          int32_t idx = std::stoi(index.data);
          std::string singleChar = (idx >= 0 && idx < (int32_t)rawStr.size()) ? std::string(1, rawStr[idx]) : "";

          if (precompute) return {.data = "\"" + singleChar + "\"", .precomputed = true, .type = Type::StringType()};
          return {
            .data = std::format("data modify storage {}:global expr_str{} set value \"{}\"", datapackNamespace, id, singleChar),
            .precomputed = false,
            .type = Type::StringType()
          };
        }

        if (target.precomputed) target.data = std::format("data modify storage {}:global expr_str{} set value {}", datapackNamespace, id, target.data);
        if (index.precomputed) index.data = std::format("scoreboard players set expr_output{} temp {}", id + 1, index.data);

        std::string runtimeCmds = target.data + "\n" + index.data + "\n";

        if (target.type.isString()) {
          useInternalFunction("internal_string_slice");
          runtimeCmds += std::format(
            "scoreboard players operation expr_output{} temp = expr_output{} temp\n"
            "scoreboard players add expr_output{} temp 1\n"
            "data modify storage {}:global macro_args set value {{out_id: {}, target_id: {}}}\n"
            "execute store result storage {}:global macro_args.start int 1 run scoreboard players get expr_output{} temp\n"
            "execute store result storage {}:global macro_args.end int 1 run scoreboard players get expr_output{} temp\n"
            "function {}:internal/loom/internal_string_slice with storage {}:global macro_args",
            id + 2,
            id + 1,
            id + 2,
            datapackNamespace,
            id,
            id,
            datapackNamespace,
            id + 1,
            datapackNamespace,
            id + 2,
            datapackNamespace,
            datapackNamespace
          );
        } else {
          runtimeCmds += std::format(
            "data modify storage {}:global macro_args set value {{out_id: {}, target_id: {}}}\n"
            "execute store result storage {}:global macro_args.index int 1 run scoreboard players get expr_output{} temp\n",
            datapackNamespace,
            id,
            id,
            datapackNamespace,
            id + 1
          );

          if (resultType.isString() || resultType.isList() || resultType.isMap() || resultType.isStruct() || resultType.isRef()) {
            useInternalFunction("internal_list_get_object");
            runtimeCmds += std::format("function {}:internal/loom/internal_list_get_object with storage {}:global macro_args", datapackNamespace, datapackNamespace);
          } else {
            useInternalFunction("internal_list_get_primitive");
            runtimeCmds += std::format("function {}:internal/loom/internal_list_get_primitive with storage {}:global macro_args", datapackNamespace, datapackNamespace);
          }
        }

        return {.data = runtimeCmds, .precomputed = false, .type = resultType};
      }

      else if constexpr (std::is_same_v<T, CastExpr>) {
        ExpressionData subExpr = compileExpression(*n.expression, id, true);
        Type targetType = parseTypeFromString(n.typeText);

        if (subExpr.type == targetType) return subExpr;

        if (TypeHandler *handler = getHandler(subExpr.type)) {
          if (auto optResult = handler->compileCast(*this, subExpr, targetType, id, precompute, node.loc)) {
            return optResult.value();
          }
        }

        throw std::runtime_error(formatError(node.loc, "Cannot cast from given type to target type."));
      }

      else if constexpr (std::is_same_v<T, AtTestExpr>) {
        return {
          .data =
            std::format("scoreboard players set expr_output{} temp 0\nexecute if block {} {} run scoreboard players set expr_output{} temp 1", id, n.posText, n.blockText, id),
          .precomputed = false,
          .type = Type::BooleanType()
        };
      }

      else if constexpr (std::is_same_v<T, BinaryExpr>) {
        ExpressionData left = compileExpression(*n.left, id, true);
        ExpressionData right = compileExpression(*n.right, id + 1, true);

        if (left.type.isInteger() && right.type.isFloat()) {
          if (left.precomputed) {
            left.data = std::to_string(static_cast<float>(std::stoi(left.data)));
            left.type = Type::FloatType();
          } else {
            std::string promoteCmd = left.data + "\n";
            promoteCmd += std::format("execute store result storage {0}:global expr_float{1} float 1 run scoreboard players get expr_output{1} temp", datapackNamespace, id);
            left.data = promoteCmd;
            left.type = Type::FloatType();
          }
        } else if (left.type.isFloat() && right.type.isInteger()) {
          if (right.precomputed) {
            right.data = std::to_string(static_cast<float>(std::stoi(right.data)));
            right.type = Type::FloatType();
          } else {
            std::string promoteCmd = right.data + "\n";
            promoteCmd +=
              std::format("execute store result storage {0}:global expr_float{1} float 1 run scoreboard players get expr_output{1} temp", datapackNamespace, id + 1);
            right.data = promoteCmd;
            right.type = Type::FloatType();
          }
        }

        if (TypeHandler *handler = getHandler(left.type)) {
          if (auto optResult = handler->compileBinaryOp(*this, n.op, left, right, id, precompute, node.loc)) {
            return optResult.value();
          }
        }

        throw std::runtime_error(formatError(node.loc, "No handler found for binary operation '" + n.op + "' on the given types."));
      }

      else if constexpr (std::is_same_v<T, ReferenceExpr>) {
        const std::string &targetVar = n.targetName;

        auto varIt = findInMap(vars, targetVar);
        if (varIt == vars.end()) {
          throw std::runtime_error(formatError(node.loc, "Cannot take reference to unknown variable: " + targetVar));
        }
        const auto &varData = varIt->second;
        if (varData.type.isRef()) {
          throw std::runtime_error(formatError(node.loc, "Cannot take a reference to a reference variable: " + targetVar));
        }

        return {.data = std::format("\"{}\"", varData.mangledName), .precomputed = true, .type = Type::RefTypeOf(varData.type)};
      }

      else {
        throw std::runtime_error(formatError(node.loc, "Unexpected expression kind."));
      }
    },
    node.data
  );
}
