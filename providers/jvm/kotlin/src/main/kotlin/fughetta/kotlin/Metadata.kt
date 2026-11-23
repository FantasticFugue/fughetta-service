package fughetta.kotlin

import kotlinx.serialization.json.*
import org.jetbrains.kotlin.metadata.ProtoBuf
import org.jetbrains.kotlin.metadata.deserialization.*
import org.jetbrains.kotlin.metadata.jvm.JvmProtoBuf
import org.jetbrains.kotlin.metadata.jvm.deserialization.JvmProtoBufUtil
import org.jetbrains.kotlin.load.kotlin.FileBasedKotlinClass
import org.jetbrains.kotlin.load.kotlin.KotlinJvmBinaryClass
import org.jetbrains.kotlin.name.ClassId
import org.jetbrains.kotlin.name.Name
import org.jetbrains.kotlin.descriptors.SourceElement
import org.jetbrains.org.objectweb.asm.ClassReader
import java.nio.file.Files
import java.nio.file.Path
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipInputStream

/** Decodes the installed compiler's metadata without loading target classes. */
object Metadata {
    private data class Header(var kind: Int = 1, var facade: String = "", var pkg: String = "",
                              val data: MutableList<String> = mutableListOf(), val strings: MutableList<String> = mutableListOf())
    private data class Binary(val owner: String, val header: Header, val methods: List<Method>, val fields: List<Pair<String, String>>)
    private data class Method(val name: String, val descriptor: String, val annotations: MutableSet<String> = mutableSetOf())
    private fun binary(bytes: ByteArray, owner: String): Binary? {
        val klass = FileBasedKotlinClass.create(bytes, MetadataVersion.INSTANCE) { id, version, header, inner ->
            object : FileBasedKotlinClass(id, version, header, inner) {
                override fun getFileContents(): ByteArray = bytes
                override val location: String get() = id.asString()
                override fun equals(other: Any?): Boolean = this === other
                override fun hashCode(): Int = System.identityHashCode(this)
                override fun toString(): String = location
            }
        } ?: return null
        val methods = mutableListOf<Method>()
        val fields = mutableListOf<Pair<String, String>>()
        klass.visitMembers(object : KotlinJvmBinaryClass.MemberVisitor {
            override fun visitMethod(name: Name, desc: String): KotlinJvmBinaryClass.MethodAnnotationVisitor {
                val method = Method(name.asString(), desc).also { methods += it }
                return object : KotlinJvmBinaryClass.MethodAnnotationVisitor {
                    override fun visitAnnotation(classId: ClassId, source: SourceElement): KotlinJvmBinaryClass.AnnotationArgumentVisitor? {
                        method.annotations += classId.asString()
                        return null
                    }
                    override fun visitParameterAnnotation(index: Int, classId: ClassId, source: SourceElement): KotlinJvmBinaryClass.AnnotationArgumentVisitor? = null
                    override fun visitAnnotationMemberDefaultValue(): KotlinJvmBinaryClass.AnnotationArgumentVisitor? = null
                    override fun visitEnd() {}
                }
            }
            override fun visitField(name: Name, desc: String, initializer: Any?): KotlinJvmBinaryClass.AnnotationVisitor? {
                fields += name.asString() to desc
                return null
            }
        }, bytes)
        val value = klass.classHeader
        return Binary(owner, Header(value.kind.id, value.multifileClassName.orEmpty(), value.packageName.orEmpty(),
            value.data.orEmpty().toMutableList(), value.strings.orEmpty().toMutableList()), methods, fields)
    }
    private fun argumentTypes(descriptor: String): List<String> {
        val result = mutableListOf<String>()
        var index = 1
        while (descriptor[index] != ')') {
            val first = index
            while (descriptor[index] == '[') ++index
            index = if (descriptor[index] == 'L') descriptor.indexOf(';', index) + 1 else index + 1
            result += descriptor.substring(first, index)
        }
        return result
    }

    fun read(path: Path, javaRelease: Int): JsonObject = readMany(listOf(path), javaRelease)

    private fun <T> readArchive(
        entries: Sequence<ZipEntry>, javaRelease: Int, capture: (ZipEntry) -> T, readBytes: (T) -> ByteArray,
        allowNested: Boolean = true, accept: (ByteArray) -> Unit,
    ) {
        val selected = sortedMapOf<String, Pair<Int, T>>()
        for (entry in entries) {
            var name = entry.name
            if (allowNested && (name == "classes.jar" || (name.startsWith("libs/") && name.endsWith(".jar")))) {
                ZipInputStream(readBytes(capture(entry)).inputStream()).use { nested ->
                    readArchive(generateSequence { nested.nextEntry }, javaRelease, { nested.readBytes() }, { it },
                        allowNested = false, accept = accept)
                }
                continue
            }
            if (!name.endsWith(".class")) continue
            var release = 0
            if (name.startsWith("META-INF/versions/")) {
                require(javaRelease > 0) { "multi-release JAR requires an explicit java_release" }
                val relative = name.removePrefix("META-INF/versions/")
                release = relative.substringBefore('/').toInt()
                name = relative.substringAfter('/')
                if (release > javaRelease) continue
            }
            if ((selected[name]?.first ?: -1) <= release) selected[name] = release to capture(entry)
        }
        selected.values.forEach { (_, data) -> accept(readBytes(data)) }
    }

    fun readMany(paths: List<Path>, javaRelease: Int): JsonObject {
        val owners = mutableSetOf<String>()
        val binaries = sortedMapOf<String, Binary>()
        var duplicate: String? = null
        fun accept(bytes: ByteArray) {
            val owner = ClassReader(bytes).className
            if (owners.add(owner)) binary(bytes, owner)?.let { binaries[owner] = it }
            else duplicate = duplicate?.let { minOf(it, owner) } ?: owner
        }
        val classMagic = byteArrayOf(0xca.toByte(), 0xfe.toByte(), 0xba.toByte(), 0xbe.toByte())
        for (path in paths) when {
            Files.isDirectory(path) -> Files.walk(path).use { files ->
                files.filter { Files.isRegularFile(it) && it.toString().endsWith(".class") }
                    .forEach { accept(Files.readAllBytes(it)) }
            }
            Files.newInputStream(path).use { it.readNBytes(4).contentEquals(classMagic) } -> accept(Files.readAllBytes(path))
            else -> ZipFile(path.toFile()).use { archive ->
                readArchive(archive.entries().asSequence(), javaRelease, { it }, { entry ->
                    archive.getInputStream(entry).use { it.readBytes() }
                }, accept = ::accept)
            }
        }
        require(duplicate == null) { "duplicate JVM class in artifact: $duplicate" }
        return buildJsonObject {
            putJsonArray("classes") { binaries.values.forEach { add(Decoder(it, binaries).read()) } }
        }
    }

    private class Decoder(private val binary: Binary, private val binaries: Map<String, Binary>) {
        private val header = binary.header
        private lateinit var names: NameResolver
        private lateinit var table: TypeTable
        private val types = Types()
        private val identities = linkedMapOf<String, JsonObject>()
        private val declarations = mutableListOf<JsonObject>()
        private val parameterNames = mutableMapOf<Int, String>()
        private val parameterOwners = mutableMapOf<Int, Pair<String, Int>>()
        private val parameterIds = mutableMapOf<String, Int>()
        private inline fun <T> scope(localTable: ProtoBuf.TypeTable? = null, block: () -> T): T {
            val oldNames = parameterNames.toMap(); val oldOwners = parameterOwners.toMap(); val oldIds = parameterIds.toMap(); val oldTable = table
            if (localTable != null) table = TypeTable(localTable)
            try { return block() }
            finally {
                parameterNames.clear(); parameterNames.putAll(oldNames)
                parameterOwners.clear(); parameterOwners.putAll(oldOwners)
                parameterIds.clear(); parameterIds.putAll(oldIds)
                table = oldTable
            }
        }
        private var classKey: String? = null
        private var className: String? = null
        private val pkg = header.pkg.ifEmpty { binary.owner.substringBeforeLast('/', "").replace('/', '.') }
        private fun variance(value: ProtoBuf.TypeParameter.Variance): String = when (value) {
            ProtoBuf.TypeParameter.Variance.IN -> "in"
            ProtoBuf.TypeParameter.Variance.OUT -> "out"
            else -> ""
        }
        private fun projection(value: ProtoBuf.Type.Argument.Projection): String = when (value) {
            ProtoBuf.Type.Argument.Projection.IN -> "in"
            ProtoBuf.Type.Argument.Projection.OUT -> "out"
            ProtoBuf.Type.Argument.Projection.STAR -> "star"
            else -> ""
        }
        private fun key(kind: String, name: String) = buildJsonObject { put("kind", kind); put("name", name) }.toString()
        private fun identity(key: String, owner: String? = null): String {
            val id = "metadata:$key" + (owner?.let { ":owner:$it" } ?: "")
            identities.putIfAbsent(id, buildJsonObject {
                put("id", id); put("key", key); owner?.let { put("owner_key", it) }
            })
            return id
        }
        private fun classIdentity(name: String, alias: Boolean = false) = identity(key(if (alias) "alias" else "class", name))
        private fun render(type: ProtoBuf.Type): String {
            val base = when {
                type.hasClassName() -> names.getQualifiedClassName(type.className).replace('/', '.')
                type.hasTypeAliasName() -> names.getQualifiedClassName(type.typeAliasName).replace('/', '.')
                type.hasTypeParameter() -> parameterNames[type.typeParameter] ?: "T${type.typeParameter}"
                type.hasTypeParameterName() -> names.getString(type.typeParameterName)
                else -> "<unresolved>"
            }
            val args = type.argumentList.map { argument ->
                if (argument.projection == ProtoBuf.Type.Argument.Projection.STAR) "*"
                else (when (argument.projection) { ProtoBuf.Type.Argument.Projection.IN -> "in "; ProtoBuf.Type.Argument.Projection.OUT -> "out "; else -> "" }) + render(requireNotNull(argument.type(table)))
            }
            return base + (if (args.isEmpty()) "" else args.joinToString(", ", "<", ">")) + (if (type.nullable) "?" else "")
        }
        private fun type(type: ProtoBuf.Type): String {
            val core = when {
                type.hasTypeParameter() || type.hasTypeParameterName() -> {
                    val entry = if (type.hasTypeParameter()) type.typeParameter else parameterIds.getValue(names.getString(type.typeParameterName))
                    val owner = parameterOwners[entry]
                    types.node("parameter") {
                        put("name", parameterNames[entry] ?: "T$entry")
                        owner?.let { put("binder", identity(it.first)); put("parameter_index", it.second) }
                    }
                }
                type.hasClassName() || type.hasTypeAliasName() -> {
                    val alias = type.hasTypeAliasName()
                    val name = names.getQualifiedClassName(if (alias) type.typeAliasName else type.className)
                    types.node(if (type.argumentCount == 0) "named" else "application") {
                        put("name", name.replace('/', '.')); put("declaration", classIdentity(name, alias))
                        putJsonArray("arguments") { type.argumentList.filter { it.projection != ProtoBuf.Type.Argument.Projection.STAR }.forEach { add(type(requireNotNull(it.type(table)))) } }
                        putJsonObject("extensions") { putJsonArray("kotlin.projections") {
                            type.argumentList.forEach { argument -> add(buildJsonObject {
                                put("variance", projection(argument.projection))
                                if (argument.projection != ProtoBuf.Type.Argument.Projection.STAR) put("type", type(requireNotNull(argument.type(table))))
                            }) }
                        } }
                    }
                }
                else -> types.node("unresolved") { put("name", render(type)) }
            }
            val nullable = if (type.nullable) types.node("nullable") { putJsonArray("arguments") { add(core) } } else core
            return type.flexibleUpperBound(table)?.let { upper -> types.node("flexible") { put("lower_bound", nullable); put("upper_bound", type(upper)) } } ?: nullable
        }
        private fun parameters(values: List<ProtoBuf.TypeParameter>, owner: String): List<String> {
            values.forEachIndexed { index, parameter ->
                parameterNames[parameter.id] = names.getString(parameter.name); parameterIds[names.getString(parameter.name)] = parameter.id; parameterOwners[parameter.id] = owner to index
            }
            return values.mapIndexed { index, parameter -> types.node("parameter") {
                put("name", names.getString(parameter.name)); put("binder", identity(owner)); put("parameter_index", index)
                putJsonArray("constraints") { parameter.upperBounds(table).forEach { add(type(it)) } }
                putJsonArray("qualifiers") { add(variance(parameter.variance)); if (parameter.reified) add("reified") }
            } }
        }
        private fun callableKey(kind: String, name: String, parameters: List<ProtoBuf.TypeParameter>, receiver: ProtoBuf.Type?, contexts: List<ProtoBuf.Type>, values: List<ProtoBuf.ValueParameter>?): String = buildJsonObject {
            put("kind", kind)
            val relative = className?.substringAfterLast('/')
            put("name", pkg.replace('.', '/') + "/" + (relative?.let { "$it." } ?: "") + name)
            putJsonArray("type_parameters") { parameters.forEach { parameter -> add(buildJsonObject {
                put("variance", variance(parameter.variance))
                putJsonArray("bounds") {
                    val bounds = parameter.upperBounds(table)
                    if (bounds.isEmpty()) add("kotlin.Any?") else bounds.forEach { add(render(it)) }
                }
            }) } }
            receiver?.let { put("receiver", render(it)) }
            putJsonArray("contexts") { contexts.forEach { add(render(it)) } }
            values?.let { putJsonArray("parameters") { it.forEach { parameter -> add(buildJsonObject {
                put("type", render(parameter.type(table))); put("vararg", parameter.hasVarargElementType() || parameter.hasVarargElementTypeId())
            }) } } }
        }.toString()
        private fun member(owner: String, kind: String, name: String, descriptor: String) = buildJsonObject {
            put("owner", owner); put("kind", kind); put("name", name); put("descriptor", descriptor)
        }
        private fun methods(name: String, descriptor: String): List<JsonObject> {
            val owners = mutableListOf(binary.owner)
            if (header.kind == 5 && header.facade.isNotEmpty()) owners += header.facade
            val original = binary.methods.singleOrNull { it.name == name && it.descriptor == descriptor }
            if (original?.annotations?.contains("kotlin/jvm/JvmStatic") == true && '$' in binary.owner) owners += binary.owner.substringBeforeLast('$')
            val result = mutableListOf<JsonObject>()
            for (owner in owners.distinct()) {
                val methods = binaries[owner]?.methods ?: continue
                methods.filter { it.name == name && it.descriptor == descriptor }.forEach { result += member(owner, "method", it.name, it.descriptor) }
                val base = argumentTypes(descriptor)
                val returns = descriptor.substringAfter(')')
                methods.filter { it.name == "$name\$default" || (name == "<init>" && it.name == name && it.descriptor != descriptor && argumentTypes(it.descriptor).lastOrNull() == "Lkotlin/jvm/internal/DefaultConstructorMarker;") }.forEach { generated ->
                    val args = argumentTypes(generated.descriptor)
                    val instance = listOf("L$owner;") + base
                    if ((args.take(base.size) == base || args.take(instance.size) == instance) && generated.descriptor.substringAfter(')') == returns)
                        result += member(owner, "method", generated.name, generated.descriptor)
                }
                if (original?.annotations?.contains("kotlin/jvm/JvmOverloads") == true) {
                    methods.filter { it.name == name && it.descriptor != descriptor }.forEach { generated ->
                        val args = argumentTypes(generated.descriptor)
                        if (args.size < base.size && base.take(args.size) == args && generated.descriptor.substringAfter(')') == returns)
                            result += member(owner, "method", generated.name, generated.descriptor)
                    }
                }
            }
            return result.distinct()
        }
        private fun declaration(key: String, name: String, kind: String, flags: Int, type: String?, members: List<JsonObject>, owner: String? = null,
                                modifiers: List<String> = emptyList(), parameterDetails: List<JsonObject> = emptyList()) {
            declarations += buildJsonObject {
                put("id", identity(key, owner)); put("key", key); owner?.let { put("owner_key", it) }
                classKey?.takeIf { it != key }?.let { put("containing_key", it) }
                put("name", name); put("qualified_name", if (classKey == key) className!!.replace('/', '.') else (className?.replace('/', '.') ?: pkg).let { if (it.isEmpty()) name else "$it.$name" })
                put("kind", kind); put("visibility", Flags.VISIBILITY.get(flags).toString())
                type?.let { put("type", it) }
                putJsonArray("modifiers") { modifiers.forEach { add(it) } }
                put("parameters", JsonArray(parameterDetails)); put("jvm_members", JsonArray(members))
            }
        }
        private fun function(value: ProtoBuf.Function) = scope(if (value.hasTypeTable()) value.typeTable else null) {
            value.typeParameterList.forEach { parameterNames[it.id] = names.getString(it.name); parameterIds[names.getString(it.name)] = it.id }
            val receiver = value.receiverType(table)
            val contexts = value.contextReceiverTypes(table)
            val name = names.getString(value.name)
            val key = callableKey("function", name, value.typeParameterList, receiver, contexts, value.valueParameterList)
            val generic = parameters(value.typeParameterList, key)
            val signature = types.node("function") {
                put("return_type", type(value.returnType(table))); receiver?.let { put("receiver", type(it)) }
                putJsonArray("parameter_groups") {
                    if (contexts.isNotEmpty()) add(buildJsonObject { putJsonArray("parameters") { contexts.forEach { add(buildJsonObject { put("type", type(it)); putJsonArray("modifiers") { add("context") } }) } } })
                    add(buildJsonObject { putJsonArray("parameters") { value.valueParameterList.forEach { parameter -> add(buildJsonObject {
                        put("type", type(parameter.type(table))); putJsonArray("modifiers") {
                            if (parameter.hasVarargElementType() || parameter.hasVarargElementTypeId()) add("vararg")
                            if (Flags.IS_CROSSINLINE.get(parameter.flags)) add("crossinline")
                            if (Flags.IS_NOINLINE.get(parameter.flags)) add("noinline")
                        }
                    }) } } })
                }
                put("type_parameters", JsonArray(generic.map(::JsonPrimitive)))
                putJsonArray("qualifiers") { if (Flags.IS_SUSPEND.get(value.flags)) add("suspend") }
            }
            val method = JvmProtoBufUtil.getJvmMethodSignature(value, names, table)
            val modifiers = buildList { if (Flags.IS_INLINE.get(value.flags)) add("inline"); if (Flags.IS_SUSPEND.get(value.flags)) add("suspend"); if (Flags.IS_OPERATOR.get(value.flags)) add("operator"); if (Flags.IS_INFIX.get(value.flags)) add("infix") }
            val details = value.valueParameterList.mapIndexed { index, parameter -> buildJsonObject {
                put("group", if (contexts.isEmpty()) 0 else 1); put("index", index); put("name", names.getString(parameter.name))
                putJsonObject("annotations") { put("kotlin.has_default", Flags.DECLARES_DEFAULT_VALUE.get(parameter.flags)) }
            } }
            declaration(key, name, if (classKey == null) "function" else "method", value.flags, signature,
                method?.let { methods(it.name, it.desc) } ?: emptyList(), modifiers = modifiers, parameterDetails = details)
        }
        private fun field(name: String, descriptor: String): List<JsonObject> {
            val owners = mutableListOf(binary.owner)
            if (header.kind == 5 && header.facade.isNotEmpty()) owners += header.facade
            if (binary.owner.endsWith("\$Companion")) owners += binary.owner.substringBeforeLast('$')
            return owners.distinct().filter { owner -> binaries[owner]?.fields?.contains(name to descriptor) == true }
                .map { owner -> member(owner, "field", name, descriptor) }
        }
        private fun property(value: ProtoBuf.Property) = scope {
            value.typeParameterList.forEach { parameterNames[it.id] = names.getString(it.name); parameterIds[names.getString(it.name)] = it.id }
            val name = names.getString(value.name)
            val key = callableKey("property", name, value.typeParameterList, value.receiverType(table), value.contextReceiverTypes(table), null)
            parameters(value.typeParameterList, key)
            val members = mutableListOf<JsonObject>()
            JvmProtoBufUtil.getJvmFieldSignature(value, names, table, false)?.let { members += field(it.name, it.desc) }
            if (value.hasExtension(JvmProtoBuf.propertySignature)) {
                val signature = value.getExtension(JvmProtoBuf.propertySignature)
                for (method in listOfNotNull(signature.getter.takeIf { signature.hasGetter() }, signature.setter.takeIf { signature.hasSetter() }))
                    if (method.hasName() && method.hasDesc()) members += methods(names.getString(method.name), names.getString(method.desc))
            }
            declaration(key, name, "property", value.flags, type(value.returnType(table)), members,
                modifiers = buildList { add(if (Flags.IS_VAR.get(value.flags)) "var" else "val"); if (Flags.IS_CONST.get(value.flags)) add("const"); if (Flags.IS_LATEINIT.get(value.flags)) add("lateinit") })
        }
        private fun alias(value: ProtoBuf.TypeAlias) = scope {
            val name = names.getString(value.name)
            val qualified = className?.let { "$it.$name" } ?: pkg.replace('.', '/').let { if (it.isEmpty()) name else "$it/$name" }
            val key = key("alias", qualified)
            value.typeParameterList.forEach { parameterNames[it.id] = names.getString(it.name); parameterIds[names.getString(it.name)] = it.id }
            val generic = parameters(value.typeParameterList, key)
            val type = types.node("alias") {
                put("name", qualified.replace('/', '.')); put("declaration", identity(key))
                putJsonArray("arguments") { add(type(value.expandedType(table))) }
                put("type_parameters", JsonArray(generic.map(::JsonPrimitive)))
            }
            declaration(key, name, "type", value.flags, type, emptyList())
        }
        fun read(): JsonObject {
            if (header.kind == 1 && header.data.isNotEmpty()) {
                val (resolver, value) = JvmProtoBufUtil.readClassDataFrom(header.data.toTypedArray(), header.strings.toTypedArray())
                names = resolver; table = TypeTable(value.typeTable)
                className = resolver.getQualifiedClassName(value.fqName); classKey = key("class", className!!)
                val generic = parameters(value.typeParameterList, classKey!!)
                val type = types.node("named") {
                    put("name", className!!.replace('/', '.')); put("declaration", identity(classKey!!))
                    putJsonArray("constraints") { value.supertypes(table).forEach { add(type(it)) } }
                    put("type_parameters", JsonArray(generic.map(::JsonPrimitive)))
                }
                declaration(classKey!!, className!!.substringAfterLast('/').substringAfterLast('.'), "type", value.flags, type,
                    listOf(member(binary.owner, "class", "", "")), modifiers = buildList {
                        if (Flags.IS_DATA.get(value.flags)) add("data"); if (Flags.IS_VALUE_CLASS.get(value.flags)) add("value"); if (Flags.IS_INNER.get(value.flags)) add("inner")
                    })
                value.constructorList.forEach { constructor ->
                    val key = buildJsonObject { put("kind", "constructor"); putJsonArray("parameters") { constructor.valueParameterList.forEach { add(render(it.type(table))) } } }.toString()
                    val method = JvmProtoBufUtil.getJvmConstructorSignature(constructor, names, table)
                    val signature = types.node("function") {
                        put("return_type", type)
                        putJsonArray("parameter_groups") {
                            add(buildJsonObject {
                                putJsonArray("parameters") {
                                    constructor.valueParameterList.forEach { parameter ->
                                        add(buildJsonObject {
                                            put("type", type(parameter.type(table)))
                                            put("modifiers", JsonArray(emptyList()))
                                        })
                                    }
                                }
                            })
                        }
                    }
                    declaration(key, className!!.substringAfterLast('/').substringAfterLast('.'), "constructor", constructor.flags, signature,
                        method?.let { methods(it.name, it.desc) } ?: emptyList(), classKey)
                }
                value.functionList.forEach(::function); value.propertyList.forEach(::property); value.typeAliasList.forEach(::alias)
            } else if ((header.kind == 2 || header.kind == 5) && header.data.isNotEmpty()) {
                val (resolver, value) = JvmProtoBufUtil.readPackageDataFrom(header.data.toTypedArray(), header.strings.toTypedArray())
                names = resolver; table = TypeTable(value.typeTable)
                value.functionList.forEach(::function); value.propertyList.forEach(::property); value.typeAliasList.forEach(::alias)
            }
            return buildJsonObject {
                put("owner", binary.owner); put("metadata_kind", header.kind); put("package", pkg)
                put("declarations", JsonArray(declarations)); put("identities", JsonArray(identities.values.toList())); put("types", JsonArray(types.records.values.toList()))
            }
        }
    }
}
