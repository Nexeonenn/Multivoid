//! Production async HTTP master / lobby server for VOTV coop (zero-open-ports MP).
//!
//! RULE 3: VPS infra, never ships in the mod. The endpoint list, the security posture
//! and the careful spots are in the README beside this crate; the byte-exact spot (the
//! coturn TURN credential) is called out inline and unit-tested.
//!
//! Concurrency model: a single `Mutex<MasterState>` guards the lobby maps + rate
//! buckets + the /v1/lobbies cache. Handlers are SYNCHRONOUS (pure CPU) and run
//! entirely under the lock; every socket await (read body, write response) happens
//! OUTSIDE the lock. This mirrors the Python asyncio single-loop model — no lock is
//! ever held across an await — while still using the multi-thread tokio runtime.


use coop_server::common::{clamp_str, env_int, env_str, log};
use coop_server::http_transport::{
    json_bytes, read_head, write_response, ConnGuard, HeadErr, CONNS, HTTP_TIMEOUT, MAX_BODY,
    MAX_CONNS, MAX_HEADER,
};
use coop_server::lobby::{
    dispatch_post, filter_lobbies, lobbies_snapshot, lock_state, resolve_client_ip, sweeper,
    LATEST_MOD, LATEST_PROTO, LATEST_URL, MAX_VERSION,
};
use coop_server::master_config::CFG;
use coop_server::thanks::{thanks_answer, Answer};
use coop_server::tls;
use serde_json::{json, Value};
use std::sync::atomic::Ordering;
use std::sync::LazyLock;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite};
use tokio::time::timeout;
use std::collections::HashMap;
use tokio::net::TcpListener;

async fn handle<S: AsyncRead + AsyncWrite + Unpin>(mut stream: S, peer_ip: String) {
    // Concurrent-connection admission cap: shed before any read.
    if CONNS.fetch_add(1, Ordering::Relaxed) >= MAX_CONNS {
        CONNS.fetch_sub(1, Ordering::Relaxed);
        log(&format!("[{peer_ip}] refused: connection cap ({MAX_CONNS})"));
        return;
    }
    let _guard = ConnGuard;

    // ---- read + parse header block (bounded + timed) ----
    let (head, leftover) = match timeout(HTTP_TIMEOUT, read_head(&mut stream, MAX_HEADER)).await {
        Ok(Ok(v)) => v,
        Ok(Err(HeadErr::TooLarge)) => {
            write_response(&mut stream, 413, &json_bytes(&json!({"error": "headers too large"}))).await;
            return;
        }
        Ok(Err(HeadErr::Closed)) | Err(_) => return, // eof / timeout
    };

    let head_str = String::from_utf8_lossy(&head);
    let mut lines = head_str.split("\r\n");
    let request_line = lines.next().unwrap_or("");
    let mut rl = request_line.splitn(3, ' ');
    let (method, raw_path) = match (rl.next(), rl.next(), rl.next()) {
        (Some(m), Some(p), Some(_)) => (m, p),
        _ => {
            write_response(&mut stream, 400, &json_bytes(&json!({"error": "bad request line"}))).await;
            return;
        }
    };

    let mut headers: HashMap<String, String> = HashMap::new();
    for ln in lines {
        if ln.is_empty() {
            continue;
        }
        if let Some((k, v)) = ln.split_once(':') {
            headers.insert(k.trim().to_lowercase(), v.trim().to_string());
        }
    }

    let (path, query) = match raw_path.split_once('?') {
        Some((p, q)) => (p, q),
        None => (raw_path, ""),
    };
    let client_ip = resolve_client_ip(&peer_ip, &headers);

    // ---- body (POST only, bounded) ----
    let mut body_obj = Value::Object(serde_json::Map::new());
    if method == "POST" {
        let clen: i64 = headers
            .get("content-length")
            .and_then(|s| s.trim().parse::<i64>().ok())
            .unwrap_or(-1);
        if clen < 0 || clen as usize > MAX_BODY {
            write_response(&mut stream, 413, &json_bytes(&json!({"error": "body too large"}))).await;
            return;
        }
        let clen = clen as usize;
        let mut raw = leftover;
        if raw.len() < clen {
            let need = clen - raw.len();
            let mut rest = vec![0u8; need];
            match timeout(HTTP_TIMEOUT, stream.read_exact(&mut rest)).await {
                Ok(Ok(_)) => raw.extend_from_slice(&rest),
                _ => return, // eof / timeout
            }
        }
        raw.truncate(clen);
        if !raw.is_empty() {
            match serde_json::from_slice::<Value>(&raw) {
                Ok(v) if v.is_object() => body_obj = v,
                _ => {
                    write_response(&mut stream, 400, &json_bytes(&json!({"error": "bad json"}))).await;
                    return;
                }
            }
        }
    }

    // ---- route ----
    if method == "GET" && path == "/v1/lobbies" {
        let mut vf = String::new();
        for kv in query.split('&') {
            if let Some(rest) = kv.strip_prefix("version=") {
                vf = clamp_str(rest, MAX_VERSION);
            }
        }
        // Refresh + snapshot under the lock; serialize the response OFF the lock (L5).
        let (all_body, rows) = {
            let mut state = lock_state();
            lobbies_snapshot(&mut state)
        };
        if vf.is_empty() {
            write_response(&mut stream, 200, &all_body).await;
        } else {
            let body = filter_lobbies(&rows, &vf);
            write_response(&mut stream, 200, &body).await;
        }
    } else if method == "GET" && path == "/v1/latest" {
        // Env-overridable release info (resolved once; see the LATEST_* comment above).
        static LATEST: LazyLock<(i64, String, String)> = LazyLock::new(|| {
            (
                env_int("COOP_LATEST_PROTO", LATEST_PROTO),
                env_str("COOP_LATEST_MOD", LATEST_MOD),
                env_str("COOP_LATEST_URL", LATEST_URL),
            )
        });
        let (proto, mod_str, url) = &*LATEST;
        write_response(
            &mut stream,
            200,
            &json_bytes(&json!({"proto": proto, "mod": mod_str, "url": url})),
        )
        .await;
    } else if method == "GET" && path == "/v1/thanks" {
        // The thanks list the mod's main menu rolls. Three answers, and the mod acts on the
        // difference: the text; 404, the only answer that lets a client retire the copy it cached
        // from us; 503, a list we have and could not serve this moment, on which a client keeps
        // what it holds. The body is serialized once per re-read window, not per request.
        match thanks_answer().await {
            Answer::Body(body) => write_response(&mut stream, 200, &body).await,
            Answer::NoList => {
                write_response(&mut stream, 404, &json_bytes(&json!({"error": "no list"}))).await
            }
            Answer::Unavailable => {
                write_response(&mut stream, 503, &json_bytes(&json!({"error": "list unavailable"}))).await
            }
        }
    } else if method == "GET" && path == "/healthz" {
        let n = lock_state().lobbies.len();
        write_response(&mut stream, 200, &json_bytes(&json!({"ok": true, "lobbies": n}))).await;
    } else if method == "POST" {
        match dispatch_post(path, &client_ip, &body_obj) {
            Some((status, resp)) => write_response(&mut stream, status, &json_bytes(&resp)).await,
            None => write_response(&mut stream, 404, &json_bytes(&json!({"error": "not found"}))).await,
        }
    } else {
        write_response(&mut stream, 404, &json_bytes(&json!({"error": "not found"}))).await;
    }
}

#[tokio::main]
async fn main() {
    // fail fast on missing secrets (same as the Python FATAL exits)
    if CFG.turn_secret.is_empty() {
        log("FATAL: COOP_TURN_SECRET not set -- refusing to mint TURN creds");
        std::process::exit(1);
    }
    if CFG.signaling_token.is_empty() {
        log("FATAL: COOP_SIGNALING_TOKEN not set -- clients could not reach signaling");
        std::process::exit(1);
    }

    // Resolve the TLS decision BEFORE binding anything: with COOP_REQUIRE_TLS=1
    // and no cert this exits, and it must do so without having briefly bound a
    // cleartext port (Restart=always would otherwise flap one open every cycle).
    let tls_acceptor = tls::acceptor_from_env();

    let addr = format!("0.0.0.0:{}", CFG.port);
    let listener = match TcpListener::bind(&addr).await {
        Ok(l) => l,
        Err(e) => {
            log(&format!("FATAL: bind {addr} failed: {e}"));
            std::process::exit(1);
        }
    };
    log(&format!(
        "master listening on {addr} (signaling={} stun={} turn={})",
        if CFG.signaling_url.is_empty() { "?" } else { &CFG.signaling_url },
        if CFG.stun_uri.is_empty() { "?" } else { &CFG.stun_uri },
        if CFG.turn_uri.is_empty() { "off" } else { "on" }
    ));

    tokio::spawn(sweeper());

    // TLS listener on its OWN port, beside the plaintext one (Tier B arc 1).
    // Parallel ports -- not an in-place flip -- so every client build keeps
    // working through the cutover window and no intermediate state is knowingly
    // broken; the plaintext listener is retired in arc 5, gated on the accept
    // log below showing zero unknown-source plaintext connections.
    match tls_acceptor {
        Some(acceptor) => {
            let tls_port = env_int("COOP_MASTER_TLS_PORT", 10443) as u16;
            let tls_addr = format!("0.0.0.0:{tls_port}");
            match TcpListener::bind(&tls_addr).await {
                Ok(l) => {
                    log(&format!("master TLS listening on {tls_addr}"));
                    tokio::spawn(serve_tls(l, acceptor));
                }
                Err(e) => {
                    // A configured TLS listener that cannot bind is FATAL: coming
                    // up plaintext-only on a box meant to serve TLS is the silent
                    // downgrade this tier exists to prevent.
                    log(&format!("FATAL: bind {tls_addr} failed: {e}"));
                    std::process::exit(1);
                }
            }
        }
        None => log("TLS not configured (COOP_TLS_CERT/COOP_TLS_KEY unset) -- plaintext only"),
    }

    serve_plain(listener).await
}

/// Plaintext accept loop. Logs EVERY accept with the listener tag: this log is
/// the evidence base for the arc-5 retirement gate ("zero unknown-source
/// plaintext connections over 24h"). Without it the gate would be a hope -- the
/// 400 path below never logs, so a stray connection could pass unseen.
async fn serve_plain(listener: TcpListener) -> ! {
    loop {
        match listener.accept().await {
            Ok((stream, addr)) => {
                let peer_ip = addr.ip().to_string();
                log(&format!("accept [listener=plain] [{peer_ip}]"));
                tokio::spawn(handle(stream, peer_ip));
            }
            Err(e) => log(&format!("accept error: {e}")),
        }
    }
}

/// TLS accept loop. Handshake failures are logged (they are the observable a
/// client-side cert problem produces); successful accepts are NOT logged per
/// connection -- the lobby-list poll would make that pure noise, and the arc-5
/// positive proof comes from the signaling server's per-identity registration
/// lines instead.
async fn serve_tls(listener: TcpListener, acceptor: tokio_rustls::TlsAcceptor) -> ! {
    loop {
        match listener.accept().await {
            Ok((stream, addr)) => {
                let peer_ip = addr.ip().to_string();
                let acceptor = acceptor.clone();
                tokio::spawn(async move {
                    match acceptor.accept(stream).await {
                        Ok(tls_stream) => handle(tls_stream, peer_ip).await,
                        Err(e) => log(&format!("tls handshake failed [{peer_ip}]: {e}")),
                    }
                });
            }
            Err(e) => log(&format!("tls accept error: {e}")),
        }
    }
}
