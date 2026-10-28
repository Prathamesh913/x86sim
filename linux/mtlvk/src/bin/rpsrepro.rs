//! x86Code: replay a render pipeline mtlvk refused (var/metal/failed/<id>-{vert,frag}.metallib) through
//! metal2vulkan's compiler, with its full error. Usage:
//!   rpsrepro <profile.json> <vert.metallib> <frag.metallib|-> [I:FORMAT_RAW ...] [samples=N]
use metal2vulkan::device::{Device, DeviceDescription, DevicePolicy};
use metal2vulkan::library::Library;
use metal2vulkan::metal::PixelFormat;
use metal2vulkan::pipeline::{ColorAttachmentDescriptor, FunctionDescriptor, RenderPipelineDescriptor, StageSource};
use metal2vulkan::Budget;

fn func(path: &str) -> FunctionDescriptor {
    let lib = Library::parse(&std::fs::read(path).expect("read")).expect("parse");
    let name = lib.functions().next().expect("function").name.to_string();
    FunctionDescriptor::new(lib.function(&name).expect("function"))
}

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    let profile = std::fs::read_to_string(&a[0]).expect("profile");
    let dev = Device::new(DeviceDescription::from_khronos_json(&profile, None).expect("profile"), DevicePolicy::default()).expect("device");
    let mut d = RenderPipelineDescriptor::new(StageSource::Function(func(&a[1])));
    if a[2] != "-" {
        d.fragment = Some(StageSource::Function(func(&a[2])));
    }
    for s in &a[3..] {
        if let Some(n) = s.strip_prefix("samples=") {
            d.raster_sample_count = n.parse().unwrap();
        } else if let Some(n) = s.strip_prefix("stencil=") {
            d.stencil_format = Some(PixelFormat::try_from(n.parse::<u32>().unwrap()).unwrap());
        } else if let Some(n) = s.strip_prefix("depth=") {
            d.depth_format = Some(PixelFormat::try_from(n.parse::<u32>().unwrap()).unwrap());
        } else {
            let (i, f) = s.split_once(':').unwrap();
            let pf = PixelFormat::try_from(f.parse::<u32>().unwrap()).unwrap();
            d.color_attachments[i.parse::<usize>().unwrap()] = Some(ColorAttachmentDescriptor::new(pf));
        }
    }
    match dev.compile_render_pipeline(&d, &Budget::standard()) {
        Ok(_) => println!("OK"),
        Err(e) => println!("ERR {e}\n{e:#?}"),
    }
}
