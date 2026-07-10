#pragma once
#include <string>
#include <tree_sitter/api.h>

std::string randomMangleString();
std::string randomFunctionMangleString();
std::string formatError(TSNode node, const std::string &message);
