package fughetta.kotlin

import kotlinx.serialization.json.*
import fughetta.jvm.JsonLines
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.int
import kotlinx.serialization.json.boolean
import kotlin.test.Test
import kotlin.test.assertNotEquals
import kotlin.test.assertEquals
import kotlin.test.assertTrue
import kotlin.test.assertSame
import kotlin.test.assertNotSame
import org.jetbrains.kotlin.analysis.low.level.api.fir.sessions.cache.LLFirSessionCache
import org.jetbrains.kotlin.analysis.api.analyze
import org.jetbrains.kotlin.psi.KtFile
import org.jetbrains.kotlin.psi.KtNamedFunction
import java.nio.file.Files
import java.nio.file.Path

class EnvironmentTest {
    @Test
    fun compileTimeConstantsFollowValuesNotSpelling() {
        val root = Files.createTempDirectory(Path.of(System.getProperty("java.io.tmpdir")), "constants-")
        try {
            val text = "const val limit = 20 + 2\nval plain = 22\n"
            val file = Files.writeString(root.resolve("Constants.kt"), text)
            val stdlib = Path.of(kotlin.Unit::class.java.protectionDomain.codeSource.location.toURI())
            val inputs = listOf(ModuleInputs("main", listOf(file), listOf(stdlib),
                Path.of(System.getProperty("java.home")), emptyList(), emptyList()))
            Workspace(root, "project").use { workspace ->
                workspace.install(inputs, emptyMap())
                fun records() = workspace.evaluate("main", "unit", "context", { "stdlib" },
                    { error("fixture declarations have native identities") })
                fun constant(records: Projection) = records.symbols.values.single { it["name"]?.jsonPrimitive?.content == "limit" }
                val before = records()
                val initial = constant(before)
                assertEquals("22", initial.getValue("extensions").jsonObject["kotlin.constant_value"]?.jsonPrimitive?.content)
                assertTrue(before.symbols.values.single { it["name"]?.jsonPrimitive?.content == "plain" }
                    .getValue("extensions").jsonObject.isEmpty())
                workspace.install(inputs, mapOf(file to text.replace("20 + 2", "21 + 1")))
                assertEquals(initial["extensions"], constant(records())["extensions"])
                workspace.install(inputs, mapOf(file to text.replace("20 + 2", "23")))
                val changed = constant(records())
                assertEquals(initial["id"], changed["id"])
                assertEquals(initial["type"], changed["type"])
                assertEquals("23", changed.getValue("extensions").jsonObject["kotlin.constant_value"]?.jsonPrimitive?.content)
            }
        } finally { root.toFile().deleteRecursively() }
    }

    @Test
    fun callableValuesKeepReadsSeparateFromInvokeTargets() {
        val root = Files.createTempDirectory(Path.of(System.getProperty("java.io.tmpdir")), "invoke-")
        try {
            val text = "class Callable { operator fun invoke(value: Int) = value }\n" +
                "fun apply(target: Callable, callback: (Int) -> Int) = target(callback(1))\n"
            val file = Files.writeString(root.resolve("Invoke.kt"), text)
            val stdlib = Path.of(kotlin.Unit::class.java.protectionDomain.codeSource.location.toURI())
            val inputs = listOf(ModuleInputs("main", listOf(file), listOf(stdlib),
                Path.of(System.getProperty("java.home")), emptyList(), emptyList()))
            Workspace(root, "project").use { workspace ->
                workspace.install(inputs, emptyMap())
                val records = workspace.evaluate("main", "unit", "context", { "stdlib" },
                    { error("fixture declarations have native identities") })
                fun symbol(name: String) = records.symbols.values.single { it["name"]?.jsonPrimitive?.content == name }.getValue("id")
                val apply = symbol("apply")
                val variables = setOf(symbol("target"), symbol("callback"))
                val reads = records.references.filter { it["from"] == apply && it["target"] in variables }
                assertEquals(variables, reads.map { it.getValue("target") }.toSet())
                assertTrue(reads.all { it.getValue("roles").jsonArray.contains(JsonPrimitive("read")) &&
                    !it.getValue("roles").jsonArray.contains(JsonPrimitive("call")) })
                val calls = records.references.filter { it["from"] == apply && it.getValue("roles").jsonArray.contains(JsonPrimitive("implicit")) }
                assertEquals(2, calls.size)
                assertTrue(calls.any { it["target"] == symbol("invoke") })
                assertTrue(calls.all { it["target"] !in variables && it["resolution"]?.jsonPrimitive?.content == "resolved" })
                assertEquals(calls.map { it["target"] }.toSet(), records.relations.filter { it["from"] == apply &&
                    it["kind"]?.jsonPrimitive?.content == "calls" }.map { it["to"] }.toSet())
            }
        } finally { root.toFile().deleteRecursively() }
    }

    @Test
    fun dataClassGeneratedMembersTrackConstructorPropertyTypes() {
        val root = Files.createTempDirectory(Path.of(System.getProperty("java.io.tmpdir")), "data-members-")
        try {
            val text = "data class Box(val value: Int)\nfun copied(box: Box) = box.copy()\nfun first(box: Box) = box.component1()\n"
            val file = Files.writeString(root.resolve("Data.kt"), text)
            val stdlib = Path.of(kotlin.Unit::class.java.protectionDomain.codeSource.location.toURI())
            val inputs = listOf(ModuleInputs("main", listOf(file), listOf(stdlib),
                Path.of(System.getProperty("java.home")), emptyList(), emptyList()))
            Workspace(root, "project").use { workspace ->
                workspace.install(inputs, emptyMap())
                fun records() = workspace.evaluate("main", "unit", "context", { "stdlib" },
                    { error("these declarations have native identities") })
                fun member(records: Projection, name: String) = records.symbols.values.single {
                    it["name"]?.jsonPrimitive?.content == name && it["kind"]?.jsonPrimitive?.content == "method"
                }
                fun resultType(records: Projection, symbol: JsonObject): String {
                    val signature = records.types.records.getValue(symbol.getValue("type").jsonPrimitive.content)
                    return records.types.records.getValue(signature.getValue("return_type").jsonPrimitive.content)
                        .getValue("name").jsonPrimitive.content
                }
                val before = records()
                val copy = member(before, "copy")
                val component = member(before, "component1")
                assertTrue(before.references.any { it["target"] == copy["id"] })
                assertTrue(before.references.any { it["target"] == component["id"] })
                assertEquals("kotlin.Int", resultType(before, component))
                val generatedNames = before.symbols.values.filter { it["kind"]?.jsonPrimitive?.content == "method" }
                    .map { it.getValue("name").jsonPrimitive.content }.toSet()
                assertTrue(generatedNames.containsAll(setOf("copy", "component1", "equals", "hashCode", "toString")), generatedNames.toString())
                for (name in listOf("copy", "component1", "equals", "hashCode", "toString")) {
                    val symbol = member(before, name)
                    assertTrue(symbol.getValue("modifiers").jsonArray.contains(JsonPrimitive("implicit")))
                    assertTrue(before.occurrences.single { it["symbol"] == symbol["id"] }
                        .getValue("roles").jsonArray.contains(JsonPrimitive("implicit")))
                }
                assertTrue(copy.getValue("parameters").jsonArray.single().jsonObject.getValue("annotations")
                    .jsonObject.getValue("kotlin.has_default").jsonPrimitive.boolean)
                workspace.install(inputs, mapOf(file to text.replace("value: Int", "value: String")))
                val after = records()
                assertEquals(component["id"], member(after, "component1")["id"])
                assertEquals("kotlin.String", resultType(after, member(after, "component1")))
                assertNotEquals(copy["id"], member(after, "copy")["id"])
                assertTrue(after.references.any { it["target"] == member(after, "copy")["id"] })
            }
        } finally { root.toFile().deleteRecursively() }
    }

    @Test
    fun implicitMembersHaveQueryableDeclarationsAndRelativeIdentities() {
        val root = Files.createTempDirectory(Path.of(System.getProperty("java.io.tmpdir")), "implicit-members-")
        try {
            val text = "class Empty\nclass Box(var value: Int)\nfun make() = Empty()\n"
            val file = Files.writeString(root.resolve("Members.kt"), text)
            val stdlib = Path.of(kotlin.Unit::class.java.protectionDomain.codeSource.location.toURI())
            val inputs = listOf(ModuleInputs("main", listOf(file), listOf(stdlib),
                Path.of(System.getProperty("java.home")), emptyList(), emptyList()))
            Workspace(root, "project").use { workspace ->
                workspace.install(inputs, emptyMap())
                fun records() = workspace.evaluate("main", "unit", "context", { "stdlib" },
                    { error("these declarations have native identities") })
                val before = records()
                fun find(name: String, kind: String) = before.symbols.values.single {
                    it["name"]?.jsonPrimitive?.content == name && it["kind"]?.jsonPrimitive?.content == kind
                }
                val empty = find("Empty", "type")
                val constructor = find("Empty", "constructor")
                assertEquals(empty["id"], constructor["owner"])
                assertTrue(before.references.any { it["target"] == constructor["id"] })
                val property = find("value", "property")
                assertTrue(!before.occurrences.single { it["symbol"] == property["id"] }
                    .getValue("roles").jsonArray.contains(JsonPrimitive("implicit")))
                for (accessor in listOf(find("get-value", "method"), find("set-value", "method"))) {
                    assertEquals(property["id"], accessor["owner"])
                    assertEquals(property["id"], accessor.getValue("native_key").jsonObject["owner"])
                    assertTrue(before.occurrences.single { it["symbol"] == accessor["id"] }
                        .getValue("roles").jsonArray.contains(JsonPrimitive("implicit")))
                }
                val ids = before.symbols.keys.toSet()
                workspace.install(inputs, mapOf(file to "\n" + text))
                assertEquals(ids, records().symbols.keys.toSet())
            }
        } finally { root.toFile().deleteRecursively() }
    }

    @Test
    fun constructorOverloadsResolveToTheirDeclaredEntities() {
        val root = Files.createTempDirectory(Path.of(System.getProperty("java.io.tmpdir")), "constructors-")
        try {
            val text = "package sample\nclass Box(val value: Int) { constructor(text: String) : this(text.length) }\n" +
                "fun first() = Box(1)\nfun second() = Box(\"x\")\n"
            val file = Files.writeString(root.resolve("Constructors.kt"), text)
            val stdlib = Path.of(kotlin.Unit::class.java.protectionDomain.codeSource.location.toURI())
            val inputs = listOf(ModuleInputs("main", listOf(file), listOf(stdlib),
                Path.of(System.getProperty("java.home")), emptyList(), emptyList()))
            Workspace(root, "project").use { workspace ->
                workspace.install(inputs, emptyMap())
                fun records() = workspace.evaluate("main", "unit", "context", { "stdlib" },
                    { error("constructors have native identities") })
                val before = records()
                val owner = before.symbols.values.single { it["kind"]?.jsonPrimitive?.content == "type" }
                val constructors = before.symbols.values.filter { it["kind"]?.jsonPrimitive?.content == "constructor" }
                assertEquals(2, constructors.size)
                val targets = constructors.map { it.getValue("id") }.toSet()
                assertEquals(2, targets.size)
                for (constructor in constructors) {
                    assertEquals(owner["id"], constructor["owner"])
                    val signature = before.types.records.getValue(constructor.getValue("type").jsonPrimitive.content)
                    assertEquals("function", signature.getValue("kind").jsonPrimitive.content)
                    assertEquals(owner["id"], before.types.records.getValue(signature.getValue("return_type").jsonPrimitive.content)["declaration"])
                }
                val factories = before.symbols.values.filter { it["name"]?.jsonPrimitive?.content in setOf("first", "second") }.map { it["id"] }.toSet()
                val calls = before.references.filter { it["from"] in factories && it["target"] in targets }
                assertEquals(targets, calls.map { it.getValue("target") }.toSet())
                assertTrue(calls.all { it.getValue("roles").jsonArray.contains(JsonPrimitive("call")) })
                val delegation = before.references.single { it["from"] in targets && it["target"] in targets }
                assertNotEquals(delegation["from"], delegation["target"])
                assertTrue(delegation.getValue("roles").jsonArray.contains(JsonPrimitive("call")))
                assertTrue(!delegation.getValue("roles").jsonArray.contains(JsonPrimitive("implicit")))
                assertTrue(before.relations.any { it["from"] == delegation["from"] && it["to"] == delegation["target"] })
                workspace.install(inputs, mapOf(file to text.replace("this(text.length)", "this(text.length + 1)")))
                val after = records()
                assertEquals(targets, after.symbols.values.filter { it["kind"]?.jsonPrimitive?.content == "constructor" }.map { it.getValue("id") }.toSet())
            }
        } finally { root.toFile().deleteRecursively() }
    }

    @Test
    fun kotlinConsumersSeeJavaSourceEdits() {
        val root = Files.createTempDirectory(Path.of(System.getProperty("java.io.tmpdir")), "java-input-")
        try {
            val original = "package sample; public class JavaApi { public static int value() { return 42; } }"
            val java = Files.writeString(root.resolve("JavaApi.java"), original)
            val consumer = Files.writeString(root.resolve("Consumer.kt"), "package sample\nfun answer() = JavaApi.value()\n")
            val stdlib = Path.of(kotlin.Unit::class.java.protectionDomain.codeSource.location.toURI())
            val inputs = listOf(
                ModuleInputs("java", listOf(java), listOf(stdlib), Path.of(System.getProperty("java.home")), emptyList(), emptyList()),
                ModuleInputs("kotlin", listOf(consumer), listOf(stdlib), Path.of(System.getProperty("java.home")), listOf("java"), emptyList()))
            Workspace(root, "project").use { workspace ->
                workspace.install(inputs, emptyMap())
                fun returnType(): String {
                    val records = workspace.evaluate("kotlin", "unit", "context", { "stdlib" },
                        { error("fixture declarations have native identities") })
                    assertTrue(records.diagnostics.none { it["severity"]?.jsonPrimitive?.content == "error" }, records.diagnostics.toString())
                    val function = records.symbols.values.single { it["name"]?.jsonPrimitive?.content == "answer" }
                    val signature = records.types.records.getValue(function.getValue("type").jsonPrimitive.content)
                    var type = records.types.records.getValue(signature.getValue("return_type").jsonPrimitive.content)
                    if (type["kind"]?.jsonPrimitive?.content == "flexible")
                        type = records.types.records.getValue(type.getValue("lower_bound").jsonPrimitive.content)
                    return type.getValue("name").jsonPrimitive.content
                }
                assertEquals("kotlin.Int", returnType())
                workspace.install(inputs, mapOf(java to original.replace("int value() { return 42; }", "String value() { return \"updated\"; }")))
                assertEquals("kotlin.String", returnType())
                workspace.install(inputs, mapOf(java to original.replace("public class JavaApi", "class Renamed"),
                    consumer to "package sample\nfun answer() = Renamed.value()\n"))
                assertEquals("kotlin.Int", returnType())
                workspace.install(inputs, mapOf(java to original.replace("public class JavaApi", "class Renamed") + " class Extra {}"))
                assertEquals("kotlin.Int", returnType())
                assertEquals(original, Files.readString(java))
            }
        } finally { root.toFile().deleteRecursively() }
    }

    @Test
    fun propertyAccessorsOwnTheirBodiesAndKeepStableIdentities() {
        val root = Files.createTempDirectory(Path.of(System.getProperty("java.io.tmpdir")), "properties-")
        try {
            val text = "package sample\nvar score: Int = 0\n get() = field + 1\n set(input) { field = input }\nval fixed = 7\n"
            val file = Files.writeString(root.resolve("Properties.kt"), text)
            val stdlib = Path.of(kotlin.Unit::class.java.protectionDomain.codeSource.location.toURI())
            val inputs = listOf(ModuleInputs("main", listOf(file), listOf(stdlib),
                Path.of(System.getProperty("java.home")), emptyList(), emptyList()))
            Workspace(root, "project").use { workspace ->
                workspace.install(inputs, emptyMap())
                fun records() = workspace.evaluate("main", "unit", "context", { "stdlib" },
                    { error("property members have native identities") })
                fun symbol(records: Projection, name: String) = records.symbols.values.single { it["name"]?.jsonPrimitive?.content == name }
                val before = records()
                val property = symbol(before, "score")
                assertTrue(property.getValue("modifiers").jsonArray.contains(JsonPrimitive("var")))
                assertTrue(symbol(before, "fixed").getValue("modifiers").jsonArray.contains(JsonPrimitive("val")))
                val getter = symbol(before, "get-score")
                val setter = symbol(before, "set-score")
                assertEquals("sample/score.get", getter.getValue("qualified_name").jsonPrimitive.content)
                assertEquals("sample/score.set", setter.getValue("qualified_name").jsonPrimitive.content)
                for (accessor in listOf(getter, setter)) {
                    assertEquals(property["id"], accessor["owner"])
                    assertEquals("function", before.types.records.getValue(accessor.getValue("type").jsonPrimitive.content)["kind"]?.jsonPrimitive?.content)
                    assertTrue(before.occurrences.any { it["symbol"] == accessor["id"] && it.containsKey("body") })
                    assertTrue(before.references.any { it["from"] == accessor["id"] })
                }
                val field = before.identities.bindings.values.single {
                    Json.parseToJsonElement(it.getValue("declaration").jsonObject.getValue("native_key").jsonPrimitive.content)
                        .jsonObject["kind"]?.jsonPrimitive?.content == "backing_field"
                }.getValue("symbol")
                assertTrue(before.references.any { it["from"] == getter["id"] && it["target"] == field })
                assertTrue(before.references.any { it["from"] == setter["id"] && it["target"] == field })
                workspace.install(inputs, mapOf(file to text.replace("field + 1", "field + 2")))
                val after = records()
                for (name in listOf("score", "get-score", "set-score"))
                    assertEquals(symbol(before, name)["id"], symbol(after, name)["id"])
            }
        } finally { root.toFile().deleteRecursively() }
    }

    @Test
    fun classifierRecordsTrackAliasExpansionAndGenericBounds() {
        val root = Files.createTempDirectory(Path.of(System.getProperty("java.io.tmpdir")), "classifiers-")
        try {
            val text = "package sample\nclass Box<out T : CharSequence>\ntypealias Rows<T> = List<T>\n" +
                "fun <T> present(value: T & Any): T & Any = value\n"
            val file = Files.writeString(root.resolve("Types.kt"), text)
            val stdlib = Path.of(kotlin.Unit::class.java.protectionDomain.codeSource.location.toURI())
            val inputs = listOf(ModuleInputs("main", listOf(file), listOf(stdlib),
                Path.of(System.getProperty("java.home")), emptyList(), emptyList()))
            Workspace(root, "project").use { workspace ->
                workspace.install(inputs, emptyMap())
                fun records() = workspace.evaluate("main", "unit", "context", { "stdlib" },
                    { error("fixture declarations have native identities") })
                fun type(records: Projection, name: String, kind: String = "type"): JsonObject {
                    val symbol = records.symbols.values.single { it["name"]?.jsonPrimitive?.content == name && it["kind"]?.jsonPrimitive?.content == kind }
                    return records.types.records.getValue(symbol.getValue("type").jsonPrimitive.content)
                }
                fun bound(records: Projection): String {
                    val parameter = records.types.records.getValue(type(records, "Box").getValue("type_parameters").jsonArray.single().jsonPrimitive.content)
                    assertTrue(parameter.getValue("qualifiers").jsonArray.contains(JsonPrimitive("out")))
                    return records.types.records.getValue(parameter.getValue("constraints").jsonArray.single().jsonPrimitive.content)
                        .getValue("name").jsonPrimitive.content
                }
                fun expansion(records: Projection): String {
                    val alias = type(records, "Rows")
                    assertEquals("alias", alias.getValue("kind").jsonPrimitive.content)
                    assertEquals(1, alias.getValue("type_parameters").jsonArray.size)
                    return records.types.records.getValue(alias.getValue("arguments").jsonArray.single().jsonPrimitive.content)
                        .getValue("name").jsonPrimitive.content
                }
                val before = records()
                val present = type(before, "present", "function")
                val nonNull = before.types.records.getValue(present.getValue("return_type").jsonPrimitive.content)
                assertEquals("intersection", nonNull.getValue("kind").jsonPrimitive.content)
                assertTrue(nonNull.getValue("qualifiers").jsonArray.contains(JsonPrimitive("definitely_non_null")))
                assertEquals(setOf("T", "kotlin.Any"), nonNull.getValue("elements").jsonArray.map {
                    before.types.records.getValue(it.jsonPrimitive.content).getValue("name").jsonPrimitive.content
                }.toSet())
                assertEquals(present["return_type"], present.getValue("parameter_groups").jsonArray.single().jsonObject
                    .getValue("parameters").jsonArray.single().jsonObject["type"])
                assertEquals("kotlin.CharSequence", bound(before))
                assertEquals("kotlin.collections.List", expansion(before))
                workspace.install(inputs, mapOf(file to text.replace("CharSequence", "Any").replace("List<T>", "Set<T>")))
                val after = records()
                assertEquals("kotlin.Any", bound(after))
                assertEquals("kotlin.collections.Set", expansion(after))
                for (name in listOf("Box", "Rows")) {
                    assertEquals(type(before, name)["declaration"], type(after, name)["declaration"])
                    assertNotEquals(type(before, name)["id"], type(after, name)["id"])
                }
            }
        } finally {
            root.toFile().deleteRecursively()
        }
    }

    @Test
    fun pluginsApplyOnlyToTheirModuleAndReinstallWithOptions() {
        val root = Files.createTempDirectory(Path.of(System.getProperty("java.io.tmpdir")), "plugins-")
        val source = "package sample\nannotation class Open\n@Open class Subject\n"
        val enabled = Files.writeString(root.resolve("Enabled.kt"), source)
        val disabled = Files.writeString(root.resolve("Disabled.kt"), source)
        val stdlib = Path.of(kotlin.Unit::class.java.protectionDomain.codeSource.location.toURI())
        val plugin = Path.of(System.getProperty("fughetta.test.allopen"))
        fun input(id: String, file: Path, annotation: String?) = ModuleInputs(id, listOf(file), listOf(stdlib),
            Path.of(System.getProperty("java.home")), emptyList(), emptyList(),
            compilerArguments = annotation?.let { listOf("-P", "plugin:org.jetbrains.kotlin.allopen:annotation=$it") }.orEmpty(),
            plugins = if (annotation != null) listOf(plugin) else emptyList())
        fun modality(environment: Environment, id: String): String {
            val file = environment.session.modulesWithFiles.getValue(environment.modules.getValue(id)).single() as KtFile
            return analyze(file) {
                file.declarations.filterIsInstance<org.jetbrains.kotlin.psi.KtClass>().single { it.name == "Subject" }.symbol.modality.toString()
            }
        }
        Environment(listOf(input("enabled", enabled, "sample.Open"), input("disabled", disabled, null))).use {
            assertEquals("OPEN", modality(it, "enabled"))
            assertEquals("FINAL", modality(it, "disabled"))
            it.update(mapOf(enabled to source.replace("@Open ", "")))
            assertEquals("FINAL", modality(it, "enabled"))
        }
        val configured = input("configured", enabled, null).copy(
            compilerArguments = listOf("-Xcompiler-plugin=$plugin=annotation=sample.Open",
                "-Xcompiler-plugin-order=org.jetbrains.kotlin.allopen>unused.plugin"),
            plugins = listOf(plugin))
        Environment(listOf(configured, input("disabled", disabled, null))).use {
            assertEquals("OPEN", modality(it, "configured"))
            assertEquals("FINAL", modality(it, "disabled"))
        }
        kotlin.test.assertFailsWith<IllegalStateException> {
            Environment(listOf(configured.copy(compilerArguments = configured.compilerArguments +
                "-Xcompiler-plugin-order=org.jetbrains.kotlin.allopen>org.jetbrains.kotlin.allopen"))).close()
        }
        Environment(listOf(input("enabled", enabled, "sample.Other"))).use {
            assertEquals("FINAL", modality(it, "enabled"))
        }
    }

    @OptIn(org.jetbrains.kotlin.analysis.low.level.api.fir.LLFirInternals::class)
    @Test
    fun dependentModuleSeesEditedReturnType() {
        val root = Files.createTempDirectory(Path.of(System.getProperty("java.io.tmpdir")), "analysis-")
        try {
            val library = root.resolve("generated/Library.kt")
            val base = "open class Base { open fun score() = 1 }\n" +
                "suspend fun <T : CharSequence> String.decorate(value: T, count: Int = 2): T = value\n" +
                "var counter = 0\nfun bump() { counter = 1; counter += 2; counter++; println(counter) }\n" +
                "val bucket = mutableListOf<Int>()\nfun append() { bucket += 1 }\n" +
                "val numbers = intArrayOf(1)\nfun adjust() { numbers[0] += 2; numbers[0]++; numbers[0] = 7; println(numbers[0]) }\n" +
                "val matrix = arrayOf(mutableListOf(1))\nfun appendMatrix() { matrix[0] += 2 }\n"
            val original = "package sample\nfun value() = 42\n" + base
            val objectPath = Files.writeString(root.resolve("source-object"), original)
            val consumer = Files.writeString(root.resolve("Consumer.kt"), "// 🦊\npackage sample\nimport kotlin.String as Text\nimport kotlin.collections.*\nfun answer() = value()\nfun missing() = nowhere()\nval unfinished =\nclass Derived : Base() { override fun score() = 2 }\n")
            val unrelated = Files.writeString(root.resolve("Unrelated.kt"), "package isolated\nfun independent() = 17\n")
            val jdk = Path.of(System.getProperty("java.home"))
            val stdlib = Path.of(kotlin.Unit::class.java.protectionDomain.codeSource.location.toURI())
            val inputs = listOf(
                ModuleInputs("unrelated", listOf(unrelated), listOf(stdlib), jdk, emptyList(), emptyList()),
                ModuleInputs("consumer", listOf(consumer), listOf(stdlib), jdk, listOf("library"), emptyList()),
                ModuleInputs("library", listOf(library), listOf(stdlib), jdk, emptyList(), emptyList(),
                    compilerArguments = listOf("-language-version", "2.3", "-jvm-target", "17", "-module-name", "library-output"),
                    generated = mapOf(library to objectPath)),
            )
            Environment(inputs).use { environment ->
                val identities = Identities("project")
                var consumerId: String? = null
                fun returnType(): String {
                    val file = environment.session.modulesWithFiles.getValue(environment.modules.getValue("consumer")).single() as KtFile
                    val values = Types()
                    val id = analyze(file) {
                        val function = file.declarations.filterIsInstance<KtNamedFunction>().single { it.name == "answer" }
                        val symbol = identities.identify(this, function.symbol, { "consumer" },
                            { error("expected a non-local callable") })
                        consumerId?.let { assertEquals(it, symbol) }
                        consumerId = symbol
                        values.project(this, function.symbol.returnType,
                            declaration = { identities.identify(this, it, { "stdlib" },
                                { error("expected a library classifier") }) },
                            parameter = { error("unexpected type parameter in this fixture") })
                    }
                    return values.records.getValue(id).getValue("name").jsonPrimitive.content
                }
                assertEquals("2.3", environment.modules.getValue("library").languageVersionSettings.languageVersion.versionString)
                assertEquals("library-output", environment.modules.getValue("library").name)
                assertEquals("kotlin.Int", returnType())
                val cache = environment.session.project.getService(LLFirSessionCache::class.java)
                val unrelatedSession = cache.getSession(environment.modules.getValue("unrelated"), false)
                val updated = "package sample\nfun value() = \"changed\"\n" + base
                environment.update(mapOf(library to updated))
                assertEquals(updated, environment.session.modulesWithFiles.getValue(environment.modules.getValue("library")).single().text)
                assertEquals("kotlin.String", returnType())
                assertSame(unrelatedSession, cache.getSession(environment.modules.getValue("unrelated"), false))
                val librarySession = cache.getSession(environment.modules.getValue("library"), false)
                val consumerSession = cache.getSession(environment.modules.getValue("consumer"), false)
                environment.update(mapOf(library to updated.replace("counter += 2", "counter = 2")))
                assertSame(librarySession, cache.getSession(environment.modules.getValue("library"), false), "body edits retain the compiler module session")
                assertSame(consumerSession, cache.getSession(environment.modules.getValue("consumer"), false), "body edits retain consumer resolve sessions")
                val consumerTextBeforeMove = Files.readString(consumer)
                environment.update(mapOf(library to updated.replace("package sample", "package relocated"),
                    consumer to consumerTextBeforeMove.replace("package sample", "package sample\nimport relocated.*\nimport relocated.value as movedValue").replace("= value()", "= movedValue()")))
                assertEquals("kotlin.String", returnType())
                val moved = Projection(root, "project", "unit", "context")
                val movedFile = environment.session.modulesWithFiles.getValue(environment.modules.getValue("consumer")).single() as KtFile
                analyze(movedFile) { moved.file(this, movedFile, { "library" }, { error("expected native import identity") }) }
                val imports = moved.scopes.single { it["kind"]?.jsonPrimitive?.content == "translation_unit" }.getValue("lookup").jsonArray
                assertTrue(imports.any { it.jsonObject["wildcard"]?.jsonPrimitive?.boolean == true })
                assertTrue(imports.any { it.jsonObject["name"]?.jsonPrimitive?.content == "movedValue" })
                assertTrue(moved.references.any { it.getValue("roles").jsonArray.contains(JsonPrimitive("import")) })
                environment.update(mapOf(library to updated, consumer to consumerTextBeforeMove))
                assertEquals("kotlin.String", returnType())
                assertSame(unrelatedSession, cache.getSession(environment.modules.getValue("unrelated"), false))
                val projection = Projection(root, "project", "unit", "context", setOf(library))
                environment.session.modulesWithFiles.values.flatten().filterIsInstance<KtFile>().forEach { file ->
                    analyze(file) {
                        projection.file(this, file, module = { symbol ->
                            when (symbol.psi?.containingFile?.virtualFile?.path) {
                                library.toString() -> "library"
                                consumer.toString() -> "consumer"
                                else -> "stdlib"
                            }
                        }, anchor = { error("fixture has no local declaration") })
                    }
                }
                fun origins(records: Projection) = (records.symbols.values + records.occurrences + records.references +
                    records.diagnostics + records.scopes + records.relations + records.typedRanges).map {
                    it.getValue("ownership").jsonObject.getValue("origin").jsonObject
                }
                origins(projection).forEach { origin ->
                    val path = origin.getValue("location").jsonObject.getValue("source").jsonObject.getValue("path").jsonPrimitive.content
                    assertEquals(if (path == "generated/Library.kt") "generated" else "source", origin.getValue("kind").jsonPrimitive.content)
                }
                System.getenv("FUGHETTA_RECORD_VALIDATOR")?.let { validator ->
                    val templateFile = root.resolve("unit.json")
                    val templateProcess = ProcessBuilder(validator, "--unit", templateFile.toString()).inheritIO().start()
                    assertEquals(0, templateProcess.waitFor())
                    val template = Files.readString(templateFile)
                    val shardFile = Files.writeString(root.resolve("records.json"),
                        projection.shard(Json.parseToJsonElement(template).jsonObject).toString())
                    val checked = ProcessBuilder(validator, "--shard", shardFile.toString()).inheritIO().start()
                    assertEquals(0, checked.waitFor())
                }
                val decorated = projection.symbols.values.single { it["name"]?.jsonPrimitive?.content == "decorate" }
                val signature = projection.types.records.getValue(decorated.getValue("type").jsonPrimitive.content)
                assertEquals("function", signature.getValue("kind").jsonPrimitive.content)
                assertEquals("kotlin.String", projection.types.records.getValue(signature.getValue("receiver").jsonPrimitive.content)["name"]?.jsonPrimitive?.content)
                assertEquals(2, signature.getValue("parameter_groups").jsonArray.single().jsonObject.getValue("parameters").jsonArray.size)
                assertEquals(1, signature.getValue("type_parameters").jsonArray.size)
                assertTrue(signature.getValue("qualifiers").jsonArray.contains(JsonPrimitive("suspend")))
                assertTrue(decorated.getValue("modifiers").jsonArray.contains(JsonPrimitive("suspend")))
                assertEquals("2", decorated.getValue("parameters").jsonArray[1].jsonObject["default_value"]?.jsonPrimitive?.content)
                val value = projection.symbols.values.single { it.getValue("name").jsonPrimitive.content == "value" && it["kind"]?.jsonPrimitive?.content == "function" }
                val reference = projection.references.single { it["target"] == value["id"] }
                assertEquals("resolved", reference.getValue("resolution").jsonPrimitive.content)
                val consumerText = Files.readString(consumer)
                val callStart = consumerText.codePointCount(0, consumerText.indexOf("value()"))
                assertTrue(projection.typedRanges.none { record ->
                    val range = record.getValue("range").jsonObject
                    range.getValue("path").jsonPrimitive.content == "Consumer.kt" && range.getValue("begin").jsonPrimitive.int == callStart
                }, "a call that is neither generic, overloaded nor virtual has no typed range")
                assertEquals(consumerText.codePointCount(0, consumerText.indexOf("value()")),
                    reference.getValue("location").jsonObject.getValue("source").jsonObject.getValue("begin").jsonPrimitive.int)
                val answer = projection.symbols.values.single { it.getValue("name").jsonPrimitive.content == "answer" }
                val call = projection.relations.single { it["to"] == value["id"] }
                assertEquals(answer["id"], call["from"])
                assertEquals(reference["id"], call["reference"])
                val scope = projection.scopes.single { it["id"] == answer["declaring_scope"] }
                assertTrue(scope.getValue("declared_members").jsonArray.contains(answer.getValue("id")))
                val baseSymbol = projection.symbols.values.single { it["name"]?.jsonPrimitive?.content == "Base" && it["kind"]?.jsonPrimitive?.content == "type" }
                val derived = projection.symbols.values.single { it["name"]?.jsonPrimitive?.content == "Derived" && it["kind"]?.jsonPrimitive?.content == "type" }
                assertTrue(projection.relations.any { it["kind"]?.jsonPrimitive?.content == "inherits" &&
                    it["from"] == derived["id"] && it["to"] == baseSymbol["id"] })
                val override = projection.relations.single { it["kind"]?.jsonPrimitive?.content == "overrides" }
                assertEquals("method", projection.symbols.getValue(override.getValue("from").jsonPrimitive.content)["kind"]?.jsonPrimitive?.content)
                assertEquals(baseSymbol["id"], projection.symbols.getValue(override.getValue("to").jsonPrimitive.content)["owner"])
                assertEquals(21, projection.occurrences.count { !it.getValue("roles").jsonArray.contains(JsonPrimitive("implicit")) })
                fun accesses(records: Projection, name: String): List<Set<String>> {
                    val symbol = records.symbols.values.single { it["name"]?.jsonPrimitive?.content == name }
                    return records.references.filter { it["target"] == symbol["id"] }
                        .sortedBy { it.getValue("location").jsonObject.getValue("source").jsonObject.getValue("begin").jsonPrimitive.int }
                        .map { it.getValue("roles").jsonArray.map { role -> role.jsonPrimitive.content }.filter { it == "read" || it == "write" }.toSet() }
                }
                assertEquals(listOf(setOf("write"), setOf("read", "write"), setOf("read", "write"), setOf("read")), accesses(projection, "counter"))
                assertEquals(listOf(setOf("read")), accesses(projection, "bucket"))
                val operationNames = projection.references.filter { reference ->
                    val position = reference.getValue("location").jsonObject.getValue("source").jsonObject
                    !reference.getValue("roles").jsonArray.contains(JsonPrimitive("implicit")) &&
                        position.getValue("path").jsonPrimitive.content == "generated/Library.kt" &&
                        updated.substring(position.getValue("begin").jsonPrimitive.int, position.getValue("end").jsonPrimitive.int) in setOf("+=", "++")
                }.map { reference ->
                    assertTrue(reference.getValue("roles").jsonArray.contains(JsonPrimitive("call")))
                    assertTrue(projection.relations.any { it["reference"] == reference["id"] && it["kind"]?.jsonPrimitive?.content == "calls" })
                    assertTrue(reference["target"] != null, reference.toString())
                    val binding = projection.identities.bindings.getValue(reference.getValue("target").jsonPrimitive.content)
                    Json.parseToJsonElement(binding.getValue("declaration").jsonObject.getValue("native_key").jsonPrimitive.content)
                        .jsonObject.getValue("name").jsonPrimitive.content.substringAfterLast('/').substringAfterLast('.')
                }
                assertEquals(listOf("plus", "inc", "plusAssign", "plus", "inc", "plusAssign"), operationNames)
                val arrayCalls = projection.references.filter { it.getValue("roles").jsonArray.contains(JsonPrimitive("implicit")) }
                assertEquals(7, arrayCalls.size)
                assertEquals(listOf("get", "set", "get", "set", "set", "get", "get"), arrayCalls.map { reference ->
                    assertEquals("resolved", reference.getValue("resolution").jsonPrimitive.content)
                    assertTrue(projection.relations.any { it["reference"] == reference["id"] && it["resolution"]?.jsonPrimitive?.content == "resolved" })
                    val identity = projection.identities.bindings.getValue(reference.getValue("target").jsonPrimitive.content)
                    Json.parseToJsonElement(identity.getValue("declaration").jsonObject.getValue("native_key").jsonPrimitive.content)
                        .jsonObject.getValue("name").jsonPrimitive.content.substringAfterLast('/').substringAfterLast('.')
                })
                assertTrue(projection.diagnostics.any { it.getValue("code").jsonPrimitive.content == "UNRESOLVED_REFERENCE" })
                assertTrue(projection.diagnostics.any { it.getValue("code").jsonPrimitive.content == "SYNTAX" })
                assertTrue(projection.references.any { it.getValue("resolution").jsonPrimitive.content == "unresolved" })
                assertEquals(original, Files.readString(objectPath))
                assertEquals(false, Files.exists(library))
                Workspace(root, "project").use { workspace ->
                    workspace.install(inputs, emptyMap())
                    val request = buildJsonObject {
                        put("request_id", 17); put("operation", "units")
                        putJsonObject("params") { putJsonArray("files") { add(consumer.toString()) } }
                    }.toString() + "\n"
                    val output = java.io.ByteArrayOutputStream()
                    JsonLines().serve(request.byteInputStream(), output) { operation, params ->
                        assertEquals("units", operation)
                        JsonArray(workspace.units(params.getValue("files").jsonArray.map {
                            Path.of(it.jsonPrimitive.content)
                        }.toSet()).map { JsonPrimitive(it) })
                    }
                    val reply = Json.parseToJsonElement(output.toString(Charsets.UTF_8)).jsonObject
                    assertEquals(17, reply.getValue("request_id").jsonPrimitive.int)
                    assertEquals(listOf("consumer"), reply.getValue("result").jsonArray.map { it.jsonPrimitive.content })
                    fun exportedType(): String {
                        val records = workspace.evaluate("consumer", "unit", "context", { "stdlib" },
                            { error("fixture has no local declaration") })
                        val declaration = records.symbols.values.single { it["name"]?.jsonPrimitive?.content == "answer" }
                        val function = records.types.records.getValue(declaration.getValue("type").jsonPrimitive.content)
                        return records.types.records.getValue(function.getValue("return_type").jsonPrimitive.content)
                            .getValue("name").jsonPrimitive.content
                    }
                    fun records(module: String) = workspace.evaluate(module, "unit", "context", { "stdlib" },
                        { error("fixture has no local declaration") })
                    assertEquals("kotlin.Int", exportedType())
                    val initial = records("consumer")
                    val firstValues = requireNotNull(workspace.observe("library"))
                    assertTrue(origins(initial).all { it.getValue("kind").jsonPrimitive.content == "source" })
                    assertTrue(origins(records("library")).all { it.getValue("kind").jsonPrimitive.content == "generated" })
                    val independent = records("unrelated")
                    assertSame(initial, records("consumer"))
                    workspace.install(inputs, mapOf(library to updated))
                    assertEquals("kotlin.String", exportedType())
                    val changed = records("consumer")
                    val changedValues = requireNotNull(workspace.observe("library"))
                    assertNotEquals(firstValues.declarations, changedValues.declarations, "inferred return types belong to declaration observations")
                    assertNotSame(initial, changed)
                    assertSame(independent, records("unrelated"))
                    workspace.install(inputs, mapOf(library to updated))
                    assertSame(changed, records("consumer"))
                    workspace.install(inputs, mapOf(library to updated.replace("counter += 2", "counter = 2")))
                    assertSame(changed, records("consumer"), "body-only edits keep the consumer's value projection")
                    assertEquals(changedValues, workspace.observe("library"), "body-only implementation edits do not change observed declaration or inline values")
                    assertEquals(listOf(setOf("write"), setOf("write"), setOf("read", "write"), setOf("read")), accesses(records("library"), "counter"))
                    assertTrue(origins(records("library")).all { it.getValue("kind").jsonPrimitive.content == "generated" })
                    val inlineVersion = updated.replace("fun bump()", "inline fun bump()")
                    workspace.install(inputs, mapOf(library to inlineVersion))
                    val inlineConsumer = records("consumer")
                    val inlineValues = requireNotNull(workspace.observe("library"))
                    workspace.install(inputs, mapOf(library to inlineVersion.replace("counter += 2", "counter = 2")))
                    assertNotSame(inlineConsumer, records("consumer"), "inline implementations remain consumed across module boundaries")
                    val modifiedInline = requireNotNull(workspace.observe("library"))
                    assertEquals(inlineValues.declarations, modifiedInline.declarations)
                    assertNotEquals(inlineValues.inlineBodies, modifiedInline.inlineBodies)
                    workspace.install(inputs, mapOf(library to original))
                    assertEquals("kotlin.Int", exportedType())
                    assertEquals(firstValues, workspace.observe("library"), "semantic observations return to the prior worldline value")
                    assertNotSame(changed, records("consumer"), "returning to an earlier source invalidates inferred signature reads")
                    // A consumer-limited digest observes only declarations its lookups could name;
                    // the whole-module digest still covers every declaration.
                    val selectors = records("consumer").observationScope().selectors
                    val restricted = requireNotNull(workspace.observe("library", selectors))
                    workspace.install(inputs, mapOf(library to original + "fun unrelatedDeclaration(): Int = 1\n"))
                    assertEquals(restricted, workspace.observe("library", selectors),
                        "declarations outside the consumer's observed names do not change its restricted digest")
                    assertNotEquals(restricted, workspace.observe("library"),
                        "the whole-module digest still observes every declaration")
                    workspace.install(inputs, mapOf(library to original))
                }
            }
        } finally {
            root.toFile().deleteRecursively()
        }
    }
}
