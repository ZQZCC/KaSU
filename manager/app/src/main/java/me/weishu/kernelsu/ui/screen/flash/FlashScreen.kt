package me.weishu.kernelsu.ui.screen.flash

import androidx.compose.material3.SnackbarHostState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.lifecycle.compose.dropUnlessResumed
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.weishu.kernelsu.R
import me.weishu.kernelsu.data.repository.isSoftRebootPreferred
import me.weishu.kernelsu.ui.navigation3.LocalNavigator
import me.weishu.kernelsu.ui.util.reboot

@Composable
fun FlashScreen(flashIt: FlashIt) {
    val navigator = LocalNavigator.current
    val scope = rememberCoroutineScope()
    var text by rememberSaveable { mutableStateOf("") }
    val logContent = remember { StringBuilder() }
    var showRebootAction by rememberSaveable { mutableStateOf(false) }
    var flashingStatus by rememberSaveable { mutableStateOf(FlashingStatus.FLASHING) }
    val softReboot = isSoftRebootPreferred()
    val snackbarHost = remember { SnackbarHostState() }

    fun showMessage(message: String) {
        scope.launch {
            snackbarHost.showSnackbar(message)
        }
    }

    FlashEffect(
        flashIt = flashIt,
        text = text,
        logContent = logContent,
        onTextUpdate = { text = it },
        onShowRebootChange = { showRebootAction = it },
        onFlashingStatusChange = { flashingStatus = it },
    )

    val state = FlashUiState(
        text = text,
        showRebootAction = showRebootAction,
        flashingStatus = flashingStatus,
        rebootLabelRes = if (softReboot) R.string.reboot_soft else R.string.reboot,
    )
    val actions = FlashScreenActions(
        onBack = dropUnlessResumed { navigator.pop() },
        onSaveLog = saveLog(logContent, scope) { showMessage(it) },
        onReboot = {
            scope.launch {
                withContext(Dispatchers.IO) {
                    reboot(if (softReboot) "soft_reboot" else "")
                }
            }
        },
    )

    FlashScreenMaterial(state, actions, snackbarHost)
}
