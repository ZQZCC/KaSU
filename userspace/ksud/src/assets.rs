use anyhow::Result;
use rust_embed::RustEmbed;

#[cfg(target_os = "android")]
mod android {
    use crate::assets::Asset;
    use crate::defs::BINARY_DIR;
    use crate::utils::ensure_binary;
    use const_format::concatcp;

    pub const RESETPROP_PATH: &str = concatcp!(BINARY_DIR, "resetprop");
    pub const BUSYBOX_PATH: &str = concatcp!(BINARY_DIR, "busybox");

    pub fn ensure_binaries(ignore_if_exist: bool) -> anyhow::Result<()> {
        for file in Asset::iter() {
            if file == "waitsys" {
                // Keep the internal helper embedded without extracting it.
                continue;
            }
            let asset =
                Asset::get(&file).ok_or_else(|| anyhow::anyhow!("asset not found: {file}"))?;
            ensure_binary(format!("{BINARY_DIR}{file}"), &asset.data, ignore_if_exist)?;
        }

        // Create resetprop -> ksud symlink (resetprop is now built into ksud)
        let resetprop_link = RESETPROP_PATH;
        let _ = std::fs::remove_file(resetprop_link);
        std::os::unix::fs::symlink("/data/adb/ksud", resetprop_link)?;

        Ok(())
    }
}

#[cfg(target_os = "android")]
pub use android::*;

#[cfg(all(target_arch = "aarch64", target_os = "android"))]
#[derive(RustEmbed)]
#[folder = "bin/aarch64"]
struct Asset;

// Host-side tools retain architecture-prefixed asset names.
#[cfg(not(target_os = "android"))]
#[derive(RustEmbed)]
#[folder = "bin"]
struct Asset;

#[allow(unused)]
pub fn get_asset_data(name: &str) -> Result<std::borrow::Cow<'static, [u8]>> {
    let asset = Asset::get(name).ok_or_else(|| anyhow::anyhow!("asset not found: {name}"))?;
    Ok(asset.data)
}
