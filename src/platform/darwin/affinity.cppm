module affinity;
import async;

namespace affinity {
    // Darwin schedules service threads without CPU pinning.
    void bind_current(cores::core_index) noexcept {}
} // namespace affinity
