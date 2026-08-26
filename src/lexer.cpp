#include "lexer.hpp"

#include <cctype>
#include <unordered_map>

const char *tokenKindName(TokenKind kind) {
  switch (kind) {
  case TokenKind::Identifier:
    return "identifier";
  case TokenKind::IntegerLit:
    return "integer";
  case TokenKind::FloatLit:
    return "float";
  case TokenKind::StringLit:
    return "string";
  case TokenKind::KwLet:
    return "let";
  case TokenKind::KwConst:
    return "const";
  case TokenKind::KwStruct:
    return "struct";
  case TokenKind::KwEnum:
    return "enum";
  case TokenKind::KwFunc:
    return "func";
  case TokenKind::KwIf:
    return "if";
  case TokenKind::KwElse:
    return "else";
  case TokenKind::KwWhile:
    return "while";
  case TokenKind::KwDo:
    return "do";
  case TokenKind::KwFor:
    return "for";
  case TokenKind::KwIn:
    return "in";
  case TokenKind::KwReturn:
    return "return";
  case TokenKind::KwImport:
    return "import";
  case TokenKind::KwAs:
    return "as";
  case TokenKind::KwExport:
    return "export";
  case TokenKind::KwExtern:
    return "extern";
  case TokenKind::KwNamespace:
    return "namespace";
  case TokenKind::KwTrue:
    return "true";
  case TokenKind::KwFalse:
    return "false";
  case TokenKind::Plus:
    return "+";
  case TokenKind::Minus:
    return "-";
  case TokenKind::Star:
    return "*";
  case TokenKind::Slash:
    return "/";
  case TokenKind::Percent:
    return "%";
  case TokenKind::Eq:
    return "=";
  case TokenKind::EqEq:
    return "==";
  case TokenKind::BangEq:
    return "!=";
  case TokenKind::Lt:
    return "<";
  case TokenKind::Gt:
    return ">";
  case TokenKind::LtEq:
    return "<=";
  case TokenKind::GtEq:
    return ">=";
  case TokenKind::AmpAmp:
    return "&&";
  case TokenKind::PipePipe:
    return "||";
  case TokenKind::Bang:
    return "!";
  case TokenKind::Amp:
    return "&";
  case TokenKind::Dot:
    return ".";
  case TokenKind::DotDot:
    return "..";
  case TokenKind::Comma:
    return ",";
  case TokenKind::Colon:
    return ":";
  case TokenKind::ColonColon:
    return "::";
  case TokenKind::Semicolon:
    return ";";
  case TokenKind::LParen:
    return "(";
  case TokenKind::RParen:
    return ")";
  case TokenKind::LBrace:
    return "{";
  case TokenKind::RBrace:
    return "}";
  case TokenKind::LBracket:
    return "[";
  case TokenKind::RBracket:
    return "]";
  case TokenKind::Question:
    return "?";
  case TokenKind::Dollar:
    return "$";
  case TokenKind::Hash:
    return "#";
  case TokenKind::At:
    return "@";
  case TokenKind::Caret:
    return "^";
  case TokenKind::Tilde:
    return "~";
  case TokenKind::Newline:
    return "newline";
  case TokenKind::EndOfFile:
    return "eof";
  case TokenKind::Symbol:
    return "symbol";
  }
  return "?";
}

static const std::unordered_map<std::string_view, TokenKind> &keywordTable() {
  static const std::unordered_map<std::string_view, TokenKind> table = {
    {"let", TokenKind::KwLet},       {"const", TokenKind::KwConst},         {"struct", TokenKind::KwStruct}, {"enum", TokenKind::KwEnum},   {"func", TokenKind::KwFunc},
    {"if", TokenKind::KwIf},         {"else", TokenKind::KwElse},           {"while", TokenKind::KwWhile},   {"do", TokenKind::KwDo},       {"for", TokenKind::KwFor},
    {"in", TokenKind::KwIn},         {"return", TokenKind::KwReturn},       {"import", TokenKind::KwImport}, {"as", TokenKind::KwAs},       {"export", TokenKind::KwExport},
    {"extern", TokenKind::KwExtern}, {"namespace", TokenKind::KwNamespace}, {"true", TokenKind::KwTrue},     {"false", TokenKind::KwFalse},
  };
  return table;
}

Lexer::Lexer(std::string_view source) : source(source) {}

bool Lexer::atEnd() const { return pos >= source.size(); }

char Lexer::peek(size_t ahead) const {
  size_t p = pos + ahead;
  if (p >= source.size()) return '\0';
  return source[p];
}

char Lexer::advance() {
  char c = source[pos++];
  if (c == '\n') {
    line++;
    col = 1;
  } else {
    col++;
  }
  return c;
}

bool Lexer::match(char expected) {
  if (atEnd() || source[pos] != expected) return false;
  advance();
  return true;
}

void Lexer::skipLineComment() {
  while (!atEnd() && peek() != '\n') advance();
}

Token Lexer::makeToken(TokenKind kind, size_t startByte, uint32_t startLine, uint32_t startCol) {
  return Token{
    .kind = kind,
    .text = source.substr(startByte, pos - startByte),
    .startByte = static_cast<uint32_t>(startByte),
    .endByte = static_cast<uint32_t>(pos),
    .line = startLine,
    .col = startCol,
  };
}

Token Lexer::lexIdentifierOrKeyword() {
  size_t startByte = pos;
  uint32_t startLine = line, startCol = col;

  while (!atEnd() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) advance();

  Token tok = makeToken(TokenKind::Identifier, startByte, startLine, startCol);

  std::string lowered(tok.text);
  for (auto &c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  const auto &kw = keywordTable();
  auto it = kw.find(lowered);
  if (it != kw.end()) tok.kind = it->second;

  return tok;
}

Token Lexer::lexNumber() {
  size_t startByte = pos;
  uint32_t startLine = line, startCol = col;

  while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) advance();

  if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
    advance();
    while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
    return makeToken(TokenKind::FloatLit, startByte, startLine, startCol);
  }

  return makeToken(TokenKind::IntegerLit, startByte, startLine, startCol);
}

Token Lexer::lexString(char quote) {
  size_t startByte = pos;
  uint32_t startLine = line, startCol = col;

  advance();
  while (!atEnd() && peek() != quote) {
    if (peek() == '\\' && peek(1) != '\0') {
      advance();
      advance();
    } else {
      advance();
    }
  }
  if (!atEnd()) advance();

  return makeToken(TokenKind::StringLit, startByte, startLine, startCol);
}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> tokens;

  while (true) {
    while (!atEnd() && (peek() == ' ' || peek() == '\t' || peek() == '\r')) advance();

    if (atEnd()) {
      tokens.push_back(makeToken(TokenKind::EndOfFile, pos, line, col));
      break;
    }

    size_t startByte = pos;
    uint32_t startLine = line, startCol = col;
    char c = peek();

    if (c == '\n') {
      advance();
      tokens.push_back(makeToken(TokenKind::Newline, startByte, startLine, startCol));
      continue;
    }

    if (c == '-' && peek(1) == '-') {
      skipLineComment();
      continue;
    }

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      tokens.push_back(lexIdentifierOrKeyword());
      continue;
    }

    if (std::isdigit(static_cast<unsigned char>(c))) {
      tokens.push_back(lexNumber());
      continue;
    }

    if (c == '"' || c == '\'') {
      tokens.push_back(lexString(c));
      continue;
    }

    advance();
    switch (c) {
    case '+':
      tokens.push_back(makeToken(TokenKind::Plus, startByte, startLine, startCol));
      break;
    case '-':
      tokens.push_back(makeToken(TokenKind::Minus, startByte, startLine, startCol));
      break;
    case '*':
      tokens.push_back(makeToken(TokenKind::Star, startByte, startLine, startCol));
      break;
    case '/':
      tokens.push_back(makeToken(TokenKind::Slash, startByte, startLine, startCol));
      break;
    case '%':
      tokens.push_back(makeToken(TokenKind::Percent, startByte, startLine, startCol));
      break;
    case '=':
      if (match('=')) tokens.push_back(makeToken(TokenKind::EqEq, startByte, startLine, startCol));
      else tokens.push_back(makeToken(TokenKind::Eq, startByte, startLine, startCol));
      break;
    case '!':
      if (match('=')) tokens.push_back(makeToken(TokenKind::BangEq, startByte, startLine, startCol));
      else tokens.push_back(makeToken(TokenKind::Bang, startByte, startLine, startCol));
      break;
    case '<':
      if (match('=')) tokens.push_back(makeToken(TokenKind::LtEq, startByte, startLine, startCol));
      else tokens.push_back(makeToken(TokenKind::Lt, startByte, startLine, startCol));
      break;
    case '>':
      if (match('=')) tokens.push_back(makeToken(TokenKind::GtEq, startByte, startLine, startCol));
      else tokens.push_back(makeToken(TokenKind::Gt, startByte, startLine, startCol));
      break;
    case '&':
      if (match('&')) tokens.push_back(makeToken(TokenKind::AmpAmp, startByte, startLine, startCol));
      else tokens.push_back(makeToken(TokenKind::Amp, startByte, startLine, startCol));
      break;
    case '|':
      if (match('|')) tokens.push_back(makeToken(TokenKind::PipePipe, startByte, startLine, startCol));
      else tokens.push_back(makeToken(TokenKind::Symbol, startByte, startLine, startCol));
      break;
    case '.':
      if (match('.')) tokens.push_back(makeToken(TokenKind::DotDot, startByte, startLine, startCol));
      else tokens.push_back(makeToken(TokenKind::Dot, startByte, startLine, startCol));
      break;
    case ',':
      tokens.push_back(makeToken(TokenKind::Comma, startByte, startLine, startCol));
      break;
    case ':':
      if (match(':')) tokens.push_back(makeToken(TokenKind::ColonColon, startByte, startLine, startCol));
      else tokens.push_back(makeToken(TokenKind::Colon, startByte, startLine, startCol));
      break;
    case ';':
      tokens.push_back(makeToken(TokenKind::Semicolon, startByte, startLine, startCol));
      break;
    case '(':
      tokens.push_back(makeToken(TokenKind::LParen, startByte, startLine, startCol));
      break;
    case ')':
      tokens.push_back(makeToken(TokenKind::RParen, startByte, startLine, startCol));
      break;
    case '{':
      tokens.push_back(makeToken(TokenKind::LBrace, startByte, startLine, startCol));
      break;
    case '}':
      tokens.push_back(makeToken(TokenKind::RBrace, startByte, startLine, startCol));
      break;
    case '[':
      tokens.push_back(makeToken(TokenKind::LBracket, startByte, startLine, startCol));
      break;
    case ']':
      tokens.push_back(makeToken(TokenKind::RBracket, startByte, startLine, startCol));
      break;
    case '?':
      tokens.push_back(makeToken(TokenKind::Question, startByte, startLine, startCol));
      break;
    case '$':
      tokens.push_back(makeToken(TokenKind::Dollar, startByte, startLine, startCol));
      break;
    case '#':
      tokens.push_back(makeToken(TokenKind::Hash, startByte, startLine, startCol));
      break;
    case '@':
      tokens.push_back(makeToken(TokenKind::At, startByte, startLine, startCol));
      break;
    case '^':
      tokens.push_back(makeToken(TokenKind::Caret, startByte, startLine, startCol));
      break;
    case '~':
      tokens.push_back(makeToken(TokenKind::Tilde, startByte, startLine, startCol));
      break;
    default:
      tokens.push_back(makeToken(TokenKind::Symbol, startByte, startLine, startCol));
      break;
    }
  }

  return tokens;
}
