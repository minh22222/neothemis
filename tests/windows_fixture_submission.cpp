#ifdef _WIN32
extern "C" __declspec(dllimport) int neothemis_fixture_write_answer();

int main() {
    return neothemis_fixture_write_answer();
}
#endif
