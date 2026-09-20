//! The thanks list the mod's main menu rolls, served by the master at `/v1/thanks` as one
//! file's own text. DEPLOY CONTENT, not code: copy a new file to the box and the next read
//! serves it, with no restart and no rebuild. The file is a copy of
//! `src/votv-coop/assets/thanks/thanks.txt`; the mod parses and bounds it, this only hands it over.

use crate::common::env_str;
use std::sync::{LazyLock, Mutex};
use std::time::{Duration, Instant};

/// Where the file lives unless `COOP_THANKS_FILE` says otherwise.
const THANKS_FILE: &str = "/etc/coop-thanks.txt";
/// The mod refuses a longer text, so a longer file is not served at all.
pub const THANKS_MAX_BYTES: usize = 64 * 1024;
/// A flood of requests costs one read per window.
const THANKS_REREAD: Duration = Duration::from_secs(30);

/// The text of a thanks-list file, or None when it cannot be served whole: unreadable, empty,
/// over the cap, or not UTF-8 (the mod drops ill-formed text anyway; refusing here keeps the
/// JSON encoder from being handed replacement characters nobody wrote).
pub fn read_thanks_file(path: &str) -> Option<String> {
    let bytes = std::fs::read(path).ok()?;
    if bytes.is_empty() || bytes.len() > THANKS_MAX_BYTES {
        return None;
    }
    String::from_utf8(bytes).ok()
}

/// The served text, re-read from disk when the last read is older than `THANKS_REREAD`. None
/// answers 404, which the mod reads as "no newer list" and stays on the copy it has.
pub fn thanks_text() -> Option<String> {
    static CACHE: LazyLock<Mutex<(Option<Instant>, Option<String>)>> =
        LazyLock::new(|| Mutex::new((None, None)));
    let mut cache = CACHE.lock().unwrap_or_else(|e| e.into_inner());
    let stale = cache.0.map(|at| at.elapsed() >= THANKS_REREAD).unwrap_or(true);
    if stale {
        cache.1 = read_thanks_file(&env_str("COOP_THANKS_FILE", THANKS_FILE));
        cache.0 = Some(Instant::now());
    }
    cache.1.clone()
}

#[cfg(test)]
mod tests {
    use super::{read_thanks_file, THANKS_MAX_BYTES};

    #[test]
    fn thanks_file_is_served_whole_or_not_at_all() {
        let path = std::env::temp_dir().join(format!("coop-thanks-test-{}.txt", std::process::id()));
        let p = path.to_str().unwrap();

        let _ = std::fs::remove_file(&path);
        assert_eq!(read_thanks_file(p), None, "a missing file serves nothing");

        std::fs::write(&path, "revision = 2\n[Testers]\nname\n").unwrap();
        assert_eq!(read_thanks_file(p).as_deref(), Some("revision = 2\n[Testers]\nname\n"));

        std::fs::write(&path, "").unwrap();
        assert_eq!(read_thanks_file(p), None, "an empty file serves nothing");

        std::fs::write(&path, vec![b'a'; THANKS_MAX_BYTES + 1]).unwrap();
        assert_eq!(read_thanks_file(p), None, "a file over the cap serves nothing");

        std::fs::write(&path, [0xFFu8, 0xFE, 0x41]).unwrap();
        assert_eq!(read_thanks_file(p), None, "text that is not UTF-8 serves nothing");

        let _ = std::fs::remove_file(&path);
    }
}
