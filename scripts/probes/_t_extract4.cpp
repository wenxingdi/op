#include <cstdio>
#include "RunAppPath.h"
int main() {
    // 逐字符构造 D:/Program Files (x86)\shumen\game.exe（零字面量转义风险）
    wchar_t buf[64];
    int k = 0;
    const wchar_t *parts[] = {L"D", L":", L"\\", L"Program", L" ", L"Files", L" ", L"(",
                              L"x", L"8", L"6", L")", L"\\", L"shumen", L"\\", L"game",
                              L".", L"e", L"x", L"e"};
    for (auto p : parts) { size_t L2 = 0; while (p[L2]) buf[k++] = p[L2++]; }
    buf[k] = 0;
    printf("len=%d\n", k);
    auto d = op::runapp::ExtractAppDirectory(buf);
    printf("dir_len=%zu hex:", d.size());
    for (size_t i = 0; i < d.size(); ++i) printf(" %02X", (unsigned)d[i]);
    printf("\n");
    return 0;
}
