package ka.su.data.model

data class FeatureState(
    val status: String = "",
    val configured: Long? = null,
)

data class FeatureSettings(
    val suCompat: FeatureState = FeatureState(),
    val selinuxHide: FeatureState = FeatureState(),
    val sulog: FeatureState = FeatureState(),
    val adbRoot: FeatureState = FeatureState(),
    val hideBootloaderEnabled: Boolean? = null,
)
