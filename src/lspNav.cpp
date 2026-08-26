#include "lspNav.hpp"
#include "lexer.hpp"
#include "parser.hpp"

#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace lspnav {
namespace {

bool inSpan(const SourceLoc &loc, uint32_t offset) { return offset >= loc.startByte && offset < loc.endByte; }

std::string formatParams(const std::vector<Param> &params) {
  std::string s;
  for (size_t i = 0; i < params.size(); i++) {
    if (i) s += ", ";
    s += params[i].name + ": " + params[i].typeText;
  }
  return s;
}

std::string wrap(const std::string &s) { return "```loom\n" + s + "\n```"; }

std::string baseTypeName(const std::string &raw) {
  size_t start = 0;
  while (start < raw.size() && (raw[start] == '&' || raw[start] == ' ')) start++;
  size_t end = raw.size();
  while (end > start && raw[end - 1] == ' ') end--;
  while (end > start + 1 && raw[end - 2] == '[' && raw[end - 1] == ']') {
    end -= 2;
    while (end > start && raw[end - 1] == ' ') end--;
  }
  return raw.substr(start, end - start);
}

template <typename T> struct Tagged {
  const T *decl;
  std::string file;
};

struct GlobalIndex {
  std::unordered_map<std::string, std::vector<Tagged<FuncDeclStmt>>> funcs;
  std::unordered_map<std::string, Tagged<StructDeclStmt>> structs;
  std::unordered_map<std::string, Tagged<EnumDeclStmt>> enums;
  std::unordered_map<std::string, const NamespaceStmt *> namespaces; // outline/local use only
};

void indexBlock(
  const Block &block,
  const std::string &file,
  const std::string &fileDir,
  std::vector<std::string> &nsPath,
  GlobalIndex &idx,
  const ImportLoader &loader,
  std::unordered_set<std::string> &visitedFiles,
  bool onlyExported
) {
  std::string prefix;
  for (const auto &p : nsPath) prefix += p + "::";

  for (const auto &stmtPtr : block.statements) {
    std::visit(
      [&](auto &&n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, FuncDeclStmt>) {
          if (onlyExported && !n.isExport) return;
          idx.funcs[prefix + n.name].push_back({&n, file});
          if (!prefix.empty()) idx.funcs[n.name].push_back({&n, file});
        } else if constexpr (std::is_same_v<T, StructDeclStmt>) {
          if (onlyExported && !n.isExport) return;
          idx.structs.emplace(prefix + n.name, Tagged<StructDeclStmt>{&n, file});
          if (!prefix.empty()) idx.structs.emplace(n.name, Tagged<StructDeclStmt>{&n, file});
        } else if constexpr (std::is_same_v<T, EnumDeclStmt>) {
          if (onlyExported && !n.isExport) return;
          idx.enums.emplace(prefix + n.name, Tagged<EnumDeclStmt>{&n, file});
          if (!prefix.empty()) idx.enums.emplace(n.name, Tagged<EnumDeclStmt>{&n, file});
        } else if constexpr (std::is_same_v<T, NamespaceStmt>) {
          idx.namespaces[prefix + n.name] = &n;
          nsPath.push_back(n.name);
          indexBlock(*n.body, file, fileDir, nsPath, idx, loader, visitedFiles, onlyExported);
          nsPath.pop_back();
        } else if constexpr (std::is_same_v<T, ImportStmt>) {
          if (!loader) return;
          std::string resolvedPath;
          const Block *importedBlock = loader(n.path, fileDir, resolvedPath);
          if (!importedBlock || resolvedPath.empty() || visitedFiles.contains(resolvedPath)) return;
          visitedFiles.insert(resolvedPath);

          size_t slash = resolvedPath.find_last_of("/\\");
          std::string importedDir = slash == std::string::npos ? "." : resolvedPath.substr(0, slash);

          GlobalIndex subIdx;
          std::vector<std::string> subNsPath;
          indexBlock(*importedBlock, resolvedPath, importedDir, subNsPath, subIdx, loader, visitedFiles, /*onlyExported=*/true);

          std::string aliasPrefix = n.alias ? (*n.alias + "::") : "";
          for (auto &[name, overloads] : subIdx.funcs) {
            for (auto &ref : overloads) idx.funcs[aliasPrefix + name].push_back(ref);
          }
          for (auto &[name, ref] : subIdx.structs) idx.structs.emplace(aliasPrefix + name, ref);
          for (auto &[name, ref] : subIdx.enums) idx.enums.emplace(aliasPrefix + name, ref);
        }
      },
      stmtPtr->data
    );
  }
}

GlobalIndex buildIndex(const Block &program, const std::string &fromDir, const ImportLoader &loader) {
  GlobalIndex idx;
  std::vector<std::string> nsPath;
  std::unordered_set<std::string> visited;
  indexBlock(program, "", fromDir, nsPath, idx, loader, visited, /*onlyExported=*/false);
  return idx;
}

const Tagged<StructDeclStmt> *lookupStruct(const GlobalIndex &idx, const std::string &name) {
  auto it = idx.structs.find(name);
  return it == idx.structs.end() ? nullptr : &it->second;
}
const Tagged<EnumDeclStmt> *lookupEnum(const GlobalIndex &idx, const std::string &name) {
  auto it = idx.enums.find(name);
  return it == idx.enums.end() ? nullptr : &it->second;
}
const StructMethodDecl *findMethod(const StructDeclStmt &s, const std::string &name) {
  for (const auto &m : s.methods) {
    if (m.name == name) return &m;
  }
  return nullptr;
}
const StructFieldDecl *findField(const StructDeclStmt &s, const std::string &name) {
  for (const auto &f : s.fields) {
    if (f.name == name) return &f;
  }
  return nullptr;
}
const EnumVariantDecl *findVariant(const EnumDeclStmt &e, const std::string &name) {
  for (const auto &v : e.variants) {
    if (v.name == name) return &v;
  }
  return nullptr;
}

struct WalkCtx {
  const StructDeclStmt *structCtx = nullptr;
  bool hasImplicitThis = false;
  std::vector<const Param *> params;
  std::vector<std::pair<std::string, SourceLoc>> forIterators;
  std::vector<const Block *> blocks;
};

struct VarResolution {
  enum class Kind { None, Param, ForIter, VarDecl, ImplicitThis, ImplicitField, ImplicitMethod } kind = Kind::None;
  const Param *param = nullptr;
  SourceLoc forIterLoc;
  const VarDeclStmt *varDecl = nullptr;
  const StructFieldDecl *field = nullptr;
  const StructMethodDecl *method = nullptr;
};

VarResolution resolveVar(const WalkCtx &ctx, const std::string &name) {
  for (auto it = ctx.params.rbegin(); it != ctx.params.rend(); ++it) {
    if ((*it)->name == name) return {.kind = VarResolution::Kind::Param, .param = *it};
  }
  for (auto it = ctx.forIterators.rbegin(); it != ctx.forIterators.rend(); ++it) {
    if (it->first == name) return {.kind = VarResolution::Kind::ForIter, .forIterLoc = it->second};
  }
  for (auto bit = ctx.blocks.rbegin(); bit != ctx.blocks.rend(); ++bit) {
    for (const auto &stmtPtr : (*bit)->statements) {
      if (const auto *vd = std::get_if<VarDeclStmt>(&stmtPtr->data)) {
        if (vd->name == name) return {.kind = VarResolution::Kind::VarDecl, .varDecl = vd};
      }
    }
  }
  if (name == "this" && ctx.hasImplicitThis) return {.kind = VarResolution::Kind::ImplicitThis};
  if (ctx.structCtx) {
    if (const StructFieldDecl *f = findField(*ctx.structCtx, name)) return {.kind = VarResolution::Kind::ImplicitField, .field = f};
    if (const StructMethodDecl *m = findMethod(*ctx.structCtx, name)) return {.kind = VarResolution::Kind::ImplicitMethod, .method = m};
  }
  return {};
}

std::optional<std::string> declaredTypeOf(const WalkCtx &ctx, const std::string &name) {
  VarResolution r = resolveVar(ctx, name);
  switch (r.kind) {
  case VarResolution::Kind::Param:
    return baseTypeName(r.param->typeText);
  case VarResolution::Kind::VarDecl:
    if (r.varDecl->typeText) return baseTypeName(*r.varDecl->typeText);
    return std::nullopt;
  case VarResolution::Kind::ForIter:
    return "int";
  case VarResolution::Kind::ImplicitThis:
    return ctx.structCtx->name;
  case VarResolution::Kind::ImplicitField:
    return baseTypeName(r.field->typeText);
  default:
    return std::nullopt;
  }
}

struct Resolved {
  SourceLoc targetLoc;
  std::string file;
  std::string hover;
};

std::optional<Resolved> resolveVarHover(const WalkCtx &ctx, const std::string &name) {
  VarResolution r = resolveVar(ctx, name);
  switch (r.kind) {
  case VarResolution::Kind::Param:
    return Resolved{.targetLoc = r.param->loc, .hover = wrap("(parameter) " + r.param->name + ": " + r.param->typeText)};
  case VarResolution::Kind::ForIter:
    return Resolved{.targetLoc = r.forIterLoc, .hover = wrap("(loop variable) " + name + ": int")};
  case VarResolution::Kind::VarDecl: {
    const VarDeclStmt &vd = *r.varDecl;
    std::string kw = vd.isConst ? "const" : "let";
    std::string ty = vd.typeText ? *vd.typeText : "(inferred)";
    return Resolved{.targetLoc = vd.nameLoc, .hover = wrap(kw + " " + vd.name + ": " + ty)};
  }
  case VarResolution::Kind::ImplicitThis:
    return Resolved{.targetLoc = ctx.structCtx->nameLoc, .hover = wrap("this: &" + ctx.structCtx->name + " (implicit)")};
  case VarResolution::Kind::ImplicitField: {
    const StructFieldDecl &f = *r.field;
    return Resolved{.targetLoc = f.nameLoc, .hover = wrap("(struct field, implicit self) " + ctx.structCtx->name + "." + f.name + ": " + f.typeText)};
  }
  case VarResolution::Kind::ImplicitMethod: {
    const StructMethodDecl &m = *r.method;
    std::string ret = m.returnTypeText ? (": " + *m.returnTypeText) : "";
    return Resolved{.targetLoc = m.nameLoc, .hover = wrap("func " + ctx.structCtx->name + "::" + m.name + "(" + formatParams(m.params) + ")" + ret + " (implicit self)")};
  }
  default:
    return std::nullopt;
  }
}

std::optional<Resolved> resolveCallName(const WalkCtx &ctx, const GlobalIndex &idx, const std::string &rawName) {
  std::string structPart, methodPart;
  size_t sep = rawName.rfind("::");
  bool qualified = sep != std::string::npos;
  if (qualified) {
    structPart = rawName.substr(0, sep);
    methodPart = rawName.substr(sep + 2);
  }

  if (qualified) {
    if (const Tagged<StructDeclStmt> *s = lookupStruct(idx, structPart)) {
      if (const StructMethodDecl *m = findMethod(*s->decl, methodPart)) {
        std::string kind = m->isStatic ? "static func" : "func";
        std::string ret = m->returnTypeText ? (": " + *m->returnTypeText) : "";
        return Resolved{.targetLoc = m->nameLoc, .file = s->file, .hover = wrap(kind + " " + s->decl->name + "::" + m->name + "(" + formatParams(m->params) + ")" + ret)};
      }
    }
    return std::nullopt;
  }

  if (ctx.structCtx) {
    if (const StructMethodDecl *m = findMethod(*ctx.structCtx, rawName)) {
      std::string ret = m->returnTypeText ? (": " + *m->returnTypeText) : "";
      return Resolved{.targetLoc = m->nameLoc, .hover = wrap("func " + ctx.structCtx->name + "::" + m->name + "(" + formatParams(m->params) + ")" + ret)};
    }
  }
  if (auto it = idx.funcs.find(rawName); it != idx.funcs.end() && !it->second.empty()) {
    const auto &ref = it->second.front();
    const FuncDeclStmt &f = *ref.decl;
    std::string ret = f.returnTypeText ? (": " + *f.returnTypeText) : "";
    std::string extra = it->second.size() > 1 ? (" (+" + std::to_string(it->second.size() - 1) + " more overload(s))") : "";
    return Resolved{.targetLoc = f.nameLoc, .file = ref.file, .hover = wrap("func " + f.name + "(" + formatParams(f.params) + ")" + ret) + extra};
  }
  if (const Tagged<StructDeclStmt> *s = lookupStruct(idx, rawName)) {
    if (const StructMethodDecl *ctor = findMethod(*s->decl, s->decl->name)) {
      return Resolved{.targetLoc = ctor->nameLoc, .file = s->file, .hover = wrap(s->decl->name + "(" + formatParams(ctor->params) + ")")};
    }
    return Resolved{.targetLoc = s->decl->nameLoc, .file = s->file, .hover = wrap("struct " + s->decl->name)};
  }
  return std::nullopt;
}

std::optional<std::string> staticTypeOf(const WalkCtx &ctx, const GlobalIndex &idx, const Expr &expr) {
  return std::visit(
    [&](auto &&n) -> std::optional<std::string> {
      using T = std::decay_t<decltype(n)>;
      if constexpr (std::is_same_v<T, VarRefExpr>) {
        return declaredTypeOf(ctx, n.name);
      } else if constexpr (std::is_same_v<T, MemberExpr>) {
        auto objTy = staticTypeOf(ctx, idx, *n.object);
        if (!objTy) return std::nullopt;
        const Tagged<StructDeclStmt> *s = lookupStruct(idx, *objTy);
        if (!s) return std::nullopt;
        const StructFieldDecl *f = findField(*s->decl, n.property);
        if (!f) return std::nullopt;
        return baseTypeName(f->typeText);
      } else if constexpr (std::is_same_v<T, MethodCallExpr>) {
        auto objTy = staticTypeOf(ctx, idx, *n.object);
        if (!objTy) return std::nullopt;
        const Tagged<StructDeclStmt> *s = lookupStruct(idx, *objTy);
        if (!s) return std::nullopt;
        const StructMethodDecl *m = findMethod(*s->decl, n.method);
        if (!m || !m->returnTypeText) return std::nullopt;
        return baseTypeName(*m->returnTypeText);
      } else if constexpr (std::is_same_v<T, CallExpr>) {
        size_t sep = n.name.rfind("::");
        if (sep != std::string::npos) {
          const Tagged<StructDeclStmt> *s = lookupStruct(idx, n.name.substr(0, sep));
          if (!s) return std::nullopt;
          const StructMethodDecl *m = findMethod(*s->decl, n.name.substr(sep + 2));
          if (!m || !m->returnTypeText) return std::nullopt;
          return baseTypeName(*m->returnTypeText);
        }
        if (const Tagged<StructDeclStmt> *s = lookupStruct(idx, n.name)) return s->decl->name;
        if (auto it = idx.funcs.find(n.name); it != idx.funcs.end() && !it->second.empty() && it->second.front().decl->returnTypeText) {
          return baseTypeName(*it->second.front().decl->returnTypeText);
        }
        return std::nullopt;
      } else if constexpr (std::is_same_v<T, CastExpr>) {
        return baseTypeName(n.typeText);
      } else {
        return std::nullopt;
      }
    },
    expr.data
  );
}

std::optional<Resolved> resolveMember(const WalkCtx &ctx, const GlobalIndex &idx, const Expr &object, const std::string &prop) {
  if (const auto *vr = std::get_if<VarRefExpr>(&object.data)) {
    if (const Tagged<EnumDeclStmt> *e = lookupEnum(idx, vr->name)) {
      if (const EnumVariantDecl *v = findVariant(*e->decl, prop)) {
        std::string valStr = v->value ? (" = " + exprToString(**v->value)) : "";
        return Resolved{.targetLoc = v->nameLoc, .file = e->file, .hover = wrap("(enum variant) " + e->decl->name + "." + v->name + valStr)};
      }
      return std::nullopt;
    }
  }
  std::optional<std::string> ty = staticTypeOf(ctx, idx, object);
  if (!ty) return std::nullopt;
  if (const Tagged<StructDeclStmt> *s = lookupStruct(idx, *ty)) {
    if (const StructFieldDecl *f = findField(*s->decl, prop)) {
      return Resolved{.targetLoc = f->nameLoc, .file = s->file, .hover = wrap("(struct field) " + s->decl->name + "." + f->name + ": " + f->typeText)};
    }
  }
  return std::nullopt;
}

std::optional<Resolved> resolveMethodCall(const WalkCtx &ctx, const GlobalIndex &idx, const Expr &object, const std::string &method) {
  std::optional<std::string> ty = staticTypeOf(ctx, idx, object);
  if (!ty) return std::nullopt;
  const Tagged<StructDeclStmt> *s = lookupStruct(idx, *ty);
  if (!s) return std::nullopt;
  const StructMethodDecl *m = findMethod(*s->decl, method);
  if (!m) return std::nullopt;
  std::string kind = m->isStatic ? "static func" : "func";
  std::string ret = m->returnTypeText ? (": " + *m->returnTypeText) : "";
  return Resolved{.targetLoc = m->nameLoc, .file = s->file, .hover = wrap(kind + " " + s->decl->name + "::" + m->name + "(" + formatParams(m->params) + ")" + ret)};
}

std::optional<Resolved> resolveTypeRef(const GlobalIndex &idx, const std::string &rawType, SourceLoc loc) {
  std::string base = baseTypeName(rawType);
  if (const Tagged<StructDeclStmt> *s = lookupStruct(idx, base)) {
    return Resolved{.targetLoc = s->decl->nameLoc, .file = s->file, .hover = wrap("struct " + s->decl->name)};
  }
  if (const Tagged<EnumDeclStmt> *e = lookupEnum(idx, base)) {
    return Resolved{.targetLoc = e->decl->nameLoc, .file = e->file, .hover = wrap("enum " + e->decl->name)};
  }
  static const std::unordered_set<std::string> primitives = {"int", "float", "bool", "string"};
  if (primitives.count(base)) return Resolved{.targetLoc = loc, .hover = wrap("type " + base)};
  return std::nullopt;
}

bool walkExpr(const Expr &expr, WalkCtx &ctx, const GlobalIndex &idx, uint32_t offset, std::optional<Resolved> &out);

bool walkBlock(const Block &block, WalkCtx ctx, const GlobalIndex &idx, uint32_t offset, std::optional<Resolved> &out);

bool walkStmt(const Stmt &stmt, WalkCtx ctx, const GlobalIndex &idx, uint32_t offset, std::optional<Resolved> &out) {
  return std::visit(
    [&](auto &&n) -> bool {
      using T = std::decay_t<decltype(n)>;
      if constexpr (std::is_same_v<T, IfStmt>) {
        if (walkExpr(*n.condition, ctx, idx, offset, out)) return true;
        if (walkBlock(*n.thenBlock, ctx, idx, offset, out)) return true;
        if (n.elseBranch && walkStmt(**n.elseBranch, ctx, idx, offset, out)) return true;
        return false;
      } else if constexpr (std::is_same_v<T, WhileStmt>) {
        if (walkExpr(*n.condition, ctx, idx, offset, out)) return true;
        return walkBlock(*n.body, ctx, idx, offset, out);
      } else if constexpr (std::is_same_v<T, DoWhileStmt>) {
        if (walkBlock(*n.body, ctx, idx, offset, out)) return true;
        return walkExpr(*n.condition, ctx, idx, offset, out);
      } else if constexpr (std::is_same_v<T, ForStmt>) {
        if (inSpan(n.iteratorLoc, offset)) {
          out = Resolved{.targetLoc = n.iteratorLoc, .hover = wrap("(loop variable) " + n.iterator + ": int")};
          return true;
        }
        if (walkExpr(*n.start, ctx, idx, offset, out)) return true;
        if (walkExpr(*n.end, ctx, idx, offset, out)) return true;
        ctx.forIterators.emplace_back(n.iterator, n.iteratorLoc);
        bool found = walkBlock(*n.body, ctx, idx, offset, out);
        ctx.forIterators.pop_back();
        return found;
      } else if constexpr (std::is_same_v<T, VarDeclStmt>) {
        if (inSpan(n.nameLoc, offset)) {
          std::string kw = n.isConst ? "const" : "let";
          std::string ty = n.typeText ? *n.typeText : "(inferred)";
          out = Resolved{.targetLoc = n.nameLoc, .hover = wrap(kw + " " + n.name + ": " + ty)};
          return true;
        }
        if (n.typeText && inSpan(n.typeLoc, offset)) {
          if (auto r = resolveTypeRef(idx, *n.typeText, n.typeLoc)) {
            out = r;
            return true;
          }
        }
        return walkExpr(*n.value, ctx, idx, offset, out);
      } else if constexpr (std::is_same_v<T, AssignStmt>) {
        if (inSpan(n.nameLoc, offset)) {
          if (auto r = resolveVarHover(ctx, n.name)) {
            out = r;
            return true;
          }
        }

        std::optional<std::string> curTy = declaredTypeOf(ctx, n.name);
        for (const auto &pc : n.path) {
          if (!pc.isIndex) {
            if (curTy) {
              if (const Tagged<StructDeclStmt> *s = lookupStruct(idx, *curTy)) {
                if (const StructFieldDecl *f = findField(*s->decl, pc.propertyName)) {
                  if (inSpan(pc.loc, offset)) {
                    out = Resolved{.targetLoc = f->nameLoc, .file = s->file, .hover = wrap("(struct field) " + s->decl->name + "." + f->name + ": " + f->typeText)};
                    return true;
                  }
                  curTy = baseTypeName(f->typeText);
                } else {
                  curTy.reset();
                }
              } else {
                curTy.reset();
              }
            }
          } else {
            if (pc.index && walkExpr(*pc.index, ctx, idx, offset, out)) return true;
          }
        }
        return walkExpr(*n.value, ctx, idx, offset, out);
      } else if constexpr (std::is_same_v<T, FuncDeclStmt>) {
        if (inSpan(n.nameLoc, offset)) {
          std::string ret = n.returnTypeText ? (": " + *n.returnTypeText) : "";
          out = Resolved{.targetLoc = n.nameLoc, .hover = wrap("func " + n.name + "(" + formatParams(n.params) + ")" + ret)};
          return true;
        }
        for (const auto &p : n.params) {
          if (inSpan(p.loc, offset)) {
            out = Resolved{.targetLoc = p.loc, .hover = wrap("(parameter) " + p.name + ": " + p.typeText)};
            return true;
          }
          if (inSpan(p.typeLoc, offset)) {
            if (auto r = resolveTypeRef(idx, p.typeText, p.typeLoc)) {
              out = r;
              return true;
            }
          }
        }
        if (n.returnTypeText && inSpan(n.returnTypeLoc, offset)) {
          if (auto r = resolveTypeRef(idx, *n.returnTypeText, n.returnTypeLoc)) {
            out = r;
            return true;
          }
        }
        WalkCtx inner;
        inner.params.reserve(n.params.size());
        for (const auto &p : n.params) inner.params.push_back(&p);
        return walkBlock(*n.body, inner, idx, offset, out);
      } else if constexpr (std::is_same_v<T, StructDeclStmt>) {
        if (inSpan(n.nameLoc, offset)) {
          out = Resolved{.targetLoc = n.nameLoc, .hover = wrap("struct " + n.name)};
          return true;
        }
        for (const auto &f : n.fields) {
          if (inSpan(f.nameLoc, offset)) {
            std::string vis = f.isPrivate ? "private " : "";
            out = Resolved{.targetLoc = f.nameLoc, .hover = wrap(vis + "(struct field) " + n.name + "." + f.name + ": " + f.typeText)};
            return true;
          }
          if (inSpan(f.typeLoc, offset)) {
            if (auto r = resolveTypeRef(idx, f.typeText, f.typeLoc)) {
              out = r;
              return true;
            }
          }
        }
        for (const auto &m : n.methods) {
          if (inSpan(m.nameLoc, offset)) {
            std::string vis = m.isPrivate ? "private " : "";
            std::string kind = m.isStatic ? "static func" : "func";
            std::string ret = m.returnTypeText ? (": " + *m.returnTypeText) : "";
            out = Resolved{.targetLoc = m.nameLoc, .hover = wrap(vis + kind + " " + n.name + "::" + m.name + "(" + formatParams(m.params) + ")" + ret)};
            return true;
          }
          for (const auto &p : m.params) {
            if (inSpan(p.loc, offset)) {
              out = Resolved{.targetLoc = p.loc, .hover = wrap("(parameter) " + p.name + ": " + p.typeText)};
              return true;
            }
            if (inSpan(p.typeLoc, offset)) {
              if (auto r = resolveTypeRef(idx, p.typeText, p.typeLoc)) {
                out = r;
                return true;
              }
            }
          }
          if (m.returnTypeText && inSpan(m.returnTypeLoc, offset)) {
            if (auto r = resolveTypeRef(idx, *m.returnTypeText, m.returnTypeLoc)) {
              out = r;
              return true;
            }
          }
          WalkCtx inner;
          inner.structCtx = &n;
          inner.hasImplicitThis = true;
          inner.params.reserve(m.params.size());
          for (const auto &p : m.params) inner.params.push_back(&p);
          if (walkBlock(*m.body, inner, idx, offset, out)) return true;
        }
        return false;
      } else if constexpr (std::is_same_v<T, EnumDeclStmt>) {
        if (inSpan(n.nameLoc, offset)) {
          out = Resolved{.targetLoc = n.nameLoc, .hover = wrap("enum " + n.name)};
          return true;
        }
        for (const auto &v : n.variants) {
          if (inSpan(v.nameLoc, offset)) {
            std::string valStr = v.value ? (" = " + exprToString(**v.value)) : "";
            out = Resolved{.targetLoc = v.nameLoc, .hover = wrap("(enum variant) " + n.name + "." + v.name + valStr)};
            return true;
          }
        }
        return false;
      } else if constexpr (std::is_same_v<T, NamespaceStmt>) {
        if (inSpan(n.nameLoc, offset)) {
          out = Resolved{.targetLoc = n.nameLoc, .hover = wrap("namespace " + n.name)};
          return true;
        }
        return walkBlock(*n.body, ctx, idx, offset, out);
      } else if constexpr (std::is_same_v<T, ReturnStmt>) {
        return n.value && walkExpr(**n.value, ctx, idx, offset, out);
      } else if constexpr (std::is_same_v<T, CommandStmt>) {
        for (const auto &part : n.parts) {
          if (part.isInterpolation && part.interpExpr && walkExpr(*part.interpExpr, ctx, idx, offset, out)) return true;
        }
        return false;
      } else if constexpr (std::is_same_v<T, ContextStmt>) {
        return walkBlock(*n.body, ctx, idx, offset, out);
      } else if constexpr (std::is_same_v<T, ExprStmt>) {
        return walkExpr(*n.expr, ctx, idx, offset, out);
      } else if constexpr (std::is_same_v<T, BlockStmt>) {
        return walkBlock(*n.block, ctx, idx, offset, out);
      } else {
        return false;
      }
    },
    stmt.data
  );
}

bool walkBlock(const Block &block, WalkCtx ctx, const GlobalIndex &idx, uint32_t offset, std::optional<Resolved> &out) {
  ctx.blocks.push_back(&block);
  for (const auto &stmtPtr : block.statements) {
    if (walkStmt(*stmtPtr, ctx, idx, offset, out)) return true;
  }
  return false;
}

bool walkExpr(const Expr &expr, WalkCtx &ctx, const GlobalIndex &idx, uint32_t offset, std::optional<Resolved> &out) {
  return std::visit(
    [&](auto &&n) -> bool {
      using T = std::decay_t<decltype(n)>;
      if constexpr (std::is_same_v<T, BinaryExpr>) {
        return walkExpr(*n.left, ctx, idx, offset, out) || walkExpr(*n.right, ctx, idx, offset, out);
      } else if constexpr (std::is_same_v<T, UnaryExpr>) {
        return walkExpr(*n.operand, ctx, idx, offset, out);
      } else if constexpr (std::is_same_v<T, TernaryExpr>) {
        return walkExpr(*n.condition, ctx, idx, offset, out) || walkExpr(*n.ifTrue, ctx, idx, offset, out) || walkExpr(*n.ifFalse, ctx, idx, offset, out);
      } else if constexpr (std::is_same_v<T, MemberExpr>) {
        if (walkExpr(*n.object, ctx, idx, offset, out)) return true;
        if (inSpan(n.propertyLoc, offset)) {
          if (auto r = resolveMember(ctx, idx, *n.object, n.property)) {
            out = r;
            return true;
          }
        }
        return false;
      } else if constexpr (std::is_same_v<T, SliceExpr>) {
        return walkExpr(*n.target, ctx, idx, offset, out) || (n.start && walkExpr(*n.start, ctx, idx, offset, out)) || (n.end && walkExpr(*n.end, ctx, idx, offset, out));
      } else if constexpr (std::is_same_v<T, ElementExpr>) {
        return walkExpr(*n.target, ctx, idx, offset, out) || walkExpr(*n.index, ctx, idx, offset, out);
      } else if constexpr (std::is_same_v<T, CallExpr>) {
        for (const auto &a : n.arguments) {
          if (walkExpr(*a, ctx, idx, offset, out)) return true;
        }
        if (inSpan(n.nameLoc, offset)) {
          if (auto r = resolveCallName(ctx, idx, n.name)) {
            out = r;
            return true;
          }
        }
        return false;
      } else if constexpr (std::is_same_v<T, MethodCallExpr>) {
        if (walkExpr(*n.object, ctx, idx, offset, out)) return true;
        for (const auto &a : n.arguments) {
          if (walkExpr(*a, ctx, idx, offset, out)) return true;
        }
        if (inSpan(n.methodLoc, offset)) {
          if (auto r = resolveMethodCall(ctx, idx, *n.object, n.method)) {
            out = r;
            return true;
          }
        }
        return false;
      } else if constexpr (std::is_same_v<T, VarRefExpr>) {
        if (inSpan(expr.loc, offset)) {
          if (auto r = resolveVarHover(ctx, n.name)) {
            out = r;
            return true;
          }
        }
        return false;
      } else if constexpr (std::is_same_v<T, CastExpr>) {
        if (walkExpr(*n.expression, ctx, idx, offset, out)) return true;
        if (inSpan(n.typeLoc, offset)) {
          if (auto r = resolveTypeRef(idx, n.typeText, n.typeLoc)) {
            out = r;
            return true;
          }
        }
        return false;
      } else if constexpr (std::is_same_v<T, StructExpr>) {
        for (const auto &f : n.fields) {
          if (f.value && walkExpr(*f.value, ctx, idx, offset, out)) return true;
        }
        return false;
      } else if constexpr (std::is_same_v<T, ListExpr>) {
        for (const auto &e : n.elements) {
          if (walkExpr(*e, ctx, idx, offset, out)) return true;
        }
        return false;
      } else {
        return false;
      }
    },
    expr.data
  );
}

std::optional<Resolved> findResolved(const Block &program, const std::string &fromDir, const ImportLoader &loader, uint32_t offset) {
  GlobalIndex idx = buildIndex(program, fromDir, loader);
  std::optional<Resolved> found;
  WalkCtx ctx;
  walkBlock(program, ctx, idx, offset, found);
  return found;
}

} // namespace

std::optional<NavResult> findDefinition(const Block &program, const std::string &fromDir, const ImportLoader &loader, uint32_t offset) {
  auto found = findResolved(program, fromDir, loader, offset);
  if (!found) return std::nullopt;
  return NavResult{.targetLoc = found->targetLoc, .file = found->file};
}

std::optional<NavResult> findHover(const Block &program, const std::string &fromDir, const ImportLoader &loader, uint32_t offset, std::string &outHover) {
  auto found = findResolved(program, fromDir, loader, offset);
  if (!found) return std::nullopt;
  outHover = found->hover;
  return NavResult{.targetLoc = found->targetLoc, .file = found->file};
}

namespace {

void collectSymbols(const Block &block, std::vector<SymbolEntry> &out) {
  for (const auto &stmtPtr : block.statements) {
    std::visit(
      [&](auto &&n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, FuncDeclStmt>) {
          out.push_back(SymbolEntry{.name = n.name, .kind = "function", .loc = n.nameLoc});
        } else if constexpr (std::is_same_v<T, StructDeclStmt>) {
          SymbolEntry entry{.name = n.name, .kind = "struct", .loc = n.nameLoc};
          for (const auto &f : n.fields) entry.children.push_back(SymbolEntry{.name = f.name, .kind = "field", .loc = f.nameLoc});
          for (const auto &m : n.methods) entry.children.push_back(SymbolEntry{.name = m.name, .kind = "method", .loc = m.nameLoc});
          out.push_back(std::move(entry));
        } else if constexpr (std::is_same_v<T, EnumDeclStmt>) {
          SymbolEntry entry{.name = n.name, .kind = "enum", .loc = n.nameLoc};
          for (const auto &v : n.variants) entry.children.push_back(SymbolEntry{.name = v.name, .kind = "enum-member", .loc = v.nameLoc});
          out.push_back(std::move(entry));
        } else if constexpr (std::is_same_v<T, NamespaceStmt>) {
          SymbolEntry entry{.name = n.name, .kind = "namespace", .loc = n.nameLoc};
          collectSymbols(*n.body, entry.children);
          out.push_back(std::move(entry));
        }
      },
      stmtPtr->data
    );
  }
}

const char *KEYWORDS[] = {"let",     "const",  "struct", "enum", "func",   "if",       "else",      "while",      "do",      "for",
                          "in",      "return", "import", "as",   "export", "extern",   "namespace", "true",       "false",   "public",
                          "private", "static", "this",   "at",   "align",  "anchored", "facing",    "positioned", "rotated", "on"};

const char *PRIMITIVE_TYPES[] = {"int", "float", "bool", "string"};

enum class CompletionContext { Name, Type, MemberAccess, ScopeAccess, Expression };

struct PositionAnalysis {
  CompletionContext ctx = CompletionContext::Expression;
  std::vector<std::string> chain;
};

std::vector<Token> tokensBefore(const std::string &text, uint32_t offset) {
  Lexer lexer(text);
  std::vector<Token> all = lexer.tokenize();

  std::vector<Token> before;
  for (const Token &t : all) {
    if (t.kind == TokenKind::Newline || t.kind == TokenKind::EndOfFile) continue;
    if (t.startByte >= offset) break;
    before.push_back(t);
  }
  if (!before.empty() && before.back().endByte >= offset) before.pop_back();
  return before;
}

enum class EnclosureKind { None, StructBody, EnumBody, ParamList, StructLiteral, Other };

EnclosureKind classifyOpener(const std::vector<Token> &toks, size_t openIdx) {
  if (toks[openIdx].kind == TokenKind::LBrace) {
    if (openIdx == 0 || toks[openIdx - 1].kind != TokenKind::Identifier) return EnclosureKind::Other;
    if (openIdx >= 2 && toks[openIdx - 2].kind == TokenKind::KwStruct) return EnclosureKind::StructBody;
    if (openIdx >= 2 && toks[openIdx - 2].kind == TokenKind::KwEnum) return EnclosureKind::EnumBody;
    return EnclosureKind::StructLiteral;
  }
  if (toks[openIdx].kind == TokenKind::LParen) {
    if (openIdx >= 2 && toks[openIdx - 1].kind == TokenKind::Identifier && toks[openIdx - 2].kind == TokenKind::KwFunc) return EnclosureKind::ParamList;
    return EnclosureKind::Other;
  }
  return EnclosureKind::None;
}

EnclosureKind enclosingConstruct(const std::vector<Token> &toks, size_t fromIdxExclusive) {
  std::vector<TokenKind> stack;
  for (size_t i = fromIdxExclusive; i-- > 0;) {
    TokenKind k = toks[i].kind;
    if (k == TokenKind::RParen || k == TokenKind::RBracket || k == TokenKind::RBrace) {
      stack.push_back(k);
    } else if (k == TokenKind::LParen) {
      if (!stack.empty() && stack.back() == TokenKind::RParen) stack.pop_back();
      else return classifyOpener(toks, i);
    } else if (k == TokenKind::LBracket) {
      if (!stack.empty() && stack.back() == TokenKind::RBracket) stack.pop_back();
      else return EnclosureKind::Other;
    } else if (k == TokenKind::LBrace) {
      if (!stack.empty() && stack.back() == TokenKind::RBrace) stack.pop_back();
      else return classifyOpener(toks, i);
    }
  }
  return EnclosureKind::None;
}

std::vector<std::string> readChain(const std::vector<Token> &before, TokenKind sepKind) {
  std::vector<std::string> chain;
  int idx = static_cast<int>(before.size()) - 1;
  while (idx >= 1 && before[idx].kind == sepKind && before[idx - 1].kind == TokenKind::Identifier) {
    chain.insert(chain.begin(), std::string(before[idx - 1].text));
    idx -= 2;
  }
  return chain;
}

PositionAnalysis analyzePosition(const std::string &text, uint32_t offset) {
  PositionAnalysis result;
  std::vector<Token> before = tokensBefore(text, offset);
  if (before.empty()) return result;

  const Token &prev = before.back();

  switch (prev.kind) {
  case TokenKind::KwLet:
  case TokenKind::KwConst:
  case TokenKind::KwFunc:
  case TokenKind::KwStruct:
  case TokenKind::KwEnum:
  case TokenKind::KwNamespace:
  case TokenKind::KwAs:
  case TokenKind::KwFor:
    result.ctx = CompletionContext::Name;
    return result;
  default:
    break;
  }

  if (prev.kind == TokenKind::Dot) {
    std::vector<std::string> chain = readChain(before, TokenKind::Dot);
    if (!chain.empty()) {
      result.ctx = CompletionContext::MemberAccess;
      result.chain = std::move(chain);
    }
    return result;
  }

  if (prev.kind == TokenKind::ColonColon) {
    std::vector<std::string> chain = readChain(before, TokenKind::ColonColon);
    if (!chain.empty()) {
      result.ctx = CompletionContext::ScopeAccess;
      result.chain = std::move(chain);
    }
    return result;
  }

  if (prev.kind == TokenKind::Colon) {
    if (enclosingConstruct(before, before.size() - 1) == EnclosureKind::StructLiteral) return result;

    int depth = 0;
    for (auto it = before.rbegin() + 1; it != before.rend(); ++it) {
      TokenKind k = it->kind;
      if (k == TokenKind::Semicolon || k == TokenKind::LBrace || k == TokenKind::RBrace) break;
      if (k == TokenKind::Colon) depth++;
      else if (k == TokenKind::Question) {
        if (depth == 0) return result;
        depth--;
      }
    }
    result.ctx = CompletionContext::Type;
    return result;
  }

  EnclosureKind enc = enclosingConstruct(before, before.size());
  if (enc == EnclosureKind::ParamList || enc == EnclosureKind::StructBody || enc == EnclosureKind::EnumBody) result.ctx = CompletionContext::Name;
  return result;
}

void scopeAtStmt(const Stmt &stmt, WalkCtx ctx, uint32_t offset, WalkCtx &result);

void scopeAtBlock(const Block &block, WalkCtx ctx, uint32_t offset, WalkCtx &result) {
  if (offset < block.startByte || offset > block.endByte) return;
  ctx.blocks.push_back(&block);
  result = ctx;
  for (const auto &stmtPtr : block.statements) scopeAtStmt(*stmtPtr, ctx, offset, result);
}

void scopeAtStmt(const Stmt &stmt, WalkCtx ctx, uint32_t offset, WalkCtx &result) {
  std::visit(
    [&](auto &&n) {
      using T = std::decay_t<decltype(n)>;
      if constexpr (std::is_same_v<T, IfStmt>) {
        scopeAtBlock(*n.thenBlock, ctx, offset, result);
        if (n.elseBranch) scopeAtStmt(**n.elseBranch, ctx, offset, result);
      } else if constexpr (std::is_same_v<T, WhileStmt>) {
        scopeAtBlock(*n.body, ctx, offset, result);
      } else if constexpr (std::is_same_v<T, DoWhileStmt>) {
        scopeAtBlock(*n.body, ctx, offset, result);
      } else if constexpr (std::is_same_v<T, ForStmt>) {
        WalkCtx inner = ctx;
        inner.forIterators.emplace_back(n.iterator, n.iteratorLoc);
        scopeAtBlock(*n.body, inner, offset, result);
      } else if constexpr (std::is_same_v<T, FuncDeclStmt>) {
        WalkCtx inner;
        inner.params.reserve(n.params.size());
        for (const auto &p : n.params) inner.params.push_back(&p);
        scopeAtBlock(*n.body, inner, offset, result);
      } else if constexpr (std::is_same_v<T, StructDeclStmt>) {
        for (const auto &m : n.methods) {
          WalkCtx inner;
          inner.structCtx = &n;
          inner.hasImplicitThis = true;
          inner.params.reserve(m.params.size());
          for (const auto &p : m.params) inner.params.push_back(&p);
          scopeAtBlock(*m.body, inner, offset, result);
        }
      } else if constexpr (std::is_same_v<T, NamespaceStmt>) {
        scopeAtBlock(*n.body, ctx, offset, result);
      } else if constexpr (std::is_same_v<T, ContextStmt>) {
        scopeAtBlock(*n.body, ctx, offset, result);
      } else if constexpr (std::is_same_v<T, BlockStmt>) {
        scopeAtBlock(*n.block, ctx, offset, result);
      }
    },
    stmt.data
  );
}

WalkCtx scopeAt(const Block &program, uint32_t offset) {
  WalkCtx result;
  scopeAtBlock(program, WalkCtx{}, offset, result);
  return result;
}

void addFieldsAndMethods(const StructDeclStmt &s, bool showPrivate, bool wantStatic, std::vector<CompletionEntry> &items) {
  if (!wantStatic) {
    for (const auto &f : s.fields) {
      if (f.isPrivate && !showPrivate) continue;
      items.push_back(CompletionEntry{.label = f.name, .kind = "field", .detail = ": " + f.typeText});
    }
  }
  for (const auto &m : s.methods) {
    if (m.isPrivate && !showPrivate) continue;
    if (m.isStatic != wantStatic) continue;
    std::string ret = m.returnTypeText ? (": " + *m.returnTypeText) : "";
    items.push_back(CompletionEntry{.label = m.name, .kind = "method", .detail = "(" + formatParams(m.params) + ")" + ret});
  }
}

void addLocalScopeCompletions(const WalkCtx &scope, std::vector<CompletionEntry> &items) {
  std::unordered_set<std::string> seen;
  for (const auto *p : scope.params) {
    if (seen.insert(p->name).second) items.push_back(CompletionEntry{.label = p->name, .kind = "variable", .detail = ": " + p->typeText});
  }
  for (const auto &fi : scope.forIterators) {
    if (seen.insert(fi.first).second) items.push_back(CompletionEntry{.label = fi.first, .kind = "variable", .detail = ": int"});
  }
  for (auto it = scope.blocks.rbegin(); it != scope.blocks.rend(); ++it) {
    for (const auto &stmtPtr : (*it)->statements) {
      if (const auto *vd = std::get_if<VarDeclStmt>(&stmtPtr->data)) {
        if (seen.insert(vd->name).second) {
          std::string detail = vd->typeText ? (": " + *vd->typeText) : "";
          items.push_back(CompletionEntry{.label = vd->name, .kind = "variable", .detail = detail});
        }
      }
    }
  }
  if (scope.hasImplicitThis && seen.insert("this").second) {
    items.push_back(CompletionEntry{.label = "this", .kind = "variable", .detail = ": &" + scope.structCtx->name});
  }
  if (scope.structCtx) addFieldsAndMethods(*scope.structCtx, /*showPrivate=*/true, /*includeStatic=*/false, items);
}

} // namespace

std::vector<SymbolEntry> documentSymbols(const Block &program) {
  std::vector<SymbolEntry> out;
  collectSymbols(program, out);
  return out;
}

std::vector<CompletionEntry> completionItems(const Block &program, const std::string &text, const std::string &fromDir, const ImportLoader &loader, uint32_t offset) {
  std::vector<CompletionEntry> items;

  PositionAnalysis pos = analyzePosition(text, offset);

  if (pos.ctx == CompletionContext::Name) return items;

  GlobalIndex idx = buildIndex(program, fromDir, loader);

  if (pos.ctx == CompletionContext::Type) {
    for (const char *ty : PRIMITIVE_TYPES) items.push_back(CompletionEntry{.label = ty, .kind = "keyword", .detail = ""});
    for (const auto &[name, s] : idx.structs) items.push_back(CompletionEntry{.label = name, .kind = "struct", .detail = "struct"});
    for (const auto &[name, e] : idx.enums) items.push_back(CompletionEntry{.label = name, .kind = "enum", .detail = "enum"});
    for (const auto &[name, ns] : idx.namespaces) items.push_back(CompletionEntry{.label = name, .kind = "namespace", .detail = "namespace"});
    return items;
  }

  if (pos.ctx == CompletionContext::MemberAccess) {
    WalkCtx scope = scopeAt(program, offset);
    const std::string &base = pos.chain.front();

    std::optional<std::string> curType;
    if (base == "this" && scope.hasImplicitThis) curType = scope.structCtx->name;
    else curType = declaredTypeOf(scope, base);

    if (!curType) {
      if (pos.chain.size() == 1) {
        if (const Tagged<EnumDeclStmt> *e = lookupEnum(idx, base)) {
          for (const auto &v : e->decl->variants) items.push_back(CompletionEntry{.label = v.name, .kind = "enum-member", .detail = ""});
        }
      }
      return items;
    }

    bool ok = true;
    for (size_t i = 1; i < pos.chain.size() && ok; i++) {
      const Tagged<StructDeclStmt> *s = lookupStruct(idx, *curType);
      const StructFieldDecl *f = s ? findField(*s->decl, pos.chain[i]) : nullptr;
      if (!f) {
        ok = false;
        break;
      }
      curType = baseTypeName(f->typeText);
    }

    if (ok) {
      if (const Tagged<StructDeclStmt> *s = lookupStruct(idx, *curType)) {
        bool showPrivate = scope.structCtx && scope.structCtx->name == s->decl->name;
        addFieldsAndMethods(*s->decl, showPrivate, /*includeStatic=*/false, items);
      }
    }
    return items;
  }

  if (pos.ctx == CompletionContext::ScopeAccess) {
    std::string joined;
    for (size_t i = 0; i < pos.chain.size(); i++) {
      if (i) joined += "::";
      joined += pos.chain[i];
    }

    if (auto nsIt = idx.namespaces.find(joined); nsIt != idx.namespaces.end()) {
      for (const auto &stmtPtr : nsIt->second->body->statements) {
        std::visit(
          [&](auto &&n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, FuncDeclStmt>) {
              std::string ret = n.returnTypeText ? (": " + *n.returnTypeText) : "";
              items.push_back(CompletionEntry{.label = n.name, .kind = "function", .detail = "(" + formatParams(n.params) + ")" + ret});
            } else if constexpr (std::is_same_v<T, StructDeclStmt>) {
              items.push_back(CompletionEntry{.label = n.name, .kind = "struct", .detail = "struct"});
            } else if constexpr (std::is_same_v<T, EnumDeclStmt>) {
              items.push_back(CompletionEntry{.label = n.name, .kind = "enum", .detail = "enum"});
            } else if constexpr (std::is_same_v<T, NamespaceStmt>) {
              items.push_back(CompletionEntry{.label = n.name, .kind = "namespace", .detail = "namespace"});
            }
          },
          stmtPtr->data
        );
      }
      return items;
    }

    if (const Tagged<StructDeclStmt> *s = lookupStruct(idx, joined)) addFieldsAndMethods(*s->decl, /*showPrivate=*/false, /*includeStatic=*/true, items);
    return items;
  }

  for (const char *kw : KEYWORDS) items.push_back(CompletionEntry{.label = kw, .kind = "keyword", .detail = ""});

  addLocalScopeCompletions(scopeAt(program, offset), items);

  for (const auto &[name, overloads] : idx.funcs) {
    if (overloads.empty()) continue;
    const FuncDeclStmt &f = *overloads.front().decl;
    std::string ret = f.returnTypeText ? (": " + *f.returnTypeText) : "";
    items.push_back(CompletionEntry{.label = name, .kind = "function", .detail = "(" + formatParams(f.params) + ")" + ret});
  }
  for (const auto &[name, s] : idx.structs) items.push_back(CompletionEntry{.label = name, .kind = "struct", .detail = "struct"});
  for (const auto &[name, e] : idx.enums) items.push_back(CompletionEntry{.label = name, .kind = "enum", .detail = "enum"});
  for (const auto &[name, ns] : idx.namespaces) items.push_back(CompletionEntry{.label = name, .kind = "namespace", .detail = "namespace"});

  return items;
}

} // namespace lspnav
