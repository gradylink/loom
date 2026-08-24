#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

class TypeHandler;
class TypeRegistry;

class Compiler {
  friend class TypeHandler;

public:
  Compiler(const std::string_view &source, const std::string &datapackNamespace, std::filesystem::path currentDir = ".");
  ~Compiler();

  struct CompiledFunction {
    std::string name;
    std::string data;
    std::optional<std::string> tag;
    bool internal = true;
  };

  std::vector<CompiledFunction> compile();

  // The abritrary numbers here are "loom" obfuscated in different ways.
  // X coord: Math.floor(Math.pow(asciiSum("loom"), 2.75))
  // Z coord: Math.floor(asciiProduct("loom") / 10) // asciiProduct starts with 1
  // UUID: first section is "loom" in ASCII
  std::string globalInit = std::string(setupScoreboards) +
                           "forceload add 18483211 14504281\n"
                           "execute unless entity 6c6f6f6d-0-0-0-ffff run summon item_display 18483211 0 14504281 {UUID:[I;1819242349,0,0,65535]}\n"
                           "setblock 18483211 -64 14504281 chest{Items:[{id:stick}]} replace\n";

public:
  enum class EnumType { Integer, String, Float };

  struct EnumVariant {
    std::string name;
    std::variant<int32_t, std::string, float> value;
  };

  struct EnumData {
    std::string name;
    EnumType type = EnumType::Integer;
    std::unordered_map<std::string, EnumVariant> variants;

    bool exported = false;
  };

  struct StructData;

  struct Type {
    enum Kind { Integer, Boolean, String, Enum, List, Float, Struct, Reference } kind = Integer;
    const EnumData *enumRef = nullptr;
    const StructData *structRef = nullptr;
    bool isReference = false;

    std::unique_ptr<Type> baseType = nullptr;

    Type() = default;

    Type(Kind k, const EnumData *eRef, const StructData *sRef, std::unique_ptr<Type> base) : kind(k), enumRef(eRef), structRef(sRef), baseType(std::move(base)) {}

    static Type IntegerType() { return {Integer, nullptr, nullptr, nullptr}; }
    static Type FloatType() { return {Float, nullptr, nullptr, nullptr}; }
    static Type BooleanType() { return {Boolean, nullptr, nullptr, nullptr}; }
    static Type StringType() { return {String, nullptr, nullptr, nullptr}; }
    static Type EnumTypeOf(const EnumData *ref) { return {Enum, ref, nullptr, nullptr}; }
    static Type StructTypeOf(const StructData *ref) { return {Struct, nullptr, ref, nullptr}; }
    static Type ListTypeOf(Type type) { return {List, nullptr, nullptr, std::make_unique<Type>(std::move(type))}; }
    static Type RefTypeOf(Type type) { return {Reference, nullptr, nullptr, std::make_unique<Type>(std::move(type))}; }

    Type(const Type &o) : kind(o.kind), enumRef(o.enumRef), structRef(o.structRef), baseType(o.baseType ? std::make_unique<Type>(*o.baseType) : nullptr) {}

    Type &operator=(const Type &o) {
      if (this != &o) {
        kind = o.kind;
        enumRef = o.enumRef;
        structRef = o.structRef;
        baseType = o.baseType ? std::make_unique<Type>(*o.baseType) : nullptr;
      }
      return *this;
    }

    Type(Type &&) noexcept = default;
    Type &operator=(Type &&) noexcept = default;

    bool operator==(const Type &o) const {
      if (kind != o.kind || enumRef != o.enumRef || structRef != o.structRef) return false;
      if (baseType && o.baseType) return *baseType == *o.baseType;
      return baseType == o.baseType;
    }
    bool operator!=(const Type &o) const { return !(*this == o); }

    bool isInteger() const {
      if (kind == Integer) return true;
      if (kind == Enum && enumRef) return enumRef->type == EnumType::Integer;
      return false;
    }
    bool isFloat() const {
      if (kind == Float) return true;
      if (kind == Enum && enumRef) return enumRef->type == EnumType::Float;
      return false;
    }
    bool isBoolean() const { return kind == Boolean; }
    bool isString() const {
      if (kind == String) return true;
      if (kind == Enum && enumRef) return enumRef->type == EnumType::String;
      return false;
    }
    bool isList() const { return kind == List; }
    bool isStruct() const { return kind == Struct; }
    bool isRef() const { return kind == Reference; }

    const Type &deref() const { return isRef() ? *baseType : *this; }
  };

  struct StructField {
    std::string name;
    std::unique_ptr<Type> type;

    StructField() = default;

    StructField(std::string n, std::unique_ptr<Type> t) : name(std::move(n)), type(std::move(t)) {}

    StructField(const StructField &o) : name(o.name), type(o.type ? std::make_unique<Type>(*o.type) : nullptr) {}

    StructField &operator=(const StructField &o) {
      if (this != &o) {
        name = o.name;
        type = o.type ? std::make_unique<Type>(*o.type) : nullptr;
      }
      return *this;
    }

    StructField(StructField &&) noexcept = default;
    StructField &operator=(StructField &&) noexcept = default;
  };

  struct StructData {
    std::string name;
    std::vector<StructField> fields;
    bool exported = false;
  };

  struct FunctionData {
    std::string name;
    std::string mangledName;
    std::optional<Type> returnType;
    std::vector<Type> params;
    std::optional<std::string> tag;

    bool exported = false;
    bool internal = true;
  };

  struct VariableData {
    std::string name;
    std::string mangledName;
    Type type;
    TSNode scope;
    std::optional<std::string> value;

    bool constant = false;
    bool exported = false;

    std::optional<std::string> refTargetMangledName = std::nullopt;

    std::string getStorageName() const { return refTargetMangledName.value_or(mangledName); }
  };

  struct ExpressionData {
    std::string data;
    bool precomputed;
    Type type;
    std::optional<std::string> branchCondition = std::nullopt;
  };

  struct InternalFunction {
    std::string name;
    std::string data;
    bool used = false;
  };
  std::vector<InternalFunction> internalFunctions;
  inline void useInternalFunction(const std::string &name) {
    for (auto &func : internalFunctions) {
      if (func.name == name) {
        func.used = true;
        return;
      }
    }

    throw std::runtime_error("Failed to find internal function: " + name);
  }

  using BuiltinCompileCallback =
    std::function<std::optional<ExpressionData>(Compiler &compiler, const std::vector<TSNode> &argNodes, unsigned int id, bool precompute, TSNode node)>;
  void registerBuiltin(const std::string &name, BuiltinCompileCallback callback);
  bool isBuiltin(const std::string &name) const { return builtins.count(name) > 0; }

  static std::unordered_set<std::string> globalExternVars;

private:
  const std::string datapackNamespace;
  const std::string source;
  const std::filesystem::path currentDir;

  TSParser *parser;
  TSTree *tree;
  TSNode root;

  std::unordered_map<std::string, std::vector<FunctionData>> funcs;
  std::unordered_map<std::string, VariableData> vars;
  std::unordered_map<std::string, EnumData> enums;
  std::unordered_map<std::string, StructData> structs;
  std::vector<CompiledFunction> compiledFunctions;

  std::unordered_map<std::string, BuiltinCompileCallback> builtins;

  unsigned int currentExpressionId = 0;

  std::string currentNamespacePrefix = "";

  std::string currentFuncRefCopybacks = "";
  int controlFlowDepth = 0;

  void processDeclarations(TSNode parentNode);
  void processCompilation(TSNode parentNode);

public:
  std::string prefixName(const std::string &name) const {
    if (currentNamespacePrefix.empty()) return name;
    return currentNamespacePrefix + "::" + name;
  }

  template <typename T>
  typename std::unordered_map<std::string, T>::const_iterator findInMap(const std::unordered_map<std::string, T> &map, const std::string &name, bool toLower = false) const {
    std::string searchName = name;
    if (toLower) {
      std::transform(searchName.begin(), searchName.end(), searchName.begin(), ::tolower);
    }
    if (searchName.starts_with("::")) {
      return map.find(searchName.substr(2));
    }

    std::vector<std::string> parts;
    if (!currentNamespacePrefix.empty()) {
      size_t pos = 0;
      std::string s = currentNamespacePrefix;
      while ((pos = s.find("::")) != std::string::npos) {
        parts.push_back(s.substr(0, pos));
        s.erase(0, pos + 2);
      }
      parts.push_back(s);
    }

    for (int i = (int)parts.size(); i >= 0; --i) {
      std::string candidate = "";
      for (int j = 0; j < i; ++j) {
        candidate += parts[j] + "::";
      }
      candidate += searchName;
      auto it = map.find(candidate);
      if (it != map.end()) return it;
    }
    return map.end();
  }

  template <typename T> std::string resolveSymbolName(const std::unordered_map<std::string, T> &map, const std::string &name, bool toLower = false) const {
    auto it = findInMap(map, name, toLower);
    if (it != map.end()) return it->first;
    std::string searchName = name;
    if (toLower) {
      std::transform(searchName.begin(), searchName.end(), searchName.begin(), ::tolower);
    }
    return prefixName(searchName);
  }

public:
  std::unique_ptr<TypeRegistry> typeRegistry;
  void registerDefaultTypeHandlers();
  TypeHandler *getHandler(const Type &type);

  const std::string &getDatapackNamespace() const { return datapackNamespace; }

  void addCompiledFunction(const CompiledFunction &func) { compiledFunctions.push_back(func); }

  static constexpr const char *setupScoreboards = "scoreboard objectives add vars dummy\n"
                                                  "scoreboard objectives add temp dummy\n"
                                                  "scoreboard players set invert temp -1\n";

  std::string_view getNodeText(TSNode node);
  std::string_view getFieldText(TSNode node, const std::string &field);

  std::optional<VariableData> lookupVariable(const std::string &name) const;

  Type parseTypeFromString(const std::string &typeText) const;

  ExpressionData compileExpression(TSNode node, unsigned int id = 1, bool precompute = true);
  ExpressionData compileExpressionImpl(TSNode node, unsigned int id = 1, bool precompute = true);

  std::string compileVariableDeclaration(TSNode child, TSNode scope, bool isGlobal);

  std::string compileIf(TSNode ifRoot);
  std::string compileWhile(TSNode whileNode);
  std::string compileDoWhile(TSNode doWhileNode);
  std::string compileFor(TSNode forNode);

  std::string compileBlock(TSNode node);
  std::optional<std::string> optimizeCommand(const std::string &commandName, const std::vector<TSNode> &args);
};
