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
      if (vars.find(name) == vars.end()) {
        throw std::runtime_error(formatError(child, "Assignment to undefined variable: " + name));
      }
      if (vars[name].constant) {
        throw std::runtime_error(formatError(child, "Cannot reassign constant variable: " + name));
      }

      TSNode expNode = ts_node_child_by_field_name(child, "value", 5);

      std::vector<TSNode> indexNodes;
      uint32_t namedCount = ts_node_named_child_count(child);
      for (uint32_t i = 1; i < namedCount - 1; i++) {
        indexNodes.push_back(ts_node_named_child(child, i));
      }
      const bool &isIndexed = !indexNodes.empty();

      if (isIndexed) {
        if (!vars[name].type.isList() && !vars[name].type.isString()) {
          throw std::runtime_error(formatError(child, "Cannot use index assignment on non-container variable: " + name));
        }

        const ExpressionData expr = compileExpression(expNode, 1, true);
        ret += expr.data + "\n";

        Compiler::Type expectedType = vars[name].type;
        bool endsInStringSubscript = false;

        if (expectedType.isString()) {
          if (indexNodes.size() != 1) {
            throw std::runtime_error(formatError(child, "Flat strings can only take a single index descriptor."));
          }
          endsInStringSubscript = true;
        } else {
          for (size_t i = 0; i < indexNodes.size(); i++) {
            if (!expectedType.isList()) {
              throw std::runtime_error(formatError(indexNodes[i], "Too many indices provided for this list depth."));
            }
            expectedType = *expectedType.baseType;
          }
          if (expectedType.isString()) {
            endsInStringSubscript = true;
          }
        }

        if (endsInStringSubscript) {
          if (!expr.type.isString()) {
            throw std::runtime_error(formatError(expNode, "Type mismatch: cannot assign non-string to a string character index."));
          }
        } else {
          if (expr.type != expectedType) {
            throw std::runtime_error(formatError(expNode, "Type mismatch: cannot assign value to this nested list element type."));
          }
        }

        std::vector<ExpressionData> compiledIndices;
        bool allIndicesPrecomputed = true;
        for (size_t i = 0; i < indexNodes.size(); i++) {
          ExpressionData idxExpr = compileExpression(indexNodes[i], 2, true);
          if (!idxExpr.type.isInteger()) {
            throw std::runtime_error(formatError(indexNodes[i], "Indices must evaluate to integers."));
          }
          if (!idxExpr.precomputed) {
            allIndicesPrecomputed = false;
          }
          compiledIndices.push_back(idxExpr);
        }

        std::string pathSuffix = "";
        size_t listDimensions = endsInStringSubscript ? indexNodes.size() - 1 : indexNodes.size();
        for (size_t i = 0; i < listDimensions; i++) {
          pathSuffix += "[" + compiledIndices[i].data + "]";
        }

        if (endsInStringSubscript) {
          ExpressionData stringCharIdx = compiledIndices.back();

          ret += std::format("data modify storage {0}:global macro_args.var_name set value \"{1}\"\n", datapackNamespace, vars[name].mangledName);

          if (expr.precomputed) {
            ret += std::format("data modify storage {0}:global macro_args.value set value {1}\n", datapackNamespace, expr.data);
          } else {
            ret += std::format("data modify storage {0}:global macro_args.value set from storage {0}:global expr_str1\n", datapackNamespace);
          }

          if (allIndicesPrecomputed) {
            ret += std::format("data modify storage {0}:global macro_args.path set value \"{1}\"\n", datapackNamespace, pathSuffix);
          } else {
            ret += std::format("data modify storage {0}:global macro_args.path set value \"\"\n", datapackNamespace);
            for (size_t i = 0; i < listDimensions; i++) {
              if (!compiledIndices[i].precomputed) ret += compiledIndices[i].data + "\n";
              ret += std::format("data modify storage {0}:global macro_args.string_before set from storage {0}:global macro_args.path\n", datapackNamespace);
              if (compiledIndices[i].precomputed) {
                ret += std::format("data modify storage {0}:global macro_args.index_to_append set value \"{1}\"\n", datapackNamespace, compiledIndices[i].data);
              } else {
                ret +=
                  std::format("execute store result storage {0}:global macro_args.index_to_append int 1 run scoreboard players get expr_output2 temp\n", datapackNamespace);
              }
              useInternalFunction("internal_path_append");
              ret += std::format("function {0}:internal/loom/internal_path_append with storage {0}:global macro_args\n", datapackNamespace);
            }
          }

          if (stringCharIdx.precomputed) {
            int idxVal = std::stoi(stringCharIdx.data);
            useInternalFunction("internal_string_mutate_static");
            ret += std::format(
              "data modify storage {0}:global macro_args.index set value {1}\n"
              "data modify storage {0}:global macro_args.index_plus_one set value {2}\n"
              "function {0}:internal/loom/internal_string_mutate_static with storage {0}:global macro_args\n",
              datapackNamespace,
              idxVal,
              idxVal + 1
            );
          } else {
            ret += stringCharIdx.data + "\n";
            useInternalFunction("internal_string_mutate_dynamic");
            ret += std::format(
              "execute store result storage {0}:global macro_args.index int 1 run scoreboard players get expr_output2 temp\n"
              "scoreboard players operation expr_output3 temp = expr_output2 temp\n"
              "scoreboard players add expr_output3 temp 1\n"
              "execute store result storage {0}:global macro_args.index_plus_one int 1 run scoreboard players get expr_output3 temp\n"
              "function {0}:internal/loom/internal_string_mutate_dynamic with storage {0}:global macro_args\n",
              datapackNamespace
            );
          }
          continue;
        }

        if (allIndicesPrecomputed) {
          if (expr.precomputed) {
            ret += std::format("data modify storage {0}:global vars.{1}{2} set value {3}\n", datapackNamespace, vars[name].mangledName, pathSuffix, expr.data);
          } else if (expr.type.isString() || expr.type.isList()) {
            ret += std::format("data modify storage {0}:global vars.{1}{2} set from storage {0}:global expr_str1\n", datapackNamespace, vars[name].mangledName, pathSuffix);
          } else {
            ret += std::format(
              "execute store result storage {0}:global vars.{1}{2} int 1 run scoreboard players get expr_output1 temp\n",
              datapackNamespace,
              vars[name].mangledName,
              pathSuffix
            );
          }
        } else {
          ret += std::format("data modify storage {0}:global macro_args.path set value \"\"\n", datapackNamespace);
          for (size_t i = 0; i < indexNodes.size(); i++) {
            if (!compiledIndices[i].precomputed) ret += compiledIndices[i].data + "\n";
            ret += std::format("data modify storage {0}:global macro_args.string_before set from storage {0}:global macro_args.path\n", datapackNamespace);
            if (compiledIndices[i].precomputed) {
              ret += std::format("data modify storage {0}:global macro_args.index_to_append set value \"{1}\"\n", datapackNamespace, compiledIndices[i].data);
            } else {
              ret += std::format("execute store result storage {0}:global macro_args.index_to_append int 1 run scoreboard players get expr_output2 temp\n", datapackNamespace);
            }
            useInternalFunction("internal_path_append");
            ret += std::format("function {0}:internal/loom/internal_path_append with storage {0}:global macro_args\n", datapackNamespace);
          }

          ret += std::format("data modify storage {0}:global macro_args.var_name set value \"{1}\"\n", datapackNamespace, vars[name].mangledName);
          if (expr.precomputed) {
            useInternalFunction("internal_list_nested_set_value");
            ret += std::format(
              "data modify storage {0}:global macro_args.value set value {1}\nfunction {0}:internal/loom/internal_list_nested_set_value with storage {0}:global macro_args\n",
              datapackNamespace,
              expr.data
            );
          } else if (expr.type.isString() || expr.type.isList()) {
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

      if (vars[name].type.kind == Compiler::Type::Enum) {
        if (expr.type.kind != Compiler::Type::Enum || expr.type.enumRef != vars[name].type.enumRef) {
          throw std::runtime_error(formatError(expNode, "Assignment to enum variable requires a variant of the same enum: " + name));
        }
      } else {
        if (vars[name].type.isBoolean() && expr.type.isInteger()) {
          throw std::runtime_error(formatError(expNode, "Cannot assign an 'int' to 'bool' variable: " + name));
        }
        if (vars[name].type.isString() && !expr.type.isString()) {
          throw std::runtime_error(formatError(expNode, "Invalid type for 'string' variable: " + name));
        }
      }

      if (expr.precomputed) {
        if (vars[name].type.isString() || vars[name].type.isFloat() || vars[name].type.isList()) {
          ret += std::format("data modify storage {0}:global vars.{1} set value {2}\n", datapackNamespace, vars[name].mangledName, expr.data);
        } else {
          ret += std::format("scoreboard players set {} vars {}\n", vars[name].mangledName, expr.data);
        }
      } else {
        if (std::string(ts_node_type(expNode)) == "binary_expression" && (vars[name].type.isInteger() || vars[name].type.isBoolean())) {
          const std::string_view op = getFieldText(expNode, "operator");

          if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
            TSNode leftNode = ts_node_child_by_field_name(expNode, "left", 4);
            TSNode rightNode = ts_node_child_by_field_name(expNode, "right", 5);

            if (std::string(ts_node_type(leftNode)) == "variable_ref" && std::string(getFieldText(leftNode, "name")) == name) {
              const ExpressionData rightExpr = compileExpression(rightNode, 1, false);
              ret += std::format("{}\nscoreboard players operation {} vars {}= expr_output1 temp\n", rightExpr.data, vars[name].mangledName, op);
              continue;
            }
            if (std::string(ts_node_type(rightNode)) == "variable_ref" && std::string(getFieldText(rightNode, "name")) == name) {
              const ExpressionData leftExpr = compileExpression(leftNode, 1, false);
              ret += std::format("{}\nscoreboard players operation {} vars {}= expr_output1 temp\n", leftExpr.data, vars[name].mangledName, op);
              continue;
            }
          }
        }

        if (vars[name].type.isString() || vars[name].type.isList()) {
          ret += std::format("{0}\ndata modify storage {1}:global vars.{2} set from storage {1}:global expr_str1\n", expr.data, datapackNamespace, vars[name].mangledName);
        } else if (vars[name].type.isFloat()) {
          ret += std::format("{0}\ndata modify storage {1}:global vars.{2} set from storage {1}:global expr_float1\n", expr.data, datapackNamespace, vars[name].mangledName);
        } else {
          ret += std::format("{}\nscoreboard players operation {} vars = expr_output1 temp\n", expr.data, vars[name].mangledName);
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
      ret += std::format("function {}:{} with storage {}:function_input\n", datapackNamespace, macroFuncName, datapackNamespace);
      continue;
    }

    if (type != "comment") throw std::runtime_error(formatError(child, "Invalid block statement: " + type));
  }

  std::erase_if(vars, [&node](const auto &pair) { return ts_node_eq(pair.second.scope, node); });

  return ret;
}
