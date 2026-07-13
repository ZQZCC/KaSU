package me.weishu.kernelsu.ui.component.statustag

import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

@Composable
fun StatusTag(
    label: String,
    modifier: Modifier = Modifier,
    backgroundColor: Color,
    contentColor: Color,
    minHeight: Dp? = null,
    horizontalPadding: Dp = 4.dp,
    verticalPadding: Dp = 2.dp,
    maxLines: Int = Int.MAX_VALUE
) {
    StatusTagMaterial(
        label = label,
        modifier = modifier,
        backgroundColor = backgroundColor,
        contentColor = contentColor,
        minHeight = minHeight,
        horizontalPadding = horizontalPadding,
        verticalPadding = verticalPadding,
        maxLines = maxLines
    )
}
