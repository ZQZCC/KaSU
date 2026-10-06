use anyhow::{Context, Result, ensure};
use std::collections::HashMap;
use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::Path;

pub const PROFILE_SIZE: usize = 784;
pub const MAX_PROFILES: usize = u16::MAX as usize;
const LEGACY_PROFILE_SIZE: usize = 776;
const MAGIC: u32 = 0x7f4b_5355;
const VERSION: u32 = 4;
const MAX_FILE_SIZE: usize = 8 + MAX_PROFILES * PROFILE_SIZE;

pub struct StoredPolicy {
    pub profiles: Vec<u8>,
    pub needs_rewrite: bool,
}

fn validate_record(record: &[u8]) -> Result<()> {
    ensure!(record.len() == PROFILE_SIZE, "invalid app profile size");
    ensure!(
        record[..4] == VERSION.to_le_bytes(),
        "invalid profile version"
    );
    ensure!(record[4..260].contains(&0), "unterminated profile key");
    ensure!(
        record[264] <= 1 && record[272] <= 1,
        "invalid profile boolean"
    );
    if record[264] == 1 {
        let count = u32::from_le_bytes(record[544..548].try_into()?);
        ensure!(count <= 32, "invalid profile groups count");
        ensure!(
            record[704] != 0 && record[704..768].contains(&0),
            "invalid profile domain"
        );
    } else {
        ensure!(record[273] <= 1, "invalid unmount boolean");
    }
    if record[260..264] == 9999_u32.to_le_bytes() {
        ensure!(
            record[4] == b'$' && record[5] == 0,
            "invalid default profile key"
        );
    }
    Ok(())
}

pub fn decode(bytes: &[u8]) -> Result<StoredPolicy> {
    ensure!(
        (8..=MAX_FILE_SIZE).contains(&bytes.len()),
        "invalid policy file size"
    );
    ensure!(
        bytes[..4] == MAGIC.to_le_bytes(),
        "invalid policy file magic"
    );
    let version = u32::from_le_bytes(bytes[4..8].try_into()?);
    ensure!(
        (2..=VERSION).contains(&version),
        "unsupported policy file version"
    );
    let record_size = if version == VERSION {
        PROFILE_SIZE
    } else {
        LEGACY_PROFILE_SIZE
    };
    let payload = &bytes[8..];
    ensure!(
        payload.len().is_multiple_of(record_size),
        "truncated policy record"
    );
    ensure!(
        payload.len() / record_size <= MAX_PROFILES,
        "too many app profiles"
    );
    let mut result = StoredPolicy {
        profiles: Vec::with_capacity(payload.len() / record_size * PROFILE_SIZE),
        needs_rewrite: version != VERSION,
    };
    let mut positions = HashMap::new();
    for source in payload.chunks_exact(record_size) {
        let mut record = [0u8; PROFILE_SIZE];
        record[..record_size].copy_from_slice(source);
        result.needs_rewrite |= record[..4] != VERSION.to_le_bytes();
        record[..4].copy_from_slice(&VERSION.to_le_bytes());
        if version < VERSION && record[264] == 1 {
            record[776..784].copy_from_slice(&1_u64.to_le_bytes());
            if version == 2 && record[704..714] == *b"u:r:su:s0\0" {
                record[704..768].fill(0);
                record[704..715].copy_from_slice(b"u:r:ksu:s0\0");
            }
        }
        validate_record(&record)?;
        let uid = u32::from_le_bytes(record[260..264].try_into()?);
        // The old loader used the final record for a repeated UID.
        if let Some(&offset) = positions.get(&uid) {
            result.profiles[offset..offset + PROFILE_SIZE].copy_from_slice(&record);
            result.needs_rewrite = true;
        } else {
            positions.insert(uid, result.profiles.len());
            result.profiles.extend_from_slice(&record);
        }
    }
    Ok(result)
}

pub fn load(path: &Path) -> Result<StoredPolicy> {
    let file = match File::open(path) {
        Ok(file) => file,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            return Ok(StoredPolicy {
                profiles: Vec::new(),
                needs_rewrite: true,
            });
        }
        Err(error) => return Err(error).context("open policy file"),
    };
    ensure!(
        file.metadata()?.is_file(),
        "policy path is not a regular file"
    );
    let mut bytes = Vec::new();
    file.take((MAX_FILE_SIZE + 1) as u64)
        .read_to_end(&mut bytes)?;
    decode(&bytes).context("decode policy file")
}

pub fn save(path: &Path, profiles: &[u8]) -> Result<()> {
    ensure!(
        profiles.len().is_multiple_of(PROFILE_SIZE),
        "invalid policy snapshot size"
    );
    ensure!(
        profiles.len() / PROFILE_SIZE <= MAX_PROFILES,
        "too many app profiles"
    );
    for record in profiles.chunks_exact(PROFILE_SIZE) {
        validate_record(record)?;
    }
    let parent = path.parent().context("policy path has no parent")?;
    fs::create_dir_all(parent)?;
    let mut temp = tempfile::NamedTempFile::new_in(parent)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        temp.as_file()
            .set_permissions(fs::Permissions::from_mode(0o644))?;
    }
    temp.write_all(&MAGIC.to_le_bytes())?;
    temp.write_all(&VERSION.to_le_bytes())?;
    temp.write_all(profiles)?;
    temp.as_file().sync_all()?;
    temp.persist(path)
        .map_err(|error| error.error)
        .context("replace policy file")?;
    #[cfg(unix)]
    File::open(parent)?
        .sync_all()
        .context("sync policy directory")?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn profile(uid: u32, allow: bool) -> Vec<u8> {
        let mut record = vec![0u8; PROFILE_SIZE];
        record[..4].copy_from_slice(&VERSION.to_le_bytes());
        record[4..9].copy_from_slice(b"test\0");
        record[260..264].copy_from_slice(&uid.to_le_bytes());
        record[264] = u8::from(allow);
        record[272] = 1;
        record[704..714].copy_from_slice(b"u:r:su:s0\0");
        record
    }

    fn encoded(version: u32, profiles: &[Vec<u8>]) -> Vec<u8> {
        let mut bytes = MAGIC.to_le_bytes().to_vec();
        bytes.extend_from_slice(&version.to_le_bytes());
        let size = if version == VERSION {
            PROFILE_SIZE
        } else {
            LEGACY_PROFILE_SIZE
        };
        for profile in profiles {
            bytes.extend_from_slice(&profile[..size]);
        }
        bytes
    }

    #[test]
    fn v4_round_trip_preserves_profiles_and_flags() {
        let mut root = profile(10001, true);
        root[776..784].copy_from_slice(&0x42_u64.to_le_bytes());
        let bytes = encoded(4, &[root.clone(), profile(10002, false)]);
        let decoded = decode(&bytes).unwrap();
        assert!(!decoded.needs_rewrite);
        assert_eq!(&decoded.profiles[..PROFILE_SIZE], root);
        assert_eq!(&decoded.profiles[PROFILE_SIZE..], profile(10002, false));
    }

    #[test]
    fn migrates_v2_domain_and_v3_flags() {
        for version in [2, 3] {
            let decoded = decode(&encoded(version, &[profile(10001, true)])).unwrap();
            assert!(decoded.needs_rewrite);
            assert_eq!(&decoded.profiles[776..784], &1_u64.to_le_bytes());
            let domain = if version == 2 {
                b"u:r:ksu:s0\0".as_slice()
            } else {
                b"u:r:su:s0\0".as_slice()
            };
            assert_eq!(&decoded.profiles[704..704 + domain.len()], domain);
        }
    }

    #[test]
    fn repeated_uid_keeps_final_record() {
        let decoded = decode(&encoded(4, &[profile(10001, true), profile(10001, false)])).unwrap();
        assert!(decoded.needs_rewrite);
        assert_eq!(decoded.profiles, profile(10001, false));
    }

    #[test]
    fn rejects_corrupt_file_without_partial_policy() {
        let valid = encoded(4, &[profile(10001, true)]);
        for length in [0, 7, 9, valid.len() - 1] {
            assert!(decode(&valid[..length]).is_err());
        }
        let mut bad = profile(10002, true);
        bad[264] = 2;
        assert!(decode(&encoded(4, &[profile(10001, true), bad])).is_err());
        let mut bad = valid;
        bad[0] = 0;
        assert!(decode(&bad).is_err());
        bad[0] = MAGIC.to_le_bytes()[0];
        bad[4..8].copy_from_slice(&5_u32.to_le_bytes());
        assert!(decode(&bad).is_err());
    }

    #[test]
    fn rejects_invalid_key_domain_groups_and_default() {
        for field in [4..260, 704..768] {
            let mut bad = profile(10001, true);
            bad[field].fill(b'x');
            assert!(decode(&encoded(4, &[bad])).is_err());
        }
        let mut bad = profile(10001, true);
        bad[544..548].copy_from_slice(&33_u32.to_le_bytes());
        assert!(decode(&encoded(4, &[bad])).is_err());
        assert!(decode(&encoded(4, &[profile(9999, false)])).is_err());
    }

    #[test]
    fn atomic_replace_and_failed_validation_preserve_file() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join(".allowlist");
        assert!(load(&path).unwrap().needs_rewrite);
        save(&path, &profile(10001, true)).unwrap();
        save(&path, &profile(10001, false)).unwrap();
        let before = fs::read(&path).unwrap();
        assert!(save(&path, &[0; PROFILE_SIZE]).is_err());
        assert_eq!(fs::read(&path).unwrap(), before);
        assert_eq!(load(&path).unwrap().profiles, profile(10001, false));
        assert_eq!(fs::read_dir(dir.path()).unwrap().count(), 1);
    }

    #[test]
    fn missing_file_and_empty_policy_are_supported() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("ksu/.allowlist");
        assert!(load(&path).unwrap().profiles.is_empty());
        save(&path, &[]).unwrap();
        let restored = load(&path).unwrap();
        assert!(restored.profiles.is_empty());
        assert!(!restored.needs_rewrite);
    }

    #[test]
    fn legacy_default_non_root_profile_preserves_policy() {
        for version in [2, 3, 4] {
            let mut record = profile(9999, false);
            record[4..260].fill(0);
            record[4] = b'$';
            record[273] = 1;
            let decoded = decode(&encoded(version, &[record])).unwrap();
            assert_eq!(decoded.profiles[273], 1);
            assert_eq!(decoded.profiles[264], 0);
        }
    }

    #[test]
    fn failed_replacement_does_not_remove_existing_directory() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join(".allowlist");
        fs::create_dir(&path).unwrap();
        fs::write(path.join("keep"), b"original").unwrap();
        assert!(save(&path, &profile(10001, true)).is_err());
        assert_eq!(fs::read(path.join("keep")).unwrap(), b"original");
        assert_eq!(fs::read_dir(dir.path()).unwrap().count(), 1);
    }
}
