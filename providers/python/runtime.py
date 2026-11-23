"""Describe selected members of a module inside the Worker's execute role."""

import argparse
import ast
import contextlib
import ctypes
import dis
import functools
import importlib
import inspect
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import types
import typing


def is_instance_method(value):
    cls = type(value)
    return cls.__module__ == "builtins" and cls.__name__ == "instancemethod"


class DlInfo(ctypes.Structure):
    _fields_ = [
        ("library", ctypes.c_char_p),
        ("base", ctypes.c_void_p),
        ("symbol", ctypes.c_char_p),
        ("symbol_address", ctypes.c_void_p),
    ]


def native_address_lookup():
    dladdr = getattr(ctypes.CDLL(None), "dladdr", None)
    if dladdr is None:
        return lambda value: None
    dladdr.argtypes = [ctypes.c_void_p, ctypes.POINTER(DlInfo)]
    dladdr.restype = ctypes.c_int

    def lookup(pointer):
        info = DlInfo()
        if not pointer or not dladdr(pointer, ctypes.byref(info)) or not info.library:
            return None
        entry = {"library": os.fsdecode(info.library), "offset": pointer - (info.base or 0), "_address": pointer}
        if info.symbol and pointer == info.symbol_address:
            entry["linkage_name"] = os.fsdecode(info.symbol)
        return entry

    return lookup


def c_function(value):
    if not isinstance(value, types.BuiltinFunctionType):
        return None
    get_function = ctypes.pythonapi.PyCFunction_GetFunction
    get_function.argtypes = [ctypes.py_object]
    get_function.restype = ctypes.c_void_p
    return get_function(value)


def native_entry_key(entry):
    return entry["library"], entry.get("linkage_name", ""), entry["offset"]


def vectorcall_address(value):
    vectorcall = ctypes.pythonapi.PyVectorcall_Function
    vectorcall.argtypes = [ctypes.py_object]
    vectorcall.restype = ctypes.c_void_p
    return vectorcall(value)


def native_subtype(actual, owner):
    if not actual:
        return False
    subtype = ctypes.pythonapi.PyType_IsSubtype
    subtype.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    subtype.restype = ctypes.c_int
    return bool(subtype(actual, id(owner)))


def rejects_argument(shape, value):
    if not shape:
        return False
    domain = shape["domain"]
    kind = type(value)
    nanobind = shape["wrapper"] == "nanobind"
    strict = shape["noconvert"]
    if value is None:
        return domain != "boolean" or nanobind or strict
    if kind in (bytes, bytearray):
        return domain != "text" or nanobind
    if domain == "boolean":
        return kind is str or ((nanobind or strict) and kind in (int, float))
    if kind is str:
        return domain != "text"
    if kind is float:
        if domain != "floating":
            return True
        if nanobind and strict and shape.get("bits") == 32 and value == value:
            return ctypes.c_float(value).value != value
        return False
    if kind is bool:
        return domain == "text" or (nanobind and strict and domain in ("integer", "floating"))
    if kind is not int:
        return False
    if domain == "text":
        return True
    if domain == "floating":
        return nanobind and strict
    if domain != "integer":
        return False
    width = shape["bits"]
    if shape["signed"]:
        limit = 1 << (width - 1)
        return not -limit <= value < limit
    return not 0 <= value < (1 << width)


def pybind_pointer(conduit, receiver):
    method, abi, type_info, encoding = conduit
    capsule = ctypes.pythonapi.PyCapsule_New
    capsule.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]
    capsule.restype = ctypes.py_object
    get_pointer = ctypes.pythonapi.PyCapsule_GetPointer
    get_pointer.argtypes = [ctypes.py_object, ctypes.c_char_p]
    get_pointer.restype = ctypes.c_void_p
    try:
        native = method(receiver, abi, capsule(type_info, b"St9type_info", None), b"raw_pointer_ephemeral")
        return get_pointer(native, encoding) if native is not None else 0
    except (TypeError, ValueError, RuntimeError):
        return 0


def loaded_image(file, address):
    stat = os.fstat(file.fileno())
    with open("/proc/self/maps", encoding="utf-8", errors="surrogateescape") as maps:
        for row in maps:
            bounds, permissions, _, device, inode, *_ = row.split(maxsplit=5)
            begin, end = (int(value, 16) for value in bounds.split("-"))
            if begin <= address < end:
                major, minor = (int(value, 16) for value in device.split(":"))
                return ("x" in permissions and int(inode) == stat.st_ino and
                        (major, minor) == (os.major(stat.st_dev), os.minor(stat.st_dev)))
    return False


class BindingCaptures:
    def __init__(self, host, lookup, memory):
        self.host = host
        self.lookup = lookup
        self.memory = memory
        self.layouts = {}
        self.pointer_size = ctypes.sizeof(ctypes.c_void_p)

    def image_key(self, address):
        return address["library"], address["_address"] - address["offset"]

    def layout(self, address):
        if address is None:
            return {}
        library = address["library"]
        image = self.image_key(address)
        if image not in self.layouts:
            result = {}
            try:
                with open(library, "rb") as file:
                    if not loaded_image(file, address["_address"]):
                        return result
                    fd = file.fileno()
                    output = subprocess.run([self.host, f"/proc/self/fd/{fd}", "--python-layout"], pass_fds=(fd,),
                                            capture_output=True, check=True, timeout=30)
                result = json.loads(output.stdout)
            except (OSError, ValueError, subprocess.SubprocessError):
                pass
            self.layouts[image] = result
        return self.layouts[image]

    def read(self, address, size):
        if address <= 0:
            return b""
        try:
            return os.pread(self.memory, size, address)
        except (OSError, OverflowError):
            return b""

    def integer(self, address, size, *, signed=False, missing=0):
        data = self.read(address, size)
        return int.from_bytes(data, sys.byteorder, signed=signed) if len(data) == size else missing

    def pointer(self, address, layout, field, *, missing=0):
        offset = layout[field]
        if not address or offset < 0 or offset + self.pointer_size > layout["size"]:
            return missing
        return self.integer(address + offset, self.pointer_size, missing=missing)

    def bits(self, address, layout, field, width, *, missing=0):
        index = layout[field]
        byte, shift = divmod(index, 8)
        if not address or not 0 <= byte < layout["size"] or shift + width > 8:
            return missing
        data = self.integer(address + byte, 1, missing=None)
        return missing if data is None else (data >> shift) & ((1 << width) - 1)

    def named_entry(self, address, offset):
        if linkage := self.layout(address).get("functions", {}).get(str(offset)):
            return {"library": address["library"], "offset": offset, "linkage_name": linkage}
        return None

    def target(self, record, layout, field):
        impl = self.lookup(self.pointer(record, layout, "impl"))
        if not impl:
            return None
        debug = self.layout(impl)
        impl_offset = str(impl["offset"])
        if (body := debug.get("callables", {}).get(impl_offset)) is not None:
            return self.named_entry(impl, body)
        if kind := debug.get("captures", {}).get(impl_offset):
            pointer = self.pointer(record, layout, field)
            if kind == "function" or (kind == "member" and (pointer & 1) == 0):
                if target := self.lookup(pointer):
                    return self.named_entry(target, target["offset"])
        return None

    def bound_method(self, value):
        cls = type(value)
        if self.memory is None or cls.__module__ != "nanobind" or cls.__name__ != "nb_bound_method":
            return None
        dispatcher = vectorcall_address(value)
        address = self.lookup(dispatcher)
        layouts = self.layouts.get(self.image_key(address), {}).get("records", {}) if address else {}
        record = layouts.get("nb_bound_method")
        if not record or record["size"] != cls.__basicsize__ or self.pointer(id(value), record, "vectorcall") != dispatcher:
            return None
        receiver = self.pointer(id(value), record, "self")
        header = layouts.get("_object")
        return self.pointer(id(value), record, "func"), receiver, self.pointer(receiver, header, "ob_type") if header else 0

    def records(self, value, wrapper, dispatcher):
        if self.memory is None:
            return []
        if wrapper == "pybind11":
            debug = self.layout(dispatcher)
            layouts = debug.get("records", {})
            holder, record = layouts.get("function_record_PyObject"), layouts.get("function_record")
            if not holder or not record or holder["size"] != type(value.__self__).__basicsize__:
                return []
            if "12cpp_function10dispatcher" not in debug.get("functions", {}).get(str(dispatcher["offset"]), ""):
                return []
            current = self.pointer(id(value.__self__), holder, "cpp_func_rec", missing=None)
            visited = set()
            records = []
            while current and current not in visited and len(visited) < 4096:
                visited.add(current)
                records.append((current, record, "data"))
                current = self.pointer(current, record, "next", missing=None)
            return records if current == 0 and len(records) < 4096 else []
        if wrapper == "nanobind" and type(value).__name__ in ("nb_func", "nb_method"):
            dispatcher = vectorcall_address(value)
            layouts = self.layout(self.lookup(dispatcher)).get("records", {})
            holder, record = layouts.get("nb_func"), layouts.get("func_data")
            cls = type(value)
            if not holder or not record or (holder["size"], record["size"]) != (cls.__basicsize__, cls.__itemsize__):
                return []
            if self.pointer(id(value), holder, "vectorcall") != dispatcher:
                return []
            count = len(getattr(value, "__nb_signature__", ()))
            if count < 4096:
                return [(id(value) + holder["size"] + i * record["size"], record, "capture") for i in range(count)]
        return []

    def entries(self, value, wrapper, dispatcher):
        return [target for record in self.records(value, wrapper, dispatcher) if (target := self.target(*record))]

    def pybind_conduit(self, value, owner, impl, info):
        if not info:
            return None
        conduit = inspect.getattr_static(owner, "_pybind11_conduit_v1_", None)
        if conduit is None:
            return None
        conduit = unwrap_callable(conduit)
        if wrapper_kind(conduit) != "pybind11" or type(conduit.__self__) is not type(value.__self__):
            return None
        dispatcher = self.lookup(c_function(conduit))
        entries = self.entries(conduit, "pybind11", dispatcher)
        if len(entries) != 1 or "18cpp_conduit_method" not in entries[0]["linkage_name"]:
            return None
        holder = type(value.__self__).__name__.removeprefix("pybind11_detail_function_record_")
        _, _, abi = holder.partition("_")
        if not abi:
            return None
        if info["offset"] is None:
            # Resolve imported RTTI in the already loaded extension's dependency scope.
            name = "_ZTI" + info["encoding"]
            try:
                image = ctypes.CDLL(impl["library"], mode=os.RTLD_NOLOAD | os.RTLD_LOCAL)
                type_info = ctypes.cast(getattr(image, name), ctypes.c_void_p).value
            except (OSError, AttributeError):
                return None
            mapped = self.lookup(type_info)
            if not mapped or mapped.get("linkage_name") != name:
                return None
        else:
            type_info = self.image_key(impl)[1] + info["offset"]
            mapped = self.lookup(type_info)
            if not mapped or self.image_key(mapped) != self.image_key(impl):
                return None
        return conduit, abi.encode(), type_info, info["encoding"].encode()

    def argument_count(self, address, layout, field):
        offset = layout.get(field, -1)
        if not 0 <= offset <= layout["size"] - 2:
            return None
        return self.integer(address + offset, 2, missing=None)

    def maximum_arguments(self, wrapper, address, layout, debug):
        if wrapper == "pybind11":
            if any(name not in layout or self.bits(address, layout, name, 1, missing=None) != 0
                   for name in ("has_args", "has_kwargs")):
                return None
        else:
            mask = debug.get("records", {}).get("func_flags", {}).get("variadic")
            field = layout.get("flags", -1)
            if mask is None or not 0 <= field <= layout["size"] - 4:
                return None
            value = self.integer(address + field, 4, missing=None)
            if value is None or value & mask:
                return None
        return self.argument_count(address, layout, "nargs")

    def argument_records(self, wrapper, address, layout, debug, maximum):
        if maximum is None:
            return None
        metadata = debug.get("records", {}).get("argument_record" if wrapper == "pybind11" else "arg_data", {})
        size = metadata.get("size", 0)
        if wrapper == "pybind11":
            if "arguments_begin" not in layout or "arguments_end" not in layout:
                return None
            begin = self.pointer(address, layout, "arguments_begin", missing=None)
            end = self.pointer(address, layout, "arguments_end", missing=None)
            if begin is None or end is None:
                return None
            if begin == end:
                return metadata, ()
            if not 0 < end - begin <= maximum * size or (end - begin) % size:
                return None
            count = (end - begin) // size
        else:
            begin = self.pointer(address, layout, "args", missing=None)
            if begin is None:
                return None
            if not begin:
                return metadata, ()
            if size <= 0:
                return None
            count = maximum
        return metadata, tuple(begin + i * size for i in range(count))

    def keyword_slots(self, parameters, positional_only, shapes, receiver_slots, keyword, value):
        slots = set()
        for index, (name, _) in enumerate(parameters):
            if index < positional_only:
                continue
            if name is None:
                slots.add(index)
                continue
            if not name:
                continue
            data = self.read(name, len(keyword))
            if not keyword.startswith(data):
                continue
            # Binding records include the receiver, but the caster shapes omit it.
            if (len(data) < len(keyword) or index < receiver_slots or not shapes
                    or not rejects_argument(shapes[index - receiver_slots], value)):
                slots.add(index)
        return slots

    def conversion_disabled(self, wrapper, location, metadata, debug):
        if wrapper == "pybind11":
            return "convert_bit" in metadata and self.bits(location, metadata, "convert_bit", 1, missing=None) == 0
        mask = debug.get("records", {}).get("cast_flags", {}).get("convert")
        offset = metadata.get("flags", -1)
        if mask and 0 <= offset <= metadata["size"] - 4:
            flags = self.integer(location + offset, 4, missing=None)
            return flags is not None and (flags & mask) == 0
        return False

    def overloads(self, value, owner=None):
        if sys.platform != "linux" or not sys._is_gil_enabled():
            return []
        wrapper = wrapper_kind(value)
        records = self.records(value, wrapper, self.lookup(c_function(value)))
        candidates = []
        for address, layout, field in records:
            impl = self.lookup(self.pointer(address, layout, "impl"))
            debug = self.layout(impl)
            maximum = self.maximum_arguments(wrapper, address, layout, debug)
            minimum, parameters = None, None
            no_conversion = ()
            if arguments := self.argument_records(wrapper, address, layout, debug, maximum):
                metadata, locations = arguments
                parameters = [tuple(self.pointer(location, metadata, key, missing=None) if key in metadata else None
                                    for key in ("name", "default")) for location in locations]
                parameters.extend([(0, 0)] * (maximum - len(parameters)))
                minimum = sum(default == 0 for _, default in parameters)
                no_conversion = tuple(self.conversion_disabled(wrapper, location, metadata, debug) for location in locations)
            positional = self.argument_count(address, layout, "nargs_pos")
            positional_only = self.argument_count(address, layout, "nargs_pos_only") or 0
            entry, virtual, shapes = None, None, ()
            if impl:
                offset = str(impl["offset"])
                kind = debug.get("captures", {}).get(offset)
                if kind == ("member" if owner is not None else "function"):
                    shapes = debug.get("argument_shapes", {}).get(offset, ())
                    receiver_slots = int(owner is not None)
                    shapes = tuple({**shape, "wrapper": wrapper,
                                    "noconvert": index < len(no_conversion) and no_conversion[index]} if shape else None
                                   for index, shape in enumerate(shapes, receiver_slots))
                    entry = self.target(address, layout, field)
                    if entry is None and owner is not None:
                        virtual = self.virtual_record(value, owner, wrapper, address, layout, field, impl, debug)
            candidates.append((minimum, maximum, positional, positional_only, parameters, shapes, entry, virtual))
        return candidates if any(virtual or entry for *_, entry, virtual in candidates) else []

    def virtual_record(self, value, owner, wrapper, address, layout, field, impl, debug):
        encoded = self.pointer(address, layout, field)
        if not impl or debug.get("captures", {}).get(str(impl["offset"])) != "member" or (encoded & 1) == 0:
            return None
        size = self.pointer_size
        offset = layout[field]
        if offset + 2 * size > layout["size"] or (encoded - 1) % size:
            return None
        layouts = debug.get("records", {})
        instance = layouts.get("instance" if wrapper == "pybind11" else "nb_inst")
        header = layouts.get("_object")
        if not instance or not header or owner.__basicsize__ < instance["size"]:
            return None
        if wrapper == "pybind11":
            scope = self.pointer(address, layout, "scope")
        else:
            holder = layouts.get("nb_func")
            scope = self.pointer(id(value), holder, "scope") if holder else 0
        if scope != id(owner):
            return None
        adjustment = self.integer(address + offset + size, size, signed=True)
        conduit = None
        if wrapper == "pybind11":
            conduit = self.pybind_conduit(value, owner, impl, debug.get("member_type_info", {}).get(str(impl["offset"])))
        return wrapper, owner, header, instance, encoded - 1, adjustment, conduit

    def dispatch_target(self, candidates, call_arguments, keywords, receiver_slots, receiver, receiver_object):
        def accepts_call(count, arguments, minimum, maximum, positional, parameters, shapes, named_slots):
            if any(rejects_argument(shape, argument) for shape, argument in zip(shapes, arguments)):
                return False
            if count is None:
                prefix = len(arguments) + receiver_slots
                return (positional is None or prefix <= positional) and (maximum is None or prefix + len(keywords) <= maximum)
            if (minimum is not None and count < minimum) or (maximum is not None and count > maximum):
                return False
            if maximum is None or count != len(arguments) + receiver_slots + len(keywords):
                return True
            prefix = count - len(keywords)
            if positional is not None and prefix > positional:
                return False
            if parameters is None:
                return True
            if any(not any(index >= prefix for index in slots) for slots in named_slots):
                return False
            supplied_by_name = set().union(*named_slots)
            return all(default != 0 or index in supplied_by_name
                       for index, (_, default) in enumerate(parameters[prefix:], prefix))

        matches = {}
        unknown_entry = False
        for minimum, maximum, positional, positional_only, parameters, shapes, entry, virtual in candidates:
            if len(candidates) == 1 and entry:
                return {"native": entry, "dispatch": "direct"}
            shapes = shapes if len(shapes) + receiver_slots == maximum else ()
            named_slots = [self.keyword_slots(parameters, positional_only, shapes, receiver_slots, key, value)
                           for key, value in keywords] if parameters is not None else []
            if any(not slots for slots in named_slots):
                continue
            if call_arguments and not any(
                accepts_call(count, arguments, minimum, maximum, positional, parameters, shapes, named_slots)
                for count, arguments in call_arguments
            ):
                continue
            target = self.virtual_entry(virtual, receiver, receiver_object) if virtual else entry
            if target is None:
                unknown_entry = True
                continue
            mode = "virtual" if virtual else "direct"
            match = matches.setdefault(native_entry_key(target), {"native": target, "dispatch": mode})
            if match["dispatch"] != mode:
                match["dispatch"] = "indirect"
        if len(matches) == 1 and not unknown_entry:
            return next(iter(matches.values()))
        return {"candidates": [match["native"] for match in matches.values()],
                "complete": bool(matches) and not unknown_entry}

    def virtual_entry(self, method, receiver, receiver_object):
        wrapper, owner, header, instance, slot, adjustment, conduit = method
        actual = self.pointer(receiver, header, "ob_type")
        if actual != id(owner) and not native_subtype(actual, owner):
            return None
        if wrapper == "pybind11":
            if actual == id(owner) and self.bits(receiver, instance, "simple_bit", 1) and self.bits(receiver, instance, "registered_bit", 1):
                native = self.pointer(receiver, instance, "value")
            else:
                native = pybind_pointer(conduit, receiver_object) if conduit and receiver_object is not None else 0
        else:
            # nanobind's subtype conversion returns the initialized instance pointer.
            if self.bits(receiver, instance, "state_bit", 2) != 2:
                return None
            offset = instance["offset"]
            if offset + 4 > instance["size"]:
                return None
            storage = receiver + self.integer(receiver + offset, 4, signed=True)
            native = storage if self.bits(receiver, instance, "direct_bit", 1) else self.integer(storage, self.pointer_size)
        if native and (table := self.integer(native + adjustment, self.pointer_size)):
            if target := self.lookup(self.integer(table + slot, self.pointer_size)):
                debug = self.layout(target)
                offset = debug.get("adjustment_thunks", {}).get(str(target["offset"]), target["offset"])
                if offset is not None:
                    return self.named_entry(target, offset)
        return None


def wrapper_kind(value):
    cls = type(value)
    if cls.__module__ == "nanobind" and cls.__name__ in ("nb_func", "nb_method", "nb_bound_method"):
        return "nanobind"
    if isinstance(value, types.BuiltinFunctionType):
        owner = type(value.__self__)
        if owner.__name__.startswith("pybind11_detail_function_record_"):
            return "pybind11"
        if owner.__name__ == "PyCapsule":
            return "capsule"
    return None


def unwrap_callable(value):
    if isinstance(value, (staticmethod, classmethod, types.MethodType)) or is_instance_method(value):
        value = value.__func__
    try:
        return inspect.unwrap(value)
    except ValueError:
        return value


def binding_fields(value, captures):
    dispatcher = captures.lookup(c_function(value))
    if wrapper := wrapper_kind(value):
        fields = {"wrapper": wrapper}
        defining = sys.modules.get(getattr(value, "__module__", None))
        if isinstance(defining, types.ModuleType):
            fields["wrapper_library"] = vars(defining).get("__file__")
        if targets := captures.entries(value, wrapper, dispatcher):
            fields["native_targets"] = targets
        return fields
    if dispatcher:
        del dispatcher["_address"]
        return {"native_entry": dispatcher}
    return {}


@functools.cache
def local_source(path):
    source = Path(path).resolve()
    if not source.is_relative_to(Path.cwd()) or source.suffix != ".py":
        return None
    try:
        return SourceLocations(source)
    except (OSError, UnicodeError, SyntaxError):
        return None


def function_signature(function):
    if not getattr(function, "__annotate__", None) or "__signature__" in vars(function):
        return inspect.signature(function, eval_str=False)
    # Read deferred annotations from the declaration without calling __annotate__.
    code = function.__code__
    clean = types.FunctionType(code, function.__globals__, argdefs=function.__defaults__, closure=function.__closure__)
    clean.__kwdefaults__ = function.__kwdefaults__
    signature = inspect.signature(clean, eval_str=False)
    source = local_source(code.co_filename)
    declaration = source.find_declaration(code.co_name, code.co_firstlineno, ast.FunctionDef, ast.AsyncFunctionDef) if source else None
    if declaration is None:
        return signature
    args = declaration.args
    annotations = {arg.arg: ast.unparse(arg.annotation)
                   for arg in (*args.posonlyargs, *args.args, args.vararg, *args.kwonlyargs, args.kwarg)
                   if arg is not None and arg.annotation is not None}
    return signature.replace(
        parameters=[parameter.replace(annotation=annotations.get(parameter.name, parameter.annotation))
                    for parameter in signature.parameters.values()],
        return_annotation=ast.unparse(declaration.returns) if declaration.returns else signature.return_annotation,
    )


def signatures(value):
    wrapper = wrapper_kind(value)
    if wrapper == "nanobind":
        return [entry[0] for entry in getattr(value, "__nb_signature__", ()) if isinstance(entry[0], str)]
    try:
        if isinstance(value, types.FunctionType):
            unwrapped = inspect.unwrap(value, stop=lambda function: hasattr(function, "__signature__"))
            if isinstance(unwrapped, types.FunctionType):
                overloads = typing.get_overloads(unwrapped) if "__signature__" not in vars(unwrapped) else ()
                return [str(function_signature(function)) for function in overloads or (unwrapped,)]
        return [str(inspect.signature(value, eval_str=False))]
    except (TypeError, ValueError, NameError):
        if wrapper in ("pybind11", "capsule"):
            prefix = value.__name__ + "("
            lines = [line.strip() for line in (value.__doc__ or "").splitlines()]
            overloads = [line.partition(". ")[2] for line in lines if line[:1].isdigit()]
            return [line for line in overloads if line.startswith(prefix)] or [line for line in lines if line.startswith(prefix)]
        if (text := getattr(value, "__text_signature__", None)) and isinstance(text, str):
            return [text.strip()]
        return []


def member_detail(value):
    if isinstance(value, (staticmethod, classmethod)):
        return {
            "kind": type(value).__name__,
            "signatures": signatures(value.__func__),
        }
    if isinstance(value, property):
        return {
            "kind": "property",
            "get": signatures(value.fget) if value.fget else [],
            "set": signatures(value.fset) if value.fset else [],
            "delete": signatures(value.fdel) if value.fdel else [],
        }
    if type(value) is functools.cached_property:
        return {"kind": "cached_property", "get": signatures(value.func)}
    cls = type(value)
    row = {"kind": cls.__name__, "type": f"{cls.__module__}.{cls.__qualname__}"}
    if callable(value):
        row["signatures"] = signatures(value.__func__ if is_instance_method(value) else value)
    for slot, access in (("__get__", "get"), ("__set__", "set"), ("__delete__", "delete")):
        if (method := inspect.getattr_static(cls, slot, None)) is not None:
            row[access] = signatures(method)
    return row


def type_surface(cls):
    members = {}
    declarations = {}
    definitions = {}
    for base in type.__getattribute__(cls, "__mro__"):
        namespace = type.__getattribute__(base, "__dict__")
        owner = f"{base.__module__}.{base.__qualname__}"
        stored = namespace.get("__annotations__")
        annotations = stored if isinstance(stored, dict) else {}
        declared = "__annotations__" not in namespace
        if declared and isinstance(namespace.get("__firstlineno__"), int):
            module = sys.modules.get(namespace.get("__module__"))
            file = vars(module).get("__file__") if isinstance(module, types.ModuleType) else None
            source = local_source(file) if isinstance(file, str) else None
            declaration = (source.find_declaration(type.__getattribute__(base, "__name__"), namespace["__firstlineno__"], ast.ClassDef)
                           if source else None)
            if declaration is not None:
                annotations = {node.target.id: ast.unparse(node.annotation) for node in declaration.body
                               if isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name) and node.simple}
        for name in dict.fromkeys((*namespace, *annotations)):
            if not isinstance(name, str) or name.startswith("_") or name in members:
                continue
            if name in annotations and name not in declarations:
                annotation = inspect.formatannotation(annotations[name]) if isinstance(stored, dict) else annotations[name]
                declarations[name] = annotation, owner, declared
            if name in namespace:
                members[name] = {**member_detail(namespace[name]), "owner": owner}
                definitions[name] = base, namespace[name]
    for name, (annotation, owner, source) in declarations.items():
        detail = members.setdefault(name, {"kind": "attribute", "owner": owner})
        detail["annotation"] = annotation
        if detail["owner"] != owner:
            detail["annotation_owner"] = owner
        if source:
            detail["annotation_source"] = "declaration"
    return {
        "module": type.__getattribute__(cls, "__module__"),
        "qualified_name": type.__getattribute__(cls, "__qualname__"),
        "members": sorted(members),
        "details": members,
    }, definitions


def trace_imports(modules, exports, inherited, captures, overloads, native_entries, native_methods):
    monitoring = sys.monitoring
    tool = next((i for i in range(6) if monitoring.get_tool(i) is None), None)
    if tool is None:
        raise RuntimeError("No CPython monitoring slot is available.")
    seen = set()
    pending = {}
    continuations = {}
    continuation_counts = {}
    observed = {}
    unresolved = {}
    arities = {}
    unknown_argument = object()
    method_entries = {}
    native_method_names = {name.rsplit(".", 1)[1] for name in native_methods}

    def method_address(descriptor, receiver):
        if (type(descriptor) is types.MethodDescriptorType and receiver is not None
                and native_subtype(id(type(receiver)), descriptor.__objclass__)):
            return c_function(descriptor.__get__(receiver, type(receiver)))
        return None

    class LocalArgument:
        def __init__(self, name):
            self.name = name

    class ForwardedKeywords:
        def __init__(self, local):
            self.local = local

    class KeywordMap:
        def __init__(self, values, complete):
            self.values = values
            self.complete = complete

    def call_operands(code):
        result = {}
        operands = []
        has_null = False
        expanded = None
        exception_targets = {entry.target for entry in dis.Bytecode(code).exception_entries}
        instructions = list(dis.get_instructions(code))
        for index, op in enumerate(instructions):
            boundary = op.is_jump_target or op.offset in exception_targets
            if boundary or op.opname != "CALL_FUNCTION_EX":
                expanded = None
            if boundary:
                operands.clear()
                has_null = False
            if op.opname in ("LOAD_CONST", "LOAD_SMALL_INT"):
                operands.append(op.argval)
                continue
            if op.opname in ("LOAD_FAST", "LOAD_FAST_BORROW", "LOAD_FAST_CHECK"):
                operands.append(LocalArgument(op.argval))
                continue
            if op.opname in ("LOAD_FAST_LOAD_FAST", "LOAD_FAST_BORROW_LOAD_FAST_BORROW"):
                operands.extend(LocalArgument(name) for name in op.argval)
                continue
            if (op.opname == "DICT_MERGE" and op.arg == 1 and index + 1 < len(instructions)
                    and instructions[index + 1].opname == "CALL_FUNCTION_EX" and len(operands) >= 2):
                target, incoming = operands[-2:]
                if isinstance(target, KeywordMap) and target.complete and not target.values and isinstance(incoming, LocalArgument):
                    operands[-2:] = [ForwardedKeywords(incoming)]
                    continue
            if (op.opname in ("LOAD_NAME", "BUILD_TUPLE", "DICT_MERGE", "DICT_UPDATE")
                    or (op.opname == "BUILD_MAP" and op.arg != 0)):
                # Only reread fast locals from a contiguous argument-load sequence.
                operands[:] = [unknown_argument if isinstance(value, LocalArgument) else value for value in operands]
            if (op.opname in ("LOAD_DEREF", "LOAD_NAME")
                    or (op.opname == "LOAD_GLOBAL" and not (op.arg & 1))):
                operands.append(unknown_argument)
                continue
            if op.opname in ("BUILD_TUPLE", "BUILD_MAP"):
                width = op.arg * (2 if op.opname == "BUILD_MAP" else 1)
                start = max(0, len(operands) - width)
                tail = operands[start:]
                if len(tail) < width:
                    has_null = False
                values = (unknown_argument,) * (width - len(tail)) + tuple(tail)
                if op.opname == "BUILD_MAP":
                    names = values[::2]
                    complete = all(type(name) is str for name in names)
                    values = KeywordMap(dict(zip(names, values[1::2])) if complete else {}, complete)
                operands[start:] = [values]
                continue
            if op.opname in ("DICT_MERGE", "DICT_UPDATE") and (op.arg == 1 or 0 < op.arg < len(operands)):
                incoming = operands.pop() if operands else unknown_argument
                if not operands:
                    operands.append(unknown_argument)
                    has_null = False
                target = operands[-op.arg]
                complete = isinstance(target, KeywordMap) and target.complete and isinstance(incoming, KeywordMap) and incoming.complete
                if complete or op.opname == "DICT_MERGE":
                    # Unlike DICT_UPDATE, a successful DICT_MERGE preserves all known entries.
                    merged = dict(target.values) if isinstance(target, KeywordMap) else {}
                    if isinstance(incoming, KeywordMap):
                        merged.update(incoming.values)
                    operands[-op.arg] = KeywordMap(merged, complete)
                else:
                    operands[-op.arg] = unknown_argument
                continue
            if op.opname in ("CALL", "CALL_KW"):
                supplied = op.arg
                named = op.opname == "CALL_KW"
                stack_arguments = supplied + named
                positional = ()
                keywords = ()
                values = operands[-stack_arguments:] if stack_arguments else []
                values = [unknown_argument] * (stack_arguments - len(values)) + values
                if not named:
                    positional = tuple(values)
                elif type(names := values[-1]) is tuple and 0 < len(names) <= supplied and all(type(name) is str for name in names):
                    split = supplied - len(names)
                    positional = tuple(values[:split])
                    keywords = tuple((name.encode("utf-8", "surrogatepass") + b"\0", value)
                                     for name, value in zip(names, values[split:-1]))
                result[op.offset] = supplied, positional, keywords, has_null and len(operands) == stack_arguments, True, None
            elif op.opname == "CALL_FUNCTION_EX":
                keywords = ()
                complete = expanded is not None
                if operands and isinstance(keywords_map := operands[-1], (KeywordMap, ForwardedKeywords)):
                    if isinstance(keywords_map, ForwardedKeywords):
                        keywords = keywords_map
                    else:
                        keywords = tuple((name.encode("utf-8", "surrogatepass") + b"\0", value)
                                         for name, value in keywords_map.values.items())
                        complete = keywords_map.complete
                    if len(operands) >= 2 and isinstance(operands[-2], (tuple, LocalArgument)):
                        expanded = operands[-2]
                following = instructions[index + 1].offset if index + 1 < len(instructions) else None
                if following in exception_targets:
                    following = None
                result[op.offset] = None, expanded, keywords, True, complete, following
            elif op.opname == "PUSH_NULL" and operands and isinstance(operands[-1], (tuple, LocalArgument)):
                # A callargs tuple with a known length, immediately below the NULL keyword slot.
                expanded = operands[-1]
            operands.clear()
            has_null = op.opname == "PUSH_NULL" or (op.opname == "LOAD_GLOBAL" and op.arg & 1 != 0)
        return result

    def record(code, instruction, function, arg):
        address = id(function)
        selected = exports.get(address)
        receiver_object = arg
        receiver = id(receiver_object)
        receiver_type = id(type(receiver_object))
        is_bound = False
        c_method_address = None
        if selected is None and isinstance(function, types.MethodType):
            address = id(function.__func__)
            selected = exports.get(address)
            receiver_object = function.__self__
            receiver = id(receiver_object)
            receiver_type = id(type(receiver_object))
            is_bound = True
        if (selected is None and isinstance(function, types.BuiltinFunctionType)
                and function.__name__ in native_method_names and function.__self__ is not None):
            receiver_object = function.__self__
            descriptor = inspect.getattr_static(type(receiver_object), function.__name__, None)
            if (candidate := method_address(descriptor, receiver_object)) and candidate == c_function(function):
                address = id(descriptor)
                selected = exports.get(address)
                receiver = id(receiver_object)
                receiver_type = id(type(receiver_object))
                is_bound = True
                c_method_address = candidate
        if selected is None:
            if method := captures.bound_method(function):
                address, receiver, receiver_type = method
                selected = exports.get(address)
                receiver_object = None
                is_bound = True
        if selected is None and (aliases := inherited.get(address)):
            matching = [(owner, export) for owner, export in aliases if native_subtype(receiver_type, owner)]
            selected = next((export for owner, export in matching
                             if all(other is owner or native_subtype(id(owner), other) for other, _ in matching)), None)
        if selected is not None:
            member = selected[1]
            seen.add((code, instruction, member))
            if member in native_methods:
                address = c_method_address or method_address(function, receiver_object)
                if entry := captures.lookup(address):
                    del entry["_address"]
                    pending[sys._getframe(1), instruction] = member, {"native": entry, "dispatch": "direct"}
                return
            if entry := native_entries.get(member):
                pending[sys._getframe(1), instruction] = member, {"native": entry, "dispatch": "direct"}
                return
            if dispatch := overloads.get(member):
                frame = sys._getframe(1)
                receiver_slots, candidates = dispatch
                if code not in arities:
                    arities[code] = call_operands(code)
                call_arguments = []
                keywords = ()
                follow = None
                if call := arities[code].get(instruction):
                    supplied, values, keywords, has_null, complete_keywords, following = call
                    if supplied is not None and (
                        any(isinstance(value, LocalArgument) for value in values)
                        or any(isinstance(value, LocalArgument) for _, value in keywords)
                    ):
                        locals_at_call = frame.f_locals

                        def argument(value):
                            return locals_at_call.get(value.name, unknown_argument) if isinstance(value, LocalArgument) else value

                        values = tuple(argument(value) for value in values)
                        keywords = tuple((name, argument(value)) for name, value in keywords)
                    if supplied is None:
                        # CALL_FUNCTION_EX skips C_RETURN for Python bound method objects.
                        if isinstance(function, types.MethodType):
                            if following is None:
                                return
                            follow = following
                        local_positionals = isinstance(values, LocalArgument)
                        if local_positionals:
                            positional_tuple = frame.f_locals.get(values.name)
                            values = positional_tuple if type(positional_tuple) is tuple else None
                        if isinstance(keywords, ForwardedKeywords):
                            # Expanding an arbitrary iterable can change the original mapping
                            # after DICT_MERGE has copied it.
                            mapping = frame.f_locals.get(keywords.local.name) if values is not None else None
                            complete_keywords = type(mapping) is dict and all(type(name) is str for name in mapping)
                            keywords = ()
                            if complete_keywords:
                                keywords = tuple((name.encode("utf-8", "surrogatepass") + b"\0", value)
                                                 for name, value in mapping.items())
                            elif local_positionals:
                                values = None
                        if values is None and arg is monitoring.MISSING:
                            values = ()
                        if values is not None and (values or is_bound or not receiver_slots):
                            count = len(values) + int(is_bound) + len(keywords) if complete_keywords else None
                            if values and arg is not monitoring.MISSING:
                                values = (arg,) + values[1:]
                            positional = values[1:] if receiver_slots and not is_bound else values
                            call_arguments.append((count, positional))
                        elif arg is not monitoring.MISSING and (is_bound or not receiver_slots):
                            call_arguments.append((None, (arg,)))
                    elif not receiver_slots:
                        if not is_bound:
                            positional = (arg,) + values[1:] if values else ()
                            call_arguments.append((supplied, positional))
                        # A bound Python method can pass self to a free function. For other
                        # callables only consider an implicit argument if the stack is unclear.
                        if is_bound or not has_null:
                            positional = (receiver_object, arg) + values[1:] if is_bound and values else (receiver_object,) + values
                            call_arguments.append((supplied + 1, positional))
                    elif is_bound:
                        # Binding already supplies the receiver, so arg is the first C++ argument.
                        if values:
                            values = (arg,) + values[1:]
                        call_arguments.append((supplied + 1, values))
                    else:
                        # The monitor gives arg0 as the native receiver in both forms.
                        # An explicit receiver occupies the first positional slot.
                        scalar_first = values and type(values[0]) in (str, int, float, bool)
                        # CPython can expand a bound pybind11 method after the bytecode pushes NULL.
                        if not has_null or wrapper_kind(function) == "pybind11":
                            call_arguments.append((supplied + 1, values))
                        if supplied and not scalar_first:
                            call_arguments.append((supplied, values[1:]))
                target = captures.dispatch_target(candidates, call_arguments, keywords, receiver_slots, receiver, receiver_object)
                sample = member, target
                if follow is None:
                    pending[frame, instruction] = sample
                else:
                    continuations[frame] = instruction, follow, sample
                    count = continuation_counts.get(code, 0)
                    continuation_counts[code] = count + 1
                    if count == 0:
                        monitoring.set_local_events(tool, code, monitoring.events.INSTRUCTION)

    def publish_call(code, instruction, sample):
        member, target = sample
        site = code, instruction, member
        if "native" not in target:
            result = unresolved.setdefault(site, {"candidates": {}, "complete": True})
            result["candidates"].update((native_entry_key(entry), entry) for entry in target["candidates"])
            result["complete"] &= target["complete"]
        else:
            native = target["native"]
            if member in native_methods:
                method_entries[member] = native
            key = native_entry_key(native)
            targets = observed.setdefault(site, {})
            previous = targets.get(key)
            if previous is not None and previous["dispatch"] != target["dispatch"]:
                target["dispatch"] = "indirect"
            targets[key] = target

    def take_continuation(code, frame):
        state = continuations.pop(frame, None)
        if state is not None:
            continuation_counts[code] -= 1
            if not continuation_counts[code]:
                del continuation_counts[code]
                monitoring.set_local_events(tool, code, 0)
        return state

    def advanced(code, instruction):
        if (state := take_continuation(code, sys._getframe(1))) is not None:
            call_offset, following, sample = state
            if instruction == following:
                publish_call(code, call_offset, sample)

    def unwound(code, _instruction, _error):
        if code in continuation_counts:
            take_continuation(code, sys._getframe(1))

    def returned(code, instruction, _function, _arg):
        if (sample := pending.pop((sys._getframe(1), instruction), None)) is not None:
            publish_call(code, instruction, sample)

    def raised(_code, instruction, _function, _arg):
        pending.pop((sys._getframe(1), instruction), None)

    monitoring.use_tool_id(tool, "fughetta-python-runtime")
    try:
        monitoring.register_callback(tool, monitoring.events.CALL, record)
        event_mask = monitoring.events.CALL
        if overloads or native_entries or native_methods:
            monitoring.register_callback(tool, monitoring.events.C_RETURN, returned)
            monitoring.register_callback(tool, monitoring.events.C_RAISE, raised)
        if overloads:
            monitoring.register_callback(tool, monitoring.events.INSTRUCTION, advanced)
            monitoring.register_callback(tool, monitoring.events.PY_UNWIND, unwound)
            event_mask |= monitoring.events.PY_UNWIND
        monitoring.set_events(tool, event_mask)
        for module in modules:
            importlib.import_module(module)
    finally:
        monitoring.set_events(tool, 0)
        for code in continuation_counts:
            monitoring.set_local_events(tool, code, 0)
        for event in (monitoring.events.CALL, monitoring.events.C_RETURN, monitoring.events.C_RAISE,
                      monitoring.events.INSTRUCTION, monitoring.events.PY_UNWIND):
            monitoring.register_callback(tool, event, None)
        monitoring.free_tool_id(tool)

    root = Path.cwd()
    positions = {}
    files = {}
    calls = []
    for code, offset, member in seen:
        if code.co_filename.startswith("<"):
            continue
        path = Path(code.co_filename).resolve()
        if not path.is_relative_to(root) or not path.is_file():
            continue
        if code not in positions:
            positions[code] = {ins.offset: ins.positions for ins in dis.get_instructions(code)}
        at = positions[code].get(offset)
        if at is None or None in at:
            continue
        if path not in files:
            try:
                text = path.read_bytes().decode("utf-8")
            except (OSError, UnicodeError):
                files[path] = None
                continue
            lines = text.split("\n")
            starts = [0]
            for line in lines[:-1]:
                starts.append(starts[-1] + len(line) + 1)
            files[path] = lines, starts
        if files[path] is None:
            continue
        lines, starts = files[path]
        if not 0 < at.lineno <= at.end_lineno <= len(lines):
            continue
        begin = starts[at.lineno - 1] + len(lines[at.lineno - 1].encode("utf-8")[:at.col_offset].decode("utf-8"))
        end = starts[at.end_lineno - 1] + len(lines[at.end_lineno - 1].encode("utf-8")[:at.end_col_offset].decode("utf-8"))
        if begin < end:
            call = {"member": member, "path": path.relative_to(root).as_posix(), "caller": code.co_qualname,
                    "first_line": code.co_firstlineno, "begin": begin, "end": end}
            site = code, offset, member
            if targets := observed.get(site):
                call["native_targets"] = [targets[key] for key in sorted(targets)]
            if outcome := unresolved.get(site):
                call["unresolved_native_dispatch"] = True
                candidates = outcome["candidates"]
                call["native_candidates"] = [candidates[key] for key in sorted(candidates)]
                call["native_candidates_complete"] = outcome["complete"]
            calls.append(call)
    return sorted(calls, key=lambda call: (call["path"], call["begin"], call["end"], call["caller"], call["member"])), method_entries


class SourceLocations:
    def __init__(self, path):
        self.text = path.read_bytes().decode("utf-8")
        self.lines = self.text.split("\n")
        self.starts = [0]
        for line in self.lines[:-1]:
            self.starts.append(self.starts[-1] + len(line) + 1)
        self.declarations = {}
        for node in ast.walk(ast.parse(self.text, filename=str(path))):
            if not isinstance(node, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            first = min([node.lineno, *(decorator.lineno for decorator in node.decorator_list)])
            self.declarations[node.name, first, type(node)] = node

    def position(self, line, column):
        return self.starts[line - 1] + len(self.lines[line - 1].encode("utf-8")[:column].decode("utf-8"))

    def find_declaration(self, name, first_line, *kinds):
        for kind in kinds:
            if (node := self.declarations.get((name, first_line, kind))) is not None:
                return node
        return None

    def declaration(self, code):
        node = self.find_declaration(code.co_name, code.co_firstlineno, ast.FunctionDef, ast.AsyncFunctionDef)
        if node is None:
            raise ValueError(f"Cannot find the source declaration for {code.co_qualname}.")
        args = node.args
        selected = [*args.posonlyargs, *args.args, args.vararg, *args.kwonlyargs, args.kwarg]
        start = self.position(node.lineno, node.col_offset)
        name = self.text.index("def", start) + 3
        while self.text[name] in " \t":
            name += 1
        return name, {arg.arg: self.identifier(arg) for arg in selected if arg is not None}

    def identifier(self, node):
        begin = self.position(node.lineno, node.col_offset)
        line = self.lines[node.lineno - 1][begin - self.starts[node.lineno - 1]:]
        # AST names are NFKC-normalized. The token in the source can contain a
        # different number of Unicode scalars.
        end = 1
        while end < len(line) and ("_" + line[end]).isidentifier():
            end += 1
        return begin, begin + end

    def returns(self, code):
        ranges = {}
        for instruction in dis.get_instructions(code):
            at = instruction.positions
            if instruction.opname not in ("RETURN_VALUE", "RETURN_CONST") or at is None or None in at:
                continue
            begin = self.position(at.lineno, at.col_offset)
            end = self.position(at.end_lineno, at.end_col_offset)
            if begin < end:
                ranges[instruction.offset] = begin, end
        return ranges


def observation_targets(module, functions, root):
    paths = {}
    targets = {}
    for name in functions:
        function = module
        for part in name.split("."):
            function = inspect.getattr_static(function, part)
        if isinstance(function, (staticmethod, classmethod)):
            function = function.__func__
        function = inspect.unwrap(function)
        if not isinstance(function, types.FunctionType):
            raise ValueError(f"{module.__name__}.{name} is not a Python function.")
        code = function.__code__
        path = Path(code.co_filename).resolve()
        if not path.is_relative_to(root) or not path.is_file():
            raise ValueError(f"{module.__name__}.{name} is outside the checkout.")
        if path not in paths:
            paths[path] = SourceLocations(path)
        source = paths[path]
        declaration, parameters = source.declaration(code)
        targets[code] = (path.relative_to(root).as_posix(), declaration, parameters, source.returns(code))
    return targets


def harvest_test(module, functions, test_module, arguments):
    monitoring = sys.monitoring
    tool = next((i for i in range(6) if monitoring.get_tool(i) is None), None)
    if tool is None:
        raise RuntimeError("No CPython monitoring slot is available.")
    targets = observation_targets(module, functions, Path.cwd())
    observed = set()
    classes = {}

    def record(code, role, slot, location, value):
        if location is None:
            return
        cls = type(value)
        name = type.__getattribute__(cls, "__qualname__")
        namespace = type.__getattribute__(cls, "__module__")
        if isinstance(name, str) and isinstance(namespace, str):
            key = namespace, name
            previous = classes.setdefault(key, cls)
            if previous is not cls:
                classes[key] = None
            path, declaration, _, _ = targets[code]
            observed.add((path, code.co_qualname, declaration, role, slot, *location, namespace, name))

    def start(code, _offset):
        frame = sys._getframe(1)
        if frame.f_code is not code:
            return
        locals_ = frame.f_locals
        for index, (name, location) in enumerate(targets[code][2].items()):
            if name in locals_:
                record(code, "first_parameter" if index == 0 else "parameter", name, location, locals_[name])

    def returned(code, offset, value):
        record(code, "return", "return", targets[code][3].get(offset), value)

    monitoring.use_tool_id(tool, "fughetta-python-test")
    events = monitoring.events
    argv = sys.argv
    try:
        monitoring.register_callback(tool, events.PY_START, start)
        monitoring.register_callback(tool, events.PY_RETURN, returned)
        for code in targets:
            monitoring.set_local_events(tool, code, events.PY_START | events.PY_RETURN)
        sys.argv = [test_module, *arguments]
        try:
            runpy.run_module(test_module, run_name="__main__", alter_sys=True)
        except SystemExit as exit:
            if exit.code not in (None, 0):
                raise
    finally:
        sys.argv = argv
        for code in targets:
            monitoring.set_local_events(tool, code, 0)
        monitoring.register_callback(tool, events.PY_START, None)
        monitoring.register_callback(tool, events.PY_RETURN, None)
        monitoring.free_tool_id(tool)

    keys = ("path", "function", "declaration", "role", "slot", "begin", "end", "type_module", "type_name")
    surfaces = {}
    for (namespace, name), cls in sorted(classes.items()):
        if cls is None:
            continue
        members = set()
        for base in type.__getattribute__(cls, "__mro__"):
            members.update(key for key in type.__getattribute__(base, "__dict__")
                           if type(key) is str and not key.startswith("_"))
        surfaces[f"{namespace}.{name}"] = {"module": namespace, "qualified_name": name, "members": sorted(members)}
    return [dict(zip(keys, row, strict=True)) for row in sorted(observed)], surfaces


def harvest_main(argv):
    parser = argparse.ArgumentParser(description="Run a test module and collect types from selected Python functions.")
    parser.add_argument("module")
    parser.add_argument("--function", action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--test-module", required=True)
    options, arguments = parser.parse_known_args(argv)
    if arguments and arguments[0] == "--":
        arguments = arguments[1:]
    elif arguments:
        parser.error("Separate test module arguments with --.")
    if not all(part.isidentifier() for name in [options.module, options.test_module, *options.function] for part in name.split(".")):
        parser.error("Supply dotted Python module and function names.")
    execution = os.environ.get("FUGHETTA_TEST_OBSERVATION_ID")
    sys.path.insert(0, os.getcwd())
    module = importlib.import_module(options.module)
    observations, surfaces = harvest_test(module, options.function, options.test_module, arguments)
    report = {"domain": "python", "module": options.module, "symbols": [], "observations": observations, "observed_surfaces": surfaces}
    if execution:
        report["test_execution"] = execution
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=options.output.parent, delete=False) as output:
        temporary = Path(output.name)
        try:
            json.dump(report, output)
            output.write("\n")
            output.close()
            temporary.replace(options.output)
        finally:
            temporary.unlink(missing_ok=True)


def main():
    if sys.implementation.name != "cpython" or not sys.flags.isolated:
        raise SystemExit("Run the runtime host with CPython -I.")
    if sys.argv[1:2] == ["harvest"]:
        harvest_main(sys.argv[2:])
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module")
    parser.add_argument("--member", action="append", default=[])
    parser.add_argument("--trace-import", action="append", default=[], metavar="MODULE",
                        help="Import a caller module and record its calls to the selected exports.")
    parser.add_argument("--native-host", default=shutil.which("fughetta-llvm"), metavar="PATH",
                        help="Read wrapper capture layouts from the extension's native debug information.")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not all(part.isidentifier() for module in [args.module, *args.trace_import] for part in module.split(".")):
        parser.error("Supply a qualified Python module name.")

    lookup = native_address_lookup()
    sys.path.insert(0, os.getcwd())
    with contextlib.ExitStack() as resources, contextlib.redirect_stdout(sys.stderr):
        memory = None
        if args.native_host and sys.platform == "linux":
            try:
                memory = resources.enter_context(open("/proc/self/mem", "rb")).fileno()
            except OSError:
                pass
        captures = BindingCaptures(args.native_host, lookup, memory)
        module = importlib.import_module(args.module)
        members = types.ModuleType.__getattribute__(module, "__dict__")
        names = sorted(
            set(args.member)
            if args.member
            else {name for name in members if isinstance(name, str) and not name.startswith("_")}
        )
        absent = [name for name in names if name not in members]
        if absent:
            parser.error(f"Module {args.module} has no members named {', '.join(absent)}.")
        symbols = []
        exports = {}
        inherited = {}
        overloads = {}
        native_entries = {}
        native_methods = set()
        for name in names:
            value = members[name]
            cls = type(value)
            if issubclass(cls, type):
                surface, definitions = type_surface(value)
                symbols.append({"name": name, "kind": "type", "surface": surface, "signatures": signatures(value)})
                for member, (declaring_class, descriptor) in definitions.items():
                    if not (callable(descriptor) or isinstance(descriptor, (staticmethod, classmethod))):
                        continue
                    implementation = unwrap_callable(descriptor)
                    native_method = type(descriptor) is types.MethodDescriptorType and (
                        declaring_class is value or type.__getattribute__(declaring_class, "__module__") != "builtins")
                    if not native_method and not wrapper_kind(implementation):
                        continue
                    qualified = f"{name}.{member}"
                    fields = binding_fields(implementation, captures)
                    symbols.append({"name": qualified, "owner": name, "kind": "method", "surface": surface["details"][member],
                                    "signatures": signatures(implementation), **fields})
                    if not args.trace_import:
                        continue
                    static = isinstance(descriptor, staticmethod)
                    entries = (implementation, descriptor) if is_instance_method(descriptor) else (implementation,)
                    if declaring_class is value:
                        for entry in entries:
                            exports.setdefault(id(entry), (entry, qualified))
                    elif not static and not isinstance(descriptor, classmethod):
                        for entry in entries:
                            inherited.setdefault(id(entry), []).append((value, (entry, qualified)))
                    else:
                        continue
                    if native_method:
                        native_methods.add(qualified)
                    elif candidates := captures.overloads(implementation, None if static else declaring_class):
                        overloads[qualified] = int(not static), candidates
            else:
                surface, _ = type_surface(cls)
                symbol = {"name": name, "kind": "callable" if callable(value) else "value", "surface": surface}
                if callable(value):
                    symbol["signatures"] = signatures(value)
                    implementation = unwrap_callable(value)
                    symbol.update(binding_fields(implementation, captures))
                    exports.setdefault(id(value), (value, name))
                    if args.trace_import:
                        if value is implementation and (entry := symbol.get("native_entry")):
                            native_entries[name] = entry
                        elif candidates := captures.overloads(value):
                            overloads[name] = 0, candidates
                symbols.append(symbol)
        calls = []
        if args.trace_import:
            calls, method_entries = trace_imports(args.trace_import, exports, inherited, captures, overloads, native_entries, native_methods)
            for symbol in symbols:
                if entry := method_entries.get(symbol["name"]):
                    symbol["native_entry"] = entry
    with args.output.open("w", encoding="utf-8") as output:
        json.dump({"domain": "python", "module": args.module, "library": members.get("__file__"), "symbols": symbols, "calls": calls}, output)
        output.write("\n")


if __name__ == "__main__":
    main()
