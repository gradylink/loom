#include <fstream>
#include <iostream>
#include <lyra/lyra.hpp>
#include <sstream>
#include <tree_sitter/api.h>

extern "C" const TSLanguage *tree_sitter_loom(void);

int main(int argc, char *argv[]) {
  std::string inputPath;
  bool help = false;
  std::string outputPath = "loom";
  bool watch = false;
  bool optimize = true;
  auto cli = lyra::help(help) | lyra::arg(inputPath, "source file")("Path to a .loom file to compile.").required() | lyra::opt(outputPath, "output")["-o"]["--output"]("Folder to output the datapack into.") | lyra::opt(watch, "watch")["-w"]["--watch"]("Watch the input file for changes, and recompile.") | lyra::opt(optimize, "optimize")["-O"]["--optimize"]("Control whether or not to optimize specific commands.");
  auto res = cli.parse({argc, argv});
  if (!res.is_ok()) {
    std::cerr << res.message() << "\n\n"
              << cli << '\n';
    return 1;
  }
  if (help) {
    std::cout << cli << '\n';
    return 0;
  }

  std::ifstream f(inputPath);
  std::ostringstream buf;
  buf << f.rdbuf();
  const std::string source = buf.str();
  f.close();

  TSParser *parser = ts_parser_new();
  ts_parser_set_language(parser, tree_sitter_loom());

  TSTree *tree = ts_parser_parse_string(parser, nullptr, source.c_str(), source.length());
  TSNode root = ts_tree_root_node(tree);

  std::cout << ts_node_type(ts_node_child(root, 0)) << '\n';

  ts_tree_delete(tree);
  ts_parser_delete(parser);

  return 0;
}
