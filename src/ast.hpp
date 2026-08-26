#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

struct SourceLoc {
  uint32_t line = 0; /** 1-indexed */
  uint32_t col = 0;  /** 1-indexed */
  uint32_t startByte = 0;
  uint32_t endByte = 0;
};

struct Expr;
struct Stmt;
struct Block {
  std::vector<std::unique_ptr<Stmt>> statements;
  uint32_t startByte = 0;
  uint32_t endByte = 0;
};

struct IntLit {
  std::string text;
};
struct FloatLit {
  std::string text;
};
struct BoolLit {
  bool value;
};
struct StringLit {
  std::string text; /** Includes quotes */
};

struct BinaryExpr {
  std::unique_ptr<Expr> left;
  std::string op;
  std::unique_ptr<Expr> right;
};

struct UnaryExpr {
  std::string op;
  std::unique_ptr<Expr> operand;
};

struct EntityTestExpr {
  std::string selectorText;
};

struct AtTestExpr {
  std::string blockText;
  std::string posText;
};

struct TernaryExpr {
  std::unique_ptr<Expr> condition;
  std::unique_ptr<Expr> ifTrue;
  std::unique_ptr<Expr> ifFalse;
};

struct MemberExpr {
  std::unique_ptr<Expr> object;
  std::string property;
  SourceLoc propertyLoc;
};

struct SliceExpr {
  std::unique_ptr<Expr> target;
  std::unique_ptr<Expr> start;
  std::unique_ptr<Expr> end;
};

struct ElementExpr {
  std::unique_ptr<Expr> target;
  std::unique_ptr<Expr> index;
};

struct CallExpr {
  std::string name;
  SourceLoc nameLoc;
  std::vector<std::unique_ptr<Expr>> arguments;
};

struct MethodCallExpr {
  std::unique_ptr<Expr> object;
  std::string method;
  SourceLoc methodLoc;
  std::vector<std::unique_ptr<Expr>> arguments;
};

struct VarRefExpr {
  std::string name;
};

struct CastExpr {
  std::unique_ptr<Expr> expression;
  std::string typeText;
  SourceLoc typeLoc;
};

struct StructExprField {
  std::string name;
  std::unique_ptr<Expr> value;
};
struct StructExpr {
  std::string name;
  std::vector<StructExprField> fields;
};

struct ListExpr {
  std::vector<std::unique_ptr<Expr>> elements;
};

struct ReferenceExpr {
  std::string targetName;
};

struct Expr {
  SourceLoc loc;
  std::variant<
    IntLit,
    FloatLit,
    BoolLit,
    StringLit,
    BinaryExpr,
    UnaryExpr,
    EntityTestExpr,
    AtTestExpr,
    TernaryExpr,
    MemberExpr,
    SliceExpr,
    ElementExpr,
    CallExpr,
    MethodCallExpr,
    VarRefExpr,
    CastExpr,
    StructExpr,
    ListExpr,
    ReferenceExpr>
    data;
};

struct IfStmt {
  std::unique_ptr<Expr> condition;
  std::unique_ptr<Block> thenBlock;
  std::optional<std::unique_ptr<Stmt>> elseBranch;
};

struct WhileStmt {
  std::unique_ptr<Expr> condition;
  std::unique_ptr<Block> body;
};

struct DoWhileStmt {
  std::unique_ptr<Block> body;
  std::unique_ptr<Expr> condition;
};

struct ForStmt {
  std::string iterator;
  SourceLoc iteratorLoc;
  std::unique_ptr<Expr> start;
  std::unique_ptr<Expr> end;
  std::unique_ptr<Block> body;
};

struct VarDeclStmt {
  bool isConst = false;
  bool isExport = false;
  bool isExtern = false;
  std::string name;
  SourceLoc nameLoc;
  std::optional<std::string> typeText;
  SourceLoc typeLoc;
  std::unique_ptr<Expr> value;
};

struct PathComponent {
  bool isIndex = false;
  std::string propertyName;
  SourceLoc loc;
  std::unique_ptr<Expr> index;
};
struct AssignStmt {
  std::string name;
  SourceLoc nameLoc;
  std::vector<PathComponent> path;
  std::unique_ptr<Expr> value;
};

struct Param {
  std::string name;
  SourceLoc loc;
  std::string typeText;
  SourceLoc typeLoc;
};
struct FuncDeclStmt {
  std::optional<std::string> tag;
  bool isExport = false;
  bool isExtern = false;
  std::string name;
  SourceLoc nameLoc;
  std::vector<Param> params;
  std::optional<std::string> returnTypeText;
  SourceLoc returnTypeLoc;
  std::unique_ptr<Block> body;
};

struct StructFieldDecl {
  std::string name;
  SourceLoc nameLoc;
  std::string typeText;
  SourceLoc typeLoc;
  bool isPrivate = false;
};

struct StructMethodDecl {
  SourceLoc loc;
  std::string name;
  SourceLoc nameLoc;
  bool isPrivate = false;
  bool isStatic = false;
  std::vector<Param> params;
  std::optional<std::string> returnTypeText;
  SourceLoc returnTypeLoc;
  std::unique_ptr<Block> body;
};

struct StructDeclStmt {
  bool isExport = false;
  std::string name;
  SourceLoc nameLoc;
  std::vector<StructFieldDecl> fields;
  std::vector<StructMethodDecl> methods;
};

struct EnumVariantDecl {
  std::string name;
  SourceLoc nameLoc;
  std::optional<std::unique_ptr<Expr>> value;
};
struct EnumDeclStmt {
  bool isExport = false;
  std::string name;
  SourceLoc nameLoc;
  std::vector<EnumVariantDecl> variants;
};

struct NamespaceStmt {
  std::string name;
  SourceLoc nameLoc;
  std::unique_ptr<Block> body;
};

struct ImportStmt {
  std::string path;
  std::optional<std::string> alias;
};

struct ReturnStmt {
  std::optional<std::unique_ptr<Expr>> value;
};

struct CommandPart {
  bool isInterpolation = false;
  std::string literalText;
  std::unique_ptr<Expr> interpExpr;
};
struct CommandStmt {
  std::string commandName;
  std::vector<CommandPart> parts;
};

struct ContextModifier {
  std::string keyword;
  std::string primaryText;
  std::optional<std::string> secondaryText;
};
struct ContextStmt {
  std::vector<ContextModifier> modifiers;
  std::unique_ptr<Block> body;
};

struct ExprStmt {
  std::unique_ptr<Expr> expr;
};

struct BlockStmt {
  std::unique_ptr<Block> block;
};

struct Stmt {
  SourceLoc loc;
  std::variant<
    IfStmt,
    WhileStmt,
    DoWhileStmt,
    ForStmt,
    VarDeclStmt,
    AssignStmt,
    FuncDeclStmt,
    StructDeclStmt,
    EnumDeclStmt,
    NamespaceStmt,
    ImportStmt,
    ReturnStmt,
    CommandStmt,
    ContextStmt,
    ExprStmt,
    BlockStmt>
    data;
};
