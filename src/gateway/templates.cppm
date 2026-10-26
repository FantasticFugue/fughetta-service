export module templates;
import std;
import content;
export import world;

export namespace templates {
    struct descriptor {
        std::string id;
        std::string name;
        std::string language;
        std::string kind;
        std::vector<std::string> standards;
        bool package = false;
    };
    struct parameters {
        std::string template_id;
        std::string name;
        std::string package = "com.example";
        std::string standard;
        std::string directory;
        bool sample = true;
    };
    struct arguments {
        std::string template_id;
        std::string name;
        std::optional<std::string> package;
        std::optional<std::string> standard;
        std::optional<std::string> directory;
        std::optional<bool> sample;
    };
    inline auto expand(const arguments& in) -> parameters {
        return {in.template_id, in.name, in.package.value_or("com.example"), in.standard.value_or(""), in.directory.value_or(""), in.sample.value_or(true)};
    }
    struct file {
        std::string path;
        std::string text;
    };
    struct plan {
        std::string template_id;
        std::vector<file> files;
    };

    inline auto catalog() -> std::vector<descriptor> {
        return {{"cpp-console", "C++ Console", "C++", "project", {"20", "23"}},
                {"python-console", "Python Console", "Python", "project", {}},
                {"java-console", "Java Console", "Java", "project", {"17", "21"}, true},
                {"typescript-console", "TypeScript Console", "TypeScript", "project", {}},
                {"cpp-class", "C++ Class", "C++", "file", {}},
                {"python-class", "Python Class", "Python", "file", {}},
                {"java-class", "Java Class", "Java", "file", {}, true},
                {"typescript-class", "TypeScript Class", "TypeScript", "file", {}},
                {"typescript-interface", "TypeScript Interface", "TypeScript", "file", {}}};
    }
    inline void identifier(std::string_view value) {
        auto letter = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
        if (value.empty() || value.size() > 64 || !(letter(static_cast<unsigned char>(value.front())) || value.front() == '_') ||
            !std::ranges::all_of(value, [letter](unsigned char c) { return letter(c) || (c >= '0' && c <= '9') || c == '_'; }))
            throw std::invalid_argument("Use an identifier containing ASCII letters, digits, and underscores.");
        static const std::set<std::string_view> reserved{
            "class",    "interface", "public",   "private",    "static",   "void",       "int",    "double",  "float",    "char",  "boolean",
            "bool",     "long",      "short",    "byte",       "package",  "import",     "export", "default", "return",   "new",   "delete",
            "template", "namespace", "struct",   "enum",       "def",      "pass",       "None",   "True",    "False",    "async", "await",
            "function", "const",     "let",      "var",        "extends",  "implements", "this",   "self",    "for",      "while", "if",
            "else",     "try",       "catch",    "throw",      "throws",   "switch",     "case",   "break",   "continue", "null",  "true",
            "false",    "_",         "from",     "with",       "lambda",   "yield",      "in",     "is",      "not",      "and",   "or",
            "as",       "assert",    "del",      "global",     "nonlocal", "raise",      "except", "finally", "abstract", "final", "synchronized",
            "native",   "transient", "volatile", "instanceof", "super"};
        if (reserved.contains(value))
            throw std::invalid_argument("The name is a reserved word.");
    }
    inline auto package_path(std::string_view value) -> std::string {
        if (value.empty() || value.size() > 192)
            throw std::invalid_argument("A Java package is required.");
        std::string out;
        for (std::size_t start = 0;;) {
            auto end = value.find('.', start);
            auto part = value.substr(start, end == value.npos ? value.size() - start : end - start);
            identifier(part);
            if (!out.empty())
                out += '/';
            out += part;
            if (end == value.npos)
                return out;
            start = end + 1;
        }
    }
    inline auto render(const parameters& p) -> plan {
        auto items = catalog();
        auto item = std::ranges::find(items, p.template_id, &descriptor::id);
        if (item == items.end())
            throw std::invalid_argument("Unknown template.");
        if (item->kind == "project") {
            auto alnum = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
            if (p.name.empty() || p.name.size() > 64 || !alnum(p.name.front()) || !alnum(p.name.back()) ||
                !std::ranges::all_of(p.name, [alnum](unsigned char c) { return alnum(c) || c == '_' || c == '-'; }))
                throw std::invalid_argument("Use a project name containing ASCII letters, digits, underscores, and hyphens.");
        } else
            identifier(p.name);
        auto standard = p.standard.empty() && !item->standards.empty() ? item->standards.front() : p.standard;
        if ((!standard.empty() && std::ranges::find(item->standards, standard) == item->standards.end()))
            throw std::invalid_argument("Unsupported language standard for this template.");
        auto prefix = p.directory.empty() ? std::string{} : std::string{content::checked_path(p.directory)} + "/";
        if (item->kind == "project" && !prefix.empty())
            throw std::invalid_argument("Project templates start at the project root.");
        plan out{p.template_id};
        auto add = [&](std::string path, std::string text) {
            auto full = prefix + path;
            content::checked_path(full);
            out.files.push_back({std::move(full), std::move(text)});
        };
        if (p.template_id == "cpp-console") {
            add("CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\nproject(" + p.name + " LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD " + standard +
                                      ")\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\nset(CMAKE_CXX_EXTENSIONS OFF)\nadd_executable(app src/main.cpp)\n");
            add("src/main.cpp",
                p.sample ? "#include <iostream>\n\nint main() {\n    std::cout << \"Hello, world!\\n\";\n}\n" : "int main() {\n    return 0;\n}\n");
            add(".gitignore", "build/\n");
        } else if (p.template_id == "python-console") {
            add("pyproject.toml", "[project]\nname = \"" + p.name + "\"\nversion = \"0.1.0\"\nrequires-python = \">=3.10\"\n");
            add("main.py", p.sample ? "def main() -> None:\n    print(\"Hello, world!\")\n\n\nif __name__ == \"__main__\":\n    main()\n"
                                    : "def main() -> None:\n    pass\n\n\nif __name__ == \"__main__\":\n    main()\n");
            add(".gitignore", "__pycache__/\n.venv/\n*.pyc\n");
        } else if (p.template_id == "java-console") {
            auto path = package_path(p.package);
            add("pom.xml",
                "<project xmlns=\"http://maven.apache.org/POM/4.0.0\">\n  <modelVersion>4.0.0</modelVersion>\n  <groupId>" + p.package +
                    "</groupId>\n  <artifactId>" + p.name + "</artifactId>\n  <version>0.1.0</version>\n  <properties>\n    <maven.compiler.source>" +
                    standard + "</maven.compiler.source>\n    <maven.compiler.target>" + standard +
                    "</maven.compiler.target>\n    <project.build.sourceEncoding>UTF-8</project.build.sourceEncoding>\n  </properties>\n</project>\n");
            add("src/main/java/" + path + "/Main.java", "package " + p.package + ";\n\npublic class Main {\n    public static void main(String[] args) {\n" +
                                                            (p.sample ? std::string{"        System.out.println(\"Hello, world!\");\n"} : std::string{}) +
                                                            "    }\n}\n");
            add(".gitignore", "target/\n*.class\n");
        } else if (p.template_id == "typescript-console") {
            std::string name = p.name;
            std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
            add("package.json", "{\n  \"name\": \"" + name +
                                    "\",\n  \"version\": \"0.1.0\",\n  \"private\": true,\n  \"type\": \"module\",\n  \"devDependencies\": {\n    "
                                    "\"typescript\": \"*\"\n  }\n}\n");
            add("tsconfig.json", "{\n  \"compilerOptions\": {\n    \"target\": \"ES2022\",\n    \"module\": \"NodeNext\",\n    \"strict\": true,\n    "
                                 "\"outDir\": \"dist\"\n  },\n  \"include\": [\"src\"]\n}\n");
            add("src/main.ts", p.sample ? "console.log(\"Hello, world!\");\nexport {};\n" : "export {};\n");
            add(".gitignore", "node_modules/\ndist/\n");
        } else if (p.template_id == "cpp-class") {
            add(p.name + ".hpp", "#pragma once\n\nclass " + p.name + " {\npublic:\n    " + p.name + "() = default;\n};\n");
            add(p.name + ".cpp", "#include \"" + p.name + ".hpp\"\n");
        } else if (p.template_id == "python-class") {
            add(p.name + ".py", "class " + p.name + ":\n    def __init__(self) -> None:\n        pass\n");
        } else if (p.template_id == "java-class") {
            add(package_path(p.package) + "/" + p.name + ".java", "package " + p.package + ";\n\npublic class " + p.name + " {\n}\n");
        } else if (p.template_id == "typescript-class") {
            add(p.name + ".ts", "export class " + p.name + " {\n}\n");
        } else if (p.template_id == "typescript-interface") {
            add(p.name + ".ts", "export interface " + p.name + " {\n}\n");
        }
        if (item->kind == "project")
            add("README.md", "# " + p.name + "\n\nCreated with the Fughetta " + item->name + " template.\n");
        return out;
    }
} // namespace templates
