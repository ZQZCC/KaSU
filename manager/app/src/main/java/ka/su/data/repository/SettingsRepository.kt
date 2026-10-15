package ka.su.data.repository

import ka.su.data.model.FeatureSettings

interface SettingsRepository {
    var checkModuleUpdate: Boolean
    var themeMode: Int
    var keyColor: Int
    var colorStyle: String
    var colorSpec: String
    var enablePredictiveBack: Boolean
    var pageScale: Float
    var moduleDescriptionMaxLines: Int
    var enableWebDebugging: Boolean
    var moduleSortEnabledFirst: Boolean
    var moduleSortActionFirst: Boolean
    var moduleRepoSortOrder: Int
    var superuserShowSystemApps: Boolean
    var superuserShowOnlyPrimaryUserApps: Boolean
    var superuserSortOption: Int
    var suLogFilters: Set<String>?
    var useSoftReboot: Boolean
    val intentToken: String

    suspend fun getFeatureSettings(): FeatureSettings
    fun isSuEnabled(): Boolean
    fun setSuEnabled(enabled: Boolean): Boolean
    fun setSuCompatModePref(mode: Int)
    fun getSuCompatModePref(): Int

    fun isSelinuxHideEnabled(): Boolean
    fun setSelinuxHideEnabled(enabled: Boolean): Int

    fun setHideBootloaderEnabled(enabled: Boolean): Boolean

    fun setSulogEnabled(enabled: Boolean): Boolean

    fun setAdbRootEnabled(enabled: Boolean): Boolean


    fun execKsudFeatureSave()
}
