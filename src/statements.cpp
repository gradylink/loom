#include "compiler.hpp"
#include "utils.hpp"
#include <format>
#include <sstream>
#include <stdexcept>

std::string Compiler::compileBlock(TSNode node) {
  std::string ret = "";

  for (uint32_t i = 0; i < ts_node_named_child_count(node); i++) {
    TSNode child = ts_node_named_child(node, i);
    const std::string type = ts_node_type(child);

    if (type == "if") {
      ret += compileIf(child);
      continue;
    }

    if (type == "while") {
      ret += compileWhile(child);
      continue;
    }

    if (type == "do_while") {
      ret += compileDoWhile(child);
      continue;
    }

    if (type == "for") {
      ret += compileFor(child);
      continue;
    }

    if (type == "context_statement") {
      std::string contextChain = "execute";

      const uint32_t childCount = ts_node_child_count(child);

      TSNode blockNode;

      for (uint32_t i = 0; i < childCount; i++) {
        TSNode subNode = ts_node_child(child, i);
        const std::string &subType = ts_node_type(subNode);

        if (subType == "block") {
          blockNode = subNode;
          break;
        }

        if (subType == "contextModifier") {
          TSNode keywordNode = ts_node_child(subNode, 0);
          const std::string keyword = std::string(getNodeText(keywordNode));

          if (keyword == "as" || keyword == "at") {
            const std::string selector = std::string(getNodeText(ts_node_child_by_field_name(subNode, "selector", 8)));
            contextChain += std::format(" {} {}", keyword, selector);
          } else if (keyword == "align") {
            const std::string axes = std::string(getNodeText(ts_node_child_by_field_name(subNode, "axes", 4)));
            contextChain += std::format(" align {}", axes);
          } else if (keyword == "anchored") {
            const std::string anchor = std::string(getNodeText(ts_node_child_by_field_name(subNode, "anchor", 6)));
            contextChain += std::format(" anchored {}", anchor);
          } else if (keyword == "facing") {
            const std::string secondText = std::string(getNodeText(ts_node_child(subNode, 1)));
            if (secondText == "entity") {
              const std::string selector = std::string(getNodeText(ts_node_child_by_field_name(subNode, "selector", 8)));
              const std::string anchor = std::string(getNodeText(ts_node_child_by_field_name(subNode, "anchor", 6)));
              contextChain += std::format(" facing entity {} {}", selector, anchor);
            } else {
              const std::string pos = std::string(getNodeText(ts_node_child_by_field_name(subNode, "pos", 3)));
              contextChain += std::format(" facing {}", pos);
            }
          } else if (keyword == "in") {
            const std::string dim = std::string(getNodeText(ts_node_child_by_field_name(subNode, "dim", 3)));
            contextChain += std::format(" in {}", dim);
          } else if (keyword == "on") {
            const std::string relation = std::string(getNodeText(ts_node_child_by_field_name(subNode, "relation", 8)));
            contextChain += std::format(" on {}", relation);
          } else if (keyword == "positioned") {
            const std::string secondText = std::string(getNodeText(ts_node_child(subNode, 1)));
            if (secondText == "as") {
              const std::string selector = std::string(getNodeText(ts_node_child_by_field_name(subNode, "selector", 8)));
              contextChain += std::format(" positioned as {}", selector);
            } else if (secondText == "over") {
              const std::string heightmap = std::string(getNodeText(ts_node_child_by_field_name(subNode, "heightmap", 9)));
              contextChain += std::format(" positioned over {}", heightmap);
            } else {
              const std::string pos = std::string(getNodeText(ts_node_child_by_field_name(subNode, "pos", 3)));
              contextChain += std::format(" positioned {}", pos);
            }
          } else if (keyword == "rotated") {
            const std::string secondText = std::string(getNodeText(ts_node_child(subNode, 1)));
            if (secondText == "as") {
              const std::string selector = std::string(getNodeText(ts_node_child_by_field_name(subNode, "selector", 8)));
              contextChain += std::format(" rotated as {}", selector);
            } else {
              const std::string rot = std::string(getNodeText(ts_node_child_by_field_name(subNode, "rot", 3)));
              contextChain += std::format(" rotated {}", rot);
            }
          }
        }
      }

      if (ts_node_is_null(blockNode)) {
        continue;
      }

      const std::string innerBlockData = compileBlock(blockNode);
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
        const std::string &macroFuncName = std::format("_generated_function_{}", currentGeneratedFunction++);
        std::string macroBody = "";
        for (const auto &line : validLines) {
          macroBody += line + "\n";
        }
        compiledFunctions.push_back({.name = macroFuncName, .data = macroBody});
        ret += std::format("{} run function {}:{}\n", contextChain, datapackNamespace, macroFuncName);
      }
      continue;
    }

    if (type == "variable_declaration") {
      ret += compileVariableDeclaration(child, node, false);
      continue;
    }

    if (type == "assignment") {
      const std::string name = std::string(getFieldText(child, "name"));
      auto varIt = findInMap(vars, name);
      if (varIt == vars.end()) {
        throw std::runtime_error(formatError(child, "Assignment to undefined variable: " + name));
      }
      const auto &varData = varIt->second;
      if (varData.constant) {
        throw std::runtime_error(formatError(child, "Cannot reassign constant variable: " + name));
      }

      TSNode expNode = ts_node_child_by_field_name(child, "value", 5);

      std::vector<TSNode> pathNodes;
      uint32_t namedCount = ts_node_named_child_count(child);
      for (uint32_t i = 1; i < namedCount - 1; i++) {
        pathNodes.push_back(ts_node_named_child(child, i));
      }
      const bool isPathAssignment = !pathNodes.empty();

      if (isPathAssignment) {
        Compiler::Type expectedType = varData.type;
        bool endsInStringSubscript = false;

        for (size_t i = 0; i < pathNodes.size(); i++) {
          TSNode pathNode = pathNodes[i];
          std::string nodeType = std::string(ts_node_type(pathNode));

          if (nodeType == "index_access") {
            if (!expectedType.isList() && !expectedType.isString()) {
              throw std::runtime_error(formatError(pathNode, "Cannot use index assignment on non-container type."));
            }
            if (expectedType.isString()) {
              if (i != pathNodes.size() - 1) {
                throw std::runtime_error(formatError(pathNode, "String character index must be at the end of the assignment path."));
              }
              endsInStringSubscript = true;
            } else {
              expectedType = *expectedType.baseType;
              if (expectedType.isString() && i == pathNodes.size() - 1) {
                endsInStringSubscript = true;
              }
            }
          } else if (nodeType == "property_access") {
            if (expectedType.kind != Compiler::Type::Struct) {
              throw std::runtime_error(formatError(pathNode, "Cannot access property on non-struct type."));
            }
            std::string propName = std::string(getFieldText(pathNode, "property"));
            bool found = false;
            for (const auto &field : expectedType.structRef->fields) {
              if (field.name == propName) {
                expectedType = *field.type;
                found = true;
                break;
              }
            }
            if (!found) {
              throw std::runtime_error(formatError(pathNode, "Unknown field '" + propName + "' in struct."));
            }
          }
        }

        const ExpressionData expr = compileExpression(expNode, 1, true);
        ret += expr.data + "\n";

        if (endsInStringSubscript) {
          if (!expr.type.isString()) {
            throw std::runtime_error(formatError(expNode, "Type mismatch: cannot assign non-string to a string character index."));
          }
        } else {
          if (expr.type != expectedType) {
            throw std::runtime_error(formatError(expNode, "Type mismatch in assignment."));
          }
        }

        struct PathComponent {
          bool isProperty;
          std::string propName;
          ExpressionData indexExpr;
        };
        std::vector<PathComponent> compiledPath;
        bool allIndicesPrecomputed = true;

        for (size_t i = 0; i < pathNodes.size(); i++) {
          if (std::string(ts_node_type(pathNodes[i])) == "index_access") {
            TSNode idxNode = ts_node_child_by_field_name(pathNodes[i], "index", 5);
            ExpressionData idxExpr = compileExpression(idxNode, 2, true);
            if (!idxExpr.type.isInteger()) {
              throw std::runtime_error(formatError(pathNodes[i], "Indices must evaluate to integers."));
            }
            if (!idxExpr.precomputed) allIndicesPrecomputed = false;
            compiledPath.push_back({false, "", idxExpr});
          } else {
            std::string propName = std::string(getFieldText(pathNodes[i], "property"));
            compiledPath.push_back({true, propName, {}});
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

          ret += std::format("data modify storage {0}:global macro_args.var_name set value \"{1}\"\n", datapackNamespace, varData.mangledName);

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
          continue;
        }

        if (allIndicesPrecomputed) {
          if (expr.precomputed) {
            ret += std::format("data modify storage {0}:global vars.{1}{2} set value {3}\n", datapackNamespace, varData.mangledName, pathSuffix, expr.data);
          } else if (expr.type.isString() || expr.type.isList() || expr.type.isStruct()) {
            ret += std::format("data modify storage {0}:global vars.{1}{2} set from storage {0}:global expr_str1\n", datapackNamespace, varData.mangledName, pathSuffix);
          } else if (expr.type.isFloat()) {
            ret += std::format("data modify storage {0}:global vars.{1}{2} set from storage {0}:global expr_float1\n", datapackNamespace, varData.mangledName, pathSuffix);
          } else {
            ret += std::format(
              "execute store result storage {0}:global vars.{1}{2} int 1 run scoreboard players get expr_output1 temp\n",
              datapackNamespace,
              varData.mangledName,
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
                ret +=
                  std::format("execute store result storage {0}:global macro_args.index_to_append int 1 run scoreboard players get expr_output2 temp\n", datapackNamespace);
              }
              useInternalFunction("internal_path_append");
              ret += std::format("function {0}:internal/loom/internal_path_append with storage {0}:global macro_args\n", datapackNamespace);
            }
          }

          ret += std::format("data modify storage {0}:global macro_args.var_name set value \"{1}\"\n", datapackNamespace, varData.mangledName);
          if (expr.precomputed) {
            useInternalFunction("internal_list_nested_set_value");
            ret += std::format(
              "data modify storage {0}:global macro_args.value set value {1}\nfunction {0}:internal/loom/internal_list_nested_set_value with storage {0}:global macro_args\n",
              datapackNamespace,
              expr.data
            );
          } else if (expr.type.isString() || expr.type.isList() || expr.type.isStruct()) {
            useInternalFunction("internal_list_nested_set_object");
            ret += std::format("function {0}:internal/loom/internal_list_nested_set_object with storage {0}:global macro_args\n", datapackNamespace);
          } else {
            useInternalFunction("internal_list_nested_set_primitive");
            ret += std::format("function {0}:internal/loom/internal_list_nested_set_primitive with storage {0}:global macro_args\n", datapackNamespace);
          }
        }
        continue;
      }

      const ExpressionData expr = compileExpression(expNode);

      if (varData.type.kind == Compiler::Type::Enum) {
        if (expr.type.kind != Compiler::Type::Enum || expr.type.enumRef != varData.type.enumRef) {
          throw std::runtime_error(formatError(expNode, "Assignment to enum variable requires a variant of the same enum: " + name));
        }
      } else {
        if (varData.type.isBoolean() && expr.type.isInteger()) {
          throw std::runtime_error(formatError(expNode, "Cannot assign an 'int' to 'bool' variable: " + name));
        }
        if (varData.type.isString() && !expr.type.isString()) {
          throw std::runtime_error(formatError(expNode, "Invalid type for 'string' variable: " + name));
        }
      }

      if (expr.precomputed) {
        if (varData.type.isString() || varData.type.isFloat() || varData.type.isList() || varData.type.isStruct()) {
          ret += std::format("data modify storage {0}:global vars.{1} set value {2}\n", datapackNamespace, varData.mangledName, expr.data);
        } else {
          ret += std::format("scoreboard players set {} vars {}\n", varData.mangledName, expr.data);
        }
      } else {
        if (std::string(ts_node_type(expNode)) == "binary_expression" && (varData.type.isInteger() || varData.type.isBoolean())) {
          const std::string_view op = getFieldText(expNode, "operator");

          if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
            TSNode leftNode = ts_node_child_by_field_name(expNode, "left", 4);
            TSNode rightNode = ts_node_child_by_field_name(expNode, "right", 5);

            if (std::string(ts_node_type(leftNode)) == "variable_ref") {
              auto leftIt = findInMap(vars, std::string(getFieldText(leftNode, "name")));
              if (leftIt != vars.end() && leftIt == varIt) {
                const ExpressionData rightExpr = compileExpression(rightNode, 1, false);
                ret += std::format("{}\nscoreboard players operation {} vars {}= expr_output1 temp\n", rightExpr.data, varData.mangledName, op);
                continue;
              }
            }
            if (std::string(ts_node_type(rightNode)) == "variable_ref") {
              auto rightIt = findInMap(vars, std::string(getFieldText(rightNode, "name")));
              if (rightIt != vars.end() && rightIt == varIt) {
                const ExpressionData leftExpr = compileExpression(leftNode, 1, false);
                ret += std::format("{}\nscoreboard players operation {} vars {}= expr_output1 temp\n", leftExpr.data, varData.mangledName, op);
                continue;
              }
            }
          }
        }

        if (varData.type.isString() || varData.type.isList() || varData.type.isStruct()) {
          ret += std::format("{0}\ndata modify storage {1}:global vars.{2} set from storage {1}:global expr_str1\n", expr.data, datapackNamespace, varData.mangledName);
        } else if (varData.type.isFloat()) {
          ret += std::format("{0}\ndata modify storage {1}:global vars.{2} set from storage {1}:global expr_float1\n", expr.data, datapackNamespace, varData.mangledName);
        } else {
          ret += std::format("{}\nscoreboard players operation {} vars = expr_output1 temp\n", expr.data, varData.mangledName);
        }
      }
      continue;
    }

    if (type == "function_call") {
      ExpressionData expr = compileExpression(child, 1, false);
      ret += expr.data + "\n";
      continue;
    }

    if (type == "return_statement") {
      TSNode exprNode = ts_node_named_child(child, 0);
      if (!ts_node_is_null(exprNode)) {
        ExpressionData expr = compileExpression(exprNode);
        if (expr.precomputed) {
          ret += std::format("return {}\n", expr.data);
        } else {
          ret += std::format("{}\nreturn run scoreboard players get expr_output1 temp\n", expr.data);
        }
      } else {
        ret += "return 0\n";
      }
      break;
    }

    if (type == "command_statement") {
      std::string cmdName = std::string(getNodeText(ts_node_named_child(child, 0)));
      std::vector<TSNode> args;
      std::vector<std::optional<Compiler::ExpressionData>> compiledArgs;
      bool requiresMacro = false;

      for (uint32_t j = 1; j < ts_node_named_child_count(child); j++) {
        TSNode argNode = ts_node_named_child(child, j);
        std::string argType = ts_node_type(argNode);

        if (argType == "command_arg" || argType == "integer") {
          args.push_back(argNode);
          compiledArgs.push_back(std::nullopt);
        } else if (argType == "interpolation") {
          args.push_back(argNode);

          TSNode exprNode = ts_node_child_by_field_name(argNode, "expression", 10);

          Compiler::ExpressionData expr = compileExpression(exprNode);
          compiledArgs.push_back(expr);

          if (!expr.precomputed) {
            requiresMacro = true;
          }
        }
      }

      std::optional<std::string> optimized = optimizeCommand(cmdName, args);
      if (optimized.has_value()) {
        ret += optimized.value() + "\n";
        continue;
      }

      auto getSpacingBetween = [&](TSNode prev, TSNode curr) -> std::string {
        uint32_t prevEnd = ts_node_end_byte(prev);
        uint32_t currStart = ts_node_start_byte(curr);
        if (currStart > prevEnd) {
          return std::string(source.substr(prevEnd, currStart - prevEnd));
        }
        return "";
      };

      TSNode cmdNameNode = ts_node_named_child(child, 0);

      if (!requiresMacro) {
        ret += cmdName;
        TSNode lastNode = cmdNameNode;
        for (size_t i = 0; i < args.size(); i++) {
          ret += getSpacingBetween(lastNode, args[i]);
          if (compiledArgs[i].has_value()) {
            ret += compiledArgs[i].value().data;
          } else {
            ret += std::string(getNodeText(args[i]));
          }
          lastNode = args[i];
        }
        ret += "\n";
        continue;
      }

      std::string macroBody = "$" + cmdName;
      std::string macroSetup = "";
      int macroVarId = 0;
      TSNode lastNode = cmdNameNode;

      for (size_t i = 0; i < args.size(); i++) {
        macroBody += getSpacingBetween(lastNode, args[i]);
        if (compiledArgs[i].has_value()) {
          Compiler::ExpressionData &expr = compiledArgs[i].value();

          if (expr.precomputed) {
            macroBody += expr.data;
          } else {
            macroBody += std::format("$(var_{})", macroVarId);
            macroSetup += expr.data + "\n";

            if (expr.type.isString() || expr.type.isList()) {
              macroSetup += std::format("data modify storage {0}:function_input var_{1} set from storage {0}:global expr_str1\n", datapackNamespace, macroVarId);
            } else if (expr.type.isFloat()) {
              macroSetup += std::format("data modify storage {0}:function_input var_{1} set from storage {0}:global expr_float1\n", datapackNamespace, macroVarId);
            } else {
              macroSetup +=
                std::format("execute store result storage {0}:function_input var_{1} int 1 run scoreboard players get expr_output1 temp\n", datapackNamespace, macroVarId);
            }
            macroVarId++;
          }
        } else {
          macroBody += std::string(getNodeText(args[i]));
        }
        lastNode = args[i];
      }

      const std::string macroFuncName = std::format("_generated_function_{}", currentGeneratedFunction++);
      compiledFunctions.push_back({.name = macroFuncName, .data = macroBody + "\n"});

      ret += macroSetup;
      ret += std::format("function {}:internal/{} with storage {}:function_input\n", datapackNamespace, macroFuncName, datapackNamespace);
      continue;
    }

    if (type != "comment") throw std::runtime_error(formatError(child, "Invalid block statement: " + type));
  }

  std::erase_if(vars, [&node](const auto &pair) { return ts_node_eq(pair.second.scope, node); });

  return ret;
}
