"""Read ELF, Mach-O, COFF/PDB and Rust artifacts with fughetta-llvm."""
from pathlib import Path
import argparse
import json
import sys
import tempfile
from managed import put, run

parser = argparse.ArgumentParser()
parser.add_argument("host")
parser.add_argument("compiler")
parser.add_argument("work")
parser.add_argument("--lld-link")
parser.add_argument("--rustc")
args = parser.parse_args()

source = '''static int core(int x) {
  int y = x * 3;
  y += 7;
  return y;
}
API int api(int x) {
  int a = core(x);
  a *= 2;
  return a + 1;
}
'''


def read(root, artifact, *flags):
    return json.loads(run([args.host, artifact, *flags], root))


def compile_fixture(root, *flags):
    run([args.compiler, "--driver-mode=gcc", "-x", "c", *flags], root)


def symbols(report):
    return {symbol["name"]: symbol for symbol in report["symbols"]}


def lines(report, file):
    return {frame["line"] for location in report["locations"] for frame in location["frames"] if frame["file"].endswith(file)}


Path(args.work).mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="native-", dir=args.work) as directory:
    root = Path(directory).resolve()
    put(root, "m.c", source)

    # The ELF library links without a C runtime, so any host can build it.
    compile_fixture(root, "--target=x86_64-unknown-linux-gnu", "-fuse-ld=lld", "-nostdlib", "-g", "-O0",
                    "-shared", "-fPIC", '-DAPI=__attribute__((visibility("default")))', "m.c", "-o", "libm.so")
    report = read(root, "libm.so", "--lines")
    found = symbols(report)
    assert report["format"].startswith("elf64"), report["format"]
    assert found["api"]["exported"] and found["api"]["binding"] == "Global", found["api"]
    assert found["core"]["binding"] == "Local" and not found["core"]["exported"] and found["core"]["size"], found["core"]
    assert {2, 3, 4, 7, 8, 9} <= lines(report, "m.c"), lines(report, "m.c")

    # Mach-O names lose their leading underscore and assembler temporaries stay out.
    compile_fixture(root, "--target=arm64-apple-macos", "-g", "-O0", "-c", "-DAPI=", "m.c", "-o", "m.macho.o")
    report = read(root, "m.macho.o", "--lines")
    assert report["format"] == "Mach-O arm64", report["format"]
    assert sorted(symbols(report)) == ["api", "core"], sorted(symbols(report))
    assert all(symbol["size"] for symbol in report["symbols"]), report["symbols"]
    assert {2, 3, 4, 7, 8, 9} <= lines(report, "m.c"), lines(report, "m.c")

    # A COFF object keeps its symbol sizes; a DLL takes its functions, statics and lines from the PDB.
    compile_fixture(root, "--target=x86_64-pc-windows-msvc", "-gcodeview", "-O0", "-c",
                    "-DAPI=__declspec(dllexport)", "m.c", "-o", "m.obj")
    found = symbols(read(root, "m.obj"))
    assert sorted(found) == ["api", "core"] and found["api"]["size"] and found["core"]["size"], found
    if args.lld_link:
        run([args.lld_link, "/dll", "/debug", "/noentry", "/nodefaultlib", "m.obj", "/out:m.dll"], root)
        report = read(root, "m.dll", "--lines")
        found = symbols(report)
        assert found["api"]["exported"] and found["api"]["size"], found["api"]
        assert found["core"]["binding"] == "Local" and found["core"]["object_file"].endswith("m.obj"), found["core"]
        assert {2, 3, 4, 7, 8, 9} <= lines(report, "m.c"), lines(report, "m.c")

    if args.rustc:
        put(root, "r.rs", '''#[no_mangle]
pub extern "C" fn rust_api(x: i32) -> i32 {
    helper(x) + 1
}
#[inline(never)]
fn helper(x: i32) -> i32 {
    x * 2
}
''')
        artifact = {"win32": "r.dll", "darwin": "libr.dylib"}.get(sys.platform, "libr.so")
        run([args.rustc, "--crate-type", "cdylib", "-g", "-O", "r.rs", "-o", artifact], root)
        report = read(root, artifact, "--lines")
        found = symbols(report)
        assert found["rust_api"]["exported"], found["rust_api"]
        assert any(name.endswith(("6helper", "::helper")) and not symbol["exported"] for name, symbol in found.items()), sorted(found)
        assert {3, 7} <= lines(report, "r.rs"), lines(report, "r.rs")
print("ELF, Mach-O, COFF and Rust artifacts read")
