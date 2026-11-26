#include <iostream>
#include <cstdint>

extern "C" {
    int kusak_init(const char* config_path);
    void kusak_push_tick(const void* data, uint32_t len);
    void kusak_get_stats(uint64_t* p50_ns, uint64_t* p99_ns);
    void kusak_shutdown();
}

int main() {
    std::cout << "Testing libkusak_infra.so...\n";

    int ret = kusak_init(nullptr);
    std::cout << "kusak_init returned: " << ret << "\n";

    kusak_push_tick(nullptr, 0);

    uint64_t p50 = 0, p99 = 0;
    kusak_get_stats(&p50, &p99);
    std::cout << "p50: " << p50 << " ns, p99: " << p99 << " ns\n";

    kusak_shutdown();
    std::cout << "Test completed successfully.\n";
    return 0;
}
