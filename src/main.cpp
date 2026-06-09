#include "compiler.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <lyra/lyra.hpp>
#include <sstream>
#include <thread>
#include <vector>

bool runCompilation(const std::string &inputPath, const std::string &outputPath) {
  std::ifstream f(inputPath);
  if (!f.is_open()) {
    std::cerr << "Error: Could not open input file: " << inputPath << "\n";
    return false;
  }

  std::ostringstream buf;
  buf << f.rdbuf();
  const std::string source = buf.str();
  f.close();

  try {
    Compiler compiler(source);
    const auto &compiledFunctions = compiler.compile();

    std::filesystem::path functionalDir = std::filesystem::path(outputPath) / "data" / "loom" / "function";
    std::filesystem::create_directories(functionalDir);

    std::filesystem::path metaPath = std::filesystem::path(outputPath) / "pack.mcmeta";
    if (!std::filesystem::exists(metaPath)) {
      std::ofstream metaFile(metaPath);
      metaFile << R"({
  "pack": {
    "pack_format": 18,
    "supported_formats": [18, 101],
    "min_version": 18,
    "max_version": [101, 1],
    "description": "Loom Generated Datapack"
  }
})";
      metaFile.close();
    }

    for (const auto &func : compiledFunctions) {
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

    std::cout << "Successfully compiled into: " << outputPath << "\n";
    return true;

  } catch (const std::exception &e) {
    std::cerr << "Compilation Error: " << e.what() << "\n";
    return false;
  }
}

int main(int argc, char *argv[]) {
  std::string inputPath;
  bool help = false;
  std::string outputPath = "loom";
  bool watch = false;
  bool optimize = true;

  auto cli = lyra::help(help) | lyra::arg(inputPath, "source file")("Path to a .loom file to compile.").required() |
             lyra::opt(outputPath, "output")["-o"]["--output"]("Folder to output the datapack into.") |
             lyra::opt(watch, "watch")["-w"]["--watch"]("Watch the input file for changes, and recompile.");

  auto res = cli.parse({argc, argv});
  if (!res.is_ok()) {
    std::cerr << res.message() << "\n\n" << cli << '\n';
    return 1;
  }
  if (help) {
    std::cout << cli << '\n';
    return 0;
  }

  if (!std::filesystem::exists(inputPath)) {
    std::cerr << "Error: Source file does not exist: " << inputPath << "\n";
    return 1;
  }

  runCompilation(inputPath, outputPath);

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
          runCompilation(inputPath, outputPath);
        }
      }
    }
  }

  return 0;
}
