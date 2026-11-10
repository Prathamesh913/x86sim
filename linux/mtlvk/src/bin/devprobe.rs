//! x86Sim RAM 2: the idle cost of a metal2vulkan device on this host: PSS by mapping kind and malloc's figures
//! before and after `Device::system_default()`, and after a raw Vulkan instance + device without the runtime.
//! Usage: devprobe [raw|runtime]
fn pss() -> String {
    let s = std::fs::read_to_string("/proc/self/smaps_rollup").unwrap_or_default();
    let get = |k: &str| s.lines().find(|l| l.starts_with(k)).and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse::<u64>().ok()).unwrap_or(0) >> 10;
    // SAFETY: plain libc query.
    let m = unsafe { libc::mallinfo2() };
    format!("pss {} MB (anon {} file {} shmem {}) malloc in use {} MB", get("Pss:"), get("Pss_Anon:"), get("Pss_File:"), get("Pss_Shmem:"), m.uordblks >> 20)
}

fn main() {
    let mode = std::env::args().nth(1).unwrap_or_else(|| "runtime".into());
    println!("start: {}", pss());
    if mode == "cbtest" {
        cbtest();
        return;
    }
    if mode == "cbgrow" {
        cbgrow();
        return;
    }
    if mode == "cbhigh" {
        cbhigh();
        return;
    }
    if mode == "cbmalloc" {
        cbmalloc();
        return;
    }
    if mode == "raw" {
        use ash::vk;
        // SAFETY: plain Vulkan instance + device on the first physical device.
        unsafe {
            let entry = ash::Entry::load().unwrap();
            let app = vk::ApplicationInfo::default().api_version(vk::API_VERSION_1_3);
            let inst = entry.create_instance(&vk::InstanceCreateInfo::default().application_info(&app), None).unwrap();
            println!("instance: {}", pss());
            let pds = inst.enumerate_physical_devices().unwrap();
            let pd = pds.into_iter().find(|p| inst.get_physical_device_properties(*p).device_type == vk::PhysicalDeviceType::DISCRETE_GPU).unwrap();
            let nq: usize = std::env::var("NQ").ok().and_then(|v| v.parse().ok()).unwrap_or(1);
            let prio = vec![1.0f32; nq];
            let q = [vk::DeviceQueueCreateInfo::default().queue_family_index(0).queue_priorities(&prio)];
            let dev = inst.create_device(pd, &vk::DeviceCreateInfo::default().queue_create_infos(&q), None).unwrap();
            println!("device: {}", pss());
            std::hint::black_box(dev);
        }
    } else {
        use metal2vulkan::device::VkExtension as E;
        use metal2vulkan::device::VkFeature as F;
        let mut o = metal2vulkan_runtime::DeviceOptions::default();
        for w in std::env::var("WITHHOLD").unwrap_or_default().split(',').filter(|w| !w.is_empty()) {
            let e = match w {
                "accel" => vec![E::KhrAccelerationStructure, E::KhrDeferredHostOperations, E::KhrRayQuery],
                "mesh" => vec![E::ExtMeshShader],
                "descbuf" => vec![E::ExtDescriptorBuffer],
                "coopmat" => vec![E::KhrCooperativeMatrix],
                "interlock" => vec![E::ExtFragmentShaderInterlock],
                _ => panic!("{w}"),
            };
            o.withheld_extensions.extend(e);
            o.withheld_features.extend(match w {
                "accel" => vec![F::RayQuery, F::AccelerationStructure],
                "mesh" => vec![F::TaskShader, F::MeshShader],
                "interlock" => vec![F::FragmentShaderPixelInterlock, F::FragmentShaderSampleInterlock],
                _ => vec![],
            });
        }
        let d = metal2vulkan_runtime::Device::system_default_with(&o).unwrap();
        println!("extensions: {:?}", d.enabled().extensions);
        println!("runtime device: {}", pss());
        let q = d.new_command_queue().unwrap();
        println!("+queue: {}", pss());
        std::hint::black_box(q);
        std::hint::black_box(d);
    }
    for i in 1..=8 {
        std::thread::sleep(std::time::Duration::from_millis(500));
        println!("{} ms later: {}", i * 500, pss());
    }
}

fn nvctl() -> u64 {
    let s = std::fs::read_to_string("/proc/self/smaps").unwrap_or_default();
    let mut on = false;
    let mut t = 0;
    for l in s.lines() {
        let first = l.split_whitespace().next().unwrap_or("");
        if first.contains('-') && !first.ends_with(':') {
            on = l.contains("/dev/nvidiactl");
        } else if on {
            if let Some(v) = l.strip_prefix("Pss:") {
                t += v.trim().trim_end_matches(" kB").trim().parse::<u64>().unwrap_or(0);
            }
        }
    }
    t >> 10
}

/// Command pool memory on this driver: 30 pools, each one primary recording N fills; then reset with
/// RELEASE_RESOURCES, trim, free the command buffers, destroy.
fn cbtest() {
    use ash::vk;
    unsafe {
        let entry = ash::Entry::load().unwrap();
        let app = vk::ApplicationInfo::default().api_version(vk::API_VERSION_1_3);
        let inst = entry.create_instance(&vk::InstanceCreateInfo::default().application_info(&app), None).unwrap();
        let pd = inst.enumerate_physical_devices().unwrap().into_iter().find(|p| inst.get_physical_device_properties(*p).device_type == vk::PhysicalDeviceType::DISCRETE_GPU).unwrap();
        let prio = [1.0f32];
        let q = [vk::DeviceQueueCreateInfo::default().queue_family_index(0).queue_priorities(&prio)];
        let dev = inst.create_device(pd, &vk::DeviceCreateInfo::default().queue_create_infos(&q), None).unwrap();
        let queue = dev.get_device_queue(0, 0);
        let buf = dev.create_buffer(&vk::BufferCreateInfo::default().size(1 << 20).usage(vk::BufferUsageFlags::TRANSFER_DST), None).unwrap();
        let req = dev.get_buffer_memory_requirements(buf);
        let mem = dev.allocate_memory(&vk::MemoryAllocateInfo::default().allocation_size(req.size).memory_type_index(1), None).unwrap();
        dev.bind_buffer_memory(buf, mem, 0).unwrap();
        println!("device: nvctl {} MB", nvctl());
        let n: u32 = std::env::var("N").ok().and_then(|v| v.parse().ok()).unwrap_or(20000);
        let mut pools = Vec::new();
        for _ in 0..30 {
            let pool = dev.create_command_pool(&vk::CommandPoolCreateInfo::default().flags(vk::CommandPoolCreateFlags::TRANSIENT | vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER).queue_family_index(0), None).unwrap();
            let cb = dev.allocate_command_buffers(&vk::CommandBufferAllocateInfo::default().command_pool(pool).level(vk::CommandBufferLevel::PRIMARY).command_buffer_count(1)).unwrap()[0];
            dev.begin_command_buffer(cb, &vk::CommandBufferBeginInfo::default().flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT)).unwrap();
            for i in 0..n {
                dev.cmd_fill_buffer(cb, buf, ((i % 1024) * 4) as u64, 4, i);
            }
            dev.end_command_buffer(cb).unwrap();
            let cbs = [cb];
            dev.queue_submit(queue, &[vk::SubmitInfo::default().command_buffers(&cbs)], vk::Fence::null()).unwrap();
            pools.push((pool, cb));
        }
        dev.device_wait_idle().unwrap();
        println!("recorded: nvctl {} MB", nvctl());
        for (p, _) in &pools { dev.reset_command_pool(*p, vk::CommandPoolResetFlags::RELEASE_RESOURCES).unwrap(); }
        println!("reset release: nvctl {} MB", nvctl());
        for (p, _) in &pools { dev.trim_command_pool(*p, vk::CommandPoolTrimFlags::empty()); }
        println!("trim: nvctl {} MB", nvctl());
        for (p, cb) in &pools { dev.free_command_buffers(*p, &[*cb]); dev.trim_command_pool(*p, vk::CommandPoolTrimFlags::empty()); }
        println!("free cbs + trim: nvctl {} MB", nvctl());
        for (p, _) in &pools { dev.destroy_command_pool(*p, None); }
        println!("destroy: nvctl {} MB", nvctl());
    }
}

/// x86Sim RAM 5: the driver's host malloc per command buffer: 10 pools x N primaries (each recorded with a few fills
/// and submitted), then free / trim / destroy, malloc's in-use and PSS after each.
fn cbmalloc() {
    use ash::vk;
    unsafe {
        let entry = ash::Entry::load().unwrap();
        let app = vk::ApplicationInfo::default().api_version(vk::API_VERSION_1_3);
        let inst = entry.create_instance(&vk::InstanceCreateInfo::default().application_info(&app), None).unwrap();
        let pd = inst.enumerate_physical_devices().unwrap().into_iter().find(|p| inst.get_physical_device_properties(*p).device_type == vk::PhysicalDeviceType::DISCRETE_GPU).unwrap();
        let prio = [1.0f32];
        let q = [vk::DeviceQueueCreateInfo::default().queue_family_index(0).queue_priorities(&prio)];
        let dev = inst.create_device(pd, &vk::DeviceCreateInfo::default().queue_create_infos(&q), None).unwrap();
        let queue = dev.get_device_queue(0, 0);
        let buf = dev.create_buffer(&vk::BufferCreateInfo::default().size(1 << 20).usage(vk::BufferUsageFlags::TRANSFER_DST), None).unwrap();
        let req = dev.get_buffer_memory_requirements(buf);
        let mem = dev.allocate_memory(&vk::MemoryAllocateInfo::default().allocation_size(req.size).memory_type_index(1), None).unwrap();
        dev.bind_buffer_memory(buf, mem, 0).unwrap();
        println!("device: {} nvctl {}", pss(), nvctl());
        let n: u32 = std::env::var("N").ok().and_then(|v| v.parse().ok()).unwrap_or(130);
        let mut pools = Vec::new();
        for _ in 0..10 {
            let pool = dev.create_command_pool(&vk::CommandPoolCreateInfo::default().flags(vk::CommandPoolCreateFlags::TRANSIENT | vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER).queue_family_index(0), None).unwrap();
            let cbs = dev.allocate_command_buffers(&vk::CommandBufferAllocateInfo::default().command_pool(pool).level(vk::CommandBufferLevel::PRIMARY).command_buffer_count(n)).unwrap();
            // x86Sim RAM 8: ONETIME=0 begins them reusable (no ONE_TIME_SUBMIT), as the runtime's replays do
            let flags = if std::env::var("ONETIME").as_deref() == Ok("0") { vk::CommandBufferUsageFlags::empty() } else { vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT };
            for cb in &cbs {
                dev.begin_command_buffer(*cb, &vk::CommandBufferBeginInfo::default().flags(flags)).unwrap();
                for i in 0..8 {
                    dev.cmd_fill_buffer(*cb, buf, i * 4, 4, i as u32);
                }
                dev.end_command_buffer(*cb).unwrap();
            }
            dev.queue_submit(queue, &[vk::SubmitInfo::default().command_buffers(&cbs)], vk::Fence::null()).unwrap();
            pools.push((pool, cbs));
        }
        dev.device_wait_idle().unwrap();
        println!("{} cbs recorded: {} nvctl {}", n * 10, pss(), nvctl());
        for (p, _) in &pools { dev.reset_command_pool(*p, vk::CommandPoolResetFlags::RELEASE_RESOURCES).unwrap(); }
        println!("reset release: {} nvctl {}", pss(), nvctl());
        for (p, cbs) in &pools { dev.free_command_buffers(*p, cbs); dev.trim_command_pool(*p, vk::CommandPoolTrimFlags::empty()); }
        libc::malloc_trim(0);
        println!("free cbs + trim: {} nvctl {}", pss(), nvctl());
        for (p, _) in &pools { dev.destroy_command_pool(*p, None); }
        libc::malloc_trim(0);
        println!("destroy pools: {} nvctl {}", pss(), nvctl());
    }
}

/// x86Sim RAM 8: does a command pool that once recorded a large command buffer take a large chunk for each later
/// small one? 40 pools each record a big command buffer (BIG fills), one at a time (submitted, waited, the pool
/// reset with RELEASE_RESOURCES when RELEASE=1, else plainly); then all 40 record a small one (10 fills) at once,
/// in flight together. nvctl after each phase.
fn cbgrow() {
    use ash::vk;
    unsafe {
        let entry = ash::Entry::load().unwrap();
        let app = vk::ApplicationInfo::default().api_version(vk::API_VERSION_1_3);
        let inst = entry.create_instance(&vk::InstanceCreateInfo::default().application_info(&app), None).unwrap();
        let pd = inst.enumerate_physical_devices().unwrap().into_iter().find(|p| inst.get_physical_device_properties(*p).device_type == vk::PhysicalDeviceType::DISCRETE_GPU).unwrap();
        let prio = [1.0f32];
        let q = [vk::DeviceQueueCreateInfo::default().queue_family_index(0).queue_priorities(&prio)];
        let dev = inst.create_device(pd, &vk::DeviceCreateInfo::default().queue_create_infos(&q), None).unwrap();
        let queue = dev.get_device_queue(0, 0);
        let buf = dev.create_buffer(&vk::BufferCreateInfo::default().size(1 << 20).usage(vk::BufferUsageFlags::TRANSFER_DST), None).unwrap();
        let req = dev.get_buffer_memory_requirements(buf);
        let mem = dev.allocate_memory(&vk::MemoryAllocateInfo::default().allocation_size(req.size).memory_type_index(1), None).unwrap();
        dev.bind_buffer_memory(buf, mem, 0).unwrap();
        let big: u32 = std::env::var("BIG").ok().and_then(|v| v.parse().ok()).unwrap_or(20000);
        let release = std::env::var("RELEASE").as_deref() == Ok("1");
        println!("device: nvctl {}", nvctl());
        let mut pools = Vec::new();
        let record = |cb: vk::CommandBuffer, n: u32| {
            dev.begin_command_buffer(cb, &vk::CommandBufferBeginInfo::default().flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT)).unwrap();
            for i in 0..n {
                dev.cmd_fill_buffer(cb, buf, ((i % 1024) * 4) as u64, 4, i);
            }
            dev.end_command_buffer(cb).unwrap();
        };
        for _ in 0..40 {
            let pool = dev.create_command_pool(&vk::CommandPoolCreateInfo::default().flags(vk::CommandPoolCreateFlags::TRANSIENT | vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER).queue_family_index(0), None).unwrap();
            let cb = dev.allocate_command_buffers(&vk::CommandBufferAllocateInfo::default().command_pool(pool).level(vk::CommandBufferLevel::PRIMARY).command_buffer_count(1)).unwrap()[0];
            record(cb, big);
            dev.queue_submit(queue, &[vk::SubmitInfo::default().command_buffers(&[cb])], vk::Fence::null()).unwrap();
            dev.device_wait_idle().unwrap();
            let flags = if release { vk::CommandPoolResetFlags::RELEASE_RESOURCES } else { vk::CommandPoolResetFlags::empty() };
            dev.reset_command_pool(pool, flags).unwrap();
            pools.push((pool, cb));
        }
        println!("40 big, one at a time: nvctl {}", nvctl());
        for (_, cb) in &pools {
            record(*cb, 10);
        }
        let cbs: Vec<_> = pools.iter().map(|p| p.1).collect();
        dev.queue_submit(queue, &[vk::SubmitInfo::default().command_buffers(&cbs)], vk::Fence::null()).unwrap();
        println!("40 small in flight: nvctl {}", nvctl());
        dev.device_wait_idle().unwrap();
    }
}

/// x86Sim RAM 10: where NVIDIA's command memory high water lives. 40 pools record BIG commands at once (on THREADS
/// threads), submitted together; then every pool destroyed; then the recording threads ended; then a fresh pool of
/// small work. nvctl after each step.
fn cbhigh() {
    use ash::vk;
    use ash::vk::Handle;
    unsafe {
        let entry = ash::Entry::load().unwrap();
        let app = vk::ApplicationInfo::default().api_version(vk::API_VERSION_1_3);
        let inst = entry.create_instance(&vk::InstanceCreateInfo::default().application_info(&app), None).unwrap();
        let pd = inst.enumerate_physical_devices().unwrap().into_iter().find(|p| inst.get_physical_device_properties(*p).device_type == vk::PhysicalDeviceType::DISCRETE_GPU).unwrap();
        let prio = [1.0f32];
        let q = [vk::DeviceQueueCreateInfo::default().queue_family_index(0).queue_priorities(&prio)];
        let dev = inst.create_device(pd, &vk::DeviceCreateInfo::default().queue_create_infos(&q), None).unwrap();
        let queue = dev.get_device_queue(0, 0);
        let buf = dev.create_buffer(&vk::BufferCreateInfo::default().size(1 << 20).usage(vk::BufferUsageFlags::TRANSFER_DST), None).unwrap();
        let req = dev.get_buffer_memory_requirements(buf);
        let mem = dev.allocate_memory(&vk::MemoryAllocateInfo::default().allocation_size(req.size).memory_type_index(1), None).unwrap();
        dev.bind_buffer_memory(buf, mem, 0).unwrap();
        let big: u32 = std::env::var("BIG").ok().and_then(|v| v.parse().ok()).unwrap_or(20000);
        let threads: usize = std::env::var("THREADS").ok().and_then(|v| v.parse().ok()).unwrap_or(4);
        println!("device: nvctl {}", nvctl());
        let devr = &dev;
        let record = move |cb: vk::CommandBuffer, n: u32| {
            devr.begin_command_buffer(cb, &vk::CommandBufferBeginInfo::default().flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT)).unwrap();
            for i in 0..n {
                devr.cmd_fill_buffer(cb, buf, ((i % 1024) * 4) as u64, 4, i);
            }
            devr.end_command_buffer(cb).unwrap();
        };
        let (done_tx, done_rx) = std::sync::mpsc::channel::<()>();
        let done_rx = std::sync::Arc::new(std::sync::Mutex::new(done_rx));
        let (pool_tx, pool_rx) = std::sync::mpsc::channel();
        std::thread::scope(|s| {
            for _ in 0..threads {
                let pool_tx = pool_tx.clone();
                let done_rx = done_rx.clone();
                s.spawn(move || {
                    for _ in 0..40 / threads {
                        let pool = devr.create_command_pool(&vk::CommandPoolCreateInfo::default().flags(vk::CommandPoolCreateFlags::TRANSIENT | vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER).queue_family_index(0), None).unwrap();
                        let cb = devr.allocate_command_buffers(&vk::CommandBufferAllocateInfo::default().command_pool(pool).level(vk::CommandBufferLevel::PRIMARY).command_buffer_count(1)).unwrap()[0];
                        record(cb, big);
                        pool_tx.send((pool.as_raw(), cb.as_raw())).unwrap();
                    }
                    let _ = done_rx.lock().unwrap().recv();
                });
            }
            drop(pool_tx);
            let pools: Vec<(u64, u64)> = pool_rx.iter().take(40 / threads * threads).collect();
            println!("{} big recorded at once: nvctl {}", pools.len(), nvctl());
            let cbs: Vec<vk::CommandBuffer> = pools.iter().map(|p| vk::CommandBuffer::from_raw(p.1)).collect();
            dev.queue_submit(queue, &[vk::SubmitInfo::default().command_buffers(&cbs)], vk::Fence::null()).unwrap();
            dev.device_wait_idle().unwrap();
            for (p, _) in &pools {
                dev.destroy_command_pool(vk::CommandPool::from_raw(*p), None);
            }
            println!("all pools destroyed: nvctl {}", nvctl());
            for _ in 0..threads {
                done_tx.send(()).unwrap();
            }
        });
        println!("recording threads ended: nvctl {}", nvctl());
        dev.device_wait_idle().unwrap();
        std::thread::sleep(std::time::Duration::from_secs(2));
        println!("2 s later: nvctl {}", nvctl());
        dev.destroy_buffer(buf, None);
        dev.free_memory(mem, None);
        dev.destroy_device(None);
        println!("device destroyed: nvctl {}", nvctl());
        inst.destroy_instance(None);
        println!("instance destroyed: nvctl {}", nvctl());
        if std::env::var("AGAIN").is_ok() {
            let inst = entry.create_instance(&vk::InstanceCreateInfo::default().application_info(&app), None).unwrap();
            let pd = inst.enumerate_physical_devices().unwrap()[0];
            let pd = inst.enumerate_physical_devices().unwrap().into_iter().find(|p| inst.get_physical_device_properties(*p).device_type == vk::PhysicalDeviceType::DISCRETE_GPU).unwrap_or(pd);
            let dev = inst.create_device(pd, &vk::DeviceCreateInfo::default().queue_create_infos(&q), None).unwrap();
            println!("second device: nvctl {}", nvctl());
            dev.destroy_device(None);
        }
    }
}
