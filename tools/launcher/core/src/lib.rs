//! x86Code launcher core: the chunked AOT image (static discovery, incremental builds driven through libarm64jit's
//! builder), per-device feature toggles and sim control. Shared by the `x86code-aot` CLI, the `x86code-mcp` server and
//! the egui launcher.
pub mod aot;
pub mod discover;
pub mod dyldcache;
pub mod features;
pub mod hid;
pub mod host;
pub mod hot;
pub mod instance;
pub mod key;
pub mod logs;
pub mod macho;
pub mod ring;
pub mod sim;
