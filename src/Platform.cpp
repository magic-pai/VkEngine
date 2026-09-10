#include "Platform.h"
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <windows.h>
#include <commdlg.h>
namespace vke {
fs::path fileDialog(GLFWwindow *window, bool save, bool scene, bool hdr) {
    wchar_t path[32768]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = glfwGetWin32Window(window);
    ofn.lpstrFile = path;
    ofn.nMaxFile = 32768;
    ofn.lpstrFilter =
        scene ? L"VkEngine scene\0*.vkscene\0JSON\0*.json\0\0"
        : hdr ? L"HDR environment\0*.hdr\0\0"
              : L"Model files\0*.glb;*.gltf;*.fbx;*.obj;*.dae;*.3ds;*.ply;*.stl;*.blend\0All files\0*.*\0\0";
    ofn.lpstrDefExt = scene ? L"vkscene" : nullptr;
    ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return (save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn)) ? fs::path(path) : fs::path{};
}
fs::path executableDirectory() {
    wchar_t path[32768];
    auto n = GetModuleFileNameW(nullptr, path, 32768);
    if (!n || n == 32768)
        throw std::runtime_error("Cannot locate executable");
    return fs::path(path).parent_path();
}
void showError(const std::string &m) {
    auto text = fs::u8path(m).wstring();
    MessageBoxW(nullptr, text.c_str(), L"VkEngine", MB_OK | MB_ICONERROR);
}
} // namespace vke
