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

/// What a read of the file means. The two failures are NOT the same answer, and the mod acts on
/// the difference: `NoList` is this box saying it has no list, which retires the copy a client
/// cached from it, while `Unreadable` says only that this read did not work -- a file being
/// replaced in place, a permission, a bad publish -- and a client must keep what it holds.
/// Collapsing the two is what let one truncated read during a deploy delete every client's cache.
pub enum Thanks {
    Text(String),
    NoList,
    Unreadable,
}

/// Read the file, telling the two failures apart. Absent means there is no list here; present but
/// unservable (empty, over the cap, not UTF-8, or an IO error) means try again later. The size is
/// taken from the metadata first, so a misconfigured path at a huge file is refused without
/// buffering it.
pub fn read_thanks_file(path: &str) -> Thanks {
    match std::fs::metadata(path) {
        Ok(md) if md.len() as usize > THANKS_MAX_BYTES => return Thanks::Unreadable,
        Ok(_) => {}
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Thanks::NoList,
        Err(_) => return Thanks::Unreadable,
    }
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Thanks::NoList,
        Err(_) => return Thanks::Unreadable,
    };
    if bytes.is_empty() || bytes.len() > THANKS_MAX_BYTES {
        return Thanks::Unreadable;  // mid-write, or a publish that cannot be served whole
    }
    match String::from_utf8(bytes) {
        Ok(s) => Thanks::Text(s),
        Err(_) => Thanks::Unreadable,
    }
}

/// What a request gets: the response body to send, ready-made, or the authoritative "no list".
/// The body is serialized once per window rather than per request, the way the lobby list already
/// caches its bytes.
pub enum Answer {
    /// 200 with this body.
    Body(Arc<Vec<u8>>),
    /// 404: this master has no list, and a client may retire what it cached from us.
    NoList,
    /// 503: we have a list and could not serve it this moment. A client keeps what it holds.
    Unavailable,
}

/// The served answer, re-read from disk when the last read is older than `THANKS_REREAD`.
///
/// A read that fails does NOT retire the last good copy: an in-place file replacement is a few
/// microseconds of truncation, and a client that heard "no list" in that window would delete its
/// cache and roll back to the build's copy. The last good body is served instead, and only an
/// absent file -- the operator saying there is no list -- clears it.
///
/// The lock is held for the bookkeeping only. The request that finds the copy stale stamps the
/// clock first, so every request beside it serves the old copy instead of queueing, then reads
/// the disk on a blocking thread: a slow volume stalls that one request, never a runtime worker
/// the lobby routes are waiting on.
pub async fn thanks_answer() -> Answer {
    // (when the last read finished, the last GOOD body, whether the last read said "no list")
    static CACHE: LazyLock<Mutex<(Option<Instant>, Option<Arc<Vec<u8>>>, bool)>> =
        LazyLock::new(|| Mutex::new((None, None, false)));
    {
        let cache = CACHE.lock().unwrap_or_else(|e| e.into_inner());
        // Before the first read lands there is nothing to serve and no answer to give, so those
        // requests each read for themselves: a burst at start-up only.
        if let Some(at) = cache.0 {
            if at.elapsed() < THANKS_REREAD {
                return match (&cache.1, cache.2) {
                    (Some(b), _) => Answer::Body(b.clone()),
                    (None, true) => Answer::NoList,
                    (None, false) => Answer::Unavailable,
                };
            }
        }
    }
    let path = env_str("COOP_THANKS_FILE", THANKS_FILE);
    let read = tokio::task::spawn_blocking(move || read_thanks_file(&path))
        .await
        .unwrap_or(Thanks::Unreadable);

    let mut cache = CACHE.lock().unwrap_or_else(|e| e.into_inner());
    cache.0 = Some(Instant::now());
    match read {
        Thanks::Text(s) => {
            let body = Arc::new(
                serde_json::to_vec(&serde_json::json!({ "text": s })).unwrap_or_else(|_| b"{}".to_vec()),
            );
            cache.1 = Some(body.clone());
            cache.2 = false;
            Answer::Body(body)
        }
        Thanks::NoList => {
            cache.1 = None;  // the operator removed the list: the cached body is retired with it
            cache.2 = true;
            Answer::NoList
        }
        Thanks::Unreadable => match &cache.1 {
            Some(b) => Answer::Body(b.clone()),  // a bad moment, not a removal: serve the last good copy
            None => {
                cache.2 = false;
                Answer::Unavailable
            }
        },
    }
}

#[cfg(test)]
mod tests {
    use super::{read_thanks_file, Thanks, THANKS_MAX_BYTES};

    fn kind(t: Thanks) -> &'static str {
        match t {
            Thanks::Text(_) => "text",
            Thanks::NoList => "nolist",
            Thanks::Unreadable => "unreadable",
        }
    }

    // The distinction this module exists for: an ABSENT file is the operator saying there is no
    // list, and anything else that cannot be served is a bad moment. A client deletes its cached
    // copy on the first and keeps it on the second, so a test that treats them alike would let
    // the deploy race back in.
    #[test]
    fn an_absent_file_is_the_only_no_list() {
        let path = std::env::temp_dir().join(format!("coop-thanks-test-{}.txt", std::process::id()));
        let p = path.to_str().unwrap();

        let _ = std::fs::remove_file(&path);
        assert_eq!(kind(read_thanks_file(p)), "nolist", "a missing file is the authoritative no-list");

        std::fs::write(&path, "revision = 2\n[Testers]\nname\n").unwrap();
        match read_thanks_file(p) {
            Thanks::Text(s) => assert_eq!(s, "revision = 2\n[Testers]\nname\n"),
            other => panic!("a good file must serve its text, got {}", kind(other)),
        }

        // The deploy race: an in-place copy truncates the destination before it writes.
        std::fs::write(&path, "").unwrap();
        assert_eq!(kind(read_thanks_file(p)), "unreadable", "an empty file is a bad moment, not a removal");

        std::fs::write(&path, vec![b'a'; THANKS_MAX_BYTES + 1]).unwrap();
        assert_eq!(kind(read_thanks_file(p)), "unreadable", "a file over the cap is not a removal");

        std::fs::write(&path, [0xFFu8, 0xFE, 0x41]).unwrap();
        assert_eq!(kind(read_thanks_file(p)), "unreadable", "text that is not UTF-8 is not a removal");

        let _ = std::fs::remove_file(&path);
    }
}
