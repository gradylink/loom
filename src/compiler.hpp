#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <variant>
#include <vector>

class Compiler {
public:
  Compiler(const std::string_view &source, const std::string &datapackNamespace, std::filesystem::path currentDir = ".");
  ~Compiler();

  struct CompiledFunction {
    std::string name;
    std::string data;
    std::optional<std::string> tag;
  };

  std::vector<CompiledFunction> compile();

  std::string globalInit = setupScoreboards;

private:
  enum class EnumType { Integer, String };

  struct EnumVariant {
    std::string name;
    std::variant<int32_t, std::string> value;
  };

  struct EnumData {
    std::string name;
    EnumType type = EnumType::Integer;
    std::unordered_map<std::string, EnumVariant> variants;

    bool exported = false;
  };

  struct Type {
    enum Kind { Integer, Boolean, String, Enum, List } kind = Integer;
    const EnumData *enumRef = nullptr;

    std::unique_ptr<Type> baseType = nullptr;

    Type() = default;

    Type(Kind k, const EnumData *ref, std::unique_ptr<Type> base) : kind(k), enumRef(ref), baseType(std::move(base)) {}

    static Type IntegerType() { return {Integer, nullptr, nullptr}; }
    static Type BooleanType() { return {Boolean, nullptr, nullptr}; }
    static Type StringType() { return {String, nullptr, nullptr}; }
    static Type EnumTypeOf(const EnumData *ref) { return {Enum, ref, nullptr}; }
    static Type ListTypeOf(Type type) { return {List, nullptr, std::make_unique<Type>(std::move(type))}; }

    Type(const Type &o) : kind(o.kind), enumRef(o.enumRef), baseType(o.baseType ? std::make_unique<Type>(*o.baseType) : nullptr) {}

    Type &operator=(const Type &o) {
      if (this != &o) {
        kind = o.kind;
        enumRef = o.enumRef;
        baseType = o.baseType ? std::make_unique<Type>(*o.baseType) : nullptr;
      }
      return *this;
    }

    Type(Type &&) noexcept = default;
    Type &operator=(Type &&) noexcept = default;

    bool operator==(const Type &o) const {
      if (kind != o.kind || enumRef != o.enumRef) return false;
      if (baseType && o.baseType) return *baseType == *o.baseType;
      return baseType == o.baseType;
    }
    bool operator!=(const Type &o) const { return !(*this == o); }

    bool isInteger() const {
      if (kind == Integer) return true;
      if (kind == Enum && enumRef) return enumRef->type == EnumType::Integer;
      return false;
    }
    bool isBoolean() const { return kind == Boolean; }
    bool isString() const {
      if (kind == String) return true;
      if (kind == Enum && enumRef) return enumRef->type == EnumType::String;
      return false;
    }
    bool isList() const { return kind == List; }
  };
  struct FunctionData {
    std::string name;
    std::string mangledName;
    std::optional<Type> returnType;
    std::vector<Type> params;
    std::optional<std::string> tag;

    bool exported = false;
  };

  struct VariableData {
    std::string name;
    std::string mangledName;
    Type type;
    TSNode scope;
    std::optional<int32_t> value;

    bool constant = false;
    bool exported = false;
  };

  struct ExpressionData {
    std::string data;
    bool precomputed;
    Type type;
  };

  const std::string datapackNamespace;
  const std::string source;
  const std::filesystem::path currentDir;

  TSParser *parser;
  TSTree *tree;
  TSNode root;

  std::unordered_map<std::string, FunctionData> funcs;
  std::unordered_map<std::string, VariableData> vars;
  std::unordered_map<std::string, EnumData> enums;
  std::vector<CompiledFunction> compiledFunctions;

  unsigned int currentExpressionId = 0;
  unsigned int currentGeneratedFunction = 0;

  static constexpr const char *setupScoreboards = "scoreboard objectives add vars dummy\n"
                                                  "scoreboard objectives add temp dummy\n"
                                                  "scoreboard players set invert temp -1\n";

  std::string_view getNodeText(TSNode node);
  std::string_view getFieldText(TSNode node, const std::string &field);

  Type parseTypeFromString(const std::string &typeText) const;

  ExpressionData compileExpression(TSNode node, unsigned int id = 1, bool precompute = true);

  std::string compileIf(TSNode ifRoot);
  std::string compileWhile(TSNode whileNode);
  std::string compileDoWhile(TSNode doWhileNode);
  std::string compileFor(TSNode forNode);

  std::string compileBlock(TSNode node);
  std::optional<std::string> optimizeCommand(const std::string &commandName, const std::vector<TSNode> &args);
};
