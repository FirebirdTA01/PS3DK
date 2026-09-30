//! Linker scripts and call wrappers for SPU code overlays (libovis).
//!
//! An overlaid SPU program has one or more overlay *regions*; each region
//! holds several *sections* that share its local-store addresses, and each
//! section is built from the `.text` and `.rodata` of some objects.  The
//! linker script this crate writes
//!
//! * places the sections of region N at the same VMA, after the end of every
//!   earlier region, before `.text` (`INSERT BEFORE .text`);
//! * gives every section its own load address (LMA) from 0x40000 upward, one
//!   after another, so each is a separate loadable segment past the end of
//!   local store (the PPU copies those into the overlay table);
//! * lays down, after `.data`, the LS overlay table `_ovly_table`: one
//!   16-byte entry `{vma, size, lma, 0}` per section, labelled
//!   `__ovly_info_<section>`, followed by `_novlys`.
//!
//! Calls can be routed through a wrapper that maps the callee's section
//! first (`ld --wrap=<function>` plus an assembly stub calling
//! `_cellOvisUpdateAndMapSection`).  `cellOvisMkLdscript` takes the layout
//! from an XML file; `cellOvisConfigAuto` makes one region with one section
//! per object and wraps every global function the objects define.

use std::fmt::Write as _;

/// End of SPU local store: overlay load addresses start here.
pub const LS_SIZE: u32 = 0x40000;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Object {
    pub path: String,
    /// Functions whose calls are wrapped (manual mode: listed in the XML).
    pub functions: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Section {
    pub name: String,
    pub objects: Vec<Object>,
}

/// Sections sharing one LS range.
pub type Region = Vec<Section>;

#[derive(Debug)]
pub struct Error(pub String);

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

fn err<T>(msg: impl Into<String>) -> Result<T, Error> {
    Err(Error(msg.into()))
}

// ---------------------------------------------------------------- XML ----

#[derive(Debug)]
enum Tag {
    Open(String, Vec<(String, String)>, bool),
    Close(String),
}

/// The handful of XML the configuration uses: elements with quoted
/// attributes, self-closing tags, comments, the `<?xml?>` declaration and
/// text (ignored).
fn tags(text: &str) -> Result<Vec<Tag>, Error> {
    let mut out = Vec::new();
    let mut rest = text;
    while let Some(lt) = rest.find('<') {
        rest = &rest[lt..];
        if let Some(body) = rest.strip_prefix("<!--") {
            let end = body.find("-->").ok_or_else(|| Error("unterminated comment".into()))?;
            rest = &body[end + 3..];
            continue;
        }
        if rest.starts_with("<?") || rest.starts_with("<!") {
            let end = rest.find('>').ok_or_else(|| Error("unterminated declaration".into()))?;
            rest = &rest[end + 1..];
            continue;
        }
        let end = find_tag_end(rest).ok_or_else(|| Error("unterminated tag".into()))?;
        let inner = &rest[1..end];
        rest = &rest[end + 1..];
        if let Some(name) = inner.strip_prefix('/') {
            out.push(Tag::Close(name.trim().to_string()));
            continue;
        }
        let (inner, self_closing) = match inner.strip_suffix('/') {
            Some(s) => (s, true),
            None => (inner, false),
        };
        let inner = inner.trim();
        let name_end = inner.find(|c: char| c.is_whitespace()).unwrap_or(inner.len());
        let name = inner[..name_end].to_string();
        out.push(Tag::Open(name, attributes(&inner[name_end..])?, self_closing));
    }
    Ok(out)
}

fn find_tag_end(s: &str) -> Option<usize> {
    let mut quote = None;
    for (i, c) in s.char_indices() {
        match (quote, c) {
            (None, '"') | (None, '\'') => quote = Some(c),
            (Some(q), _) if c == q => quote = None,
            (None, '>') => return Some(i),
            _ => {}
        }
    }
    None
}

fn attributes(mut s: &str) -> Result<Vec<(String, String)>, Error> {
    let mut out = Vec::new();
    loop {
        s = s.trim_start();
        if s.is_empty() {
            return Ok(out);
        }
        let eq = s.find('=').ok_or_else(|| Error(format!("bad attribute near '{s}'")))?;
        let key = s[..eq].trim().to_string();
        let v = s[eq + 1..].trim_start();
        let q = v.chars().next().filter(|c| *c == '"' || *c == '\'')
            .ok_or_else(|| Error(format!("attribute {key} is not quoted")))?;
        let close = v[1..].find(q).ok_or_else(|| Error(format!("attribute {key} is not closed")))?;
        out.push((key, unescape(&v[1..1 + close])));
        s = &v[close + 2..];
    }
}

fn unescape(s: &str) -> String {
    s.replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"")
        .replace("&apos;", "'").replace("&amp;", "&")
}

fn name_attr(attrs: &[(String, String)], element: &str) -> Result<String, Error> {
    match attrs.iter().find(|(k, _)| k == "name") {
        Some((_, v)) if !v.is_empty() => Ok(v.clone()),
        _ => err(format!("<{element}> needs a name attribute")),
    }
}

/// Parse an `<ovis_config>` file: `<overlay>` regions holding `<sections>`
/// groups of `<section name=..>`, each listing `<object name=..>` elements,
/// which may list `<function name=..>` elements to wrap.
pub fn parse_config(text: &str) -> Result<Vec<Region>, Error> {
    let mut regions: Vec<Region> = Vec::new();
    let mut stack: Vec<String> = Vec::new();
    for tag in tags(text)? {
        match tag {
            Tag::Open(name, attrs, self_closing) => {
                let parent = stack.last().map(String::as_str);
                match (name.as_str(), parent) {
                    ("ovis_config", None) => {}
                    ("overlay", Some("ovis_config")) => regions.push(Vec::new()),
                    ("sections", Some("overlay")) => {}
                    ("section", Some("sections")) => {
                        let n = name_attr(&attrs, "section")?;
                        check_identifier(&n)?;
                        regions.last_mut().unwrap().push(Section { name: n, objects: Vec::new() });
                    }
                    ("object", Some("section")) => {
                        let path = name_attr(&attrs, "object")?;
                        let sec = regions.last_mut().unwrap().last_mut().unwrap();
                        sec.objects.push(Object { path, functions: Vec::new() });
                    }
                    ("function", Some("object")) => {
                        let f = name_attr(&attrs, "function")?;
                        check_identifier(&f)?;
                        let sec = regions.last_mut().unwrap().last_mut().unwrap();
                        sec.objects.last_mut().unwrap().functions.push(f);
                    }
                    (n, p) => return err(format!("unexpected <{n}> inside <{}>", p.unwrap_or("document"))),
                }
                if !self_closing {
                    stack.push(name);
                }
            }
            Tag::Close(name) => match stack.pop() {
                Some(open) if open == name => {}
                Some(open) => return err(format!("</{name}> closes <{open}>")),
                None => return err(format!("stray </{name}>")),
            },
        }
    }
    if let Some(open) = stack.pop() {
        return err(format!("<{open}> is not closed"));
    }
    regions.retain(|r| !r.is_empty());
    if regions.is_empty() {
        return err("no overlay sections in the configuration");
    }
    let mut seen = std::collections::HashSet::new();
    for s in regions.iter().flatten() {
        if !seen.insert(s.name.as_str()) {
            return err(format!("section {} appears twice", s.name));
        }
        if s.objects.is_empty() {
            return err(format!("section {} lists no object", s.name));
        }
    }
    Ok(regions)
}

fn check_identifier(s: &str) -> Result<(), Error> {
    let ok = s.chars().next().is_some_and(|c| c.is_ascii_alphabetic() || c == '_')
        && s.chars().all(|c| c.is_ascii_alphanumeric() || c == '_');
    if ok { Ok(()) } else { err(format!("'{s}' is not a valid symbol name")) }
}

/// Section name for an object in automatic mode: the path with `/` (and
/// `\`) as `__` and any other character that cannot appear in a symbol as
/// `_`, so `objs/sort.spu.o` becomes `objs__sort_spu_o`.
pub fn section_name_for_object(path: &str) -> String {
    let mut s = String::new();
    for c in path.chars() {
        match c {
            '/' | '\\' => s.push_str("__"),
            c if c.is_ascii_alphanumeric() || c == '_' => s.push(c),
            _ => s.push('_'),
        }
    }
    if s.starts_with(|c: char| c.is_ascii_digit()) {
        s.insert(0, '_');
    }
    s
}

// ----------------------------------------------------- linker script ----

/// Linker-script path: forward slashes, and quoted when it has characters
/// the script lexer would split on.
fn script_path(p: &str) -> String {
    let p = p.replace('\\', "/");
    if p.chars().any(|c| c.is_whitespace() || "(),;=\"".contains(c)) {
        format!("\"{p}\"")
    } else {
        p
    }
}

/// The two-part linker script: overlay sections before `.text`, the LS
/// overlay table after `.data`.  `align` is the section alignment (128 for
/// the XML layouts, 16 for automatic mode); sizes are rounded to it.
pub fn linker_script(regions: &[Region], align: u32) -> String {
    let m = align - 1;
    let mut s = String::new();
    s.push_str("/* SPU overlay layout generated for libovis. */\n");
    s.push_str("SECTIONS\n{\n");
    s.push_str("  ov_vma_start = .;\n");
    let _ = writeln!(s, "  ov_lma_start = 0x{LS_SIZE:x};");
    let mut earlier_stops: Vec<String> = Vec::new();
    let mut prev_lma: Option<String> = None;
    for region in regions {
        // a region starts after the end of every earlier region
        let base = earlier_stops.iter().fold("ov_vma_start".to_string(), |acc, stop| format!("MAX({acc}, {stop})"));
        let mut stops = Vec::new();
        for sec in region {
            let n = &sec.name;
            let lma_from = prev_lma.clone().unwrap_or_else(|| "ov_lma_start".into());
            let inputs: Vec<String> = sec.objects.iter()
                .map(|o| format!("{}(.text .text.* .rodata .rodata.*)", script_path(&o.path)))
                .collect();
            let _ = writeln!(s, "\n  vma_start_{n} = ({base} + {m}) & ~{m};");
            let _ = writeln!(s, "  lma_start_{n} = ({lma_from} + {m}) & ~{m};");
            let _ = writeln!(s, "  {n} vma_start_{n} : AT(lma_start_{n}) {{ {} }}", inputs.join(" "));
            let _ = writeln!(s, "  vma_stop_{n} = (vma_start_{n} + SIZEOF({n}) + {m}) & ~{m};");
            let _ = writeln!(s, "  lma_stop_{n} = (lma_start_{n} + SIZEOF({n}) + {m}) & ~{m};");
            stops.push(format!("vma_stop_{n}"));
            prev_lma = Some(format!("lma_stop_{n}"));
        }
        earlier_stops.extend(stops);
    }
    let end = earlier_stops.iter().fold("ov_vma_start".to_string(), |acc, stop| format!("MAX({acc}, {stop})"));
    let _ = writeln!(s, "\n  . = ({end} + 127) & ~127;");
    s.push_str("  ov_vma_stop = .;\n");
    // Without an AT(), ld gives the next output section the load offset of
    // the one before it, so .text would inherit the last overlay's LMA and
    // share its segment.  A word placed with LMA = VMA puts the rest of the
    // program back at its own address.
    s.push_str("  .ovis.lma_reset ov_vma_stop : AT(ov_vma_stop) { LONG(0) }\n");
    s.push_str("}\nINSERT BEFORE .text;\n\n");

    s.push_str("SECTIONS\n{\n  .data.libovis :\n  {\n    . = ALIGN(16);\n    _ovly_table = .;\n");
    let mut count = 0;
    for sec in regions.iter().flatten() {
        let n = &sec.name;
        let _ = writeln!(s, "    . = ALIGN(16);\n    __ovly_info_{n} = .;");
        let _ = writeln!(s, "    LONG(ADDR({n})); LONG((SIZEOF({n}) + {m}) & ~{m}); LONG(LOADADDR({n})); LONG(0);");
        count += 1;
    }
    let _ = writeln!(s, "    . = ALIGN(16);\n    _novlys = .;\n    LONG({count});\n    . = ALIGN(16);");
    s.push_str("  }\n}\nINSERT AFTER .data;\n");
    s
}

/// Link options for an overlaid program, one line: `--no-overlays` (the SPU
/// linker would otherwise see the overlapping sections, build its own
/// overlay manager and tables, and clash with libovis's), then a
/// `-Wl,--wrap=` per wrapped function.
pub fn ldflags(wrapped: &[(String, String)]) -> String {
    let mut s = vec![NO_OVERLAYS.to_string()];
    let mut funcs: Vec<String> = wrapped.iter().map(|(f, _)| format!("-Wl,--wrap={f}")).collect();
    funcs.dedup();
    s.extend(funcs);
    s.join(" ") + "\n"
}

/// The link option every libovis program needs.
pub const NO_OVERLAYS: &str = "-Wl,--no-overlays";

/// Assembly for the wrappers: `__wrap_<f>` maps `<section>` through
/// `_cellOvisUpdateAndMapSection` (which keeps $2 and $4..$14), then
/// branches to `__real_<f>` with the original arguments and return address.
pub fn wrappers(wrapped: &[(String, String)]) -> String {
    let mut s = String::new();
    s.push_str("/* SPU overlay call wrappers generated for libovis. */\n\t.text\n");
    for (f, sec) in wrapped {
        let _ = write!(
            s,
            "\n\t.align\t3\n\t.global\t__wrap_{f}\n\t.type\t__wrap_{f}, @function\n__wrap_{f}:\n\
             \tstqd\t$lr, 16($sp)\n\
             \tstqd\t$sp, -48($sp)\n\
             \tai\t$sp, $sp, -48\n\
             \tstqd\t$3, 32($sp)\n\
             \t/* load offset of this code: run-time minus link-time address */\n\
             \tila\t$75, 1f\n\
             \tbrsl\t$76, 1f\n\
             1:\tsf\t$76, $75, $76\n\
             \tila\t$3, __ovly_info_{sec}\n\
             \ta\t$3, $3, $76\n\
             \tbrsl\t$lr, _cellOvisUpdateAndMapSection\n\
             \tlqd\t$3, 32($sp)\n\
             \tai\t$sp, $sp, 48\n\
             \tlqd\t$lr, 16($sp)\n\
             \tbr\t__real_{f}\n\
             \t.size\t__wrap_{f}, .-__wrap_{f}\n"
        );
    }
    s
}

/// (function, section) pairs from the XML `<function>` lists.
pub fn listed_functions(regions: &[Region]) -> Vec<(String, String)> {
    let mut out = Vec::new();
    for sec in regions.iter().flatten() {
        for o in &sec.objects {
            for f in &o.functions {
                out.push((f.clone(), sec.name.clone()));
            }
        }
    }
    out
}

/// Global functions an SPU object defines in its text: what automatic mode
/// wraps.
pub fn object_functions(data: &[u8]) -> Result<Vec<String>, Error> {
    use object::{Object as _, ObjectSection as _, ObjectSymbol as _, SymbolKind, SymbolSection};
    let file = object::File::parse(data).map_err(|e| Error(format!("not an object file: {e}")))?;
    if file.architecture() != object::Architecture::Unknown && format!("{:?}", file.architecture()) != "Spu" {
        // object 0.36 has no SPU architecture value; anything it names is wrong
        return err("not an SPU object");
    }
    let mut out = Vec::new();
    for sym in file.symbols() {
        if !sym.is_global() || !sym.is_definition() || sym.kind() != SymbolKind::Text {
            continue;
        }
        if let SymbolSection::Section(idx) = sym.section() {
            let sec = file.section_by_index(idx).map_err(|e| Error(e.to_string()))?;
            let n = sec.name().unwrap_or("");
            if n == ".text" || n.starts_with(".text.") {
                out.push(sym.name().map_err(|e| Error(e.to_string()))?.to_string());
            }
        }
    }
    out.sort();
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    const XML: &str = r#"<?xml version="1.0"?>
<!-- two regions -->
<ovis_config>
  <overlay>
    <sections><section name="sec_a"><object name="objs/a.o"/></section></sections>
    <sections><section name="sec_b"><object name="objs/b.o"></object></section></sections>
  </overlay>
  <overlay>
    <sections><section name="sec_c">
      <object name="objs/c.o"><function name="fc"/></object>
    </section></sections>
  </overlay>
</ovis_config>"#;

    #[test]
    fn parses_regions_sections_objects_functions() {
        let r = parse_config(XML).unwrap();
        assert_eq!(r.len(), 2);
        assert_eq!(r[0].iter().map(|s| s.name.as_str()).collect::<Vec<_>>(), ["sec_a", "sec_b"]);
        assert_eq!(r[1][0].objects[0].path, "objs/c.o");
        assert_eq!(listed_functions(&r), [("fc".to_string(), "sec_c".to_string())]);
    }

    #[test]
    fn rejects_bad_configs() {
        assert!(parse_config("<ovis_config></ovis_config>").is_err());
        assert!(parse_config("<ovis_config><overlay><sections><section name=\"1x\"><object name=\"a.o\"/></section></sections></overlay></ovis_config>").is_err());
        assert!(parse_config("<ovis_config><overlay><sections><section name=\"s\"></section></sections></overlay></ovis_config>").is_err());
        assert!(parse_config("<ovis_config><overlay></ovis_config>").is_err());
        assert!(parse_config("<ovis_config><object name=\"a.o\"/></ovis_config>").is_err());
    }

    #[test]
    fn script_shares_vma_within_a_region_and_chains_lma() {
        let r = parse_config(XML).unwrap();
        let s = linker_script(&r, 128);
        assert!(s.contains("vma_start_sec_a = (ov_vma_start + 127) & ~127;"));
        assert!(s.contains("vma_start_sec_b = (ov_vma_start + 127) & ~127;"));
        assert!(s.contains("vma_start_sec_c = (MAX(MAX(ov_vma_start, vma_stop_sec_a), vma_stop_sec_b) + 127) & ~127;"));
        assert!(s.contains("lma_start_sec_a = (ov_lma_start + 127) & ~127;"));
        assert!(s.contains("lma_start_sec_b = (lma_stop_sec_a + 127) & ~127;"));
        assert!(s.contains("lma_start_sec_c = (lma_stop_sec_b + 127) & ~127;"));
        assert!(s.contains("INSERT BEFORE .text;"));
        assert!(s.contains(".ovis.lma_reset ov_vma_stop : AT(ov_vma_stop)"));
        assert!(s.contains("__ovly_info_sec_c = .;"));
        assert!(s.contains("LONG(3);"));
    }

    #[test]
    fn object_section_names() {
        assert_eq!(section_name_for_object("objs/bubblesort.spu.o"), "objs__bubblesort_spu_o");
        assert_eq!(section_name_for_object("x-y/q+1.o"), "x_y__q_1_o");
        assert_eq!(section_name_for_object("1.o"), "_1_o");
    }

    #[test]
    fn wrapper_and_flags() {
        let w = vec![("f".to_string(), "sec".to_string())];
        assert_eq!(ldflags(&w), "-Wl,--no-overlays -Wl,--wrap=f\n");
        assert_eq!(ldflags(&[]), "-Wl,--no-overlays\n");
        let a = wrappers(&w);
        assert!(a.contains("__wrap_f:"));
        assert!(a.contains("ila\t$3, __ovly_info_sec"));
        assert!(a.contains("br\t__real_f"));
    }
}
