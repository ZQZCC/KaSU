package ka.su.data.model

import android.content.pm.PackageInfo
import android.os.Parcelable
import kotlinx.parcelize.Parcelize
import ka.su.Natives

@Parcelize
data class AppInfo(
    val label: String,
    val packageInfo: PackageInfo,
    val profile: Natives.Profile?,
) : Parcelable {
    val packageName: String
        get() = packageInfo.packageName

    val uid: Int
        get() = packageInfo.applicationInfo!!.uid

    val allowSu: Boolean
        get() = profile?.allowSu == true
    val hasCustomProfile: Boolean
        get() = allowSu && profile?.rootUseDefault == false
}
