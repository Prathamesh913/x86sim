//! x86Code: survey which functions of metallibs metal2vulkan's translator compiles for a device profile.
//! Usage: survey <profile.json> <lib.metallib>...   Function constants get zero values (false/0) so the
//! translation is exercised; vertex and fragment functions compile alone with default lowering inputs.
use metal2vulkan::device::{Device, DeviceDescription, DevicePolicy};
use metal2vulkan::library::{FunctionConstantValue, FunctionConstantValues, FunctionStage, Library};
use metal2vulkan::pipeline::{FunctionDescriptor, LoweringInputs};
use metal2vulkan::Budget;
use std::collections::BTreeMap;

fn type_size(raw: u32) -> usize {
    match raw {
        3 => 4, 4 => 8, 5 => 12, 6 => 16,
        16 => 2, 17 => 4, 18 => 6, 19 => 8,
        29 | 33 => 4, 30 | 34 => 8, 31 | 35 => 12, 32 | 36 => 16,
        37 | 41 => 2, 38 | 42 => 4, 39 | 43 => 6, 40 | 44 => 8,
        45 | 49 | 53 => 1, 46 | 50 | 54 => 2, 47 | 51 | 55 => 3, 48 | 52 | 56 => 4,
        81 | 85 => 8,
        _ => 4,
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let profile = std::fs::read_to_string(&args[0]).expect("profile");
    let desc = DeviceDescription::from_khronos_json(&profile, None).expect("profile json");
    let dev = Device::new(desc, DevicePolicy::default()).expect("device");
    let verbose = std::env::var_os("SURVEY_VERBOSE").is_some();
    let mut reasons: BTreeMap<String, usize> = BTreeMap::new();
    let (mut tot_ok, mut tot) = (0, 0);
    for path in &args[1..] {
        let bytes = match std::fs::read(path) {
            Ok(b) => b,
            Err(e) => { println!("{path}: {e}"); continue; }
        };
        let lib = match Library::parse(&bytes) {
            Ok(l) => l,
            Err(e) => { println!("{path}: parse: {e}"); continue; }
        };
        let (mut ok, mut n) = ([0usize; 3], [0usize; 3]);
        for info in lib.functions() {
            let k = match info.stage {
                FunctionStage::Kernel => 0,
                FunctionStage::Vertex => 1,
                FunctionStage::Fragment => 2,
                _ => continue,
            };
            n[k] += 1;
            let mut consts = FunctionConstantValues::defaults();
            for c in info.constants {
                if let Some(ty) = c.ty {
                    let v = FunctionConstantValue::new(ty, vec![0u8; type_size(ty.as_raw())]);
                    consts = match c.index {
                        Some(i) => consts.with_index(i, v),
                        None => consts.with_name(c.name.clone(), v),
                    };
                }
            }
            let f = match lib.function(info.name) {
                Ok(f) => f,
                Err(e) => { *reasons.entry(format!("lookup: {e}")).or_default() += 1; continue; }
            };
            let mut fd = FunctionDescriptor::new(f);
            fd.constants = consts;
            match dev.compile_function(&fd, &LoweringInputs::default(), &Budget::standard()) {
                Ok(_) => ok[k] += 1,
                Err(e) => {
                    let s = e.to_string();
                    let key: String = s.chars().take(110).collect();
                    if verbose {
                        println!("  {} {}: {s}", ["kernel", "vertex", "fragment"][k], info.name);
                    }
                    *reasons.entry(key).or_default() += 1;
                }
            }
        }
        tot_ok += ok.iter().sum::<usize>();
        tot += n.iter().sum::<usize>();
        println!("{path}: kernels {}/{}, vertex {}/{}, fragment {}/{}", ok[0], n[0], ok[1], n[1], ok[2], n[2]);
    }
    println!("TOTAL {tot_ok}/{tot} functions compiled");
    let mut v: Vec<_> = reasons.into_iter().collect();
    v.sort_by(|a, b| b.1.cmp(&a.1));
    for (r, c) in v.iter().take(25) {
        println!("{c:5}  {r}");
    }
}
