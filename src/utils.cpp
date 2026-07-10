#include "utils.hpp"
#include <format>
#include <random>

std::string randomMangleString() {
  static constexpr std::string_view chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  static constexpr unsigned int len = 8;
  static std::random_device rd;
  static std::mt19937 generator(rd());
  std::uniform_int_distribution<> distribution(0, chars.length() - 1);

  std::string ret;
  ret.reserve(len);
  for (unsigned int i = 0; i < len; i++) ret += chars[distribution(generator)];
  return ret;
}

std::string randomFunctionMangleString() {
  static constexpr std::string_view chars = "abcdefghijklmnopqrstuvwxyz";
  static constexpr unsigned int len = 12;
  static std::random_device rd;
  static std::mt19937 generator(rd());
  std::uniform_int_distribution<> distribution(0, chars.length() - 1);

  std::string ret;
  ret.reserve(len);
  for (unsigned int i = 0; i < len; i++) ret += chars[distribution(generator)];
  return ret;
}

std::string formatError(TSNode node, const std::string &message) {
  if (ts_node_is_null(node)) return message;
  TSPoint start = ts_node_start_point(node);
  return std::format("line {}, col {}: {}", start.row + 1, start.column + 1, message);
}
