module;
#include <pthread.h>
#include <sched.h>

module affinity;
import async;

namespace affinity {
    void bind_current(cores::core_index core) noexcept {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(core.value % CPU_SETSIZE, &set);
        ::pthread_setaffinity_np(::pthread_self(), sizeof set, &set);
    }
} // namespace affinity
