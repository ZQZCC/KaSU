package ka.su.ui.viewmodel

import androidx.compose.runtime.Immutable
import ka.su.ui.theme.AppSettings

@Immutable
data class MainActivityUiState(
    val appSettings: AppSettings,
    val pageScale: Float,
    val moduleDescriptionMaxLines: Int = 4,
)
