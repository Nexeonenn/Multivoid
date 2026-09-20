//! The master's own environment: the ports and the secrets it refuses to start without.
//! Read once, at first use, and never mutated -- so every module reads one snapshot.

use crate::common::{env_int, env_str};
use std::sync::LazyLock;

pub struct Config {
    pub port: u16,
    pub turn_secret: String,
    pub signaling_token: String,
    pub signaling_url: String,
    pub stun_uri: String,
    pub turn_uri: String,
}

impl Config {
    fn from_env() -> Config {
        Config {
            port: env_int("COOP_MASTER_PORT", 10001) as u16,
            turn_secret: env_str("COOP_TURN_SECRET", ""),
            signaling_token: env_str("COOP_SIGNALING_TOKEN", ""),
            signaling_url: env_str("COOP_SIGNALING_URL", ""),
            stun_uri: env_str("COOP_STUN_URI", ""),
            turn_uri: env_str("COOP_TURN_URI", ""),
        }
    }
}

pub static CFG: LazyLock<Config> = LazyLock::new(Config::from_env);
