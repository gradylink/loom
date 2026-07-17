#include "compiler.hpp"
#include <format>

std::optional<std::string> Compiler::optimizeCommand(const std::string &commandName, const std::vector<TSNode> &args) {
  bool hasInterpolation = false;

  for (TSNode arg : args) {
    if (std::string(ts_node_type(arg)) == "interpolation") {
      hasInterpolation = true;
      break;
    }
  }

  if (!hasInterpolation) return std::nullopt;

  std::string setup = "";

  auto buildJsonTextArray = [&](size_t startIdx) -> std::string {
    if (startIdx >= args.size()) return "[]";
    std::string out = "[";
    for (size_t i = startIdx; i < args.size(); ++i) {
      TSNode arg = args[i];
      std::string argType = ts_node_type(arg);

      if (argType == "interpolation") {
        TSNode expNode = ts_node_child_by_field_name(arg, "expression", 10);
        if (std::string(ts_node_type(expNode)) == "variable_ref") {
          const auto &var = vars[std::string(getFieldText(expNode, "name"))];
          if (var.value.has_value()) {
            std::string inlined = var.value.value();
            if (var.type.isString() && inlined.length() >= 2) inlined = inlined.substr(1, inlined.length() - 2);
            out += std::format(R"({{"text":"{}","color":"white"}})", inlined);
          } else {
            if (!var.type.isInteger() && !var.type.isBoolean()) return "";
            out += std::format(R"({{"score":{{"name":"{}","objective":"vars"}},"color":"white"}})", var.mangledName);
          }
        } else {
          const auto expr = compileExpression(expNode, 1, true);
          if (expr.precomputed) {
            std::string inlined = expr.data;
            if (expr.type.isString() && inlined.length() >= 2) inlined = inlined.substr(1, inlined.length() - 2);
            out += std::format(R"({{"text":"{}","color":"white"}})", inlined);
          } else {
            if (!expr.type.isInteger() && !expr.type.isBoolean()) return "";
            setup += expr.data + "\n";
            out += R"({{"score":{{"name":"expr_output1","objective":"temp"}},"color":"white"}})";
          }
        }
      } else {
        std::string val = std::string(getNodeText(arg));
        if (val.find_first_of("{}") != std::string::npos) return "";
        out += std::format(R"({{"text":"{}","color":"white"}})", val);
      }

      if (i < args.size() - 1) out += R"(,{"text":" ","color":"white"},)";
    }
    out += "]";
    return out;
  };

  if (commandName == "say") {
    const auto &jsonArray = buildJsonTextArray(0);
    if (jsonArray.empty()) return std::nullopt;

    return std::format(R"({}tellraw @a [{{"text":"[","color":"white"}},{{"selector":"@s","color":"white"}},{{"text":"] ","color":"white"}},{}])", setup, jsonArray.substr(1));
  }

  // tellraw optimization isn't the easiest so I'm leaving it out for now
  /* if (commandName == "tellraw" && args.size() >= 2) {
    std::string target = std::string(getNodeText(args[0]));

    const auto &jsonArray = buildJsonTextArray(1);
    if (jsonArray.empty()) return std::nullopt;

    return std::format("{}tellraw {} {}", setup, target, jsonArray);
  } */

  if (commandName == "title" && args.size() >= 3) {
    std::string target = std::string(getNodeText(args[0]));
    std::string position = std::string(getNodeText(args[1]));

    const auto &jsonArray = buildJsonTextArray(2);
    if (jsonArray.empty()) return std::nullopt;

    return std::format("{}title {} {} {}", setup, target, position, jsonArray);
  }

  if ((commandName == "msg" || commandName == "tell" || commandName == "w") && args.size() >= 2) {
    std::string target = std::string(getNodeText(args[0]));

    const auto &jsonArray = buildJsonTextArray(1);
    if (jsonArray.empty()) return std::nullopt;

    return std::format(
      R"({}tellraw {} [{{"text":"[","color":"gray"}},{{"selector":"@s"}},{{"text":" -> ","color":"gray"}},{{"text":"{}"}},{{"text":"] ","color":"gray"}},{}])",
      setup,
      target,
      target,
      jsonArray.substr(1)
    );
  }

  if (commandName == "bossbar" && args.size() >= 4) {
    std::string action = std::string(getNodeText(args[1]));
    std::string id = std::string(getNodeText(args[2]));
    std::string property = std::string(getNodeText(args[3]));
    if (action == "set" && property == "name") {
      const auto &jsonArray = buildJsonTextArray(4);
      if (jsonArray.empty()) return std::nullopt;

      return std::format("{}bossbar set {} name {}", setup, id, jsonArray);
    }
  }

  return std::nullopt;
}
