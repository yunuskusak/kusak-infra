#include "infra/spsc_ring.hpp"
#include "infra/time.hpp"

extern "C" {
    __attribute__((visibility("default")))
    int kusak_init(const char* config_path) { return 0; }

    __attribute__((visibility("default")))
    void kusak_push_tick(const void* data, uint32_t len) {}

    __attribute__((visibility("default")))
    void kusak_get_stats(uint64_t* p50_ns, uint64_t* p99_ns) {
        *p50_ns = 0;
        *p99_ns = 0;
    }

    __attribute__((visibility("default")))
    void kusak_shutdown() {}
}
