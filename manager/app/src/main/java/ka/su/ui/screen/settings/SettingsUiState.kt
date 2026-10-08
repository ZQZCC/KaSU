package ka.su.ui.screen.settings

import android.os.Build
import androidx.compose.runtime.Immutable
import com.materialkolor.PaletteStyle
import com.materialkolor.dynamiccolor.ColorSpec

@Immutable
data class SettingsUiState(
    val isLoaded: Boolean = false,
    val checkModuleUpdate: Boolean = false,
    val themeMode: Int = 0,
    val keyColor: Int = 0,
    val colorStyle: String = PaletteStyle.TonalSpot.name,
    val colorSpec: String = ColorSpec.SpecVersion.SPEC_2025.name,
    val enablePredictiveBack: Boolean = Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE,
    val pageScale: Float = 1.0f,
    val moduleDescriptionMaxLines: Int = 4,
    val enableWebDebugging: Boolean = false,

    // Su Compat
    val suCompatStatus: String = "",
    val suCompatMode: Int = 0, // 0: enable default, 1: disable until reboot, 2: disable always
    val isSuEnabled: Boolean = false,

    // Kernel Umount
    val kernelUmountStatus: String = "",
    val isKernelUmountEnabled: Boolean = false,

    // SELinux Hide
    val selinuxHideStatus: String = "",
    val isSelinuxHideEnabled: Boolean = false,

    // SU Log
    val sulogStatus: String = "",
    val isSulogEnabled: Boolean = false,

    // Umount Modules
    val isDefaultUmountModules: Boolean = false,

    // ADB Root
    val adbRootStatus: String = "",
    val isAdbRootEnabled: Boolean = false,



    // Soft Reboot
    val useSoftReboot: Boolean = false
)

@Immutable
data class SettingsScreenActions(
    val onSetCheckModuleUpdate: (Boolean) -> Unit,
    val onOpenTheme: () -> Unit,
    val onOpenProfileTemplate: () -> Unit,
    val onSetSuCompatMode: (Int) -> Unit,
    val onSetKernelUmountEnabled: (Boolean) -> Unit,
    val onSetSelinuxHideEnabled: (Boolean) -> Unit,
    val onSetSulogEnabled: (Boolean) -> Unit,
    val onSetAdbRootEnabled: (Boolean) -> Unit,
    val onSetDefaultUmountModules: (Boolean) -> Unit,
    val onSetEnableWebDebugging: (Boolean) -> Unit,
    val onSetUseSoftReboot: (Boolean) -> Unit,
)
