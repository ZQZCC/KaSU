package me.weishu.kernelsu.data.repository

import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import me.weishu.kernelsu.data.model.Module
import me.weishu.kernelsu.data.model.ModuleUpdateInfo
import me.weishu.kernelsu.ksuApp
import me.weishu.kernelsu.ui.util.getRootShell
import me.weishu.kernelsu.ui.util.isNetworkAvailable
import me.weishu.kernelsu.ui.util.listModules
import me.weishu.kernelsu.ui.util.module.sanitizeVersionString
import okhttp3.Request
import org.json.JSONArray
import org.json.JSONObject

class ModuleRepositoryImpl : ModuleRepository {

    companion object {
        private const val TAG = "ModuleRepository"
    }

    override suspend fun getModules(): Result<List<Module>> = withContext(Dispatchers.IO) {
        runCatching {
            val result = listModules()
            val array = JSONArray(result)
            (0 until array.length())
                .asSequence()
                .map { array.getJSONObject(it) }
                .map { obj ->
                    val id = obj.getString("id")
                    Module(
                        id = id,
                        name = obj.optString("name"),
                        author = obj.optString("author", "Unknown"),
                        version = obj.optString("version", "Unknown"),
                        versionCode = obj.optInt("versionCode", 0),
                        description = obj.optString("description"),
                        enabled = obj.getBoolean("enabled"),
                        update = obj.optBoolean("update"),
                        remove = obj.getBoolean("remove"),
                        updateJson = obj.optString("updateJson"),
                        hasWebUi = obj.optBoolean("web"),
                        hasActionScript = obj.optBoolean("action"),
                        metamodule = (obj.optInt("metamodule") != 0) || obj.optBoolean("metamodule"),
                        size = getModuleSizeLabel(id),
                        actionIconPath = obj.optString("actionIcon").takeIf { it.isNotBlank() },
                        webUiIconPath = obj.optString("webuiIcon").takeIf { it.isNotBlank() }
                    )
                }.toList()
        }
    }

    private fun getModuleSizeLabel(id: String): String {
        val busybox = "/data/adb/ksu/bin/busybox"
        val path = "/data/adb/modules/$id"
        val command = "$busybox find ${shellQuote(path)} -type f -exec $busybox stat -c %s {} \\; | " +
                "$busybox awk '{ total += ${'$'}1 } END { print total + 0 }'"
        return runCatching {
            val out = getRootShell().newJob()
                .add(command)
                .to(ArrayList<String>(), null)
                .exec()
                .out
            val bytes = out.firstOrNull()
                ?.trim()
                ?.split(Regex("\\s+"))
                ?.firstOrNull()
                ?.toLongOrNull()
                ?: return ""
            if (bytes <= 0L) return ""
            formatModuleSize(bytes)
        }.getOrElse {
            Log.w(TAG, "Failed to calculate module size: $id", it)
            ""
        }
    }

    private fun shellQuote(value: String): String {
        return "'${value.replace("'", "'\"'\"'")}'"
    }

    private fun formatModuleSize(bytes: Long): String {
        val kb = 1024.0
        val mb = kb * 1024
        val gb = mb * 1024
        return when {
            bytes >= gb -> "%.2f GB".format(bytes / gb)
            bytes >= mb -> "%.2f MB".format(bytes / mb)
            bytes >= kb -> "%.2f KB".format(bytes / kb)
            else -> "$bytes B"
        }
    }

    override suspend fun checkUpdate(module: Module): Result<ModuleUpdateInfo> = withContext(Dispatchers.IO) {
        runCatching {
            if (!isNetworkAvailable(ksuApp)) {
                return@runCatching ModuleUpdateInfo.Empty
            }
            if (module.updateJson.isEmpty() || module.remove || module.update || !module.enabled) {
                return@runCatching ModuleUpdateInfo.Empty
            }

            val url = module.updateJson
            val response = ksuApp.okhttpClient.newCall(
                Request.Builder().url(url).build()
            ).execute()

            val result = if (response.isSuccessful) {
                response.body.string()
            } else {
                ""
            }

            if (result.isEmpty()) {
                return@runCatching ModuleUpdateInfo.Empty
            }

            val updateJson = JSONObject(result)
            var version = updateJson.optString("version", "")
            version = sanitizeVersionString(version)
            val versionCode = updateJson.optInt("versionCode", 0)
            val zipUrl = updateJson.optString("zipUrl", "")
            val changelog = updateJson.optString("changelog", "")

            if (versionCode <= module.versionCode || zipUrl.isEmpty()) {
                ModuleUpdateInfo.Empty
            } else {
                ModuleUpdateInfo(zipUrl, version, changelog)
            }
        }
    }
}
