module;
#include <windows.h>
module affinity;
import async;
namespace affinity {
    void bind_current(cores::core_index core) noexcept {
        auto index = core.value;
        for (WORD group = 0; group < ::GetActiveProcessorGroupCount(); ++group) {
            auto count = ::GetActiveProcessorCount(group);
            if (index < count) {
                GROUP_AFFINITY mask{};
                mask.Group = group;
                mask.Mask = KAFFINITY{1} << index;
                ::SetThreadGroupAffinity(::GetCurrentThread(), &mask, nullptr);
                return;
            }
            index -= count;
        }
    }
} // namespace affinity
