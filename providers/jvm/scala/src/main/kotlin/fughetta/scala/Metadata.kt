package fughetta.scala

import java.lang.classfile.Attributes
import java.lang.classfile.ClassFile
import java.lang.classfile.ClassModel
import java.lang.constant.ClassDesc
import java.lang.constant.MethodTypeDesc
import java.lang.reflect.AccessFlag
import java.nio.file.Files
import java.nio.file.Path
import java.util.jar.JarFile
import java.util.zip.ZipFile
import kotlinx.serialization.json.*
import scala.reflect.NameTransformer

internal object Metadata {
    private data class Member(val name: String, val descriptor: String, val flags: Int, val method: Boolean)

    fun read(paths: List<Path>, release: Int): JsonObject = buildJsonObject {
        val classes = linkedMapOf<String, JsonObject>()
        for (path in paths) {
            fun consume(bytes: ByteArray, tasty: Boolean = false) {
                val model = ClassFile.of().parse(bytes)
                val attributes = model.attributes().map { it.attributeName().stringValue() }
                val scalaSignature = model.findAttribute(Attributes.runtimeVisibleAnnotations()).orElse(null)
                    ?.annotations()?.any {
                        it.className().stringValue() in setOf("Lscala/reflect/ScalaSignature;", "Lscala/reflect/ScalaLongSignature;")
                    } == true
                val kind = when {
                    tasty || "TASTY" in attributes -> 2
                    scalaSignature || "Scala" in attributes || "ScalaSig" in attributes -> 1
                    else -> return
                }
                val binary = Binary(model, kind)
                classes.putIfAbsent(binary.owner, binary.report())
            }
            when {
                Files.isDirectory(path) -> Files.walk(path).use { files ->
                    files.filter { Files.isRegularFile(it) && it.toString().endsWith(".class") }.sorted()
                        .forEach { file ->
                            val tasty = file.fileName.toString().removeSuffix(".class").removeSuffix("$") + ".tasty"
                            consume(Files.readAllBytes(file), Files.isRegularFile(file.resolveSibling(tasty)))
                        }
                }
                Files.newInputStream(path).use { it.readNBytes(4).contentEquals(byteArrayOf(-54, -2, -70, -66)) } ->
                    consume(Files.readAllBytes(path))
                else -> {
                    val version = if (release > 0) Runtime.Version.parse(release.toString()) else JarFile.baseVersion()
                    JarFile(path.toFile(), false, ZipFile.OPEN_READ, version).use { archive ->
                        require(release > 0 || !archive.isMultiRelease) { "multi-release JAR requires an explicit java_release" }
                        archive.versionedStream().use { entries ->
                            entries.filter { !it.isDirectory && it.name.endsWith(".class") }.sorted(compareBy { it.name })
                                .forEach { entry -> archive.getInputStream(entry).use { consume(it.readBytes()) } }
                        }
                    }
                }
            }
        }
        put("classes", JsonArray(classes.values.toList()))
    }

    private class Binary(model: ClassModel, private val metadataKind: Int) {
        val owner = model.thisClass().asInternalName()
        private val flags = model.flags().flagsMask()
        private val base = model.superclass().orElse(null)?.asInternalName()
        private val interfaces = model.interfaces().map { it.asInternalName() }
        private val enclosing = model.findAttribute(Attributes.innerClasses()).orElse(null)?.classes()
            ?.firstOrNull { it.innerClass().asInternalName() == owner }?.outerClass()?.orElse(null)?.asInternalName()
        private val members = model.fields().map {
            Member(it.fieldName().stringValue(), it.fieldType().stringValue(), it.flags().flagsMask(), false)
        } + model.methods().map {
            Member(it.methodName().stringValue(), it.methodType().stringValue(), it.flags().flagsMask(), true)
        }
        private val types = Types()
        private val identities = linkedMapOf<String, JsonObject>()

        private fun bind(key: String, ownerKey: String? = null): String {
            val id = "metadata:${digest(listOfNotNull(key, ownerKey).strings().toString())}"
            identities.putIfAbsent(id, buildJsonObject { put("id", id); put("key", key); ownerKey?.let { put("owner_key", it) } })
            return id
        }

        private fun classKind(name: String) = when {
            name.endsWith('$') -> "scala:object"
            name == owner && flags and AccessFlag.INTERFACE.mask() != 0 -> "scala:trait"
            else -> "type"
        }

        private fun type(jvm: ClassDesc): String = when {
            jvm.isArray -> types.node("application") {
                put("name", "scala.Array"); put("arguments", listOf(type(jvm.componentType())).strings())
            }
            jvm.isClassOrInterface -> types.node("named") {
                val internal = jvm.descriptorString().removePrefix("L").removeSuffix(";")
                put("name", internal.replace('/', '.')); put("declaration", bind(nativeKey(classKind(internal), internal)))
            }
            else -> types.node("primitive") {
                put("name", primitiveDescriptors.entries.firstOrNull { it.value == jvm.descriptorString() }?.key ?: jvm.displayName())
            }
        }

        private fun visibility(access: Int) = when {
            access and AccessFlag.PRIVATE.mask() != 0 -> "private"
            access and AccessFlag.PROTECTED.mask() != 0 -> "protected"
            access and AccessFlag.PUBLIC.mask() != 0 -> "public"
            else -> "package"
        }

        private fun modifiers(access: Int) = buildList {
            if (access and AccessFlag.ABSTRACT.mask() != 0) add("abstract")
            if (access and AccessFlag.FINAL.mask() != 0) add("final")
            if (access and AccessFlag.NATIVE.mask() != 0) add("native")
            if (access and AccessFlag.STATIC.mask() != 0) add("static")
        }.strings()

        private fun jvmMember(member: Member) = buildJsonObject {
            put("owner", owner); put("kind", if (member.method) "method" else "field")
            put("name", member.name); put("descriptor", member.descriptor)
        }

        fun report(): JsonObject {
            val name = owner.replace('/', '.')
            val kind = classKind(owner)
            val key = nativeKey(kind, owner)
            val parent = enclosing?.let { nativeKey(classKind(it), it) }
            val id = bind(key)
            val classType = types.node("named") {
                put("name", name); put("declaration", id)
                put("constraints", (listOfNotNull(base) + interfaces).map { type(ClassDesc.ofInternalName(it)) }.strings())
            }
            val declarations = mutableListOf(buildJsonObject {
                put("id", id); put("key", key); parent?.let { put("containing_key", it) }
                put("name", name.substringAfterLast('.').removeSuffix("$")); put("qualified_name", name.removeSuffix("$"))
                put("kind", kind); put("type", classType); put("visibility", visibility(flags))
                put("modifiers", modifiers(flags)); putJsonArray("parameters") {}; putJsonArray("jvm_members") {}
            })
            val fields = members.filter { !it.method }.map { it.name }.toSet()
            for (member in members) {
                if (member.name == "<clinit>" || member.name == "MODULE$" || member.flags and AccessFlag.BRIDGE.mask() != 0) continue
                val signature = if (member.method) MethodTypeDesc.ofDescriptor(member.descriptor) else null
                val arguments = signature?.parameterList().orEmpty()
                val memberKind = when {
                    member.name == "<init>" -> "constructor"
                    !member.method -> "field"
                    member.name in fields && arguments.isEmpty() -> "property"
                    member.name.endsWith("\$_eq") -> "property"
                    else -> "method"
                }
                val memberKey = nativeKey(memberKind, member.name,
                    signature?.let { arguments.joinToString("") { it.descriptorString() } })
                val memberType = if (signature == null) type(ClassDesc.ofDescriptor(member.descriptor)) else types.node("function") {
                    put("return_type", type(signature.returnType()))
                    if (member.flags and AccessFlag.STATIC.mask() == 0) put("receiver", classType)
                    putJsonArray("parameter_groups") { add(buildJsonObject { putJsonArray("parameters") {
                        arguments.forEach { argument -> add(buildJsonObject { put("type", type(argument)); putJsonArray("modifiers") {} }) }
                    } }) }
                }
                val simpleName = if (memberKind == "constructor") name.substringAfterLast('.').removeSuffix("$")
                    else NameTransformer.decode(member.name)
                declarations.add(buildJsonObject {
                    put("id", bind(memberKey, key)); put("key", memberKey); put("owner_key", key); put("containing_key", key)
                    put("name", simpleName); put("qualified_name", "${name.removeSuffix("$")}.$simpleName")
                    put("kind", memberKind); put("type", memberType); put("visibility", visibility(member.flags))
                    put("modifiers", modifiers(member.flags))
                    putJsonArray("parameters") { arguments.indices.forEach { index -> add(buildJsonObject {
                        put("group", 0); put("index", index); putJsonObject("annotations") {}
                    }) } }
                    put("jvm_members", JsonArray(listOf(jvmMember(member))))
                })
            }
            return buildJsonObject {
                put("owner", owner); put("package", owner.substringBeforeLast('/', "").replace('/', '.'))
                put("language", "scala"); put("domain", "scala"); put("metadata_kind", metadataKind)
                put("declarations", JsonArray(declarations)); put("identities", JsonArray(identities.values.toList()))
                put("types", JsonArray(types.records.values.toList()))
            }
        }
    }
}
