package ka.su.ui.screen.flash

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.add
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.calculateEndPadding
import androidx.compose.foundation.layout.calculateStartPadding
import androidx.compose.foundation.layout.captionBar
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.TextAutoSize
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Save
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SmallExtendedFloatingActionButton
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.TransformOrigin
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.key
import androidx.compose.ui.layout.layout
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import ka.su.R
import ka.su.ui.component.KeyEventBlocker
import ka.su.ui.component.material.SnackBarHost
import kotlin.math.ceil

@Composable
fun FlashScreenMaterial(
    state: FlashUiState,
    actions: FlashScreenActions,
    snackBarHost: SnackbarHostState,
) {
    val scrollState = rememberScrollState()

    Scaffold(
        snackbarHost = {
            SnackBarHost(
                hostState = snackBarHost,
                modifier = Modifier.let { if (state.showRebootAction) it else it.safeDrawingPadding() })
        },
        topBar = {
            TopAppBar(
                title = {
                    Text(
                        stringResource(
                            when (state.flashingStatus) {
                                FlashingStatus.FLASHING -> R.string.flashing
                                FlashingStatus.SUCCESS -> R.string.flash_success
                                FlashingStatus.FAILED -> R.string.flash_failed
                            }
                        )
                    )
                },
                navigationIcon = {
                    IconButton(onClick = actions.onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, null)
                    }
                },
                actions = {
                    IconButton(onClick = actions.onSaveLog) {
                        Icon(Icons.Filled.Save, stringResource(R.string.save_log))
                    }
                }
            )
        },
        floatingActionButton = {
            if (state.showRebootAction) {
                SmallExtendedFloatingActionButton(
                    onClick = actions.onReboot,
                    icon = { Icon(Icons.Filled.Refresh, null) },
                    text = { Text(stringResource(state.rebootLabelRes)) },
                    modifier = Modifier.padding(
                        bottom = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding() +
                                WindowInsets.captionBar.asPaddingValues().calculateBottomPadding(),
                    )
                )
            }
        },
        contentWindowInsets = WindowInsets.systemBars.add(WindowInsets.displayCutout).only(WindowInsetsSides.Horizontal)
    ) { innerPadding ->
        val layoutDirection = LocalLayoutDirection.current
        val navBars = WindowInsets.navigationBars.asPaddingValues()
        val captionBar = WindowInsets.captionBar.asPaddingValues()
        KeyEventBlocker {
            it.key == Key.VolumeDown || it.key == Key.VolumeUp
        }

        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(
                    start = innerPadding.calculateStartPadding(layoutDirection),
                    end = innerPadding.calculateEndPadding(layoutDirection),
                )
                .verticalScroll(scrollState)
        ) {
            LaunchedEffect(state.text) {
                scrollState.animateScrollTo(scrollState.maxValue)
            }
            Spacer(Modifier.height(innerPadding.calculateTopPadding()))
            val textStyle = MaterialTheme.typography.bodySmall
            val paragraphSpacing = with(LocalDensity.current) { textStyle.lineHeight.toDp() }
            val paragraphs = remember(state.text) { state.text.split("\n\n") }
            Column(Modifier.padding(8.dp)) {
                paragraphs.forEachIndexed { index, text ->
                    if (index > 0) Spacer(Modifier.height(paragraphSpacing))
                    val isBanner = '█' in text && text.all { it in " \u3000█▀▄╔╗╚╝═║\n" }
                    Text(
                        modifier = if (isBanner) Modifier.layout { measurable, constraints ->
                            val scale = 1.5f
                            val placeable = measurable.measure(constraints)
                            layout(placeable.width, ceil(placeable.height * scale).toInt()) {
                                placeable.placeWithLayer(0, 0) {
                                    scaleY = scale
                                    transformOrigin = TransformOrigin(0f, 0f)
                                }
                            }
                        } else Modifier,
                        text = text,
                        style = if (isBanner) textStyle.copy(letterSpacing = 0.sp, lineHeight = 1.em) else textStyle,
                        fontFamily = FontFamily.Monospace,
                        softWrap = !isBanner,
                        autoSize = if (isBanner) {
                            TextAutoSize.StepBased(minFontSize = 1.sp, maxFontSize = textStyle.fontSize)
                        } else null,
                    )
                }
            }
            Spacer(
                Modifier.height(
                    16.dp + 54.dp + navBars.calculateBottomPadding() + captionBar.calculateBottomPadding()
                )
            )
        }
    }
}
