#ifdef _WIN32
#include <windows.h>

extern "C" __declspec(dllexport) int neothemis_fixture_write_answer() {
    const HANDLE output = CreateFileW(L"1.out", GENERIC_WRITE, FILE_SHARE_READ,
                                      nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        return 2;
    }
    constexpr char answer[] = "17\n";
    DWORD written = 0;
    const BOOL ok = WriteFile(output, answer, sizeof(answer) - 1, &written, nullptr);
    const BOOL closed = CloseHandle(output);
    return ok && closed && written == sizeof(answer) - 1 ? 0 : 3;
}
#endif
