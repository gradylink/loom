#pragma once

#include "compiler.hpp"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class TypeHandler {
public:
  virtual ~TypeHandler() = default;

  virtual bool handles(const Compiler::Type &type) const = 0;

  virtual std::optional<Compiler::ExpressionData> compileBinaryOp(
    Compiler &compiler, std::string_view op, const Compiler::ExpressionData &left, const Compiler::ExpressionData &right, unsigned int id, bool precompute, TSNode node
  ) const {
    return std::nullopt;
  }

  virtual std::optional<Compiler::ExpressionData>
  compileUnaryOp(Compiler &compiler, std::string_view op, const Compiler::ExpressionData &operand, unsigned int id, bool precompute, TSNode node) const {
    return std::nullopt;
  }

  virtual std::optional<Compiler::ExpressionData>
  compileBuiltinFunction(Compiler &compiler, std::string_view funcName, const std::vector<TSNode> &argNodes, unsigned int id, bool precompute, TSNode node) const {
    return std::nullopt;
  }

  virtual std::optional<Compiler::ExpressionData>
  compileMemberExpression(Compiler &compiler, const Compiler::ExpressionData &object, std::string_view property, unsigned int id, bool precompute, TSNode node) const {
    return std::nullopt;
  }
};

class TypeRegistry {
  std::vector<std::unique_ptr<TypeHandler>> handlers;

public:
  void registerHandler(std::unique_ptr<TypeHandler> handler) { handlers.push_back(std::move(handler)); }

  const TypeHandler *findHandler(const Compiler::Type &type) const {
    for (const auto &h : handlers) {
      if (h->handles(type)) return h.get();
    }
    return nullptr;
  }
};

std::unique_ptr<TypeHandler> createIntegerHandler();
std::unique_ptr<TypeHandler> createBooleanHandler();
std::unique_ptr<TypeHandler> createStringHandler();
std::unique_ptr<TypeHandler> createListHandler();
std::unique_ptr<TypeHandler> createEnumHandler();
