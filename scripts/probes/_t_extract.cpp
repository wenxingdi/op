#include <cstdio>
#include "RunAppPath.h"
#pragma comment(lib, "runtimeobject.lib")
int main() {
    auto d1 = op::runapp::ExtractAppDirectory(L"D://Program Files (x86)//shumen//game.exe");
    auto d2 = op::runapp::ExtractAppDirectory(L"C://Windows//System32//notepad.exe");
    wprintf(L"case1=[%ls]\ncase2=[%ls]\n", d1.c_str(), d2.c_str());
    return 0;
}
