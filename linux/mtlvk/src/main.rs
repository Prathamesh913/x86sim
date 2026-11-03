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
mod memstat;
mod proto;

use metal2vulkan_runtime::Device;
use std::os::unix::net::UnixListener;

pub const DEFAULT_SOCKET: &str = "var/run/mtlvk.sock";

extern "C" fn on_usr1(_: libc::c_int) {
    conn::STATS_REQ.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
}

/// x86Code RAM 2: what the phone's Metal needs of the device, and no more of the driver's memory.
/// - Ray tracing withheld (VK_KHR_acceleration_structure, VK_KHR_ray_query and
///   VK_KHR_deferred_host_operations): VKMTL has no acceleration structure, and NVIDIA's driver
///   loads libnvidia-rtcore and allocates ~60 MB PSS (37 MB of it malloc) for them in vkCreateDevice
///   (src/bin/devprobe.rs: 183 -> 123 MB). MTLVK_RAYTRACING=1 keeps them.
/// - 8 VkQueues for the logical queues' round robin (METAL2VULKAN_QUEUES; the runtime takes all 16 of
///   NVIDIA's graphics family): each costs ~2.3 MB (devprobe: 16 -> 8 queues 124 -> 106 MB).
/// - The in-memory memo of translated functions and pipelines at MTLVK_MEMO_MB (default 32; the runtime's default
///   is 256 MB): it is a copy of what the on-disk shader cache holds, and live pipeline states keep their own.
fn device_options() -> metal2vulkan_runtime::DeviceOptions {
    use metal2vulkan::device::{VkExtension as E, VkFeature as F};
    if std::env::var_os("METAL2VULKAN_QUEUES").is_none() {
        std::env::set_var("METAL2VULKAN_QUEUES", "8");
    }
    let mut o = metal2vulkan_runtime::DeviceOptions::default();
    let memo_mb = std::env::var("MTLVK_MEMO_MB").ok().and_then(|v| v.parse::<u64>().ok()).filter(|v| *v > 0).unwrap_or(32);
    o.shader_cache.memory_capacity = memo_mb << 20;
    if std::env::var("MTLVK_RAYTRACING").map(|v| v != "1").unwrap_or(true) {
        o.withheld_extensions.extend([E::KhrAccelerationStructure, E::KhrRayQuery, E::KhrDeferredHostOperations]);
        o.withheld_features.extend([F::AccelerationStructure, F::RayQuery]);
    }
    o
}

/// x86Code RAM 2: every MTLVK_TRIM_EVERY seconds (default 5; 0 = never), give back what the device's spare
/// command pools and descriptor pools hold past MTLVK_TRIM_IDLE seconds unused (default 10): NVIDIA keeps a command
/// pool's 2 MB chunks of pinned system memory until the pool is reset with RELEASE_RESOURCES, so the spares held
/// the busiest moment's command buffers for good (~2 MB per spare pool).
fn start_trim(device: Device) {
    let every = std::env::var("MTLVK_TRIM_EVERY").ok().and_then(|v| v.parse::<f64>().ok()).unwrap_or(5.0);
    let idle = std::env::var("MTLVK_TRIM_IDLE").ok().and_then(|v| v.parse::<f64>().ok()).unwrap_or(10.0);
    if every <= 0.0 {
        return;
    }
    let log = std::env::var_os("MTLVK_MEMSTATS").is_some();
    let _ = std::thread::Builder::new().name("trim".into()).spawn(move || {
        let mut last_malloc_trim = std::time::Instant::now();
        loop {
            std::thread::sleep(std::time::Duration::from_secs_f64(every));
            let (pools, descriptors) = device.trim_idle(std::time::Duration::from_secs_f64(idle));
            // glibc keeps freed pages of its arenas (an app's pipelines and command logs, gone with it); give back
            // the whole free pages after a quiet trim and at least every minute.
            let malloc = (pools > 0 || descriptors > 0 || last_malloc_trim.elapsed().as_secs() >= 60) && {
                last_malloc_trim = std::time::Instant::now();
                // SAFETY: plain libc call.
                unsafe { libc::malloc_trim(0) != 0 }
            };
            if log && (pools > 0 || descriptors > 0) {
                eprintln!("mtlvk trim: {pools} command pools, {descriptors} descriptor pools, malloc_trim {malloc}");
            }
        }
    });
}

fn main() {
    // x86Code Speed 7: SIGUSR1 -> per-connection object counts on stderr (next request)
    unsafe { libc::signal(libc::SIGUSR1, on_usr1 as libc::sighandler_t) };
    let arg = std::env::args().nth(1);
    // x86Code RAM 2: the device as the phone needs it (see device_options).
    let device = match Device::system_default_with(&device_options()) {
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
    memstat::start(device.clone());
    start_trim(device.clone());
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
