// 反向验证（一次性探针，不入库）：复现旧实现的失败模式。
// 旧实现 = 轨迹随机走 MSVC rand()，播种只发生在 OpContext 构造线程（主线程 srand）。
// 预期：两个 worker 线程从未播种，各自默认种子 1 → 两个序列恒等。
// 若此程序打印 EQUAL，则说明 utils_test 里 RandRangeDiffersAcrossWorkerThreads
// 在旧实现下必然 FAIL —— 新用例具备判别力。
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <string>

int main() {
    std::srand(123456); // 模拟主线程 SeedProcessRandom

    const auto fill = [](std::vector<int> &out) {
        for (int i = 0; i < 8; ++i)
            out.push_back(rand() % 1000001);
    };
    std::vector<int> a, b;
    std::thread t1([&] { fill(a); });
    std::thread t2([&] { fill(b); });
    t1.join();
    t2.join();

    std::string sa, sb;
    for (int v : a) sa += std::to_string(v) + ",";
    for (int v : b) sb += std::to_string(v) + ",";
    std::printf("threadA: %s\n", sa.c_str());
    std::printf("threadB: %s\n", sb.c_str());
    std::printf("RESULT: %s\n", sa == sb ? "EQUAL (旧实现失败模式复现, 新测试会正确拦截)" : "DIFFERENT");
    return sa == sb ? 0 : 1;
}
