package ka.su.ui.screen.home

import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.unit.Dp
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import ka.su.ui.LocalMainPagerState
import ka.su.ui.viewmodel.HomeViewModel

@Composable
fun HomePager(
    bottomInnerPadding: Dp,
    isCurrentPage: Boolean = true,
) {
    val viewModel = viewModel<HomeViewModel>()
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val mainState = LocalMainPagerState.current
    val uriHandler = LocalUriHandler.current
    val latestIsCurrentPage by rememberUpdatedState(isCurrentPage)
    val initialResumeHandled = rememberSaveable { mutableStateOf(false) }

    var hasActivated by rememberSaveable { mutableStateOf(false) }
    LaunchedEffect(isCurrentPage) {
        if (isCurrentPage && !hasActivated) {
            hasActivated = true
            viewModel.refresh()
        }
    }

    LifecycleResumeEffect(Unit) {
        if (initialResumeHandled.value && latestIsCurrentPage) {
            viewModel.refresh()
        }
        initialResumeHandled.value = true
        onPauseOrDispose { }
    }

    val actions = HomeActions(
        onSuperuserClick = { if (uiState.isFullFeatured) mainState.animateToPage(1) },
        onModuleClick = { if (uiState.isFullFeatured) mainState.animateToPage(2) },
        onOpenUrl = uriHandler::openUri,
    )

    HomePagerMaterial(
        state = uiState,
        actions = actions,
        bottomInnerPadding = bottomInnerPadding,
    )
}
