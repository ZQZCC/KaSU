package ka.su.ui.navigation3

import android.os.Parcelable
import androidx.navigation3.runtime.NavKey
import kotlinx.parcelize.Parcelize
import kotlinx.serialization.Serializable
import ka.su.ui.screen.flash.FlashIt
import ka.su.ui.screen.modulerepo.RepoModuleArg
import ka.su.ui.util.FlashItSerializer
import ka.su.ui.util.RepoModuleArgSerializer
import ka.su.ui.util.TemplateInfoSerializer
import ka.su.ui.viewmodel.TemplateViewModel

/**
 * Type-safe navigation keys for Navigation3.
 * Each destination is a NavKey (data object/data class) and can be saved/restored in the back stack.
 */
@Serializable
sealed interface Route : NavKey, Parcelable {
    @Parcelize
    @Serializable
    data object Main : Route

    @Parcelize
    @Serializable
    data object Home : Route

    @Parcelize
    @Serializable
    data object SuperUser : Route

    @Parcelize
    @Serializable
    data object Module : Route

    @Parcelize
    @Serializable
    data object Settings : Route

    @Parcelize
    @Serializable
    data object Sulog : Route

    @Parcelize
    @Serializable
    data object ColorPalette : Route

    @Parcelize
    @Serializable
    data object AppProfileTemplate : Route

    @Parcelize
    @Serializable
    data class TemplateEditor(
        @Serializable(with = TemplateInfoSerializer::class) val template: TemplateViewModel.TemplateInfo,
        val readOnly: Boolean
    ) : Route

    @Parcelize
    @Serializable
    data class AppProfile(val uid: Int) : Route

    @Parcelize
    @Serializable
    data class ModuleRepoDetail(@Serializable(with = RepoModuleArgSerializer::class) val module: RepoModuleArg) : Route

    @Parcelize
    @Serializable
    data object ModuleRepo : Route

    @Parcelize
    @Serializable
    data class Flash(@Serializable(with = FlashItSerializer::class) val flashIt: FlashIt) : Route

    @Parcelize
    @Serializable
    data class ExecuteModuleAction(val moduleId: String, val fromShortcut: Boolean = false) : Route
}
