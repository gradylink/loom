#include "parser.hpp"

#include <cctype>
#include <format>

static std::string formatParseError(SourceLoc loc, const std::string &message) { return std::format("line {}, col {}: {}", loc.line, loc.col, message); }

Parser::Parser(std::string_view source, std::vector<Token> tokens) : source(source), tokens(std::move(tokens)) {}

const Token &Parser::peek(size_t ahead) const {
  size_t p = pos + ahead;
  if (p >= tokens.size()) return tokens.back();
  return tokens[p];
}

const Token &Parser::previous() const { return tokens[pos - 1]; }

const Token &Parser::advance() {
  if (pos < tokens.size() - 1) pos++;
  return previous();
}

bool Parser::check(TokenKind kind) const { return peek().kind == kind; }

bool Parser::checkIdentifierText(std::string_view text) const { return peek().kind == TokenKind::Identifier && peek().text == text; }

bool Parser::match(TokenKind kind) {
  if (!check(kind)) return false;
  advance();
  return true;
}

const Token &Parser::expect(TokenKind kind, const std::string &what) {
  if (!check(kind)) error(peek(), std::format("Expected {}, found '{}'", what, peek().text.empty() ? tokenKindName(peek().kind) : std::string(peek().text)));
  return advance();
}

void Parser::error(const Token &at, const std::string &message) const { throw ParseError(locOf(at), formatParseError(locOf(at), message), message); }

SourceLoc Parser::locOf(const Token &tok) const { return SourceLoc{.line = tok.line, .col = tok.col, .startByte = tok.startByte, .endByte = tok.endByte}; }

std::unique_ptr<Expr> Parser::makeExpr(const Token &startTok, decltype(Expr::data) data) {
  auto e = std::make_unique<Expr>();
  e->loc = locOf(startTok);
  e->data = std::move(data);
  return e;
}

std::string Parser::parseNamespacedIdentifier() {
  std::string result(expect(TokenKind::Identifier, "identifier").text);
  while (check(TokenKind::ColonColon)) {
    advance();
    result += "::";
    result += expect(TokenKind::Identifier, "identifier after '::'").text;
  }
  return result;
}

static void parseTypeTextInner(Parser &self);

std::string Parser::parseTypeText() {
  SourceLoc loc;
  return parseTypeText(loc);
}

std::string Parser::parseTypeText(SourceLoc &outLoc) {
  const Token &startTok = peek();
  parseTypeTextInner(*this);
  while (check(TokenKind::LBracket)) {
    advance();
    expect(TokenKind::RBracket, "']' to close list type");
  }
  outLoc = locOf(startTok);
  outLoc.endByte = previous().endByte;
  return std::string(source.substr(startTok.startByte, outLoc.endByte - startTok.startByte));
}

static void parseTypeTextInner(Parser &self) {
  if (self.check(TokenKind::Amp)) {
    self.advance();
    parseTypeTextInner(self);
    return;
  }
  if (self.check(TokenKind::LParen)) {
    self.advance();
    parseTypeTextInner(self);
    self.expect(TokenKind::RParen, "')' to close type");
    return;
  }
  self.parseNamespacedIdentifier();
}

std::string Parser::parseSelectorText() {
  uint32_t startByte = peek().startByte;
  uint32_t endByte = startByte;

  if (check(TokenKind::At)) {
    advance();
    if (!check(TokenKind::Identifier)) error(peek(), "Expected a selector type after '@' (e.g. @e, @s)");
    advance();
    endByte = previous().endByte;

    if (check(TokenKind::LBracket)) {
      size_t i = peek().startByte;
      int depth = 0;
      do {
        char c = source[i];
        if (c == '"' || c == '\'') {
          char quote = c;
          i++;
          while (i < source.size() && source[i] != quote) {
            if (source[i] == '\\' && i + 1 < source.size()) i++;
            i++;
          }
        } else if (c == '[' || c == '{') {
          depth++;
        } else if (c == ']' || c == '}') {
          depth--;
        }
        i++;
      } while (depth > 0 && i < source.size());

      endByte = static_cast<uint32_t>(i);
      while (pos < tokens.size() - 1 && tokens[pos].startByte < endByte) pos++;
    }
  } else if (check(TokenKind::Identifier) || check(TokenKind::IntegerLit)) {
    advance();
    endByte = previous().endByte;
  } else {
    error(peek(), "Expected a selector");
  }

  return std::string(source.substr(startByte, endByte - startByte));
}

static bool isCoordToken(TokenKind k) {
  return k == TokenKind::Tilde || k == TokenKind::Caret || k == TokenKind::Minus || k == TokenKind::IntegerLit || k == TokenKind::FloatLit;
}

std::unique_ptr<Expr> Parser::tryParseAtTest() {
  if (!check(TokenKind::Identifier)) return nullptr;

  size_t save = pos;
  const Token &startTok = peek();
  uint32_t blockEndByte = peek().endByte;
  advance();

  if (check(TokenKind::Colon) && peek(1).kind == TokenKind::Identifier) {
    advance();
    advance();
    blockEndByte = previous().endByte;
  }

  if (!checkIdentifierText("at")) {
    pos = save;
    return nullptr;
  }
  advance();

  std::string blockText(source.substr(startTok.startByte, blockEndByte - startTok.startByte));

  uint32_t posStart = peek().startByte;
  for (int i = 0; i < 3; i++) {
    if (!isCoordToken(peek().kind)) error(peek(), "Expected a coordinate (e.g. ~, ~1, ^-2, 5) in block-test position");
    uint32_t runEnd = peek().endByte;
    advance();
    while (isCoordToken(peek().kind) && peek().startByte == runEnd) {
      runEnd = peek().endByte;
      advance();
    }
  }
  std::string posText(source.substr(posStart, previous().endByte - posStart));

  return makeExpr(startTok, AtTestExpr{.blockText = std::move(blockText), .posText = std::move(posText)});
}

std::unique_ptr<Expr> Parser::parseExpression() { return parseTernary(); }

std::unique_ptr<Expr> Parser::parseTernary() {
  auto cond = parseLogicalOr();
  if (check(TokenKind::Question)) {
    const Token &startTok = peek();
    advance();
    auto ifTrue = parseExpression();
    expect(TokenKind::Colon, "':' in ternary expression");
    auto ifFalse = parseExpression();
    return makeExpr(startTok, TernaryExpr{.condition = std::move(cond), .ifTrue = std::move(ifTrue), .ifFalse = std::move(ifFalse)});
  }
  return cond;
}

std::unique_ptr<Expr> Parser::parseLogicalOr() {
  auto left = parseLogicalAnd();
  while (check(TokenKind::PipePipe)) {
    const Token &opTok = peek();
    advance();
    auto right = parseLogicalAnd();
    left = makeExpr(opTok, BinaryExpr{.left = std::move(left), .op = "||", .right = std::move(right)});
  }
  return left;
}

std::unique_ptr<Expr> Parser::parseLogicalAnd() {
  auto left = parseEquality();
  while (check(TokenKind::AmpAmp)) {
    const Token &opTok = peek();
    advance();
    auto right = parseEquality();
    left = makeExpr(opTok, BinaryExpr{.left = std::move(left), .op = "&&", .right = std::move(right)});
  }
  return left;
}

std::unique_ptr<Expr> Parser::parseEquality() {
  auto left = parseComparison();
  while (check(TokenKind::EqEq) || check(TokenKind::BangEq)) {
    const Token &opTok = peek();
    std::string op(opTok.text);
    advance();
    auto right = parseComparison();
    left = makeExpr(opTok, BinaryExpr{.left = std::move(left), .op = op, .right = std::move(right)});
  }
  return left;
}

std::unique_ptr<Expr> Parser::parseComparison() {
  auto left = parseAdditive();
  while (check(TokenKind::Lt) || check(TokenKind::Gt) || check(TokenKind::LtEq) || check(TokenKind::GtEq)) {
    const Token &opTok = peek();
    std::string op(opTok.text);
    advance();
    auto right = parseAdditive();
    left = makeExpr(opTok, BinaryExpr{.left = std::move(left), .op = op, .right = std::move(right)});
  }
  return left;
}

std::unique_ptr<Expr> Parser::parseAdditive() {
  auto left = parseMultiplicative();
  while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
    const Token &opTok = peek();
    std::string op(opTok.text);
    advance();
    auto right = parseMultiplicative();
    left = makeExpr(opTok, BinaryExpr{.left = std::move(left), .op = op, .right = std::move(right)});
  }
  return left;
}

std::unique_ptr<Expr> Parser::parseMultiplicative() {
  auto left = parseUnary();
  while (check(TokenKind::Star) || check(TokenKind::Slash) || check(TokenKind::Percent)) {
    const Token &opTok = peek();
    std::string op(opTok.text);
    advance();
    auto right = parseUnary();
    left = makeExpr(opTok, BinaryExpr{.left = std::move(left), .op = op, .right = std::move(right)});
  }
  return left;
}

std::unique_ptr<Expr> Parser::parseUnary() {
  if (check(TokenKind::Minus) || check(TokenKind::Bang)) {
    const Token &opTok = peek();
    std::string op(opTok.text);
    advance();
    auto operand = parseUnary();
    return makeExpr(opTok, UnaryExpr{.op = op, .operand = std::move(operand)});
  }
  if (checkIdentifierText("entity")) {
    const Token &startTok = peek();
    advance();
    std::string selector = parseSelectorText();
    return makeExpr(startTok, EntityTestExpr{.selectorText = std::move(selector)});
  }
  return parseCast();
}

std::unique_ptr<Expr> Parser::parseCast() {
  auto expr = parsePostfix();
  while (check(TokenKind::KwAs)) {
    const Token &opTok = peek();
    advance();
    SourceLoc typeLoc;
    std::string typeText = parseTypeText(typeLoc);
    expr = makeExpr(opTok, CastExpr{.expression = std::move(expr), .typeText = std::move(typeText), .typeLoc = typeLoc});
  }
  return expr;
}

template <typename ParseItem> static void parseCommaSeparated(Parser &self, TokenKind closeKind, bool allowNewlines, ParseItem parseItem) {
  if (allowNewlines) {
    while (self.check(TokenKind::Newline)) self.advance();
  }
  if (self.check(closeKind)) return;

  parseItem();
  while (self.check(TokenKind::Comma)) {
    self.advance();
    if (allowNewlines) {
      while (self.check(TokenKind::Newline)) self.advance();
    }
    parseItem();
  }
  if (allowNewlines) {
    while (self.check(TokenKind::Newline)) self.advance();
  }
}

std::unique_ptr<Expr> Parser::parsePostfix() { return parsePostfixContinuation(parsePrimary()); }

std::unique_ptr<Expr> Parser::parsePostfixContinuation(std::unique_ptr<Expr> expr) {
  while (true) {
    if (check(TokenKind::Dot)) {
      const Token &opTok = peek();
      advance();
      const Token &propTok = expect(TokenKind::Identifier, "property name after '.'");
      std::string property(propTok.text);
      SourceLoc propertyLoc = locOf(propTok);
      if (check(TokenKind::LParen)) {
        advance();
        std::vector<std::unique_ptr<Expr>> args;
        parseCommaSeparated(*this, TokenKind::RParen, false, [&] { args.push_back(parseExpression()); });
        expect(TokenKind::RParen, "')' to close method call arguments");
        expr = makeExpr(opTok, MethodCallExpr{.object = std::move(expr), .method = std::move(property), .methodLoc = propertyLoc, .arguments = std::move(args)});
      } else {
        expr = makeExpr(opTok, MemberExpr{.object = std::move(expr), .property = std::move(property), .propertyLoc = propertyLoc});
      }
      continue;
    }
    if (check(TokenKind::LBracket)) {
      const Token &opTok = peek();
      advance();
      auto first = parseExpression();
      if (match(TokenKind::DotDot)) {
        auto end = parseExpression();
        expect(TokenKind::RBracket, "']' to close slice");
        expr = makeExpr(opTok, SliceExpr{.target = std::move(expr), .start = std::move(first), .end = std::move(end)});
      } else {
        expect(TokenKind::RBracket, "']' to close index");
        expr = makeExpr(opTok, ElementExpr{.target = std::move(expr), .index = std::move(first)});
      }
      continue;
    }
    break;
  }
  return expr;
}

std::unique_ptr<Expr> Parser::parsePrimary() {
  if (auto atTest = tryParseAtTest()) return atTest;

  const Token &tok = peek();

  switch (tok.kind) {
  case TokenKind::IntegerLit: {
    advance();
    return makeExpr(tok, IntLit{.text = std::string(tok.text)});
  }
  case TokenKind::FloatLit: {
    advance();
    return makeExpr(tok, FloatLit{.text = std::string(tok.text)});
  }
  case TokenKind::KwTrue: {
    advance();
    return makeExpr(tok, BoolLit{.value = true});
  }
  case TokenKind::KwFalse: {
    advance();
    return makeExpr(tok, BoolLit{.value = false});
  }
  case TokenKind::StringLit: {
    advance();
    return makeExpr(tok, StringLit{.text = std::string(tok.text)});
  }
  case TokenKind::Amp: {
    advance();
    std::string name = parseNamespacedIdentifier();
    return makeExpr(tok, ReferenceExpr{.targetName = std::move(name)});
  }
  case TokenKind::LParen: {
    advance();
    auto inner = parseExpression();
    expect(TokenKind::RParen, "')' to close parenthesized expression");
    return inner;
  }
  case TokenKind::LBracket: {
    advance();
    std::vector<std::unique_ptr<Expr>> elements;
    parseCommaSeparated(*this, TokenKind::RBracket, false, [&] { elements.push_back(parseExpression()); });
    expect(TokenKind::RBracket, "']' to close list");
    return makeExpr(tok, ListExpr{.elements = std::move(elements)});
  }
  case TokenKind::Identifier: {
    std::string name = parseNamespacedIdentifier();
    SourceLoc nameLoc = locOf(tok);
    nameLoc.endByte = previous().endByte;

    if (check(TokenKind::LParen)) {
      advance();
      std::vector<std::unique_ptr<Expr>> args;
      parseCommaSeparated(*this, TokenKind::RParen, false, [&] { args.push_back(parseExpression()); });
      expect(TokenKind::RParen, "')' to close call arguments");
      return makeExpr(tok, CallExpr{.name = std::move(name), .nameLoc = nameLoc, .arguments = std::move(args)});
    }

    if (check(TokenKind::LBrace)) {
      advance();
      std::vector<StructExprField> fields;
      parseCommaSeparated(*this, TokenKind::RBrace, true, [&] {
        std::string fieldName(expect(TokenKind::Identifier, "field name").text);
        expect(TokenKind::Colon, "':' after field name");
        auto value = parseExpression();
        fields.push_back(StructExprField{.name = std::move(fieldName), .value = std::move(value)});
      });
      expect(TokenKind::RBrace, "'}' to close struct literal");
      return makeExpr(tok, StructExpr{.name = std::move(name), .fields = std::move(fields)});
    }

    return makeExpr(tok, VarRefExpr{.name = std::move(name)});
  }
  default:
    error(tok, std::format("Expected an expression, found '{}'", tok.text.empty() ? tokenKindName(tok.kind) : std::string(tok.text)));
  }
}

std::string Parser::parseNamespacedArgText() {
  const Token &first = expect(TokenKind::Identifier, "identifier");
  uint32_t endByte = first.endByte;
  if (check(TokenKind::Colon) && peek(1).kind == TokenKind::Identifier) {
    advance();
    advance();
    endByte = previous().endByte;
  }
  return std::string(source.substr(first.startByte, endByte - first.startByte));
}

std::string Parser::parseImportPathText() {
  uint32_t startByte = peek().startByte;
  size_t i = startByte;
  auto isPathChar = [](char c) { return static_cast<bool>(std::isalnum(static_cast<unsigned char>(c))) || c == '.' || c == '/' || c == '_' || c == '-'; };
  while (i < source.size() && isPathChar(source[i])) i++;
  if (i == startByte) error(peek(), "Expected an import path (e.g. \"./foo.loom\")");
  uint32_t endByte = static_cast<uint32_t>(i);
  while (pos < tokens.size() - 1 && tokens[pos].startByte < endByte) pos++;
  return std::string(source.substr(startByte, endByte - startByte));
}

std::string Parser::parseVecText(int n) {
  uint32_t startByte = peek().startByte;
  for (int i = 0; i < n; i++) {
    if (!isCoordToken(peek().kind)) error(peek(), "Expected a coordinate component (e.g. ~, ~1, ^-2, 5)");
    uint32_t runEnd = peek().endByte;
    advance();
    while (isCoordToken(peek().kind) && peek().startByte == runEnd) {
      runEnd = peek().endByte;
      advance();
    }
  }
  return std::string(source.substr(startByte, previous().endByte - startByte));
}

bool Parser::atStatementEnd() const { return check(TokenKind::Semicolon) || check(TokenKind::Newline) || check(TokenKind::EndOfFile); }

void Parser::consumeStatementTerminator() {
  if (check(TokenKind::Semicolon) || check(TokenKind::Newline)) {
    advance();
    return;
  }
  if (check(TokenKind::EndOfFile)) return;
  error(peek(), std::format("Expected ';' or a newline to end the statement, found '{}'", peek().text.empty() ? tokenKindName(peek().kind) : std::string(peek().text)));
}

std::unique_ptr<Stmt> Parser::makeStmt(const Token &startTok, decltype(Stmt::data) data) {
  auto s = std::make_unique<Stmt>();
  s->loc = locOf(startTok);
  s->data = std::move(data);
  return s;
}

std::unique_ptr<Stmt> Parser::parseStatementRecovering() {
  try {
    return parseStatement();
  } catch (const ParseError &e) {
    diagnostics.push_back({.loc = e.loc, .message = e.rawMessage});
    synchronize();
    return nullptr;
  }
}

void Parser::synchronize() {
  if (check(TokenKind::EndOfFile)) return;
  advance();

  while (!check(TokenKind::EndOfFile) && !check(TokenKind::RBrace)) {
    if (check(TokenKind::Semicolon) || check(TokenKind::Newline)) {
      advance();
      return;
    }
    switch (peek().kind) {
    case TokenKind::KwLet:
    case TokenKind::KwConst:
    case TokenKind::KwFunc:
    case TokenKind::KwStruct:
    case TokenKind::KwEnum:
    case TokenKind::KwImport:
    case TokenKind::KwNamespace:
    case TokenKind::KwIf:
    case TokenKind::KwWhile:
    case TokenKind::KwDo:
    case TokenKind::KwFor:
    case TokenKind::KwReturn:
      return;
    default:
      break;
    }
    advance();
  }
}

std::unique_ptr<Block> Parser::parseBlock() {
  uint32_t startByte = expect(TokenKind::LBrace, "'{' to start a block").startByte;
  auto block = std::make_unique<Block>();
  while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
    if (match(TokenKind::Newline)) continue;
    if (recoverFromErrors) {
      if (auto stmt = parseStatementRecovering()) block->statements.push_back(std::move(stmt));
    } else {
      block->statements.push_back(parseStatement());
    }
  }
  block->startByte = startByte;
  if (recoverFromErrors && check(TokenKind::EndOfFile)) {
    diagnostics.push_back({.loc = locOf(peek()), .message = "Expected '}' to close block, found end of file"});
    block->endByte = peek().endByte;
  } else {
    block->endByte = expect(TokenKind::RBrace, "'}' to close block").endByte;
  }
  return block;
}

std::unique_ptr<Block> Parser::parseProgram() {
  auto block = std::make_unique<Block>();
  block->startByte = peek().startByte;
  while (!check(TokenKind::EndOfFile)) {
    if (match(TokenKind::Newline)) continue;
    if (recoverFromErrors) {
      if (auto stmt = parseStatementRecovering()) block->statements.push_back(std::move(stmt));
    } else {
      block->statements.push_back(parseStatement());
    }
  }
  block->endByte = peek().endByte;
  return block;
}

static bool isContextModifierIdentifier(const Token &tok) {
  if (tok.kind != TokenKind::Identifier) return false;
  static const std::string_view words[] = {"at", "align", "anchored", "facing", "on", "positioned", "rotated"};
  for (auto w : words)
    if (tok.text == w) return true;
  return false;
}

std::unique_ptr<Stmt> Parser::parseIf() {
  const Token &startTok = peek();
  advance();
  auto cond = parseExpression();
  auto thenBlock = parseBlock();
  std::optional<std::unique_ptr<Stmt>> elseBranch;
  if (check(TokenKind::KwElse)) {
    const Token &elseTok = peek();
    advance();
    if (check(TokenKind::KwIf)) {
      elseBranch = parseIf();
    } else {
      auto blk = parseBlock();
      elseBranch = makeStmt(elseTok, BlockStmt{.block = std::move(blk)});
    }
  }
  return makeStmt(startTok, IfStmt{.condition = std::move(cond), .thenBlock = std::move(thenBlock), .elseBranch = std::move(elseBranch)});
}

std::unique_ptr<Stmt> Parser::parseWhile() {
  const Token &startTok = peek();
  advance();
  auto cond = parseExpression();
  auto body = parseBlock();
  return makeStmt(startTok, WhileStmt{.condition = std::move(cond), .body = std::move(body)});
}

std::unique_ptr<Stmt> Parser::parseDoWhile() {
  const Token &startTok = peek();
  advance();
  auto body = parseBlock();
  expect(TokenKind::KwWhile, "'while' after do-block");
  auto cond = parseExpression();
  return makeStmt(startTok, DoWhileStmt{.body = std::move(body), .condition = std::move(cond)});
}

std::unique_ptr<Stmt> Parser::parseFor() {
  const Token &startTok = peek();
  advance();
  const Token &iterTok = expect(TokenKind::Identifier, "loop variable name");
  std::string iterator(iterTok.text);
  SourceLoc iteratorLoc = locOf(iterTok);
  expect(TokenKind::KwIn, "'in' after loop variable");
  auto start = parseExpression();
  expect(TokenKind::DotDot, "'..' between loop bounds");
  auto end = parseExpression();
  auto body = parseBlock();
  return makeStmt(startTok, ForStmt{.iterator = std::move(iterator), .iteratorLoc = iteratorLoc, .start = std::move(start), .end = std::move(end), .body = std::move(body)});
}

std::unique_ptr<Stmt> Parser::parseVarDecl(bool isExport, bool isExtern) {
  const Token &startTok = peek();
  bool isConst = check(TokenKind::KwConst);
  advance();
  const Token &nameTok = expect(TokenKind::Identifier, "variable name");
  std::string name(nameTok.text);
  SourceLoc nameLoc = locOf(nameTok);
  std::optional<std::string> typeText;
  SourceLoc typeLoc;
  if (match(TokenKind::Colon)) typeText = parseTypeText(typeLoc);
  expect(TokenKind::Eq, "'=' in variable declaration");
  auto value = parseExpression();
  return makeStmt(
    startTok,
    VarDeclStmt{
      .isConst = isConst,
      .isExport = isExport,
      .isExtern = isExtern,
      .name = std::move(name),
      .nameLoc = nameLoc,
      .typeText = std::move(typeText),
      .typeLoc = typeLoc,
      .value = std::move(value)
    }
  );
}

std::unique_ptr<Stmt> Parser::parseFuncDecl(std::optional<std::string> tag, bool isExport, bool isExtern) {
  const Token &startTok = peek();
  advance();
  const Token &nameTok = expect(TokenKind::Identifier, "function name");
  std::string name(nameTok.text);
  SourceLoc nameLoc = locOf(nameTok);
  expect(TokenKind::LParen, "'(' after function name");
  std::vector<Param> params;
  parseCommaSeparated(*this, TokenKind::RParen, false, [&] {
    const Token &pnameTok = expect(TokenKind::Identifier, "parameter name");
    std::string pname(pnameTok.text);
    SourceLoc pnameLoc = locOf(pnameTok);
    expect(TokenKind::Colon, "':' after parameter name");
    SourceLoc ptypeLoc;
    std::string ptype = parseTypeText(ptypeLoc);
    params.push_back(Param{.name = std::move(pname), .loc = pnameLoc, .typeText = std::move(ptype), .typeLoc = ptypeLoc});
  });
  expect(TokenKind::RParen, "')' to close parameter list");
  std::optional<std::string> returnTypeText;
  SourceLoc returnTypeLoc;
  if (match(TokenKind::Colon)) returnTypeText = parseTypeText(returnTypeLoc);
  auto body = parseBlock();
  return makeStmt(
    startTok,
    FuncDeclStmt{
      .tag = std::move(tag),
      .isExport = isExport,
      .isExtern = isExtern,
      .name = std::move(name),
      .nameLoc = nameLoc,
      .params = std::move(params),
      .returnTypeText = std::move(returnTypeText),
      .returnTypeLoc = returnTypeLoc,
      .body = std::move(body)
    }
  );
}

std::unique_ptr<Stmt> Parser::parseStructDecl(bool isExport) {
  const Token &startTok = peek();
  advance();
  const Token &nameTok = expect(TokenKind::Identifier, "struct name");
  std::string name(nameTok.text);
  SourceLoc nameLoc = locOf(nameTok);
  expect(TokenKind::LBrace, "'{' after struct name");

  std::vector<StructFieldDecl> fields;
  std::vector<StructMethodDecl> methods;

  auto skipNewlines = [&] {
    while (check(TokenKind::Newline)) advance();
  };

  skipNewlines();
  while (!check(TokenKind::RBrace)) {
    bool isPrivate = false, isPublic = false, isStatic = false;
    while (checkIdentifierText("public") || checkIdentifierText("private") || checkIdentifierText("static")) {
      if (checkIdentifierText("public")) isPublic = true;
      else if (checkIdentifierText("private")) isPrivate = true;
      else isStatic = true;
      advance();
    }
    if (isPrivate && isPublic) error(peek(), "A struct member cannot be both 'public' and 'private'.");

    if (check(TokenKind::KwFunc)) {
      const Token &methodStartTok = peek();
      advance();
      const Token &mnameTok = expect(TokenKind::Identifier, "method name");
      std::string mname(mnameTok.text);
      SourceLoc mnameLoc = locOf(mnameTok);
      expect(TokenKind::LParen, "'(' after method name");
      std::vector<Param> params;
      parseCommaSeparated(*this, TokenKind::RParen, false, [&] {
        const Token &pnameTok = expect(TokenKind::Identifier, "parameter name");
        std::string pname(pnameTok.text);
        SourceLoc pnameLoc = locOf(pnameTok);
        expect(TokenKind::Colon, "':' after parameter name");
        SourceLoc ptypeLoc;
        std::string ptype = parseTypeText(ptypeLoc);
        params.push_back(Param{.name = std::move(pname), .loc = pnameLoc, .typeText = std::move(ptype), .typeLoc = ptypeLoc});
      });
      expect(TokenKind::RParen, "')' to close parameter list");
      std::optional<std::string> returnTypeText;
      SourceLoc returnTypeLoc;
      if (match(TokenKind::Colon)) returnTypeText = parseTypeText(returnTypeLoc);
      auto body = parseBlock();
      methods.push_back(
        StructMethodDecl{
          .loc = locOf(methodStartTok),
          .name = std::move(mname),
          .nameLoc = mnameLoc,
          .isPrivate = isPrivate,
          .isStatic = isStatic,
          .params = std::move(params),
          .returnTypeText = std::move(returnTypeText),
          .returnTypeLoc = returnTypeLoc,
          .body = std::move(body)
        }
      );
    } else {
      const Token &fnameTok = expect(TokenKind::Identifier, "field name or 'func'");
      std::string fname(fnameTok.text);
      SourceLoc fnameLoc = locOf(fnameTok);
      expect(TokenKind::Colon, "':' after field name");
      SourceLoc ftypeLoc;
      std::string ftype = parseTypeText(ftypeLoc);
      fields.push_back(StructFieldDecl{.name = std::move(fname), .nameLoc = fnameLoc, .typeText = std::move(ftype), .typeLoc = ftypeLoc, .isPrivate = isPrivate});
      match(TokenKind::Comma);
    }
    skipNewlines();
  }
  expect(TokenKind::RBrace, "'}' to close struct");
  return makeStmt(startTok, StructDeclStmt{.isExport = isExport, .name = std::move(name), .nameLoc = nameLoc, .fields = std::move(fields), .methods = std::move(methods)});
}

std::unique_ptr<Stmt> Parser::parseEnumDecl(bool isExport) {
  const Token &startTok = peek();
  advance();
  const Token &nameTok = expect(TokenKind::Identifier, "enum name");
  std::string name(nameTok.text);
  SourceLoc nameLoc = locOf(nameTok);
  expect(TokenKind::LBrace, "'{' after enum name");
  std::vector<EnumVariantDecl> variants;
  parseCommaSeparated(*this, TokenKind::RBrace, true, [&] {
    const Token &vnameTok = expect(TokenKind::Identifier, "variant name");
    std::string vname(vnameTok.text);
    SourceLoc vnameLoc = locOf(vnameTok);
    std::optional<std::unique_ptr<Expr>> value;
    if (match(TokenKind::Eq)) {
      const Token &valTok = peek();
      if (check(TokenKind::StringLit)) {
        advance();
        value = makeExpr(valTok, StringLit{.text = std::string(valTok.text)});
      } else if (check(TokenKind::IntegerLit)) {
        advance();
        value = makeExpr(valTok, IntLit{.text = std::string(valTok.text)});
      } else if (check(TokenKind::FloatLit)) {
        advance();
        value = makeExpr(valTok, FloatLit{.text = std::string(valTok.text)});
      } else {
        error(peek(), "Expected a string, integer, or float literal for the enum variant's value");
      }
    }
    variants.push_back(EnumVariantDecl{.name = std::move(vname), .nameLoc = vnameLoc, .value = std::move(value)});
  });
  expect(TokenKind::RBrace, "'}' to close enum");
  return makeStmt(startTok, EnumDeclStmt{.isExport = isExport, .name = std::move(name), .nameLoc = nameLoc, .variants = std::move(variants)});
}

std::unique_ptr<Stmt> Parser::parseNamespaceDecl() {
  const Token &startTok = peek();
  advance();
  const Token &nameTok = expect(TokenKind::Identifier, "namespace name");
  std::string name(nameTok.text);
  SourceLoc nameLoc = locOf(nameTok);
  auto body = parseBlock();
  return makeStmt(startTok, NamespaceStmt{.name = std::move(name), .nameLoc = nameLoc, .body = std::move(body)});
}

std::unique_ptr<Stmt> Parser::parseImportDecl() {
  const Token &startTok = peek();
  advance();
  std::string path = parseImportPathText();
  std::optional<std::string> alias;
  if (match(TokenKind::KwAs)) alias = std::string(expect(TokenKind::Identifier, "alias name").text);
  return makeStmt(startTok, ImportStmt{.path = std::move(path), .alias = std::move(alias)});
}

std::unique_ptr<Stmt> Parser::parseReturnStmt() {
  const Token &startTok = peek();
  advance();
  std::optional<std::unique_ptr<Expr>> value;
  if (!atStatementEnd()) value = parseExpression();
  return makeStmt(startTok, ReturnStmt{.value = std::move(value)});
}

std::unique_ptr<Stmt> Parser::parseContextStmt() {
  const Token &startTok = peek();
  std::vector<ContextModifier> modifiers;
  do {
    const Token &kwTok = peek();
    ContextModifier mod;
    std::string keyword;

    if (check(TokenKind::KwAs)) {
      keyword = "as";
      advance();
      mod.primaryText = parseSelectorText();
    } else if (check(TokenKind::KwIn)) {
      keyword = "in";
      advance();
      mod.primaryText = parseNamespacedArgText();
    } else {
      keyword = std::string(peek().text);
      advance();
      if (keyword == "at") {
        mod.primaryText = parseSelectorText();
      } else if (keyword == "align") {
        mod.primaryText = std::string(expect(TokenKind::Identifier, "axes (e.g. xyz)").text);
      } else if (keyword == "anchored") {
        mod.primaryText = std::string(expect(TokenKind::Identifier, "'eyes' or 'feet'").text);
      } else if (keyword == "facing") {
        if (checkIdentifierText("entity")) {
          advance();
          mod.primaryText = parseSelectorText();
          mod.secondaryText = std::string(expect(TokenKind::Identifier, "'eyes' or 'feet'").text);
        } else {
          mod.primaryText = parseVecText(3);
        }
      } else if (keyword == "on") {
        mod.primaryText = std::string(expect(TokenKind::Identifier, "relation (e.g. owner, target)").text);
      } else if (keyword == "positioned") {
        if (check(TokenKind::KwAs)) {
          advance();
          mod.primaryText = "as";
          mod.secondaryText = parseSelectorText();
        } else if (checkIdentifierText("over")) {
          advance();
          mod.primaryText = "over";
          mod.secondaryText = std::string(expect(TokenKind::Identifier, "heightmap (e.g. world_surface)").text);
        } else {
          mod.primaryText = parseVecText(3);
        }
      } else if (keyword == "rotated") {
        if (check(TokenKind::KwAs)) {
          advance();
          mod.primaryText = "as";
          mod.secondaryText = parseSelectorText();
        } else {
          mod.primaryText = parseVecText(2);
        }
      } else {
        error(kwTok, "Unknown context modifier '" + keyword + "'");
      }
    }

    mod.keyword = std::move(keyword);
    modifiers.push_back(std::move(mod));
  } while (check(TokenKind::KwAs) || check(TokenKind::KwIn) || isContextModifierIdentifier(peek()));

  auto body = parseBlock();
  return makeStmt(startTok, ContextStmt{.modifiers = std::move(modifiers), .body = std::move(body)});
}

std::unique_ptr<Stmt> Parser::parseCommandStmt() {
  const Token &startTok = peek();
  if (!check(TokenKind::Identifier)) error(peek(), "Expected a command name");
  std::string commandName(peek().text);
  uint32_t cursor = peek().endByte;
  advance();

  std::vector<CommandPart> parts;
  std::string currentLiteral;
  auto flushLiteral = [&]() {
    if (!currentLiteral.empty()) {
      parts.push_back(CommandPart{.isInterpolation = false, .literalText = currentLiteral, .interpExpr = nullptr});
      currentLiteral.clear();
    }
  };

  while (!atStatementEnd()) {
    if (check(TokenKind::Dollar) && peek(1).kind == TokenKind::LBrace) {
      currentLiteral += std::string(source.substr(cursor, peek().startByte - cursor));
      flushLiteral();
      advance();
      advance();
      auto expr = parseExpression();
      expect(TokenKind::RBrace, "'}' to close interpolation");
      parts.push_back(CommandPart{.isInterpolation = true, .literalText = "", .interpExpr = std::move(expr)});
      cursor = previous().endByte;
      continue;
    }

    currentLiteral += std::string(source.substr(cursor, peek().endByte - cursor));
    cursor = peek().endByte;
    advance();
  }
  flushLiteral();

  return makeStmt(startTok, CommandStmt{.commandName = std::move(commandName), .parts = std::move(parts)});
}

static bool decomposeAssignTarget(std::unique_ptr<Expr> expr, std::string &outName, SourceLoc &outNameLoc, std::vector<PathComponent> &outPath) {
  SourceLoc nodeLoc = expr->loc;
  if (auto *vr = std::get_if<VarRefExpr>(&expr->data)) {
    outName = std::move(vr->name);
    outNameLoc = nodeLoc;
    return true;
  }
  if (auto *me = std::get_if<MemberExpr>(&expr->data)) {
    SourceLoc propLoc = me->propertyLoc;
    if (!decomposeAssignTarget(std::move(me->object), outName, outNameLoc, outPath)) return false;
    outPath.push_back(PathComponent{.isIndex = false, .propertyName = std::move(me->property), .loc = propLoc, .index = nullptr});
    return true;
  }
  if (auto *ee = std::get_if<ElementExpr>(&expr->data)) {
    if (!decomposeAssignTarget(std::move(ee->target), outName, outNameLoc, outPath)) return false;
    outPath.push_back(PathComponent{.isIndex = true, .propertyName = "", .loc = nodeLoc, .index = std::move(ee->index)});
    return true;
  }
  return false;
}

std::unique_ptr<Stmt> Parser::tryParseAssignOrCallStmt() {
  size_t save = pos;
  const Token &startTok = peek();
  try {
    std::string name = parseNamespacedIdentifier();
    SourceLoc nameLoc = locOf(startTok);
    nameLoc.endByte = previous().endByte;
    std::unique_ptr<Expr> expr;

    if (check(TokenKind::LParen)) {
      advance();
      std::vector<std::unique_ptr<Expr>> args;
      parseCommaSeparated(*this, TokenKind::RParen, false, [&] { args.push_back(parseExpression()); });
      expect(TokenKind::RParen, "')' to close call arguments");
      expr = makeExpr(startTok, CallExpr{.name = std::move(name), .nameLoc = nameLoc, .arguments = std::move(args)});
    } else {
      expr = makeExpr(startTok, VarRefExpr{.name = std::move(name)});
    }

    expr = parsePostfixContinuation(std::move(expr));

    if (check(TokenKind::Eq)) {
      std::string targetName;
      SourceLoc targetNameLoc;
      std::vector<PathComponent> path;
      if (!decomposeAssignTarget(std::move(expr), targetName, targetNameLoc, path)) {
        pos = save;
        return nullptr;
      }
      advance();
      auto value = parseExpression();
      return makeStmt(startTok, AssignStmt{.name = std::move(targetName), .nameLoc = targetNameLoc, .path = std::move(path), .value = std::move(value)});
    }

    if (atStatementEnd() && (std::holds_alternative<CallExpr>(expr->data) || std::holds_alternative<MethodCallExpr>(expr->data))) {
      return makeStmt(startTok, ExprStmt{.expr = std::move(expr)});
    }

    pos = save;
    return nullptr;
  } catch (const ParseError &) {
    pos = save;
    return nullptr;
  }
}

std::unique_ptr<Stmt> Parser::parseStatement() {
  std::optional<std::string> tag;
  if (check(TokenKind::Hash)) {
    advance();
    tag = parseNamespacedArgText();
    match(TokenKind::Newline);
  }

  bool isExport = false, isExtern = false;
  while (check(TokenKind::KwExport) || check(TokenKind::KwExtern)) {
    if (match(TokenKind::KwExport)) isExport = true;
    else {
      advance();
      isExtern = true;
    }
  }

  std::unique_ptr<Stmt> stmt;

  if (check(TokenKind::KwImport)) {
    if (tag.has_value() || isExport || isExtern) error(peek(), "'import' cannot be preceded by a tag or modifiers");
    stmt = parseImportDecl();
  } else if (check(TokenKind::KwEnum)) {
    if (tag.has_value() || isExtern) error(peek(), "'enum' cannot be preceded by a tag or 'extern'");
    stmt = parseEnumDecl(isExport);
  } else if (check(TokenKind::KwStruct)) {
    if (tag.has_value() || isExtern) error(peek(), "'struct' cannot be preceded by a tag or 'extern'");
    stmt = parseStructDecl(isExport);
  } else if (check(TokenKind::KwLet) || check(TokenKind::KwConst)) {
    if (tag.has_value()) error(peek(), "Variable declarations cannot be preceded by a tag");
    stmt = parseVarDecl(isExport, isExtern);
  } else if (check(TokenKind::KwFunc)) {
    stmt = parseFuncDecl(tag, isExport, isExtern);
  } else if (tag.has_value() || isExport || isExtern) {
    error(peek(), "Expected a declaration ('func', 'let', 'const', 'struct', or 'enum') after a tag/modifier");
  } else if (check(TokenKind::KwIf)) {
    stmt = parseIf();
  } else if (check(TokenKind::KwWhile)) {
    stmt = parseWhile();
  } else if (check(TokenKind::KwDo)) {
    stmt = parseDoWhile();
  } else if (check(TokenKind::KwFor)) {
    stmt = parseFor();
  } else if (check(TokenKind::KwReturn)) {
    stmt = parseReturnStmt();
  } else if (check(TokenKind::KwNamespace)) {
    stmt = parseNamespaceDecl();
  } else if (check(TokenKind::KwAs) || check(TokenKind::KwIn) || isContextModifierIdentifier(peek())) {
    stmt = parseContextStmt();
  } else if (check(TokenKind::Identifier)) {
    stmt = tryParseAssignOrCallStmt();
    if (!stmt) stmt = parseCommandStmt();
  } else {
    stmt = parseCommandStmt();
  }

  if (!check(TokenKind::EndOfFile)) consumeStatementTerminator();
  return stmt;
}

static void print(const Expr &e, std::string &out);

static void printChild(const std::unique_ptr<Expr> &e, std::string &out) {
  if (e) print(*e, out);
}

static void print(const Expr &e, std::string &out) {
  std::visit(
    [&](auto &&n) {
      using T = std::decay_t<decltype(n)>;
      if constexpr (std::is_same_v<T, IntLit> || std::is_same_v<T, FloatLit>) {
        out += n.text;
      } else if constexpr (std::is_same_v<T, BoolLit>) {
        out += n.value ? "true" : "false";
      } else if constexpr (std::is_same_v<T, StringLit>) {
        out += n.text;
      } else if constexpr (std::is_same_v<T, BinaryExpr>) {
        out += "(";
        printChild(n.left, out);
        out += " " + n.op + " ";
        printChild(n.right, out);
        out += ")";
      } else if constexpr (std::is_same_v<T, UnaryExpr>) {
        out += "(" + n.op;
        printChild(n.operand, out);
        out += ")";
      } else if constexpr (std::is_same_v<T, EntityTestExpr>) {
        out += "(entity " + n.selectorText + ")";
      } else if constexpr (std::is_same_v<T, AtTestExpr>) {
        out += "(" + n.blockText + " at " + n.posText + ")";
      } else if constexpr (std::is_same_v<T, TernaryExpr>) {
        out += "(";
        printChild(n.condition, out);
        out += " ? ";
        printChild(n.ifTrue, out);
        out += " : ";
        printChild(n.ifFalse, out);
        out += ")";
      } else if constexpr (std::is_same_v<T, MemberExpr>) {
        printChild(n.object, out);
        out += "." + n.property;
      } else if constexpr (std::is_same_v<T, SliceExpr>) {
        printChild(n.target, out);
        out += "[";
        printChild(n.start, out);
        out += "..";
        printChild(n.end, out);
        out += "]";
      } else if constexpr (std::is_same_v<T, ElementExpr>) {
        printChild(n.target, out);
        out += "[";
        printChild(n.index, out);
        out += "]";
      } else if constexpr (std::is_same_v<T, CallExpr>) {
        out += n.name + "(";
        for (size_t i = 0; i < n.arguments.size(); i++) {
          if (i) out += ", ";
          printChild(n.arguments[i], out);
        }
        out += ")";
      } else if constexpr (std::is_same_v<T, MethodCallExpr>) {
        printChild(n.object, out);
        out += "." + n.method + "(";
        for (size_t i = 0; i < n.arguments.size(); i++) {
          if (i) out += ", ";
          printChild(n.arguments[i], out);
        }
        out += ")";
      } else if constexpr (std::is_same_v<T, VarRefExpr>) {
        out += n.name;
      } else if constexpr (std::is_same_v<T, CastExpr>) {
        out += "(";
        printChild(n.expression, out);
        out += " as " + n.typeText + ")";
      } else if constexpr (std::is_same_v<T, StructExpr>) {
        out += n.name + "{";
        for (size_t i = 0; i < n.fields.size(); i++) {
          if (i) out += ", ";
          out += n.fields[i].name + ": ";
          printChild(n.fields[i].value, out);
        }
        out += "}";
      } else if constexpr (std::is_same_v<T, ListExpr>) {
        out += "[";
        for (size_t i = 0; i < n.elements.size(); i++) {
          if (i) out += ", ";
          printChild(n.elements[i], out);
        }
        out += "]";
      } else if constexpr (std::is_same_v<T, ReferenceExpr>) {
        out += "&" + n.targetName;
      }
    },
    e.data
  );
}

std::string exprToString(const Expr &expr) {
  std::string out;
  print(expr, out);
  return out;
}
