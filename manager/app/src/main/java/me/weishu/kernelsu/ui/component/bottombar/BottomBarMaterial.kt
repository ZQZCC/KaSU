package me.weishu.kernelsu.ui.component.bottombar

import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.layout.union
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Extension
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Shield
import androidx.compose.material.icons.outlined.Extension
import androidx.compose.material.icons.outlined.Home
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material.icons.outlined.Shield
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.FlexibleBottomAppBar
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import me.weishu.kernelsu.Natives
import me.weishu.kernelsu.R
import me.weishu.kernelsu.ui.LocalMainPagerState

@Composable
fun BottomBarMaterial(moduleUpdateCount: Int) {
    val fullFeatured = Natives.isFullFeatured()
    if (!fullFeatured) return

    val mainPagerState = LocalMainPagerState.current

    val items = listOf(
        Triple(R.string.home, Icons.Filled.Home, Icons.Outlined.Home),
        Triple(R.string.superuser, Icons.Filled.Shield, Icons.Outlined.Shield),
        Triple(R.string.module, Icons.Filled.Extension, Icons.Outlined.Extension),
        Triple(R.string.settings, Icons.Filled.Settings, Icons.Outlined.Settings)
    )

    FlexibleBottomAppBar(
        windowInsets = WindowInsets.systemBars.union(WindowInsets.displayCutout).only(
            WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom
        )
    ) {
        items.forEachIndexed { index, (label, selectedIcon, unselectedIcon) ->
            val selected = mainPagerState.selectedPage == index
            NavigationBarItem(
                modifier = Modifier.padding(top = 4.dp),
                selected = selected,
                onClick = {
                    if (!selected) {
                        mainPagerState.animateToPage(index)
                    }
                },
                icon = {
                    NavigationIconWithBadge(
                        icon = if (selected) selectedIcon else unselectedIcon,
                        contentDescription = stringResource(label),
                        badgeCount = if (index == 2) moduleUpdateCount else 0,
                    )
                },
                label = {
                    Text(
                        stringResource(label),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                },
                alwaysShowLabel = false
            )
        }
    }
}

@Composable
internal fun NavigationIconWithBadge(
    icon: ImageVector,
    contentDescription: String,
    badgeCount: Int,
) {
    if (badgeCount > 0) {
        BadgedBox(
            badge = {
                Badge(
                    containerColor = MaterialTheme.colorScheme.primary,
                    contentColor = MaterialTheme.colorScheme.onPrimary,
                ) {
                    Text(badgeCount.toString())
                }
            }
        ) {
            Icon(icon, contentDescription)
        }
    } else {
        Icon(icon, contentDescription)
    }
}
