#include "lsp.hpp"
#include "compiler.hpp"
#include "lexer.hpp"
#include "lspNav.hpp"
#include "parser.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace {

std::optional<std::string> readHeaderLine() {
  std::string line;
  if (!std::getline(std::cin, line)) return std::nullopt;
  if (!line.empty() && line.back() == '\r') line.pop_back();
  return line;
}

std::optional<nlohmann::json> readMessage() {
  size_t contentLength = 0;
  bool sawContentLength = false;

  while (true) {
    auto lineOpt = readHeaderLine();
    if (!lineOpt) return std::nullopt;
    const std::string &line = *lineOpt;
    if (line.empty()) break;

    static const std::string prefix = "Content-Length:";
    if (line.compare(0, prefix.size(), prefix) == 0) {
      contentLength = std::stoul(line.substr(prefix.size()));
      sawContentLength = true;
    }
  }

  if (!sawContentLength) return std::nullopt;

  std::string body(contentLength, '\0');
  std::cin.read(body.data(), static_cast<std::streamsize>(contentLength));
  if (static_cast<size_t>(std::cin.gcount()) != contentLength) return std::nullopt;

  nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
  if (parsed.is_discarded()) return std::nullopt;
  return parsed;
}

void writeMessage(const nlohmann::json &msg) {
  std::string body = msg.dump();
  std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
  std::cout.flush();
}

nlohmann::json makeResponse(const nlohmann::json &id) {
  nlohmann::json response;
  response["jsonrpc"] = "2.0";
  response["id"] = id;
  return response;
}

nlohmann::json makeNotification(const std::string &method) {
  nlohmann::json notif;
  notif["jsonrpc"] = "2.0";
  notif["method"] = method;
  return notif;
}

std::filesystem::path uriToPath(const std::string &uri) {
  static const std::string prefix = "file://";
  if (uri.rfind(prefix, 0) == 0) return std::filesystem::path(uri.substr(prefix.size()));
  return std::filesystem::path(uri);
}

std::string pathToUri(const std::filesystem::path &path) { return "file://" + path.string(); }

nlohmann::json makeDiagnostic(SourceLoc loc, const std::string &message) {
  int line0 = std::max(0, static_cast<int>(loc.line) - 1);
  int col0 = std::max(0, static_cast<int>(loc.col) - 1);

  nlohmann::json diag;
  diag["range"]["start"] = {{"line", line0}, {"character", col0}};
  diag["range"]["end"] = {{"line", line0}, {"character", col0 + 1}};
  diag["severity"] = 1;
  diag["source"] = "loom";
  diag["message"] = message;
  return diag;
}

nlohmann::json makeDiagnosticFromMessage(const std::string &msg) {
  static const std::regex locRe(R"(^line (\d+), col (\d+): ([\s\S]*)$)");
  std::smatch m;
  if (std::regex_match(msg, m, locRe)) {
    SourceLoc loc{.line = static_cast<uint32_t>(std::stoi(m[1])), .col = static_cast<uint32_t>(std::stoi(m[2]))};
    return makeDiagnostic(loc, m[3]);
  }
  return makeDiagnostic(SourceLoc{.line = 1, .col = 1}, msg);
}

uint32_t offsetForPosition(const std::string &text, int line, int character) {
  uint32_t offset = 0;
  int curLine = 0;
  while (curLine < line && offset < text.size()) {
    if (text[offset] == '\n') curLine++;
    offset++;
  }
  return std::min(offset + static_cast<uint32_t>(std::max(0, character)), static_cast<uint32_t>(text.size()));
}

nlohmann::json locToRange(SourceLoc loc) {
  int line0 = std::max(0, static_cast<int>(loc.line) - 1);
  int col0 = std::max(0, static_cast<int>(loc.col) - 1);
  int len = std::max(1, static_cast<int>(loc.endByte - loc.startByte));
  nlohmann::json range;
  range["start"] = {{"line", line0}, {"character", col0}};
  range["end"] = {{"line", line0}, {"character", col0 + len}};
  return range;
}

std::unique_ptr<Block> parseForNav(const std::string &text) {
  Lexer lexer(text);
  Parser parser(text, lexer.tokenize());
  parser.enableErrorRecovery();
  return parser.parseProgram();
}

int symbolKindFor(const std::string &kind) {
  if (kind == "function") return 12;
  if (kind == "struct") return 23;
  if (kind == "field") return 8;
  if (kind == "method") return 6;
  if (kind == "enum") return 10;
  if (kind == "enum-member") return 22;
  if (kind == "namespace") return 3;
  return 13; // variable
}

int completionKindFor(const std::string &kind) {
  if (kind == "keyword") return 14;
  if (kind == "function") return 3;
  if (kind == "struct") return 22;
  if (kind == "enum") return 13;
  if (kind == "enum-member") return 20;
  if (kind == "field") return 5;
  if (kind == "method") return 2;
  if (kind == "namespace") return 9;
  return 6; // variable
}

nlohmann::json documentSymbolToJson(const lspnav::SymbolEntry &sym) {
  nlohmann::json j;
  j["name"] = sym.name;
  j["kind"] = symbolKindFor(sym.kind);
  j["range"] = locToRange(sym.loc);
  j["selectionRange"] = locToRange(sym.loc);
  if (!sym.children.empty()) {
    nlohmann::json children = nlohmann::json::array();
    for (const auto &c : sym.children) children.push_back(documentSymbolToJson(c));
    j["children"] = children;
  }
  return j;
}

nlohmann::json diagnosticsFor(const std::string &text, const std::filesystem::path &baseDir) {
  nlohmann::json diagnostics = nlohmann::json::array();

  Lexer lexer(text);
  Parser parser(text, lexer.tokenize());
  parser.enableErrorRecovery();
  parser.parseProgram();

  if (!parser.getDiagnostics().empty()) {
    for (const ParseDiagnostic &d : parser.getDiagnostics()) diagnostics.push_back(makeDiagnostic(d.loc, d.message));
    return diagnostics;
  }

  try {
    Compiler::globalExternVars.clear();
    Compiler compiler(text, "loom", baseDir);
    compiler.compile();
  } catch (const std::exception &e) {
    diagnostics.push_back(makeDiagnosticFromMessage(e.what()));
  }

  return diagnostics;
}

class LspServer {
public:
  void run();

private:
  std::unordered_map<std::string, std::string> documents;

  std::unordered_map<std::string, std::unique_ptr<Block>> importCache;
  const Block *loadImport(const std::string &importPath, const std::string &fromDir, std::string &outResolvedPath);
  lspnav::ImportLoader importLoader();

  void handleMessage(const nlohmann::json &msg);
  void handleDidOpen(const nlohmann::json &params);
  void handleDidChange(const nlohmann::json &params);
  void handleDidClose(const nlohmann::json &params);
  void publishDiagnosticsFor(const std::string &uri);
  std::filesystem::path baseDirForUri(const std::string &uri) const;

  nlohmann::json handleDefinition(const nlohmann::json &params);
  nlohmann::json handleHover(const nlohmann::json &params);
  nlohmann::json handleDocumentSymbol(const nlohmann::json &params);
  nlohmann::json handleCompletion(const nlohmann::json &params);
};

std::filesystem::path LspServer::baseDirForUri(const std::string &uri) const {
  std::filesystem::path path = uriToPath(uri);
  if (path.has_parent_path() && !path.parent_path().empty()) return path.parent_path();
  return std::filesystem::current_path();
}

const Block *LspServer::loadImport(const std::string &importPath, const std::string &fromDir, std::string &outResolvedPath) {
  std::error_code ec;
  std::filesystem::path resolved = std::filesystem::weakly_canonical(std::filesystem::path(fromDir) / importPath, ec);
  if (ec) resolved = std::filesystem::absolute(std::filesystem::path(fromDir) / importPath);
  std::string key = resolved.string();

  if (auto it = importCache.find(key); it != importCache.end()) {
    outResolvedPath = key;
    return it->second.get();
  }

  std::ifstream file(resolved);
  if (!file) return nullptr;
  std::ostringstream ss;
  ss << file.rdbuf();

  auto block = parseForNav(ss.str());
  const Block *ptr = block.get();
  importCache[key] = std::move(block);
  outResolvedPath = key;
  return ptr;
}

lspnav::ImportLoader LspServer::importLoader() {
  return [this](const std::string &importPath, const std::string &fromDir, std::string &outResolvedPath) { return loadImport(importPath, fromDir, outResolvedPath); };
}

void LspServer::publishDiagnosticsFor(const std::string &uri) {
  auto it = documents.find(uri);
  if (it == documents.end()) return;

  nlohmann::json notif = makeNotification("textDocument/publishDiagnostics");
  notif["params"]["uri"] = uri;
  notif["params"]["diagnostics"] = diagnosticsFor(it->second, baseDirForUri(uri));
  writeMessage(notif);
}

void LspServer::handleDidOpen(const nlohmann::json &params) {
  std::string uri = params.at("textDocument").at("uri").get<std::string>();
  std::string text = params.at("textDocument").at("text").get<std::string>();
  documents[uri] = std::move(text);
  publishDiagnosticsFor(uri);
}

void LspServer::handleDidChange(const nlohmann::json &params) {
  std::string uri = params.at("textDocument").at("uri").get<std::string>();
  const auto &changes = params.at("contentChanges");
  if (!changes.empty()) {
    documents[uri] = changes.back().at("text").get<std::string>();
  }
  publishDiagnosticsFor(uri);
}

void LspServer::handleDidClose(const nlohmann::json &params) {
  std::string uri = params.at("textDocument").at("uri").get<std::string>();
  documents.erase(uri);

  nlohmann::json notif = makeNotification("textDocument/publishDiagnostics");
  notif["params"]["uri"] = uri;
  notif["params"]["diagnostics"] = nlohmann::json::array();
  writeMessage(notif);
}

nlohmann::json LspServer::handleDefinition(const nlohmann::json &params) {
  std::string uri = params.at("textDocument").at("uri").get<std::string>();
  auto it = documents.find(uri);
  if (it == documents.end()) return nullptr;

  auto pos = params.at("position");
  uint32_t offset = offsetForPosition(it->second, pos.at("line").get<int>(), pos.at("character").get<int>());

  auto program = parseForNav(it->second);
  auto result = lspnav::findDefinition(*program, baseDirForUri(uri).string(), importLoader(), offset);
  if (!result) return nullptr;

  nlohmann::json loc;
  loc["uri"] = result->file.empty() ? uri : pathToUri(result->file);
  loc["range"] = locToRange(result->targetLoc);
  return loc;
}

nlohmann::json LspServer::handleHover(const nlohmann::json &params) {
  std::string uri = params.at("textDocument").at("uri").get<std::string>();
  auto it = documents.find(uri);
  if (it == documents.end()) return nullptr;

  auto pos = params.at("position");
  uint32_t offset = offsetForPosition(it->second, pos.at("line").get<int>(), pos.at("character").get<int>());

  auto program = parseForNav(it->second);
  std::string hoverText;
  auto result = lspnav::findHover(*program, baseDirForUri(uri).string(), importLoader(), offset, hoverText);
  if (!result) return nullptr;

  nlohmann::json response;
  response["contents"] = {{"kind", "markdown"}, {"value", hoverText}};
  response["range"] = locToRange(result->targetLoc);
  return response;
}

nlohmann::json LspServer::handleDocumentSymbol(const nlohmann::json &params) {
  std::string uri = params.at("textDocument").at("uri").get<std::string>();
  auto it = documents.find(uri);
  if (it == documents.end()) return nlohmann::json::array();

  auto program = parseForNav(it->second);
  nlohmann::json result = nlohmann::json::array();
  for (const auto &sym : lspnav::documentSymbols(*program)) result.push_back(documentSymbolToJson(sym));
  return result;
}

nlohmann::json LspServer::handleCompletion(const nlohmann::json &params) {
  std::string uri = params.at("textDocument").at("uri").get<std::string>();
  auto it = documents.find(uri);
  if (it == documents.end()) return nlohmann::json::array();

  auto pos = params.at("position");
  uint32_t offset = offsetForPosition(it->second, pos.at("line").get<int>(), pos.at("character").get<int>());

  auto program = parseForNav(it->second);
  nlohmann::json result = nlohmann::json::array();
  for (const auto &item : lspnav::completionItems(*program, it->second, baseDirForUri(uri).string(), importLoader(), offset)) {
    nlohmann::json j;
    j["label"] = item.label;
    j["kind"] = completionKindFor(item.kind);
    if (!item.detail.empty()) j["detail"] = item.detail;
    result.push_back(j);
  }
  return result;
}

void LspServer::handleMessage(const nlohmann::json &msg) {
  if (!msg.contains("method")) return;
  const std::string method = msg.at("method").get<std::string>();
  nlohmann::json params = msg.value("params", nlohmann::json::object());

  if (method == "initialize") {
    nlohmann::json result;
    result["capabilities"]["textDocumentSync"] = 1;
    result["capabilities"]["definitionProvider"] = true;
    result["capabilities"]["hoverProvider"] = true;
    result["capabilities"]["documentSymbolProvider"] = true;
    result["capabilities"]["completionProvider"] = nlohmann::json::object();
    result["serverInfo"] = {{"name", "loom-lsp"}, {"version", "0.2.0"}};
    nlohmann::json response = makeResponse(msg.at("id"));
    response["result"] = result;
    writeMessage(response);
    return;
  }

  if (method == "shutdown") {
    nlohmann::json response = makeResponse(msg.at("id"));
    response["result"] = nullptr;
    writeMessage(response);
    return;
  }

  if (method == "exit") std::exit(0);

  if (method == "textDocument/didOpen") return handleDidOpen(params);
  if (method == "textDocument/didChange") return handleDidChange(params);
  if (method == "textDocument/didClose") return handleDidClose(params);

  if (method == "textDocument/definition") {
    nlohmann::json response = makeResponse(msg.at("id"));
    response["result"] = handleDefinition(params);
    writeMessage(response);
    return;
  }
  if (method == "textDocument/hover") {
    nlohmann::json response = makeResponse(msg.at("id"));
    response["result"] = handleHover(params);
    writeMessage(response);
    return;
  }
  if (method == "textDocument/documentSymbol") {
    nlohmann::json response = makeResponse(msg.at("id"));
    response["result"] = handleDocumentSymbol(params);
    writeMessage(response);
    return;
  }
  if (method == "textDocument/completion") {
    nlohmann::json response = makeResponse(msg.at("id"));
    response["result"] = handleCompletion(params);
    writeMessage(response);
    return;
  }

  if (msg.contains("id")) {
    nlohmann::json response = makeResponse(msg.at("id"));
    response["result"] = nullptr;
    writeMessage(response);
  }
}

void LspServer::run() {
  while (true) {
    std::optional<nlohmann::json> msg = readMessage();
    if (!msg) break;

    try {
      handleMessage(*msg);
    } catch (const std::exception &) {
    }
  }
}

} // namespace

void runLspServer() {
  LspServer server;
  server.run();
}
