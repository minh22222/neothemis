#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

#include <cwchar>

int main() {
    int argument_count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (!arguments) {
        return 10;
    }

    const wchar_t* output_path = nullptr;
    for (int index = 1; index + 1 < argument_count; ++index) {
        if (std::wcscmp(arguments[index], L"-o") == 0) {
            output_path = arguments[index + 1];
            break;
        }
    }
    if (!output_path) {
        LocalFree(arguments);
        return 11;
    }

    wchar_t template_path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, template_path, 32768);
    if (length == 0 || length >= 32768) {
        LocalFree(arguments);
        return 12;
    }
    wchar_t* filename = template_path + length;
    while (filename != template_path && filename[-1] != L'\\' && filename[-1] != L'/') {
        --filename;
    }
    constexpr wchar_t template_name[] = L"neothemis-windows-fixture-submission.exe";
    if (static_cast<std::size_t>(filename - template_path) +
            (sizeof(template_name) / sizeof(template_name[0])) >
        (sizeof(template_path) / sizeof(template_path[0]))) {
        LocalFree(arguments);
        return 13;
    }
    std::wcscpy(filename, template_name);

    const BOOL copied = CopyFileW(template_path, output_path, FALSE);
    LocalFree(arguments);
    return copied ? 0 : 14;
}
#endif
