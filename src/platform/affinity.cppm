export module affinity;
import async;

export namespace affinity {
    void bind_current(cores::core_index core) noexcept;
}
