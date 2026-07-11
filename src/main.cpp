#include "compiler.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <lyra/lyra.hpp>
#include <ryml.hpp>
#include <ryml_std.hpp>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

struct Config {
  std::string namespaceStr;
  std::string descriptionStr;
};

bool runCompilation(const std::string &source, const std::filesystem::path &baseDir, const std::string &outputPath, Config config) {
  try {
    Compiler compiler(source, config.namespaceStr, baseDir);
    const auto &compiledFunctions = compiler.compile();

    if (std::filesystem::exists(outputPath)) {
      std::filesystem::remove_all(outputPath);
    }

    std::filesystem::path functionalDir = std::filesystem::path(outputPath) / "data" / config.namespaceStr / "function";
    std::filesystem::create_directories(functionalDir);

    std::filesystem::path metaPath = std::filesystem::path(outputPath) / "pack.mcmeta";
    if (!std::filesystem::exists(metaPath)) {
      std::ofstream metaFile(metaPath);
      metaFile << std::format(
        R"({{
  "pack": {{
    "pack_format": 18,
    "supported_formats": [18, 101],
    "min_version": 18,
    "max_version": [101, 1],
    "description": "{}"
  }}
}})",
        config.descriptionStr
      );
      metaFile.close();
    }

    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> functionTags;

    for (const auto &func : compiledFunctions) {
      if (func.tag.has_value()) {
        std::string fullTag = func.tag.value();
        std::string tagNamespace = "minecraft";
        std::string tagName = fullTag;

        size_t colonPos = fullTag.find(':');
        if (colonPos != std::string::npos) {
          tagNamespace = fullTag.substr(0, colonPos);
          tagName = fullTag.substr(colonPos + 1);
        }

        functionTags[tagNamespace][tagName].push_back(func.name);
      }

      std::filesystem::path funcPath = functionalDir / (func.name + ".mcfunction");
      std::ofstream outFile(funcPath);
      if (outFile.is_open()) {
        outFile << func.data;
        outFile.close();
      } else {
        std::cerr << "Error: Failed to write output file: " << funcPath.string() << "\n";
        return false;
      }
    }

    std::filesystem::path globalInitPath = functionalDir / "global_init.mcfunction";
    std::ofstream outFile(globalInitPath);
    if (outFile.is_open()) {
      outFile << compiler.globalInit;
      outFile.close();
    } else {
      std::cerr << "Error: Failed to write output file: " << globalInitPath.string() << "\n";
      return false;
    }
    functionTags["minecraft"]["load"].insert(functionTags["minecraft"]["load"].begin(), "global_init");

    for (const auto &[ns, tags] : functionTags) {
      std::filesystem::path nsTagsDir = std::filesystem::path(outputPath) / "data" / ns / "tags" / "function";
      std::filesystem::create_directories(nsTagsDir);

      for (const auto &[tagName, funcs] : tags) {
        if (funcs.empty()) continue;

        std::filesystem::path tagPath = nsTagsDir / (tagName + ".json");
        std::ofstream tagOutFile(tagPath);
        if (tagOutFile.is_open()) {
          std::string data = R"({"values":[)";
          for (const auto &func : funcs) {
            data += std::format("\"{}:{}\",", config.namespaceStr, func);
          }
          data.pop_back();
          data += "]}";
          tagOutFile << data;
          tagOutFile.close();
        } else {
          std::cerr << "Error: Failed to write output file: " << tagPath.string() << "\n";
          return false;
        }
      }
    }

    std::cout << "Successfully compiled into: " << outputPath << "\n";
    return true;

  } catch (const std::exception &e) {
    std::cerr << "Compilation Error: " << e.what() << "\n";
    return false;
  }
}

bool compileFromFile(const std::string &inputPath, const std::string &baseDirOverride, const std::string &outputPath, Config config) {
  std::ifstream f(inputPath);
  if (!f.is_open()) {
    std::cerr << "Error: Could not open input file: " << inputPath << "\n";
    return false;
  }

  std::ostringstream buf;
  buf << f.rdbuf();
  f.close();

  std::filesystem::path resolvedBaseDir = baseDirOverride.empty() ? std::filesystem::path(inputPath).parent_path() : std::filesystem::path(baseDirOverride);

  return runCompilation(buf.str(), resolvedBaseDir, outputPath, config);
}

int main(int argc, char *argv[]) {
  Config config = {.namespaceStr = "loom", .descriptionStr = "Loom Generated Datapack"};

  std::ifstream configFile("loom.yml", std::ios::binary);
  if (configFile) {
    std::stringstream buffer;
    buffer << configFile.rdbuf();
    std::string data = buffer.str();

    ryml::Tree tree = ryml::parse_in_place(ryml::to_substr(data));
    ryml::ConstNodeRef root = tree.rootref();

    if (root.has_child("namespace")) root["namespace"] >> config.namespaceStr;
    if (root.has_child("description")) root["description"] >> config.descriptionStr;
  }

  std::string inputPath;
  std::string baseDir;
  bool help = false;
  std::string outputPath = config.namespaceStr;
  bool watch = false;
  bool useStdin = false;

  auto cli = lyra::help(help) | lyra::opt(outputPath, "output")["-o"]["--output"]("Folder to output the datapack into.") |
             lyra::opt(baseDir, "base directory")["-b"]["--base-dir"]("Base directory for resolving imports.") |
             lyra::opt(useStdin)["--stdin"]("Read source from standard input.") | lyra::opt(watch)["-w"]["--watch"]("Watch the input file for changes, and recompile.") |
             lyra::arg(inputPath, "source file")("Path to a .loom file to compile.");

  auto res = cli.parse({argc, argv});
  if (!res.is_ok()) {
    std::cerr << res.message() << "\n\n" << cli << '\n';
    return 1;
  }
  if (help) {
    std::cout << cli << '\n';
    return 0;
  }

  if (useStdin) {
    std::ostringstream buf;
    buf << std::cin.rdbuf();

    std::filesystem::path resolvedBaseDir = baseDir.empty() ? std::filesystem::current_path() : std::filesystem::path(baseDir);

    runCompilation(buf.str(), resolvedBaseDir, outputPath, config);
    return 0;
  }

  if (inputPath.empty()) {
    std::cerr << "Error: Must provide an input file or use --stdin\n\n" << cli << '\n';
    return 1;
  }

  if (!std::filesystem::exists(inputPath)) {
    std::cerr << "Error: Source file does not exist: " << inputPath << "\n";
    return 1;
  }

  compileFromFile(inputPath, baseDir, outputPath, config);

  if (watch) {
    std::cout << "Watching " << inputPath << " for changes... Press Ctrl+C to stop.\n";
    auto lastWrite = std::filesystem::last_write_time(inputPath);

    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));

      if (std::filesystem::exists(inputPath)) {
        const auto &currentWrite = std::filesystem::last_write_time(inputPath);
        if (currentWrite != lastWrite) {
          lastWrite = currentWrite;
          std::cout << "Change detected! Recompiling...\n";
          compileFromFile(inputPath, baseDir, outputPath, config);
        }
      }
    }
  }

  return 0;
}
