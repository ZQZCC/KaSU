package ka.su.ui.viewmodel

import android.system.OsConstants
import android.widget.Toast
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ka.su.Natives
import ka.su.R
import ka.su.data.repository.SettingsRepository
import ka.su.data.repository.SettingsRepositoryImpl
import ka.su.ksuApp
import ka.su.ui.screen.settings.SettingsUiState
import ka.su.ui.theme.ColorMode

class SettingsViewModel(
    private val repo: SettingsRepository = SettingsRepositoryImpl()
) : ViewModel() {

    private val _uiState = MutableStateFlow(readPreferences())
    val uiState: StateFlow<SettingsUiState> = _uiState.asStateFlow()

    init {
        refresh()
    }

    private fun readPreferences() = SettingsUiState(
        checkModuleUpdate = repo.checkModuleUpdate,
        themeMode = repo.themeMode,
        keyColor = repo.keyColor,
        enablePredictiveBack = repo.enablePredictiveBack,
        pageScale = repo.pageScale,
        moduleDescriptionMaxLines = repo.moduleDescriptionMaxLines,
        enableWebDebugging = repo.enableWebDebugging,
        colorStyle = repo.colorStyle,
        colorSpec = repo.colorSpec,
        useSoftReboot = repo.useSoftReboot,
    )

    fun refresh() {
        viewModelScope.launch {
            val features = repo.getFeatureSettings()
            val isSuEnabled = repo.isSuEnabled()

            val suCompatMode = if (features.suCompat.configured == 0L) 2 else if (!isSuEnabled) 1 else 0

            val isSelinuxHideEnabled = repo.isSelinuxHideEnabled()

            _uiState.update {
                readPreferences().copy(
                    isLoaded = true,
                    suCompatStatus = features.suCompat.status,
                    suCompatMode = suCompatMode,
                    isSuEnabled = isSuEnabled,
                    adbRootStatus = features.adbRoot.status,
                    isAdbRootEnabled = features.adbRoot.configured == 1L,
                    selinuxHideStatus = features.selinuxHide.status,
                    isSelinuxHideEnabled = isSelinuxHideEnabled,
                    isHideBootloaderEnabled = features.hideBootloaderEnabled,
                    sulogStatus = features.sulog.status,
                    isSulogEnabled = features.sulog.configured == 1L,
                )
            }
        }
    }

    fun setCheckModuleUpdate(enabled: Boolean) {
        repo.checkModuleUpdate = enabled
        _uiState.update { it.copy(checkModuleUpdate = enabled) }
    }

    fun setThemeMode(mode: Int) {
        repo.themeMode = mode
        _uiState.update { it.copy(themeMode = mode) }
    }

    fun setColorMode(mode: ColorMode) {
        repo.themeMode = mode.value
        _uiState.update { it.copy(themeMode = mode.value) }
    }

    fun setKeyColor(color: Int) {
        repo.keyColor = color
        _uiState.update { it.copy(keyColor = color) }
    }

    fun setColorStyle(style: String) {
        repo.colorStyle = style
        _uiState.update { it.copy(colorStyle = style) }
    }

    fun setColorSpec(spec: String) {
        repo.colorSpec = spec
        _uiState.update { it.copy(colorSpec = spec) }
    }

    fun setEnablePredictiveBack(enabled: Boolean) {
        repo.enablePredictiveBack = enabled
        _uiState.update { it.copy(enablePredictiveBack = enabled) }
    }

    fun setPageScale(scale: Float) {
        repo.pageScale = scale
        _uiState.update { it.copy(pageScale = scale) }
    }

    fun setModuleDescriptionMaxLines(lines: Int) {
        repo.moduleDescriptionMaxLines = lines
        _uiState.update { it.copy(moduleDescriptionMaxLines = lines) }
    }

    fun setEnableWebDebugging(enabled: Boolean) {
        repo.enableWebDebugging = enabled
        _uiState.update { it.copy(enableWebDebugging = enabled) }
    }

    fun setSuCompatMode(mode: Int) {
        viewModelScope.launch(Dispatchers.IO) {
            when (mode) {
                0 -> if (repo.setSuEnabled(true)) {
                    repo.execKsudFeatureSave()
                    repo.setSuCompatModePref(0)
                    _uiState.update { it.copy(suCompatMode = 0, isSuEnabled = true) }
                }

                1 -> if (repo.setSuEnabled(true)) {
                    repo.execKsudFeatureSave()
                    if (repo.setSuEnabled(false)) {
                        // "Disable until reboot" implies it should be enabled on next boot.
                        // We set the preference to 0 (Enabled) to match the persistent state.
                        repo.setSuCompatModePref(0)
                        _uiState.update { it.copy(suCompatMode = 1, isSuEnabled = false) }
                    }
                }

                2 -> if (repo.setSuEnabled(false)) {
                    repo.execKsudFeatureSave()
                    repo.setSuCompatModePref(2)
                    _uiState.update { it.copy(suCompatMode = 2, isSuEnabled = false) }
                }
            }
        }
    }

    fun setSelinuxHideEnabled(enabled: Boolean) {
        viewModelScope.launch(Dispatchers.IO) {
            val status = repo.setSelinuxHideEnabled(enabled)
            repo.execKsudFeatureSave()
            _uiState.update { it.copy(isSelinuxHideEnabled = enabled) }
            when (status) {
                0 -> {}
                -OsConstants.EAGAIN -> {
                    withContext(Dispatchers.Main) {
                        Toast.makeText(ksuApp, R.string.settings_selinux_hide_reboot_required,
                            Toast.LENGTH_LONG).show()
                    }
                }
                else -> {
                    withContext(Dispatchers.Main) {
                        Toast.makeText(ksuApp, ksuApp.getString(R.string.settings_selinux_hide_failed, status),
                            Toast.LENGTH_LONG).show()
                    }
                }
            }
        }
    }


    fun setHideBootloaderEnabled(enabled: Boolean) {
        viewModelScope.launch(Dispatchers.IO) {
            val saved = repo.setHideBootloaderEnabled(enabled)
            if (saved) {
                _uiState.update { it.copy(isHideBootloaderEnabled = enabled) }
            }
            withContext(Dispatchers.Main) {
                Toast.makeText(
                    ksuApp,
                    if (saved) R.string.reboot_to_apply else R.string.settings_hide_bootloader_failed,
                    Toast.LENGTH_SHORT,
                ).show()
            }
        }
    }

    fun setUseSoftReboot(enabled: Boolean) {
        repo.useSoftReboot = enabled
        _uiState.update { it.copy(useSoftReboot = enabled) }
    }

    fun setSulogEnabled(enabled: Boolean) {
        viewModelScope.launch(Dispatchers.IO) {
            if (repo.setSulogEnabled(enabled)) {
                repo.execKsudFeatureSave()
                _uiState.update { it.copy(isSulogEnabled = enabled) }
            }
        }
    }

    fun setAdbRootEnabled(enabled: Boolean) {
        viewModelScope.launch(Dispatchers.IO) {
            if (repo.setAdbRootEnabled(enabled)) {
                repo.execKsudFeatureSave()
                _uiState.update { it.copy(isAdbRootEnabled = enabled) }
            }
        }
    }

}
