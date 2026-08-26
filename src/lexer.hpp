#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class TokenKind {
  Identifier,
  IntegerLit,
  FloatLit,
  StringLit,

  KwLet,
  KwConst,
  KwStruct,
  KwEnum,
  KwFunc,
  KwIf,
  KwElse,
  KwWhile,
  KwDo,
  KwFor,
  KwIn,
  KwReturn,
  KwImport,
  KwAs,
  KwExport,
  KwExtern,
  KwNamespace,
  KwTrue,
  KwFalse,

  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  Eq,
  EqEq,
  BangEq,
  Lt,
  Gt,
  LtEq,
  GtEq,
  AmpAmp,
  PipePipe,
  Bang,
  Amp,
  Dot,
  DotDot,
  Comma,
  Colon,
  ColonColon,
  Semicolon,
  LParen,
  RParen,
  LBrace,
  RBrace,
  LBracket,
  RBracket,
  Question,
  Dollar,
  Hash,
  At,
  Caret,
  Tilde,

  Newline,
  EndOfFile,

  Symbol,
};

struct Token {
  TokenKind kind;
  std::string_view text;
  uint32_t startByte;
  uint32_t endByte;
  uint32_t line; /** 1-indexed */
  uint32_t col;  /** 1-indexed */
};

const char *tokenKindName(TokenKind kind);

class Lexer {
public:
  explicit Lexer(std::string_view source);

  std::vector<Token> tokenize();

private:
  std::string_view source;
  size_t pos = 0;
  uint32_t line = 1;
  uint32_t col = 1;

  bool atEnd() const;
  char peek(size_t ahead = 0) const;
  char advance();
  bool match(char expected);

  void skipLineComment();
  Token makeToken(TokenKind kind, size_t startByte, uint32_t startLine, uint32_t startCol);

  Token lexIdentifierOrKeyword();
  Token lexNumber();
  Token lexString(char quote);
};
