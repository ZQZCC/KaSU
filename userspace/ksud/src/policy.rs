use crate::{defs, ksu_uapi, ksucalls, policy_file};
use anyhow::{Context, Result, bail, ensure};
use std::io;
use std::os::fd::{AsRawFd, OwnedFd};
use std::path::Path;

const POLICY_PATH: &str = const_format::concatcp!(defs::WORKING_DIR, ".allowlist");

const _: () = {
    assert!(std::mem::size_of::<ksu_uapi::app_profile>() == policy_file::PROFILE_SIZE);
    assert!(std::mem::offset_of!(ksu_uapi::app_profile, key) == 4);
    assert!(std::mem::offset_of!(ksu_uapi::app_profile, curr_uid) == 260);
    assert!(std::mem::offset_of!(ksu_uapi::app_profile, allow_su) == 264);
};

fn call<T>(fd: &OwnedFd, command: u32, arg: &mut T) -> io::Result<()> {
    let ret = unsafe {
        libc::ioctl(
            fd.as_raw_fd(),
            command as libc::c_int,
            std::ptr::from_mut(arg),
        )
    };
    if ret < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

fn metadata(fd: &OwnedFd) -> io::Result<ksu_uapi::ksu_policy_snapshot_cmd> {
    let mut cmd = ksu_uapi::ksu_policy_snapshot_cmd {
        profiles: 0,
        generation: 0,
        count: 0,
        flags: 0,
    };
    call(fd, ksu_uapi::KSU_IOCTL_POLICY_SNAPSHOT, &mut cmd)?;
    Ok(cmd)
}

fn restore(fd: &OwnedFd) -> Result<()> {
    if metadata(fd)?.flags & ksu_uapi::KSU_POLICY_INITIALIZED != 0 {
        return Ok(());
    }
    let stored = policy_file::load(Path::new(POLICY_PATH))?;
    let mut cmd = ksu_uapi::ksu_policy_restore_cmd {
        profiles: stored.profiles.as_ptr() as u64,
        count: u32::try_from(stored.profiles.len() / policy_file::PROFILE_SIZE)?,
        flags: if stored.needs_rewrite {
            0
        } else {
            ksu_uapi::KSU_POLICY_RESTORE_SAVED
        },
    };
    call(fd, ksu_uapi::KSU_IOCTL_POLICY_RESTORE, &mut cmd).context("restore app profiles")
}

pub fn restore_before_post_fs_data() -> Result<()> {
    match ksucalls::open_policy_fd() {
        Ok(Some(fd)) => restore(&fd),
        Ok(None) => Ok(()),
        // The exclusive writer can already own the initialized policy on a soft reboot.
        Err(error) if error.raw_os_error() == Some(libc::EALREADY) => Ok(()),
        Err(error) => Err(error).context("open policy restore fd"),
    }
}

fn snapshot(fd: &OwnedFd) -> Result<(u64, Vec<u8>)> {
    let mut info = metadata(fd)?;
    ensure!(
        info.flags & ksu_uapi::KSU_POLICY_INITIALIZED != 0,
        "policy is not initialized"
    );
    for _ in 0..8 {
        ensure!(
            info.count as usize <= policy_file::MAX_PROFILES,
            "invalid policy count"
        );
        let mut profiles = vec![0u8; info.count as usize * policy_file::PROFILE_SIZE];
        let mut cmd = ksu_uapi::ksu_policy_snapshot_cmd {
            profiles: profiles.as_mut_ptr() as u64,
            generation: 0,
            count: info.count,
            flags: 0,
        };
        match call(fd, ksu_uapi::KSU_IOCTL_POLICY_SNAPSHOT, &mut cmd) {
            Ok(()) => {
                ensure!(cmd.count <= info.count, "snapshot exceeded capacity");
                profiles.truncate(cmd.count as usize * policy_file::PROFILE_SIZE);
                return Ok((cmd.generation, profiles));
            }
            Err(error) if error.raw_os_error() == Some(libc::ENOSPC) => info = cmd,
            Err(error) => return Err(error).context("export policy snapshot"),
        }
    }
    bail!("policy count kept changing during snapshot export")
}

pub fn run_daemon() -> Result<()> {
    ksucalls::ensure_uapi_version_matched()?;
    let fd = ksucalls::open_policy_fd()?.context("userspace policy storage is disabled")?;
    restore(&fd)?;
    log::info!("policy storage ready");
    loop {
        let mut event = libc::pollfd {
            fd: fd.as_raw_fd(),
            events: libc::POLLIN,
            revents: 0,
        };
        let ret = unsafe { libc::poll(&raw mut event, 1, -1) };
        if ret < 0 {
            let error = io::Error::last_os_error();
            if error.kind() == io::ErrorKind::Interrupted {
                continue;
            }
            return Err(error).context("wait for policy update");
        }
        ensure!(
            event.revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL) == 0,
            "policy fd closed or permission denied"
        );
        if event.revents & libc::POLLIN != 0 {
            let (mut generation, profiles) = snapshot(&fd)?;
            policy_file::save(Path::new(POLICY_PATH), &profiles).context("persist app profiles")?;
            // Acknowledge only this durable snapshot; a newer generation stays readable.
            call(&fd, ksu_uapi::KSU_IOCTL_POLICY_ACK, &mut generation)
                .context("acknowledge policy save")?;
        }
    }
}
