// SPDX-License-Identifier: MIT
//! Minimal C ABI over vendored mdbase-rs 0.4.0-rc.4 (pinned ca71aeb).
//! Only the operations needed for plan T01-T10 are exposed. All panics are
//! caught at the boundary. Caller must free every Rust-allocated buffer with
//! `todobench_mdbase_free`.

#![allow(clippy::not_unsafe_ptr_arg_deref)]

use std::os::raw::c_char;
use std::panic::AssertUnwindSafe;
use std::path::Path;
use std::ptr;
use std::slice;

use mdbase::{Collection, SpecProfile};
use serde_json::{json, Value};

// Opaque handle so C++ owns exactly one thread-affine Collection.
pub struct MdbaseCollection {
    inner: Collection,
}

fn bytes_to_path(bytes: &[u8]) -> Result<&Path, String> {
    let s = std::str::from_utf8(bytes).map_err(|e| format!("path is not UTF-8: {e}"))?;
    Ok(Path::new(s))
}

fn alloc_return(text: &str) -> (*mut c_char, usize) {
    let bytes = text.as_bytes();
    let len = bytes.len();
    if len == 0 {
        return (ptr::null_mut(), 0);
    }
    let mut buf = Vec::with_capacity(len);
    buf.extend_from_slice(bytes);
    let ptr = buf.as_mut_ptr() as *mut c_char;
    std::mem::forget(buf);
    (ptr, len)
}

fn error_json(code: &str, message: String) -> String {
    json!({
        "valid": false,
        "result": {},
        "diagnostics": [{
            "severity": "error",
            "code": code,
            "message": message
        }]
    })
    .to_string()
}

fn finish_ffi_result(
    out_json: *mut *mut c_char,
    out_len: *mut usize,
    result: Result<(i32, String), Box<dyn std::any::Any + Send>>,
    panic_message: &str,
) -> i32 {
    if out_json.is_null() || out_len.is_null() {
        return 1;
    }
    let (code, text) = match result {
        Ok(value) => value,
        Err(_) => (2, error_json("panic", panic_message.into())),
    };
    let (buffer, length) = alloc_return(&text);
    unsafe {
        *out_json = buffer;
        *out_len = length;
    }
    code
}

// ---- C ABI ----

#[no_mangle]
pub extern "C" fn todobench_mdbase_version() -> *const c_char {
    concat!(env!("CARGO_PKG_VERSION"), "\0").as_ptr() as *const c_char
}

#[no_mangle]
pub extern "C" fn todobench_mdbase_free(ptr: *mut c_char, len: usize) {
    if ptr.is_null() || len == 0 {
        return;
    }
    unsafe {
        let _ = Vec::from_raw_parts(ptr as *mut u8, len, len);
    }
}

#[no_mangle]
pub extern "C" fn todobench_mdbase_collection_open(
    root_ptr: *const c_char,
    root_len: usize,
    out_handle: *mut *mut MdbaseCollection,
    out_json: *mut *mut c_char,
    out_len: *mut usize,
) -> i32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if root_ptr.is_null() || out_handle.is_null() || out_json.is_null() || out_len.is_null() {
            return (1, error_json("invalid_request", "null pointer".into()));
        }
        unsafe {
            *out_handle = ptr::null_mut();
        }
        let bytes = unsafe { slice::from_raw_parts(root_ptr as *const u8, root_len) };
        let path = match bytes_to_path(bytes) {
            Ok(p) => p,
            Err(e) => return (1, error_json("invalid_request", e)),
        };
        match Collection::open(path) {
            Ok(col) => {
                let boxed = Box::new(MdbaseCollection { inner: col });
                unsafe {
                    *out_handle = Box::into_raw(boxed);
                }
                let spec = if unsafe { &**out_handle }.inner.spec_profile() == SpecProfile::V03 {
                    "v0.3"
                } else {
                    "v0.2"
                };
                let resp =
                    json!({"valid": true, "result": {"spec_profile": spec}, "diagnostics": []})
                        .to_string();
                (0, resp)
            }
            Err(value) => {
                let text = value.to_string();
                (1, text)
            }
        }
    }));
    finish_ffi_result(out_json, out_len, result, "collection open panicked")
}

#[no_mangle]
pub extern "C" fn todobench_mdbase_collection_close(handle: *mut MdbaseCollection) {
    if handle.is_null() {
        return;
    }
    let _ = std::panic::catch_unwind(AssertUnwindSafe(|| unsafe {
        let _ = Box::from_raw(handle);
    }));
}

fn with_collection<F>(handle: *mut MdbaseCollection, f: F) -> (i32, String)
where
    F: FnOnce(&Collection) -> (i32, String),
{
    if handle.is_null() {
        return (
            1,
            error_json("invalid_request", "null collection handle".into()),
        );
    }
    let col = unsafe { &(*handle).inner };
    f(col)
}

fn parse_input_bytes(ptr: *const c_char, len: usize) -> Result<Value, String> {
    if len == 0 {
        return Ok(json!({}));
    }
    if ptr.is_null() {
        return Err("input pointer is null for non-empty input".into());
    }
    let bytes = unsafe { slice::from_raw_parts(ptr as *const u8, len) };
    let s = std::str::from_utf8(bytes).map_err(|e| format!("input is not UTF-8: {e}"))?;
    serde_json::from_str(s).map_err(|e| format!("input is not valid JSON: {e}"))
}

fn op_result_to_json(
    valid: bool,
    result: Value,
    diagnostics: Vec<mdbase::v03::Diagnostic>,
) -> String {
    let diags = diagnostics
        .into_iter()
        .map(|d| {
            let mut v = json!({
                "severity": d.severity,
                "code": d.code,
                "message": d.message,
            });
            if let Some(p) = d.path {
                v["path"] = json!(p);
            }
            if let Some(f) = d.field {
                v["field"] = json!(f);
            }
            if let Some(t) = d.type_name {
                v["type"] = json!(t);
            }
            if let Some(s) = d.schema_location {
                v["schema_location"] = json!(s);
            }
            if let Some(details) = d.details {
                v["details"] = details;
            }
            v
        })
        .collect::<Vec<_>>();
    json!({"valid": valid, "result": result, "diagnostics": diags}).to_string()
}

#[no_mangle]
pub extern "C" fn todobench_mdbase_inspect(
    handle: *mut MdbaseCollection,
    out_json: *mut *mut c_char,
    out_len: *mut usize,
) -> i32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if out_json.is_null() || out_len.is_null() {
            return (1, error_json("invalid_request", "null out pointer".into()));
        }
        with_collection(handle, |col| {
            let report = mdbase::v03::inspect_collection(col.root());
            let j = json!({
                "valid": report.valid,
                "config": report.config,
                "types": report.types.iter().map(|t| json!({
                    "path": t.path,
                    "name": t.name,
                    "version": t.version,
                    "frontmatter": t.frontmatter,
                    "schema": t.schema,
                })).collect::<Vec<_>>(),
                "diagnostics": report.diagnostics.iter().map(|d| json!({
                    "severity": d.severity,
                    "code": d.code,
                    "message": d.message,
                    "path": d.path,
                    "field": d.field,
                    "type": d.type_name,
                    "schema_location": d.schema_location,
                    "details": d.details,
                })).collect::<Vec<_>>(),
            });
            let valid = report.valid;
            (
                i32::from(!valid),
                json!({"valid": valid, "result": j, "diagnostics": report.diagnostics.iter().map(|d| json!({"severity": d.severity, "code": d.code, "message": d.message, "path": d.path, "field": d.field, "type": d.type_name})).collect::<Vec<_>>()}).to_string(),
            )
        })
    }));
    finish_ffi_result(out_json, out_len, result, "inspect panicked")
}

#[no_mangle]
pub extern "C" fn todobench_mdbase_validate(
    handle: *mut MdbaseCollection,
    input_ptr: *const c_char,
    input_len: usize,
    out_json: *mut *mut c_char,
    out_len: *mut usize,
) -> i32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if out_json.is_null() || out_len.is_null() {
            return (1, error_json("invalid_request", "null out pointer".into()));
        }
        let input = match parse_input_bytes(input_ptr, input_len) {
            Ok(v) => v,
            Err(e) => return (1, error_json("invalid_request", e)),
        };
        with_collection(handle, |col| {
            let ops = match col.v03_operations() {
                Ok(o) => o,
                Err(d) => {
                    return (1, op_result_to_json(false, json!({}), vec![*d]));
                }
            };
            let r = ops.validate(&input);
            (
                i32::from(!r.valid),
                op_result_to_json(r.valid, r.result, r.diagnostics),
            )
        })
    }));
    finish_ffi_result(out_json, out_len, result, "validate panicked")
}

#[no_mangle]
pub extern "C" fn todobench_mdbase_read(
    handle: *mut MdbaseCollection,
    input_ptr: *const c_char,
    input_len: usize,
    out_json: *mut *mut c_char,
    out_len: *mut usize,
) -> i32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if out_json.is_null() || out_len.is_null() {
            return (1, error_json("invalid_request", "null out pointer".into()));
        }
        let input = match parse_input_bytes(input_ptr, input_len) {
            Ok(v) => v,
            Err(e) => return (1, error_json("invalid_request", e)),
        };
        with_collection(handle, |col| {
            let ops = match col.v03_operations() {
                Ok(o) => o,
                Err(d) => return (1, op_result_to_json(false, json!({}), vec![*d])),
            };
            let r = ops.read(&input);
            (
                i32::from(!r.valid),
                op_result_to_json(r.valid, r.result, r.diagnostics),
            )
        })
    }));
    finish_ffi_result(out_json, out_len, result, "read panicked")
}

#[no_mangle]
pub extern "C" fn todobench_mdbase_query(
    handle: *mut MdbaseCollection,
    input_ptr: *const c_char,
    input_len: usize,
    out_json: *mut *mut c_char,
    out_len: *mut usize,
) -> i32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if out_json.is_null() || out_len.is_null() {
            return (1, error_json("invalid_request", "null out pointer".into()));
        }
        let input = match parse_input_bytes(input_ptr, input_len) {
            Ok(v) => v,
            Err(e) => return (1, error_json("invalid_request", e)),
        };
        with_collection(handle, |col| {
            let ops = match col.v03_operations() {
                Ok(o) => o,
                Err(d) => return (1, op_result_to_json(false, json!({}), vec![*d])),
            };
            let r = ops.query(&input);
            (
                i32::from(!r.valid),
                op_result_to_json(r.valid, r.result, r.diagnostics),
            )
        })
    }));
    finish_ffi_result(out_json, out_len, result, "query panicked")
}

#[no_mangle]
pub extern "C" fn todobench_mdbase_get_types(
    handle: *mut MdbaseCollection,
    input_ptr: *const c_char,
    input_len: usize,
    out_json: *mut *mut c_char,
    out_len: *mut usize,
) -> i32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if out_json.is_null() || out_len.is_null() {
            return (1, error_json("invalid_request", "null out pointer".into()));
        }
        let input = match parse_input_bytes(input_ptr, input_len) {
            Ok(v) => v,
            Err(e) => return (1, error_json("invalid_request", e)),
        };
        with_collection(handle, |col| {
            let ops = match col.v03_operations() {
                Ok(o) => o,
                Err(d) => return (1, op_result_to_json(false, json!({}), vec![*d])),
            };
            let r = ops.get_types(&input);
            (
                i32::from(!r.valid),
                op_result_to_json(r.valid, r.result, r.diagnostics),
            )
        })
    }));
    finish_ffi_result(out_json, out_len, result, "get_types panicked")
}

#[no_mangle]
pub extern "C" fn todobench_mdbase_list_types(
    handle: *mut MdbaseCollection,
    out_json: *mut *mut c_char,
    out_len: *mut usize,
) -> i32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if out_json.is_null() || out_len.is_null() {
            return (1, error_json("invalid_request", "null out pointer".into()));
        }
        with_collection(handle, |col| {
            let ops = match col.v03_operations() {
                Ok(o) => o,
                Err(d) => return (1, op_result_to_json(false, json!({}), vec![*d])),
            };
            let r = ops.list_types(&json!({}));
            (
                i32::from(!r.valid),
                op_result_to_json(r.valid, r.result, r.diagnostics),
            )
        })
    }));
    finish_ffi_result(out_json, out_len, result, "list_types panicked")
}

#[allow(clippy::manual_pattern_char_comparison)]
#[no_mangle]
pub extern "C" fn todobench_mdbase_resolve_link(
    handle: *mut MdbaseCollection,
    input_ptr: *const c_char,
    input_len: usize,
    out_json: *mut *mut c_char,
    out_len: *mut usize,
) -> i32 {
    let result = std::panic::catch_unwind(AssertUnwindSafe(|| {
        if out_json.is_null() || out_len.is_null() {
            return (1, error_json("invalid_request", "null out pointer".into()));
        }
        let input = match parse_input_bytes(input_ptr, input_len) {
            Ok(v) => v,
            Err(e) => return (1, error_json("invalid_request", e)),
        };
        let path = input.get("path").and_then(|v| v.as_str()).unwrap_or("");
        let link = input.get("link").and_then(|v| v.as_str()).unwrap_or("");
        if path.is_empty() || link.is_empty() {
            return (
                1,
                error_json("invalid_request", "path and link are required".into()),
            );
        }
        with_collection(handle, |col| {
            let ops = match col.v03_operations() {
                Ok(o) => o,
                Err(d) => return (1, op_result_to_json(false, json!({}), vec![*d])),
            };
            let read = ops.read(&json!({"path": path}));
            if !read.valid {
                return (1, op_result_to_json(false, json!({}), read.diagnostics));
            }
            let link_is_absolute = link.starts_with('/');
            let candidate = if link_is_absolute {
                link.trim_start_matches('/').to_string()
            } else if link.contains("[[") {
                let inner = link
                    .trim_start_matches("[[")
                    .split(|c| c == '|' || c == ']')
                    .next()
                    .unwrap_or("")
                    .trim();
                if inner.contains('/') || inner.contains('.') {
                    inner.trim_start_matches("./").to_string()
                } else {
                    let q = ops.query(
                        &json!({"where": format!("id == '{}'", inner.replace('\'', "\\'"))}),
                    );
                    if q.valid {
                        if let Some(arr) = q.result.get("results").and_then(|v| v.as_array()) {
                            if arr.len() == 1 {
                                if let Some(p) = arr[0].get("path").and_then(|v| v.as_str()) {
                                    p.to_string()
                                } else {
                                    inner.to_string()
                                }
                            } else {
                                inner.to_string()
                            }
                        } else {
                            inner.to_string()
                        }
                    } else {
                        inner.to_string()
                    }
                }
            } else if link.starts_with('[') {
                if let Some(start) = link.find('(') {
                    if let Some(end) = link.rfind(')') {
                        link[start + 1..end]
                            .split('#')
                            .next()
                            .unwrap_or("")
                            .trim()
                            .to_string()
                    } else {
                        String::new()
                    }
                } else {
                    String::new()
                }
            } else {
                link.to_string()
            };
            let target_is_external = candidate.starts_with("http://")
                || candidate.starts_with("https://")
                || candidate.starts_with("data:");
            let resolved = if target_is_external {
                Value::Null
            } else if link_is_absolute {
                // Absolute collection-root link: resolve directly from collection root
                let check = ops.read(&json!({"path": candidate}));
                if check.valid {
                    json!(candidate)
                } else {
                    Value::Null
                }
            } else {
                let base = Path::new(path).parent().unwrap_or(Path::new(""));
                let rel = base.join(&candidate);
                let rel_str = rel.to_string_lossy().replace('\\', "/");
                let check = ops.read(&json!({"path": rel_str}));
                if check.valid {
                    json!(rel_str)
                } else {
                    Value::Null
                }
            };
            let result = json!({
                "path": path,
                "link": link,
                "candidate": candidate,
                "resolved": resolved,
                "external": target_is_external,
            });
            (0, op_result_to_json(true, result, vec![]))
        })
    }));
    finish_ffi_result(out_json, out_len, result, "resolve_link panicked")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn version_non_empty() {
        assert!(!mdbase::VERSION.is_empty());
    }

    #[test]
    fn null_output_pointers_are_rejected_without_dereference() {
        assert_eq!(
            todobench_mdbase_inspect(ptr::null_mut(), ptr::null_mut(), ptr::null_mut()),
            1
        );
        assert_eq!(
            todobench_mdbase_collection_open(
                ptr::null(),
                0,
                ptr::null_mut(),
                ptr::null_mut(),
                ptr::null_mut()
            ),
            1
        );
    }

    #[test]
    fn null_non_empty_input_returns_owned_error_json() {
        let mut output = ptr::null_mut();
        let mut length = 0;
        let code =
            todobench_mdbase_validate(ptr::null_mut(), ptr::null(), 1, &mut output, &mut length);
        assert_eq!(code, 1);
        assert!(!output.is_null());
        let bytes = unsafe { slice::from_raw_parts(output as *const u8, length) };
        let value: Value = serde_json::from_slice(bytes).expect("valid error JSON");
        assert_eq!(value["diagnostics"][0]["code"], "invalid_request");
        todobench_mdbase_free(output, length);
    }
}
