#pragma once

#include "ast.hpp"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lspnav {

using ImportLoader = std::function<const Block *(const std::string &importPath, const std::string &fromDir, std::string &outResolvedPath)>;

struct NavResult {
  SourceLoc targetLoc;
  std::string file;
};

struct SymbolEntry {
  std::string name;
  std::string kind;
  SourceLoc loc;
  std::vector<SymbolEntry> children;
};

struct CompletionEntry {
  std::string label;
  std::string kind;
  std::string detail;
};

std::optional<NavResult> findDefinition(const Block &program, const std::string &fromDir, const ImportLoader &loader, uint32_t offset);
std::optional<NavResult> findHover(const Block &program, const std::string &fromDir, const ImportLoader &loader, uint32_t offset, std::string &outHover);
std::vector<SymbolEntry> documentSymbols(const Block &program);
std::vector<CompletionEntry> completionItems(const Block &program, const std::string &text, const std::string &fromDir, const ImportLoader &loader, uint32_t offset);

} // namespace lspnav
