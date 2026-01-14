#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

class Compiler {
public:
  Compiler(const std::string_view source);

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
    Type type;
    TSNode scope;
    bool constant;
    std::optional<int32_t> value;
  };

  const std::string_view source;

  TSNode root;

  std::vector<FunctionData> funcs;
  std::vector<VariableData> vars;

  std::string compileExpression(TSNode root, uint8_t id);
};
