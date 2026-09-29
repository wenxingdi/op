#include <cstdio>
#include <cstring>
int main() {
    const wchar_t *s = L"D://Program Files (x86)//shumen//game.exe";
    const size_t n = wcslen(s);
    printf("n=%zu\n", n);
    for (size_t i = 0; i < n; ++i) printf("%zu:%02X ", i, (unsigned)s[i]);
    printf("\n");
    return 0;
}
