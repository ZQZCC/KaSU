package ka.su.ui.util

import android.content.ContentResolver
import android.content.Context
import android.database.Cursor
import android.net.Uri
import android.os.SystemClock
import android.provider.OpenableColumns
import android.util.Log
import com.topjohnwu.superuser.CallbackList
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ShellUtils
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import ka.su.BuildConfig
import ka.su.Natives
import ka.su.data.model.FeatureSettings
import ka.su.data.model.FeatureState
import ka.su.ksuApp
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.util.zip.ZipFile

/**
 * @author weishu
 * @date 2023/1/1.
 */
private const val TAG = "KsuCli"

private fun getKsuDaemonPath(): String {
    return ksuApp.applicationInfo.nativeLibraryDir + File.separator + "libd.so"
}

data class FlashResult(val code: Int, val err: String, val showReboot: Boolean) {
    constructor(result: Shell.Result, showReboot: Boolean) : this(result.code, result.err.joinToString("\n"), showReboot)
    constructor(result: Shell.Result) : this(result, result.isSuccess)
}

object KsuCli {
    val SHELL: Shell by lazy { createRootShell() }
    val GLOBAL_MNT_SHELL: Shell by lazy { createRootShell(true) }
}

fun getRootShell(globalMnt: Boolean = false): Shell {
    return if (globalMnt) KsuCli.GLOBAL_MNT_SHELL else {
        KsuCli.SHELL
    }
}

inline fun <T> withNewRootShell(
    globalMnt: Boolean = false,
    block: Shell.() -> T
): T {
    return createRootShell(globalMnt).use(block)
}

fun Uri.getFileName(context: Context): String? {
    var fileName: String? = null
    val contentResolver: ContentResolver = context.contentResolver
    val cursor: Cursor? = contentResolver.query(this, null, null, null, null)
    cursor?.use {
        if (it.moveToFirst()) {
            fileName = it.getString(it.getColumnIndexOrThrow(OpenableColumns.DISPLAY_NAME))
        }
    }
    return fileName
}

fun createRootShell(globalMnt: Boolean = false): Shell {
    Shell.enableVerboseLogging = BuildConfig.DEBUG
    val builder = Shell.Builder.create()
    return try {
        if (globalMnt) {
            builder.build(getKsuDaemonPath(), "debug", "su", "-g")
        } else {
            builder.build(getKsuDaemonPath(), "debug", "su")
        }
    } catch (e: Throwable) {
        Log.w(TAG, "ksu failed: ", e)
        try {
            if (globalMnt) {
                builder.build("su", "-mm")
            } else {
                builder.build("su")
            }
        } catch (e: Throwable) {
            Log.e(TAG, "su failed: ", e)
            builder.build("sh")
        }
    }
}

fun execKsud(args: String, newShell: Boolean = false, globalMnt: Boolean = false): Boolean {
    return if (newShell) {
        withNewRootShell(globalMnt = globalMnt) {
            ShellUtils.fastCmdResult(this, "${getKsuDaemonPath()} $args")
        }
    } else {
        ShellUtils.fastCmdResult(getRootShell(globalMnt), "${getKsuDaemonPath()} $args")
    }
}

suspend fun getFeatureSettings(): FeatureSettings = withContext(Dispatchers.IO) {
    val result = getRootShell().newJob()
        .add("${getKsuDaemonPath()} feature snapshot").to(ArrayList<String>(), null).exec()
    if (!result.isSuccess) {
        Log.w(TAG, "Read feature settings: ${result.err.joinToString("\n")}")
        return@withContext FeatureSettings()
    }
    runCatching {
        val json = JSONObject(result.out.joinToString("\n"))
        fun feature(name: String): FeatureState {
            val value = json.getJSONObject(name)
            return FeatureState(value.getString("status"), value.optString("configured").toLongOrNull())
        }
        FeatureSettings(
            suCompat = feature("su_compat"),
            selinuxHide = feature("selinux_hide"),
            sulog = feature("sulog"),
            adbRoot = feature("adb_root"),
            hideBootloaderEnabled = if (json.isNull("hide_bootloader")) null else json.getBoolean("hide_bootloader"),
        )
    }.getOrElse {
        Log.w(TAG, "Invalid feature settings", it)
        FeatureSettings()
    }
}

fun install() {
    val start = SystemClock.elapsedRealtime()
    val result = execKsud("install", true)
    Log.w(TAG, "install result: $result, cost: ${SystemClock.elapsedRealtime() - start}ms")
}

fun listModules(includeSize: Boolean = false): String {
    val shell = getRootShell()
    val option = if (includeSize) " --size" else ""

    val out = shell.newJob()
        .add("${getKsuDaemonPath()} module list$option").to(ArrayList(), null).exec().out
    return out.joinToString("\n").ifBlank { "[]" }
}

fun getModuleCount(): Int {
    val result = listModules()
    runCatching {
        val array = JSONArray(result)
        return array.length()
    }.getOrElse { return 0 }
}

fun getSuperuserCount(): Int {
    return Natives.getSuperuserCount()
}

fun toggleModule(id: String, enable: Boolean): Boolean {
    val cmd = if (enable) {
        "module enable $id"
    } else {
        "module disable $id"
    }
    val result = execKsud(cmd, true)
    Log.i(TAG, "$cmd result: $result")
    return result
}

fun undoUninstallModule(id: String): Boolean {
    val cmd = "module undo-uninstall $id"
    val result = execKsud(cmd, true)
    Log.i(TAG, "undo uninstall module $id result: $result")
    return result
}

fun uninstallModule(id: String): Boolean {
    val cmd = "module uninstall $id"
    val result = execKsud(cmd, true)
    Log.i(TAG, "uninstall module $id result: $result")
    return result
}

private fun flashWithIO(
    cmd: String,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit
): Shell.Result {

    val stdoutCallback: CallbackList<String?> = object : CallbackList<String?>() {
        override fun onAddElement(s: String?) {
            onStdout(s ?: "")
        }
    }

    val stderrCallback: CallbackList<String?> = object : CallbackList<String?>() {
        override fun onAddElement(s: String?) {
            onStderr(s ?: "")
        }
    }

    return withNewRootShell {
        newJob().add(cmd).to(stdoutCallback, stderrCallback).exec()
    }
}

fun flashModule(
    uri: Uri,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit
): FlashResult {
    var file: File? = null
    try {
        val zip = File.createTempFile("module-", ".zip", ksuApp.cacheDir)
        file = zip
        val input = ksuApp.contentResolver.openInputStream(uri) ?: throw IOException("Cannot open $uri")
        input.use { source -> zip.outputStream().use { source.copyTo(it) } }
        val cmd = "module install ${zip.absolutePath}"
        val result = flashWithIO("${getKsuDaemonPath()} $cmd", onStdout, onStderr)
        Log.i("KernelSU", "install module $uri result: $result")
        return FlashResult(result, result.isSuccess && moduleNeedsReboot(zip))
    } catch (e: IOException) {
        onStderr(e.toString())
        return FlashResult(1, e.toString(), false)
    } catch (e: SecurityException) {
        onStderr(e.toString())
        return FlashResult(1, e.toString(), false)
    } finally {
        file?.delete()
    }
}

private val MODULE_ID_REGEX = Regex("^[a-zA-Z][a-zA-Z0-9._-]+$")

private fun moduleNeedsReboot(zip: File): Boolean {
    val id = runCatching {
        ZipFile(zip).use { archive ->
            val entry = archive.getEntry("module.prop") ?: return@use null
            archive.getInputStream(entry).bufferedReader().useLines { lines ->
                lines.map { it.trim() }.firstOrNull { it.startsWith("id=") }?.substringAfter('=')?.trim()
            }
        }
    }.getOrNull()
    if (id == null || !MODULE_ID_REGEX.matches(id)) return true
    val dir = "/data/adb/modules/$id"
    return !ShellUtils.fastCmdResult(getRootShell(), "[ -d $dir ] && [ ! -e $dir/update ]")
}

fun flashAnyKernel(
    uri: Uri,
    inactiveSlot: Boolean,
    onStdout: (String) -> Unit,
    onStderr: (String) -> Unit,
): FlashResult {
    var zip: File? = null
    try {
        val file = File.createTempFile("anykernel-", ".zip", ksuApp.cacheDir)
        zip = file
        val input = ksuApp.contentResolver.openInputStream(uri)
            ?: return FlashResult(1, "Cannot open the selected archive", false)
        input.use { source -> file.outputStream().use { source.copyTo(it) } }

        val slot = if (inactiveSlot) "inactive" else "active"
        val cmd = """
            (
                set -eu
                work='${file.absolutePath}.d'
                trap 'status=${'$'}?; rm -rf "${'$'}work"; exit "${'$'}status"' EXIT
                mkdir -p "${'$'}work"
                bb=/data/adb/ksu/bin/busybox
                "${'$'}bb" unzip -p '${file.absolutePath}' META-INF/com/google/android/update-binary > "${'$'}work/update-binary"
                "${'$'}bb" grep -q AnyKernel3 "${'$'}work/update-binary" || {
                    echo 'Not an AnyKernel3 archive' >&2
                    exit 1
                }
                ASH_STANDALONE=1 SLOT_SELECT=$slot AKHOME="${'$'}work/ak3" \
                    "${'$'}bb" ash "${'$'}work/update-binary" 3 1 '${file.absolutePath}'
            )
        """.trimIndent()
        val result = flashWithIO(cmd, { line ->
            when {
                line.startsWith("ui_print ") -> onStdout(line.removePrefix("ui_print "))
                line.trim() == "ui_print" -> Unit
                else -> onStderr(line)
            }
        }, onStderr)
        return FlashResult(result)
    } catch (e: IOException) {
        return FlashResult(1, e.toString(), false)
    } catch (e: SecurityException) {
        return FlashResult(1, e.toString(), false)
    } finally {
        zip?.delete()
    }
}

fun runModuleAction(
    moduleId: String, onStdout: (String) -> Unit, onStderr: (String) -> Unit
): Boolean {
    val stdoutCallback: CallbackList<String?> = object : CallbackList<String?>() {
        override fun onAddElement(s: String?) {
            onStdout(s ?: "")
        }
    }

    val stderrCallback: CallbackList<String?> = object : CallbackList<String?>() {
        override fun onAddElement(s: String?) {
            onStderr(s ?: "")
        }
    }

    val result = withNewRootShell(true) {
        newJob().add("${getKsuDaemonPath()} module action $moduleId")
            .to(stdoutCallback, stderrCallback).exec()
    }

    Log.i("KernelSU", "Module runAction result: $result")

    return result.isSuccess
}

fun reboot(reason: String = "") {
    if (reason == "soft_reboot") {
        execKsud("soft-reboot", true, true)
        return
    }
    val shell = getRootShell()
    if (reason == "recovery") {
        // KEYCODE_POWER = 26, hide incorrect "Factory data reset" message
        ShellUtils.fastCmd(shell, "/system/bin/input keyevent 26")
    }
    ShellUtils.fastCmd(shell, "/system/bin/svc power reboot $reason || /system/bin/reboot $reason")
}

fun rootAvailable(): Boolean {
    val shell = getRootShell()
    return shell.isRoot
}

fun hasMagisk(): Boolean {
    val shell = getRootShell(true)
    val result = shell.newJob().add("which magisk").exec()
    Log.i(TAG, "has magisk: ${result.isSuccess}")
    return result.isSuccess
}

fun isSepolicyValid(rules: String?): Boolean {
    if (rules == null) {
        return true
    }
    val shell = getRootShell()
    val result =
        shell.newJob().add("${getKsuDaemonPath()} sepolicy check '$rules'").to(ArrayList(), null)
            .exec()
    return result.isSuccess
}

fun getSepolicy(pkg: String): String {
    val shell = getRootShell()
    val result =
        shell.newJob().add("${getKsuDaemonPath()} profile get-sepolicy $pkg").to(ArrayList(), null)
            .exec()
    Log.i(TAG, "code: ${result.code}, out: ${result.out}, err: ${result.err}")
    return result.out.joinToString("\n")
}

fun setSepolicy(pkg: String, rules: String): Boolean {
    val shell = getRootShell()
    val result = shell.newJob().add("${getKsuDaemonPath()} profile set-sepolicy $pkg '$rules'")
        .to(ArrayList(), null).exec()
    Log.i(TAG, "set sepolicy result: ${result.code}")
    return result.isSuccess
}

fun listAppProfileTemplates(): List<String> {
    val shell = getRootShell()
    return shell.newJob().add("${getKsuDaemonPath()} profile list-templates").to(ArrayList(), null)
        .exec().out
}

fun getAppProfileTemplate(id: String): String {
    val shell = getRootShell()
    return shell.newJob().add("${getKsuDaemonPath()} profile get-template '${id}'")
        .to(ArrayList(), null).exec().out.joinToString("\n")
}

fun setAppProfileTemplate(id: String, template: String): Boolean {
    val shell = getRootShell()
    val escapedTemplate = template.replace("'", "'\\''")
    val cmd = """${getKsuDaemonPath()} profile set-template "$id" '$escapedTemplate'"""
    return shell.newJob().add(cmd)
        .to(ArrayList(), null).exec().isSuccess
}

fun deleteAppProfileTemplate(id: String): Boolean {
    val shell = getRootShell()
    return shell.newJob().add("${getKsuDaemonPath()} profile delete-template '${id}'")
        .to(ArrayList(), null).exec().isSuccess
}

fun forceStopApp(packageName: String, userId: Int? = null) {
    val shell = getRootShell()
    val userArg = userId?.let { " --user $it" } ?: ""
    val result = shell.newJob().add("am force-stop$userArg $packageName").exec()
    Log.i(TAG, "force stop $packageName result: $result")
}

fun launchApp(packageName: String, userId: Int? = null) {
    val shell = getRootShell()
    val userArg = userId?.let { " --user $it" } ?: ""
    val result =
        shell.newJob()
            .add("cmd package resolve-activity --brief$userArg $packageName | tail -n 1 | xargs cmd activity start-activity$userArg -n")
            .exec()
    Log.i(TAG, "launch $packageName result: $result")
}

fun restartApp(packageName: String, userId: Int? = null) {
    forceStopApp(packageName, userId)
    launchApp(packageName, userId)
}
