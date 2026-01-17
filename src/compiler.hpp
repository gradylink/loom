#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <vector>

class Compiler {
public:
  Compiler(const std::string_view &source);
  ~Compiler();

  struct CompiledFunction {
    std::string data;
    std::string name;
  };
  std::vector<CompiledFunction> compile(const std::string_view &source);

private:
  enum class Type {
    Integer
  };

  enum class ReturnType {
    Integer,
    Void
  };

  struct FunctionData {
    std::string name;
    ReturnType returnType;
    TSNode scope;
  };

  struct VariableData {
    std::string name;
    std::string mangledName;
    Type type;
    TSNode scope;
    bool constant;
    std::optional<int32_t> value;
  };

  struct ExpressionData {
    std::string data;
    bool precomputed;
  };

  const std::string source;

  TSParser *parser;
  TSTree *tree;
  TSNode root;

  std::unordered_map<std::string, FunctionData> funcs;
  std::unordered_map<std::string, VariableData> vars;
  unsigned int currentExpressionId = 0;

  ExpressionData compileExpression(TSNode node, unsigned int id = 1, bool precompute = true);

  std::string_view getNodeText(TSNode node);
};
