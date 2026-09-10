#pragma once
#include "Asset.h"
struct GLFWwindow;
namespace vke {
fs::path fileDialog(GLFWwindow *window, bool save, bool scene = false, bool hdr = false);
fs::path executableDirectory();
void showError(const std::string &message);
} // namespace vke
