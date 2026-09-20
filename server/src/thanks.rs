//! The thanks list the mod's main menu rolls, served by the master at `/v1/thanks` as one
//! file's own text. DEPLOY CONTENT, not code: copy a new file to the box and the next read
//! serves it, with no restart and no rebuild. The file is a copy of
//! `src/votv-coop/assets/thanks/thanks.txt`; the mod parses and bounds it, this only hands it over.

use crate::common::env_str;
use std::sync::{Arc, LazyLock, Mutex};
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
/// answers 404, which the mod reads as "this master serves no list" and shows its own copy.
///
/// The lock is held for the bookkeeping only. The request that finds the copy stale stamps the
/// clock first, so every request beside it serves the old copy instead of queueing, then reads
/// the disk on a blocking thread: a slow volume stalls that one request, never a runtime worker
/// the lobby routes are waiting on.
pub async fn thanks_text() -> Option<Arc<String>> {
    static CACHE: LazyLock<Mutex<(Option<Instant>, Option<Arc<String>>)>> =
        LazyLock::new(|| Mutex::new((None, None)));
    {
        let mut cache = CACHE.lock().unwrap_or_else(|e| e.into_inner());
        // Before the first read lands there is no old copy to serve, and "none" would be a lie
        // the mod acts on (it drops the copy it cached), so those requests each read for
        // themselves: a burst at start-up only.
        if let Some(at) = cache.0 {
            if at.elapsed() < THANKS_REREAD {
                return cache.1.clone();
            }
            cache.0 = Some(Instant::now());
        }
    }
    let path = env_str("COOP_THANKS_FILE", THANKS_FILE);
    let text = tokio::task::spawn_blocking(move || read_thanks_file(&path))
        .await
        .ok()
        .flatten()
        .map(Arc::new);
    let mut cache = CACHE.lock().unwrap_or_else(|e| e.into_inner());
    cache.0 = Some(Instant::now());
    cache.1 = text.clone();
    text
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
