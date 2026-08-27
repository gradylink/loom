#pragma once

#include "ast.hpp"
#include "compiler.hpp"
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

class TypeHandler {
public:
  virtual ~TypeHandler() = default;

  virtual void registerBuiltins(Compiler &compiler) const {}

  virtual bool handles(const Compiler::Type &type) const = 0;

  virtual std::optional<Compiler::ExpressionData> compileBinaryOp(
    Compiler &compiler, std::string_view op, const Compiler::ExpressionData &left, const Compiler::ExpressionData &right, unsigned int id, bool precompute, SourceLoc loc
  ) const {
    return std::nullopt;
  }

  virtual std::optional<Compiler::ExpressionData>
  compileUnaryOp(Compiler &compiler, std::string_view op, const Compiler::ExpressionData &operand, unsigned int id, bool precompute, SourceLoc loc) const {
    return std::nullopt;
  }

  virtual std::optional<Compiler::ExpressionData>
  compileMemberExpression(Compiler &compiler, const Compiler::ExpressionData &object, std::string_view property, unsigned int id, bool precompute, SourceLoc loc) const {
    return std::nullopt;
  }

  virtual std::optional<Compiler::ExpressionData>
  compileCast(Compiler &compiler, const Compiler::ExpressionData &expr, const Compiler::Type &targetType, unsigned int id, bool precompute, SourceLoc loc) const {
    return std::nullopt;
  }
};

class TypeRegistry {
  std::vector<std::unique_ptr<TypeHandler>> handlers;

public:
  void registerHandler(Compiler &compiler, std::unique_ptr<TypeHandler> handler) {
    handlers.push_back(std::move(handler));
    handlers.back()->registerBuiltins(compiler);
  }

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
std::unique_ptr<TypeHandler> createFloatHandler();
std::unique_ptr<TypeHandler> createStructHandler();
std::unique_ptr<TypeHandler> createMapHandler();
