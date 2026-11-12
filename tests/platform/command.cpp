#include <windows.h>
import std;

int main(int argc, char** argv) {
    const auto script = std::string{argv[0]} + ".sh";
    std::ifstream input{script};
    std::string shell;
    std::getline(input, shell);
    if (!shell.starts_with("#!"))
        return 127;
    auto quote = [](std::string_view value) {
        std::string out = "\"";
        std::size_t slashes = 0;
        for (char c : value) {
            if (c == '\\') {
                ++slashes;
                continue;
            }
            out.append(c == '"' ? slashes * 2 + 1 : slashes, '\\');
            slashes = 0;
            out += c;
        }
        out.append(slashes * 2, '\\');
        return out + "\"";
    };
    auto command = quote(shell.substr(2)) + " " + quote(script);
    for (int i = 1; i < argc; ++i)
        command += " " + quote(argv[i]);
    auto count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command.c_str(), -1, nullptr, 0);
    if (!count)
        return 127;
    std::wstring wide(count, L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command.c_str(), -1, wide.data(), count);
    STARTUPINFOW start{};
    start.cb = sizeof start;
    PROCESS_INFORMATION child{};
    if (!::CreateProcessW(nullptr, wide.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &start, &child))
        return 127;
    ::CloseHandle(child.hThread);
    ::WaitForSingleObject(child.hProcess, INFINITE);
    DWORD result = 127;
    ::GetExitCodeProcess(child.hProcess, &result);
    ::CloseHandle(child.hProcess);
    return static_cast<int>(result);
}
