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
    std::string name;
    std::string data;
    std::optional<std::string> tag;
  };

  std::vector<CompiledFunction> compile();

  std::string globalInit = setupScoreboards;

private:
  enum class Type { Integer, Boolean, String };
  enum class ReturnType { Integer, Boolean, String, Void };

  struct FunctionData {
    std::string name;
    ReturnType returnType;
    std::vector<Type> params;
    std::optional<std::string> tag;
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
    Type type;
  };

  const std::string source;

  TSParser *parser;
  TSTree *tree;
  TSNode root;

  std::unordered_map<std::string, FunctionData> funcs;
  std::unordered_map<std::string, VariableData> vars;
  std::vector<CompiledFunction> compiledFunctions;

  unsigned int currentExpressionId = 0;
  unsigned int currentGeneratedFunction = 0;

  static constexpr const char *setupScoreboards = "scoreboard objectives add vars dummy\n"
                                                  "scoreboard objectives add temp dummy\n"
                                                  "scoreboard players set invert temp -1\n";

  std::string_view getNodeText(TSNode node);
  std::string_view getFieldText(TSNode node, const std::string &field);

  ExpressionData compileExpression(TSNode node, unsigned int id = 1, bool precompute = true);

  std::string compileIf(TSNode ifRoot);
  std::string compileWhile(TSNode whileNode);
  std::string compileDoWhile(TSNode doWhileNode);
  std::string compileFor(TSNode forNode);

  std::string compileBlock(TSNode node);
  std::optional<std::string> optimizeCommand(const std::string &commandName, const std::vector<TSNode> &args);
};
