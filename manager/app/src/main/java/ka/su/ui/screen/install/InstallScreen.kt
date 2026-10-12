package ka.su.ui.screen.install

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.dropUnlessResumed
import ka.su.R
import ka.su.ui.component.material.SegmentedColumn
import ka.su.ui.component.material.SegmentedRadioItem
import ka.su.ui.navigation3.LocalNavigator
import ka.su.ui.navigation3.Route
import ka.su.ui.screen.flash.FlashIt
import ka.su.ui.util.getSystemProperty

@Composable
fun InstallScreen() {
    val navigator = LocalNavigator.current
    var inactiveSlot by rememberSaveable { mutableStateOf(false) }
    val hasSlots = remember {
        getSystemProperty("ro.boot.slot_suffix") in listOf("_a", "_b") ||
            getSystemProperty("ro.boot.slot") in listOf("a", "b")
    }
    val selectFile = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            navigator.push(Route.Flash(FlashIt.FlashAnyKernel(uri, inactiveSlot)))
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.flash_kernel)) },
                navigationIcon = {
                    IconButton(onClick = dropUnlessResumed { navigator.pop() }) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, null)
                    }
                },
            )
        },
        contentWindowInsets = WindowInsets.safeDrawing,
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            SegmentedColumn(
                title = stringResource(R.string.flash_kernel_slot),
                content = listOf(
                    {
                        SegmentedRadioItem(
                            title = stringResource(R.string.flash_kernel_active_slot),
                            selected = !inactiveSlot,
                            onClick = { inactiveSlot = false },
                        )
                    },
                    {
                        SegmentedRadioItem(
                            title = stringResource(R.string.flash_kernel_inactive_slot),
                            selected = inactiveSlot,
                            enabled = hasSlots,
                            onClick = { inactiveSlot = true },
                        )
                    },
                ),
            )
            Button(
                onClick = dropUnlessResumed {
                    selectFile.launch(arrayOf("application/zip"))
                },
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(stringResource(R.string.flash_kernel_action))
            }
        }
    }
}
