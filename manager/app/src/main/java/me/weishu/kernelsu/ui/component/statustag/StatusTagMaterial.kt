package me.weishu.kernelsu.ui.component.statustag

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

@Composable
fun StatusTagMaterial(
    label: String,
    modifier: Modifier = Modifier,
    backgroundColor: Color,
    contentColor: Color,
    minHeight: Dp? = null,
    horizontalPadding: Dp = 4.dp,
    verticalPadding: Dp = 2.dp,
    maxLines: Int = Int.MAX_VALUE
) {
    val containerModifier = modifier
        .padding(end = 4.dp)
        .let { baseModifier ->
            if (minHeight == null) {
                baseModifier
            } else {
                baseModifier.defaultMinSize(minHeight = minHeight)
            }
        }

    Box(
        modifier = containerModifier
            .background(
                color = backgroundColor,
                shape = RoundedCornerShape(4.dp)
            ),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = label,
            modifier = Modifier.padding(vertical = verticalPadding, horizontal = horizontalPadding),
            style = MaterialTheme.typography.labelSmallEmphasized,
            color = contentColor,
            maxLines = maxLines,
        )
    }
}
