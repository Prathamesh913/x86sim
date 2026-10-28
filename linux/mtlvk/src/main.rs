//! mtlvk: the native Linux half of x86Code's Metal stand-in for the iOS simulator.
//!
//! metalhostd (a Darling process running Apple's MTLSimImplementation over our MTLDevice classes) connects
//! here over a Unix socket and forwards Metal objects and command buffers in the VKMTL protocol
//! (../vkmtl_proto.h). mtlvk executes them on Vulkan through metal2vulkan-runtime. Buffer memory is
//! shared without copying: metalhostd passes the memfd behind each MTLBuffer (Darling syscall 599) and
//! mtlvk maps it and imports the pages into Vulkan (VK_EXT_external_memory_host).
//!
//! Usage: mtlvk [socket path]      (default: var/run/mtlvk.sock, relative to the repo root)
//!        mtlvk selftest           (memfd import, clear, blits on this GPU; no Darling needed)
//!        mtlvk selftest-compute [metallib]  (stage 4: kernels of tests/metal/shaders/probe.metallib)

mod conn;
mod proto;

use metal2vulkan_runtime::Device;
use std::os::unix::net::UnixListener;

pub const DEFAULT_SOCKET: &str = "var/run/mtlvk.sock";

fn main() {
    let arg = std::env::args().nth(1);
    let device = match Device::system_default() {
        Ok(d) => d,
        Err(e) => {
            eprintln!("mtlvk: no Vulkan device: {e}");
            std::process::exit(1);
        }
    };
    eprintln!("mtlvk: device {} (registryID {:#x})", device.name(), device.registry_id());
    if arg.as_deref() == Some("selftest-compute") {
        let path = std::env::args().nth(2).unwrap_or_else(|| "tests/metal/shaders/probe.metallib".into());
        let data = std::fs::read(&path).unwrap_or_else(|e| {
            eprintln!("mtlvk: {path}: {e}");
            std::process::exit(1);
        });
        std::process::exit(match conn::selftest_compute(&device, &data) {
            Ok(()) => {
                println!("mtlvk selftest-compute: PASS");
                0
            }
            Err(e) => {
                println!("mtlvk selftest-compute: FAIL: {e}");
                1
            }
        });
    }
    if arg.as_deref() == Some("selftest") {
        std::process::exit(match conn::selftest(&device) {
            Ok(()) => {
                println!("mtlvk selftest: PASS");
                0
            }
            Err(e) => {
                println!("mtlvk selftest: FAIL: {e}");
                1
            }
        });
    }
    let path = arg.unwrap_or_else(|| DEFAULT_SOCKET.to_string());
    let _ = std::fs::remove_file(&path);
    if let Some(dir) = std::path::Path::new(&path).parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let listener = UnixListener::bind(&path).unwrap_or_else(|e| {
        eprintln!("mtlvk: bind {path}: {e}");
        std::process::exit(1);
    });
    eprintln!("mtlvk: listening on {path}");
    for (n, stream) in listener.incoming().enumerate() {
        match stream {
            Ok(s) => {
                let device = device.clone();
                std::thread::Builder::new()
                    .name(format!("conn{n}"))
                    .spawn(move || conn::serve(n, device, s))
                    .ok();
            }
            Err(e) => eprintln!("mtlvk: accept: {e}"),
        }
    }
}
