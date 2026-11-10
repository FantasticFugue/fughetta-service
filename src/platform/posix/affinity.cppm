module;
#include <pthread.h>
#include <sched.h>

module affinity;
import async;

namespace affinity {
    void bind_current(cores::core_index core) noexcept {
#ifdef __linux__
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(core.value % CPU_SETSIZE, &set);
        ::pthread_setaffinity_np(::pthread_self(), sizeof set, &set);
#else
        (void)core; // Darwin schedules service threads without CPU pinning.
#endif
    }
} // namespace affinity
