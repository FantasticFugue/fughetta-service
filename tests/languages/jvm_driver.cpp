// Drives the non-Kotlin JVM scenario: compile a Java sample, package it, change
// one class, repackage in a different order, then let the jvm test validate the
// reader's unchanged-declaration handling.
import std;
import launch;

namespace {
    using namespace std::chrono_literals;
    namespace fs = std::filesystem;

    auto run(std::vector<std::string> argv, const fs::path& cwd) {
        auto command = test::command(argv.front(), cwd, std::vector<std::string>(argv.begin() + 1, argv.end()));
        test::child child{std::move(command)};
        auto code = child.wait(600s);
        auto output = child.output();
        if (code)
            throw std::runtime_error(output);
    }

    auto slurp_bytes(const fs::path& path) -> std::string {
        std::ifstream in{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{in}, {}};
    }

    auto crc32_of(std::string_view data) -> std::uint32_t {
        static const auto table = [] {
            std::array<std::uint32_t, 256> value{};
            for (std::uint32_t index = 0; index < 256; ++index) {
                std::uint32_t current = index;
                for (int bit = 0; bit < 8; ++bit)
                    current = (current & 1) ? 0xEDB88320u ^ (current >> 1) : current >> 1;
                value[index] = current;
            }
            return value;
        }();
        std::uint32_t crc = 0xFFFFFFFFu;
        for (unsigned char byte : data)
            crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }
    void le16(std::string& out, std::uint16_t value) {
        out.push_back(char(value & 0xFF));
        out.push_back(char((value >> 8) & 0xFF));
    }
    void le32(std::string& out, std::uint32_t value) {
        for (int index = 0; index < 4; ++index)
            out.push_back(char((value >> (8 * index)) & 0xFF));
    }
    auto zip_bytes(std::vector<std::pair<std::string, std::string>> entries) -> std::string {
        struct record { std::string name; std::uint32_t crc = 0, size = 0, offset = 0; };
        std::string out;
        std::vector<record> directory;
        for (const auto& [name, data] : entries) {
            record row{name, crc32_of(data), static_cast<std::uint32_t>(data.size()), static_cast<std::uint32_t>(out.size())};
            le32(out, 0x04034b50); le16(out, 20); le16(out, 0); le16(out, 0); le16(out, 0); le16(out, 0);
            le32(out, row.crc); le32(out, row.size); le32(out, row.size); le16(out, static_cast<std::uint16_t>(name.size())); le16(out, 0);
            out += name; out += data; directory.push_back(row);
        }
        auto start = static_cast<std::uint32_t>(out.size());
        for (const auto& row : directory) {
            le32(out, 0x02014b50); le16(out, 20); le16(out, 20); le16(out, 0); le16(out, 0); le16(out, 0); le16(out, 0);
            le32(out, row.crc); le32(out, row.size); le32(out, row.size); le16(out, static_cast<std::uint16_t>(row.name.size()));
            le16(out, 0); le16(out, 0); le16(out, 0); le16(out, 0); le32(out, 0); le32(out, row.offset); out += row.name;
        }
        auto size = static_cast<std::uint32_t>(out.size()) - start;
        le32(out, 0x06054b50); le16(out, 0); le16(out, 0); le16(out, static_cast<std::uint16_t>(directory.size()));
        le16(out, static_cast<std::uint16_t>(directory.size())); le32(out, size); le32(out, start); le16(out, 0);
        return out;
    }

    auto sample_source() -> std::string_view {
        return R"(package sample;
import java.lang.annotation.*;
@Retention(RetentionPolicy.RUNTIME) @interface Details {
    int number(); String text(); Class<?> type(); RetentionPolicy policy();
    Nested nested(); int[] values(); float fraction(); double precise(); long large(); boolean enabled();
}
@interface Nested { String value(); }
@Retention(RetentionPolicy.CLASS) @interface Hidden { String value(); }
@Details(number=-7, text="class\0😀", type=String[].class, policy=RetentionPolicy.RUNTIME,
    nested=@Nested("inside"), values={1, 2}, fraction=1.5f, precise=2.25, large=9223372036854775806L, enabled=true)
@Hidden("invisible")
public class Sample<T extends Number & Comparable<T>>
    extends java.util.ArrayList<java.util.List<? super T[]>> implements java.io.Serializable {
    @Hidden("field") public int field;
    @Hidden("method") public static int twice(int value) { return value * 2; }
    public int run(int value) {
        switch (value) {
            case 0: return twice(1);
            case 7: return twice(2);
            default: return twice(value) + Support.adjust(value);
        }
    }
    public String text(int value) { return "value=" + value; }
    public native int nativeEntry(long value);
    public native int nativeEntry(int value);
    public static native Object[] méthod_λ(String[] values, String text, int[] ints, int[][] matrix, Class<?> type, Object object, Throwable error);
}
)";
    }
} // namespace

int main(int argc, const char** argv) {
    try {
        test::require(argc == 4, "usage: jvm-driver <jvm-test> <javac> <work>");
        fs::path binary = fs::absolute(argv[1]), javac = fs::absolute(argv[2]), work = argv[3];
        fs::create_directories(work);
        test::scratch temp{"jvm"};
        auto root = temp.dir;
        test::spit(root / "Sample.java", sample_source());
        test::spit(root / "Support.java",
                   "package sample; public class Support { public static final int VALUE = 1; public static int adjust(int value) { return value + VALUE; } }");
        run({javac.string(), "-g", "-h", root.string(), "-d", root.string(), (root / "Sample.java").string(), (root / "Support.java").string()}, root);
        auto member = root / "sample/Sample.class";
        const auto sample_class = slurp_bytes(member);
        const auto support_class = slurp_bytes(root / "sample/Support.class");
        std::ofstream{root / "sample.jar", std::ios::binary} << zip_bytes({{"sample/Sample.class", sample_class}, {"sample/Support.class", support_class}});
        const auto embedded = zip_bytes({{"sample/Details.class", slurp_bytes(root / "sample/Details.class")}});
        const auto primary = zip_bytes({{"sample/Sample.class", sample_class}, {"classes.jar", embedded}});
        std::ofstream{root / "sample.aar", std::ios::binary}
            << zip_bytes({{"AndroidManifest.xml", "<manifest/>"}, {"libs/support.jar", zip_bytes({{"sample/Support.class", support_class}})},
                          {"classes.jar", primary}});
        std::ofstream{root / "duplicate.aar", std::ios::binary}
            << zip_bytes({{"classes.jar", primary}, {"libs/duplicate.jar", zip_bytes({{"alias.class", sample_class}})}});
        auto support = test::slurp(root / "Support.java");
        auto at = support.find("VALUE = 1");
        test::require(at != std::string::npos, "Support.VALUE");
        support.replace(at, 9, "VALUE = 2");
        test::spit(root / "Support.java", support);
        run({javac.string(), "-g", "-d", root.string(), (root / "Support.java").string()}, root);
        std::ofstream{root / "changed.jar", std::ios::binary}
            << zip_bytes({{"sample/Support.class", slurp_bytes(root / "sample/Support.class")}, {"sample/Sample.class", sample_class}});
        run({binary.string(), member.string(), (root / "sample.jar").string(), (root / "sample_Sample.h").string(),
             (root / "changed.jar").string(), (root / "sample.aar").string(), (root / "duplicate.aar").string()}, root);
        std::cout << "jvm artifact records passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
