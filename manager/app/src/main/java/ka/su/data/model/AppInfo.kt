package ka.su.data.model

import android.content.pm.PackageInfo
import android.os.Parcelable
import kotlinx.parcelize.Parcelize
import ka.su.Natives

@Parcelize
data class AppInfo(
    val label: String,
    val packageInfo: PackageInfo,
    val rootState: Int,
) : Parcelable {
    val packageName: String
        get() = packageInfo.packageName

    val uid: Int
        get() = packageInfo.applicationInfo!!.uid

    val allowSu: Boolean
        get() = (rootState and Natives.APP_STATE_ALLOW_SU) != 0
    val hasCustomProfile: Boolean
        get() = (rootState and Natives.APP_STATE_CUSTOM) != 0
}
