// one-shot probe: check which VKs are reported pressed in this environment
#include <windows.h>
#include <cstdio>
int main() {
    for (int i = 1; i < 255; ++i) {
        SHORT s = ::GetAsyncKeyState(i);
        if (s & 0x8000)
            std::printf("VK %d (0x%02X) pressed\n", i, i);
    }
    std::printf("done\n");
    return 0;
}
