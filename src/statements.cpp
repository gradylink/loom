#include "compiler.hpp"
#include "utils.hpp"
#include <format>
#include <sstream>
#include <stdexcept>

std::string Compiler::compileBlock(const Block &block) {
  std::string ret = "";
  bool returned = false;

  for (const auto &stmtPtr : block.statements) {
    if (returned) break;
    const Stmt &stmt = *stmtPtr;

    std::visit(
      [&](auto &&n) {
        using T = std::decay_t<decltype(n)>;

        if constexpr (std::is_same_v<T, IfStmt>) {
          ret += compileIf(n, stmt.loc);
        }

        else if constexpr (std::is_same_v<T, WhileStmt>) {
          ret += compileWhile(n, stmt.loc);
        }

        else if constexpr (std::is_same_v<T, DoWhileStmt>) {
          ret += compileDoWhile(n, stmt.loc);
        }

        else if constexpr (std::is_same_v<T, ForStmt>) {
          ret += compileFor(n, stmt.loc);
        }

        else if constexpr (std::is_same_v<T, ContextStmt>) {
          std::string contextChain = "execute";

          for (const auto &mod : n.modifiers) {
            if (mod.keyword == "as" || mod.keyword == "at") {
              contextChain += std::format(" {} {}", mod.keyword, mod.primaryText);
            } else if (mod.keyword == "align") {
              contextChain += std::format(" align {}", mod.primaryText);
            } else if (mod.keyword == "anchored") {
              contextChain += std::format(" anchored {}", mod.primaryText);
            } else if (mod.keyword == "facing") {
              if (mod.secondaryText.has_value()) {
                contextChain += std::format(" facing entity {} {}", mod.primaryText, *mod.secondaryText);
              } else {
                contextChain += std::format(" facing {}", mod.primaryText);
              }
            } else if (mod.keyword == "in") {
              contextChain += std::format(" in {}", mod.primaryText);
            } else if (mod.keyword == "on") {
              contextChain += std::format(" on {}", mod.primaryText);
            } else if (mod.keyword == "positioned") {
              if (mod.primaryText == "as" && mod.secondaryText.has_value()) {
                contextChain += std::format(" positioned as {}", *mod.secondaryText);
              } else if (mod.primaryText == "over" && mod.secondaryText.has_value()) {
                contextChain += std::format(" positioned over {}", *mod.secondaryText);
              } else {
                contextChain += std::format(" positioned {}", mod.primaryText);
              }
            } else if (mod.keyword == "rotated") {
              if (mod.primaryText == "as" && mod.secondaryText.has_value()) {
                contextChain += std::format(" rotated as {}", *mod.secondaryText);
              } else {
                contextChain += std::format(" rotated {}", mod.primaryText);
              }
            }
          }

          bool hasAsModifier = false;
          for (const auto &mod : n.modifiers) {
            if (mod.keyword == "as") {
              hasAsModifier = true;
              break;
            }
          }
          const bool prevInsideEntityContext = insideEntityContext;
          if (hasAsModifier) insideEntityContext = true;
          const std::string innerBlockData = compileBlock(*n.body);
          insideEntityContext = prevInsideEntityContext;
          std::vector<std::string> validLines;
          std::stringstream ss(innerBlockData);
          std::string currentLine;
          while (std::getline(ss, currentLine)) {
            if (!currentLine.empty() && currentLine.back() == '\r') currentLine.pop_back();
            if (!currentLine.empty()) validLines.push_back(currentLine);
          }

          if (validLines.size() == 1) {
            ret += std::format("{} run {}\n", contextChain, validLines[0]);
          } else if (validLines.size() > 1) {
            const std::string &macroFuncName = "_generated_function_" + randomFunctionMangleString();
            std::string macroBody = "";
            for (const auto &line : validLines) {
              macroBody += line + "\n";
            }
            compiledFunctions.push_back({.name = macroFuncName, .data = macroBody});
            ret += std::format("{} run function {}:internal/{}\n", contextChain, datapackNamespace, macroFuncName);
          }
        }

        else if constexpr (std::is_same_v<T, VarDeclStmt>) {
          ret += compileVariableDeclaration(n, stmt.loc, &block, false);
        }

        else if constexpr (std::is_same_v<T, AssignStmt>) {
          const std::string &name = n.name;
          auto varIt = findInMap(vars, name);

          if (varIt == vars.end() && currentStructContext) {
            const Compiler::StructField *implicitField = nullptr;
            for (const auto &field : currentStructContext->fields) {
              if (field.name == name) {
                implicitField = &field;
                break;
              }
            }
            if (implicitField) {
              if (!n.path.empty()) {
                throw std::runtime_error(formatError(stmt.loc, "Nested assignment through an implicit field is not yet supported; use 'this." + name + "...' explicitly."));
              }
              auto thisIt = vars.find("this");
              if (thisIt == vars.end()) {
                throw std::runtime_error(formatError(stmt.loc, "Compiler Error: missing implicit 'this' while assigning field '" + name + "'."));
              }
              const std::string &thisMangled = thisIt->second.mangledName;
              const Type &fieldType = *implicitField->type;

              const ExpressionData expr = compileExpression(*n.value, 1, true);
              if (expr.type != fieldType) {
                throw std::runtime_error(formatError(stmt.loc, "Type mismatch in assignment to field '" + name + "'."));
              }

              if (expr.precomputed) {
                ret += std::format("data modify storage {0}:global vars.{1}.{2} set value {3}\n", datapackNamespace, thisMangled, name, expr.data);
              } else if (expr.type.isString() || expr.type.isList() || expr.type.isMap() || expr.type.isStruct()) {
                ret += std::format("{0}\ndata modify storage {1}:global vars.{2}.{3} set from storage {1}:global expr_str1\n", expr.data, datapackNamespace, thisMangled, name);
              } else if (expr.type.isFloat()) {
                ret += std::format("{0}\ndata modify storage {1}:global vars.{2}.{3} set from storage {1}:global expr_float1\n", expr.data, datapackNamespace, thisMangled, name);
              } else {
                ret += std::format(
                  "{0}\nexecute store result storage {1}:global vars.{2}.{3} int 1 run scoreboard players get expr_output1 temp\n", expr.data, datapackNamespace, thisMangled, name
                );
              }
              return;
            }
          }

          if (varIt == vars.end()) {
            throw std::runtime_error(formatError(stmt.loc, "Assignment to undefined variable: " + name));
          }
          const auto &varData = varIt->second;
          if (varData.constant) {
            throw std::runtime_error(formatError(stmt.loc, "Cannot reassign constant variable: " + name));
          }

          const Expr &expNode = *n.value;
          const bool isPathAssignment = !n.path.empty();

          if (varData.isEntityLocal) {
            if (isPathAssignment) {
              throw std::runtime_error(formatError(stmt.loc, "Indexed/path assignment to an '@entity' variable is not supported; assign the whole value instead."));
            }
            if (!insideEntityContext) {
              throw std::runtime_error(formatError(stmt.loc, "Entity-local variable '" + name + "' can only be assigned inside a 'context ... as ...' block."));
            }

            const Type &actualType = varData.type;
            const ExpressionData expr = compileExpression(expNode);
            if (expr.type != actualType) {
              throw std::runtime_error(formatError(stmt.loc, "Type mismatch in assignment to entity-local variable: " + name));
            }

            useInternalFunction("internal_loom_ensure_entity_id");
            useInternalFunction("internal_loom_assign_entity_id");
            ret += std::format("function {}:internal/loom/internal_loom_ensure_entity_id\n", datapackNamespace);

            if (actualType.isString() || actualType.isList() || actualType.isMap() || actualType.isStruct() || actualType.isFloat()) {
              if (!expr.precomputed) ret += expr.data + "\n";
              ret += copyExprInto(expr, actualType.isFloat() ? "expr_float1" : "expr_str1", 1);

              ret += std::format("data modify storage {}:global macro_args set value {{}}\n", datapackNamespace);
              ret += std::format("data modify storage {0}:global macro_args.path set value \"vars.{1}\"\n", datapackNamespace, varData.mangledName);
              ret += "scoreboard players operation expr_output2 temp = @s loom_id\n";
              ret += std::format("execute store result storage {0}:global macro_args.key int 1 run scoreboard players get expr_output2 temp\n", datapackNamespace);

              if (actualType.isFloat()) {
                useInternalFunction("internal_map_set_float");
                ret += std::format("function {0}:internal/loom/internal_map_set_float with storage {0}:global macro_args\n", datapackNamespace);
              } else {
                useInternalFunction("internal_map_set_object");
                ret += std::format("function {0}:internal/loom/internal_map_set_object with storage {0}:global macro_args\n", datapackNamespace);
              }
            } else {
              if (expr.precomputed) {
                ret += std::format("scoreboard players set @s {} {}\n", varData.mangledName, expr.data);
              } else {
                ret += expr.data + "\n";
                ret += std::format("scoreboard players operation @s {} = expr_output1 temp\n", varData.mangledName);
              }
            }
            return;
          }

          if (isPathAssignment) {
            Type expectedType = varData.type.isRef() ? *varData.type.baseType : varData.type;

            if (expectedType.isMap()) {
              if (n.path.size() != 1 || !n.path[0].isIndex) {
                throw std::runtime_error(
                  formatError(stmt.loc, "Only a single 'map[key] = value' assignment is supported; chained/nested map index assignment is not yet implemented.")
                );
              }
              const Type &keyType = *expectedType.baseType;
              const Type &valueType = *expectedType.mapValueType;

              ExpressionData keyExpr = compileExpression(*n.path[0].index, 2, true);
              if (keyExpr.type != keyType) throw std::runtime_error(formatError(stmt.loc, "Key type does not match this map's key type."));

              ExpressionData valueExpr = compileExpression(expNode, 1, true);
              if (valueExpr.type != valueType) throw std::runtime_error(formatError(stmt.loc, "Type mismatch in assignment to map value."));

              ret += keyExpr.data + "\n" + valueExpr.data + "\n";
              std::string destPath = std::format("vars.{}", varData.getStorageName());

              if (keyExpr.precomputed) {
                std::string keyLit = keyType.isString() ? keyExpr.data : ("\"" + keyExpr.data + "\"");
                ret += copyExprInto(valueExpr, std::format("{}.{}", destPath, keyLit), 1);
              } else {
                ret += std::format("data modify storage {}:global macro_args set value {{}}\n", datapackNamespace);
                ret += std::format("data modify storage {0}:global macro_args.path set value \"{1}\"\n", datapackNamespace, destPath);
                ret += copyExprInto(keyExpr, "macro_args.key", 2);

                if (valueExpr.type.isFloat()) {
                  useInternalFunction("internal_map_set_float");
                  ret += std::format("function {0}:internal/loom/internal_map_set_float with storage {0}:global macro_args\n", datapackNamespace);
                } else if (valueExpr.type.isString() || valueExpr.type.isList() || valueExpr.type.isMap() || valueExpr.type.isStruct() || valueExpr.type.isRef()) {
                  useInternalFunction("internal_map_set_object");
                  ret += std::format("function {0}:internal/loom/internal_map_set_object with storage {0}:global macro_args\n", datapackNamespace);
                } else {
                  useInternalFunction("internal_map_set_primitive");
                  ret += std::format("function {0}:internal/loom/internal_map_set_primitive with storage {0}:global macro_args\n", datapackNamespace);
                }
              }
              return;
            }

            bool endsInStringSubscript = false;

            for (size_t i = 0; i < n.path.size(); i++) {
              const PathComponent &pc = n.path[i];
              if (pc.isIndex) {
                if (!expectedType.isList() && !expectedType.isString()) {
                  throw std::runtime_error(formatError(stmt.loc, "Cannot use index assignment on non-container type."));
                }
                if (expectedType.isString()) {
                  if (i != n.path.size() - 1) {
                    throw std::runtime_error(formatError(stmt.loc, "String character index must be at the end of the assignment path."));
                  }
                  endsInStringSubscript = true;
                } else {
                  expectedType = *expectedType.baseType;
                  if (expectedType.isString() && i == n.path.size() - 1) endsInStringSubscript = true;
                }
              } else {
                if (expectedType.kind != Compiler::Type::Struct) {
                  throw std::runtime_error(formatError(stmt.loc, "Cannot access property on non-struct type."));
                }
                bool found = false;
                const Compiler::StructData *ownerStruct = expectedType.structRef;
                for (const auto &field : ownerStruct->fields) {
                  if (field.name == pc.propertyName) {
                    if (field.isPrivate && currentStructContext != ownerStruct) {
                      throw std::runtime_error(formatError(stmt.loc, "Field '" + pc.propertyName + "' is private to struct '" + ownerStruct->name + "'"));
                    }
                    expectedType = *field.type;
                    found = true;
                    break;
                  }
                }
                if (!found) {
                  throw std::runtime_error(formatError(stmt.loc, "Unknown field '" + pc.propertyName + "' in struct."));
                }
              }
            }

            const ExpressionData expr = compileExpression(expNode, 1, true);
            ret += expr.data + "\n";

            if (endsInStringSubscript) {
              if (!expr.type.isString()) {
                throw std::runtime_error(formatError(stmt.loc, "Type mismatch: cannot assign non-string to a string character index."));
              }
            } else {
              if (expr.type != expectedType) {
                throw std::runtime_error(formatError(stmt.loc, "Type mismatch in assignment."));
              }
            }

            struct CompiledPathComponent {
              bool isProperty;
              std::string propName;
              ExpressionData indexExpr;
            };
            std::vector<CompiledPathComponent> compiledPath;
            bool allIndicesPrecomputed = true;

            for (const auto &pc : n.path) {
              if (pc.isIndex) {
                ExpressionData idxExpr = compileExpression(*pc.index, 2, true);
                if (!idxExpr.type.isInteger()) {
                  throw std::runtime_error(formatError(stmt.loc, "Indices must evaluate to integers."));
                }
                if (!idxExpr.precomputed) allIndicesPrecomputed = false;
                compiledPath.push_back({false, "", idxExpr});
              } else {
                compiledPath.push_back({true, pc.propertyName, {}});
              }
            }

            std::string pathSuffix = "";
            size_t listDimensions = endsInStringSubscript ? compiledPath.size() - 1 : compiledPath.size();
            for (size_t i = 0; i < listDimensions; i++) {
              if (compiledPath[i].isProperty) {
                pathSuffix += "." + compiledPath[i].propName;
              } else {
                pathSuffix += "[" + compiledPath[i].indexExpr.data + "]";
              }
            }

            if (endsInStringSubscript) {
              ExpressionData stringCharIdx = compiledPath.back().indexExpr;

              ret += std::format("data modify storage {0}:global macro_args.var_name set value \"{1}\"\n", datapackNamespace, varData.getStorageName());

              if (expr.precomputed) {
                ret += std::format("data modify storage {0}:global macro_args.value set value {1}\n", datapackNamespace, expr.data);
              } else {
                ret += std::format("data modify storage {0}:global macro_args.value set from storage {0}:global expr_str1\n", datapackNamespace);
              }

              if (stringCharIdx.precomputed) {
                ret += std::format("data modify storage {0}:global macro_args.index set value {1}\n", datapackNamespace, stringCharIdx.data);
              } else {
                ret += std::format("execute store result storage {0}:global macro_args.index int 1 run scoreboard players get expr_output2 temp\n", datapackNamespace);
              }

              ret += std::format("data modify storage {0}:global macro_args.index_plus_one set from storage {0}:global macro_args.index\n", datapackNamespace);
              ret += std::format("execute store result score _temp temp run data get storage {0}:global macro_args.index_plus_one\n", datapackNamespace);
              ret += "scoreboard players add _temp temp 1\n";
              ret += std::format("execute store result storage {0}:global macro_args.index_plus_one int 1 run scoreboard players get _temp temp\n", datapackNamespace);

              ret += std::format("data modify storage {0}:global macro_args.path set value \"{1}\"\n", datapackNamespace, pathSuffix);

              if (expr.precomputed) {
                useInternalFunction("internal_string_mutate_static");
                ret += std::format("function {0}:internal/loom/internal_string_mutate_static with storage {0}:global macro_args\n", datapackNamespace);
              } else {
                useInternalFunction("internal_string_mutate_dynamic");
                ret += std::format("function {0}:internal/loom/internal_string_mutate_dynamic with storage {0}:global macro_args\n", datapackNamespace);
              }
              return;
            }

            if (allIndicesPrecomputed) {
              if (expr.precomputed) {
                ret += std::format("data modify storage {0}:global vars.{1}{2} set value {3}\n", datapackNamespace, varData.getStorageName(), pathSuffix, expr.data);
              } else if (expr.type.isString() || expr.type.isList() || expr.type.isMap() || expr.type.isStruct()) {
                ret +=
                  std::format("data modify storage {0}:global vars.{1}{2} set from storage {0}:global expr_str1\n", datapackNamespace, varData.getStorageName(), pathSuffix);
              } else if (expr.type.isFloat()) {
                ret +=
                  std::format("data modify storage {0}:global vars.{1}{2} set from storage {0}:global expr_float1\n", datapackNamespace, varData.getStorageName(), pathSuffix);
              } else {
                ret += std::format(
                  "execute store result storage {0}:global vars.{1}{2} int 1 run scoreboard players get expr_output1 temp\n",
                  datapackNamespace,
                  varData.getStorageName(),
                  pathSuffix
                );
              }
            } else {
              ret += std::format("data modify storage {0}:global macro_args.path set value \"\"\n", datapackNamespace);
              for (size_t i = 0; i < compiledPath.size(); i++) {
                if (!compiledPath[i].isProperty && !compiledPath[i].indexExpr.precomputed) ret += compiledPath[i].indexExpr.data + "\n";
                ret += std::format("data modify storage {0}:global macro_args.string_before set from storage {0}:global macro_args.path\n", datapackNamespace);

                if (compiledPath[i].isProperty) {
                  ret += std::format("data modify storage {0}:global macro_args.prop_to_append set value \"{1}\"\n", datapackNamespace, compiledPath[i].propName);
                  useInternalFunction("internal_path_append_prop");
                  ret += std::format("function {0}:internal/loom/internal_path_append_prop with storage {0}:global macro_args\n", datapackNamespace);
                } else {
                  if (compiledPath[i].indexExpr.precomputed) {
                    ret += std::format("data modify storage {0}:global macro_args.index_to_append set value \"{1}\"\n", datapackNamespace, compiledPath[i].indexExpr.data);
                  } else {
                    ret += std::format(
                      "execute store result storage {0}:global macro_args.index_to_append int 1 run scoreboard players get expr_output2 temp\n",
                      datapackNamespace
                    );
                  }
                  useInternalFunction("internal_path_append");
                  ret += std::format("function {0}:internal/loom/internal_path_append with storage {0}:global macro_args\n", datapackNamespace);
                }
              }

              ret += std::format("data modify storage {0}:global macro_args.var_name set value \"{1}\"\n", datapackNamespace, varData.getStorageName());
              if (expr.precomputed) {
                useInternalFunction("internal_list_nested_set_value");
                ret += std::format(
                  "data modify storage {0}:global macro_args.value set value {1}\nfunction {0}:internal/loom/internal_list_nested_set_value with storage {0}:global "
                  "macro_args\n",
                  datapackNamespace,
                  expr.data
                );
              } else if (expr.type.isString() || expr.type.isList() || expr.type.isMap() || expr.type.isStruct()) {
                useInternalFunction("internal_list_nested_set_object");
                ret += std::format("function {0}:internal/loom/internal_list_nested_set_object with storage {0}:global macro_args\n", datapackNamespace);
              } else {
                useInternalFunction("internal_list_nested_set_primitive");
                ret += std::format("function {0}:internal/loom/internal_list_nested_set_primitive with storage {0}:global macro_args\n", datapackNamespace);
              }
            }
            return;
          }

          const ExpressionData expr = compileExpression(expNode);

          const Type &actualType = varData.type.isRef() ? *varData.type.baseType : varData.type;

          if (actualType.kind == Compiler::Type::Enum) {
            if (expr.type.kind != Compiler::Type::Enum || expr.type.enumRef != actualType.enumRef) {
              throw std::runtime_error(formatError(stmt.loc, "Assignment to enum variable requires a variant of the same enum: " + name));
            }
          } else {
            if (actualType.isBoolean() && expr.type.isInteger()) {
              throw std::runtime_error(formatError(stmt.loc, "Cannot assign an 'int' to 'bool' variable: " + name));
            }
            if (actualType.isString() && !expr.type.isString()) {
              throw std::runtime_error(formatError(stmt.loc, "Invalid type for 'string' variable: " + name));
            }
          }

          bool usedCompoundOp = false;

          if (expr.precomputed) {
            if (actualType.isString() || actualType.isFloat() || actualType.isList() || actualType.isMap() || actualType.isStruct()) {
              ret += std::format("data modify storage {0}:global vars.{1} set value {2}\n", datapackNamespace, varData.getStorageName(), expr.data);
            } else {
              ret += std::format("scoreboard players set {} vars {}\n", varData.getStorageName(), expr.data);
            }
          } else {
            if (auto *binExpr = std::get_if<BinaryExpr>(&expNode.data); binExpr && (actualType.isInteger() || actualType.isBoolean())) {
              const std::string &op = binExpr->op;

              if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
                if (auto *leftRef = std::get_if<VarRefExpr>(&binExpr->left->data)) {
                  auto leftIt = findInMap(vars, leftRef->name);
                  if (leftIt != vars.end() && leftIt == varIt) {
                    const ExpressionData rightExpr = compileExpression(*binExpr->right, 1, false);
                    ret += std::format("{}\nscoreboard players operation {} vars {}= expr_output1 temp\n", rightExpr.data, varData.getStorageName(), op);
                    usedCompoundOp = true;
                  }
                }
                if (!usedCompoundOp) {
                  if (auto *rightRef = std::get_if<VarRefExpr>(&binExpr->right->data)) {
                    auto rightIt = findInMap(vars, rightRef->name);
                    if (rightIt != vars.end() && rightIt == varIt) {
                      const ExpressionData leftExpr = compileExpression(*binExpr->left, 1, false);
                      ret += std::format("{}\nscoreboard players operation {} vars {}= expr_output1 temp\n", leftExpr.data, varData.getStorageName(), op);
                      usedCompoundOp = true;
                    }
                  }
                }
              }
            }

            if (!usedCompoundOp) {
              if (actualType.isString() || actualType.isList() || actualType.isMap() || actualType.isStruct()) {
                ret +=
                  std::format("{0}\ndata modify storage {1}:global vars.{2} set from storage {1}:global expr_str1\n", expr.data, datapackNamespace, varData.getStorageName());
              } else if (actualType.isFloat()) {
                ret += std::format(
                  "{0}\ndata modify storage {1}:global vars.{2} set from storage {1}:global expr_float1\n",
                  expr.data,
                  datapackNamespace,
                  varData.getStorageName()
                );
              } else {
                ret += std::format("{}\nscoreboard players operation {} vars = expr_output1 temp\n", expr.data, varData.getStorageName());
              }
            }
          }

          if (!usedCompoundOp && varData.type.isRef() && !varData.refTargetMangledName.has_value()) {
            std::string argsKey = std::format("vars.{}_refargs", varData.mangledName);
            for (auto fit = compiledFunctions.rbegin(); fit != compiledFunctions.rend(); ++fit) {
              if (fit->name.starts_with("_ref_copyback_") && fit->data.find(varData.mangledName) != std::string::npos) {
                ret += std::format("function {}:internal/{} with storage {}:global {}\n", datapackNamespace, fit->name, datapackNamespace, argsKey);
                break;
              }
            }
          }
        }

        else if constexpr (std::is_same_v<T, ExprStmt>) {
          ExpressionData expr = compileExpression(*n.expr, 1, false);
          ret += expr.data + "\n";
        }

        else if constexpr (std::is_same_v<T, ReturnStmt>) {
          ret += currentFuncRefCopybacks;

          if (n.value.has_value()) {
            ExpressionData expr = compileExpression(**n.value);
            if (expr.type.isBoolean() || expr.type.isInteger()) {
              if (controlFlowDepth > 0) {
                if (expr.precomputed) {
                  ret += std::format("scoreboard players set _loom_ret_val temp {}\n", expr.data);
                } else {
                  ret += std::format("{}\nscoreboard players operation _loom_ret_val temp = expr_output1 temp\n", expr.data);
                }
                ret += "scoreboard players set _loom_returned temp 1\n";
                ret += "return 0\n";
              } else {
                if (expr.precomputed) {
                  ret += std::format("return {}\n", expr.data);
                } else {
                  ret += std::format("{}\nreturn run scoreboard players get expr_output1 temp\n", expr.data);
                }
              }
            } else {
              if (controlFlowDepth > 0) {
                ret += compileExpression(**n.value, 1, false).data + "\n";
                ret += "scoreboard players set _loom_returned temp 1\n";
                ret += "return 0\n";
              } else {
                ret += compileExpression(**n.value, 1, false).data + "\nreturn 0\n";
              }
            }
          } else {
            if (controlFlowDepth > 0) {
              ret += "scoreboard players set _loom_returned temp 1\n";
            }
            ret += "return 0\n";
          }
          returned = true;
        }

        else if constexpr (std::is_same_v<T, CommandStmt>) {
          bool requiresMacro = false;
          std::vector<std::optional<ExpressionData>> compiledParts;
          compiledParts.reserve(n.parts.size());

          for (const auto &part : n.parts) {
            if (part.isInterpolation) {
              ExpressionData expr = compileExpression(*part.interpExpr);
              compiledParts.push_back(expr);
              if (!expr.precomputed) requiresMacro = true;
            } else {
              compiledParts.push_back(std::nullopt);
            }
          }

          if (!requiresMacro) {
            ret += n.commandName;
            for (size_t i = 0; i < n.parts.size(); i++) {
              if (compiledParts[i].has_value()) {
                ret += compiledParts[i]->data;
              } else {
                ret += n.parts[i].literalText;
              }
            }
            ret += "\n";
          } else {
            std::string macroBody = "$" + n.commandName;
            std::string macroSetup = "";
            int macroVarId = 0;

            for (size_t i = 0; i < n.parts.size(); i++) {
              if (compiledParts[i].has_value()) {
                ExpressionData &expr = compiledParts[i].value();

                if (expr.precomputed) {
                  macroBody += expr.data;
                } else {
                  macroBody += std::format("$(var_{})", macroVarId);
                  macroSetup += expr.data + "\n";

                  if (expr.type.isString() || expr.type.isList() || expr.type.isMap()) {
                    macroSetup += std::format("data modify storage {0}:function_input var_{1} set from storage {0}:global expr_str1\n", datapackNamespace, macroVarId);
                  } else if (expr.type.isFloat()) {
                    macroSetup += std::format("data modify storage {0}:function_input var_{1} set from storage {0}:global expr_float1\n", datapackNamespace, macroVarId);
                  } else {
                    macroSetup += std::format(
                      "execute store result storage {0}:function_input var_{1} int 1 run scoreboard players get expr_output1 temp\n",
                      datapackNamespace,
                      macroVarId
                    );
                  }
                  macroVarId++;
                }
              } else {
                macroBody += n.parts[i].literalText;
              }
            }

            const std::string macroFuncName = "_generated_function_" + randomFunctionMangleString();
            compiledFunctions.push_back({.name = macroFuncName, .data = macroBody + "\n"});

            ret += macroSetup;
            ret += std::format("function {}:internal/{} with storage {}:function_input\n", datapackNamespace, macroFuncName, datapackNamespace);
          }
        }

        else {
          throw std::runtime_error(formatError(stmt.loc, "Invalid block statement."));
        }
      },
      stmt.data
    );
  }

  std::erase_if(vars, [&block](const auto &pair) { return pair.second.scope == &block; });

  return ret;
}
