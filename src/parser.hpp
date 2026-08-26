#pragma once

#include "ast.hpp"
#include "lexer.hpp"
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct ParseError : std::runtime_error {
  SourceLoc loc;
  std::string rawMessage;
  ParseError(SourceLoc loc, const std::string &formatted, std::string rawMessage) : std::runtime_error(formatted), loc(loc), rawMessage(std::move(rawMessage)) {}
};

struct ParseDiagnostic {
  SourceLoc loc;
  std::string message;
};

class Parser {
public:
  Parser(std::string_view source, std::vector<Token> tokens);

  std::unique_ptr<Expr> parseExpression();

  std::unique_ptr<Block> parseProgram();

  void enableErrorRecovery() { recoverFromErrors = true; }
  const std::vector<ParseDiagnostic> &getDiagnostics() const { return diagnostics; }

  const Token &peek(size_t ahead = 0) const;
  const Token &previous() const;
  const Token &advance();
  bool check(TokenKind kind) const;
  bool checkIdentifierText(std::string_view text) const;
  bool match(TokenKind kind);
  const Token &expect(TokenKind kind, const std::string &what);
  [[noreturn]] void error(const Token &at, const std::string &message) const;

  std::string parseNamespacedIdentifier();

private:
  std::string_view source;
  std::vector<Token> tokens;
  size_t pos = 0;

  bool recoverFromErrors = false;
  std::vector<ParseDiagnostic> diagnostics;
  std::unique_ptr<Stmt> parseStatementRecovering();
  void synchronize();

  SourceLoc locOf(const Token &tok) const;
  std::unique_ptr<Expr> makeExpr(const Token &startTok, decltype(Expr::data) data);

  std::string parseTypeText();
  std::string parseTypeText(SourceLoc &outLoc);
  std::string parseSelectorText();
  std::string parseNamespacedArgText();
  std::string parseImportPathText();
  std::string parseVecText(int n);

  std::unique_ptr<Block> parseBlock();
  std::unique_ptr<Stmt> parseStatement();
  std::unique_ptr<Stmt> makeStmt(const Token &startTok, decltype(Stmt::data) data);

  std::unique_ptr<Stmt> parseIf();
  std::unique_ptr<Stmt> parseWhile();
  std::unique_ptr<Stmt> parseDoWhile();
  std::unique_ptr<Stmt> parseFor();
  std::unique_ptr<Stmt> parseVarDecl(bool isExport, bool isExtern);
  std::unique_ptr<Stmt> parseFuncDecl(std::optional<std::string> tag, bool isExport, bool isExtern);
  std::unique_ptr<Stmt> parseStructDecl(bool isExport);
  std::unique_ptr<Stmt> parseEnumDecl(bool isExport);
  std::unique_ptr<Stmt> parseNamespaceDecl();
  std::unique_ptr<Stmt> parseImportDecl();
  std::unique_ptr<Stmt> parseReturnStmt();
  std::unique_ptr<Stmt> parseContextStmt();
  std::unique_ptr<Stmt> parseCommandStmt();

  std::unique_ptr<Stmt> tryParseAssignOrCallStmt();

  bool atStatementEnd() const;
  void consumeStatementTerminator();

  std::unique_ptr<Expr> parseTernary();
  std::unique_ptr<Expr> parseLogicalOr();
  std::unique_ptr<Expr> parseLogicalAnd();
  std::unique_ptr<Expr> parseEquality();
  std::unique_ptr<Expr> parseComparison();
  std::unique_ptr<Expr> parseAdditive();
  std::unique_ptr<Expr> parseMultiplicative();
  std::unique_ptr<Expr> parseUnary();
  std::unique_ptr<Expr> parseCast();
  std::unique_ptr<Expr> parsePostfix();
  std::unique_ptr<Expr> parsePostfixContinuation(std::unique_ptr<Expr> expr);
  std::unique_ptr<Expr> parsePrimary();
  std::unique_ptr<Expr> tryParseAtTest();
};

std::string exprToString(const Expr &expr);
