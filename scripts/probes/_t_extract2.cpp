#include <cstdio>
#include "RunAppPath.h"
int main() {
    const wchar_t *s = L"D://Program Files (x86)//shumen//game.exe";
    const size_t n = wcslen(s);
    printf("n=%zu\n", n);
    // 复刻函数内部的 .exe 探测
    size_t end = n;
    for (size_t i = 0; i + 4 <= n; ++i) {
        if (s[i] == L'.' && (s[i+1] == L'e' || s[i+1] == L'E') &&
            (s[i+2] == L'x' || s[i+2] == L'X') && (s[i+3] == L'e' || s[i+3] == L'E')) {
            end = i + 4;
            printf("exe hit at i=%zu, end=%zu\n", i, end);
            break;
        }
    }
    if (end == n) printf("exe NOT hit, fallback to first whitespace\n");
    auto d = op::runapp::ExtractAppDirectory(s);
    printf("dir_len=%zu chars:", d.size());
    for (size_t i = 0; i < d.size(); ++i) printf(" %02X(%lc)", (unsigned)d[i], (unsigned)d[i]);
    printf("\n");
    return 0;
}
