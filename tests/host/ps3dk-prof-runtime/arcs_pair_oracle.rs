// Test-only production pair oracle. Err must be a failing process result.
fn main() {
    let args: Vec<_> = std::env::args().collect();
    let arcs = std::fs::read(&args[1]).expect("arcs fixture");
    let timing = std::fs::read(&args[2]).expect("timing fixture");
    let result = ps3dk_prof::arcs::read_arcs(&arcs)
        .map_err(|e| e.to_string())
        .and_then(|a| ps3dk_prof::companions::match_companions(&a, &timing));
    match result {
        Ok(pair) if pair.is_final() => println!("FINAL PAIR"),
        Ok(_) => {
            eprintln!("not final");
            std::process::exit(1);
        }
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(1);
        }
    }
}
