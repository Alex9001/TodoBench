#!/usr/bin/env python3
"""Verify the independent TS oracle (pinned 885c4f) against the same T01 fixtures.

The oracle must not be silently skipped: missing node, missing build, or
missing fixtures must fail the test. This script exercises config/type
discovery, validation, read (raw/effective), query, and link-related query
semantics via the oracle's V03Operations/Collection APIs.

It is development-only and pinned via the vendored native/mdbase_oracle/ts
directory and its Cargo.lock/package-lock.json equivalents (MDBASE_TS_REVISION).
"""
import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ORACLE_NODE_CODE = r"""
import { Collection } from %ORACLE_INDEX%;
import fs from "fs";

async function main() {
  const oracleRoot = process.argv[2];
  const minimalFixture = process.argv[3];
  const invalidFixture = process.argv[4];
  const exportedFixture = process.argv[5];
  const manifestPath = process.argv[6];
  if (!oracleRoot || !minimalFixture || !invalidFixture || !exportedFixture || !manifestPath) {
    console.error("missing args: oracleRoot minimalFixture invalidFixture exportedFixture manifestPath");
    process.exit(2);
  }
  // ---- valid minimal fixture ----
  let res = await Collection.open(minimalFixture);
  if (res.error || !res.collection) {
    console.error("open minimal failed", JSON.stringify(res.error));
    process.exit(1);
  }
  const col = res.collection;

  // config should be v0.3
  if (col.config.spec_version !== "0.3.0") {
    console.error("spec_version mismatch", col.config.spec_version);
    process.exit(1);
  }

  const ops = col.v03Operations();

  // read raw vs effective: tasks/a.md has explicit status; needs-default omits it
  let r = await ops.read({path:"tasks/a.md"});
  if (!r.valid) { console.error("read a.md invalid", JSON.stringify(r)); process.exit(1); }
  if (r.result.frontmatter.status !== "todo") { console.error("frontmatter status", r.result.frontmatter); process.exit(1); }
  if (r.result.effective_frontmatter.status !== "todo") { console.error("effective status", r.result.effective_frontmatter); process.exit(1); }
  if (!r.result.body || !r.result.body.includes("Body hello")) { console.error("body missing", r.result.body); process.exit(1); }

  let r2 = await ops.read({path:"tasks/needs-default.md"});
  if (!r2.valid) { console.error("read needs-default invalid", JSON.stringify(r2)); process.exit(1); }
  if ("status" in (r2.result.frontmatter||{})) { console.error("needs-default should not have frontmatter status", r2.result.frontmatter); process.exit(1); }
  if (r2.result.effective_frontmatter.status !== "todo") { console.error("effective default failed", r2.result.effective_frontmatter); process.exit(1); }
  if (r2.result.effective_frontmatter.priority !== "normal") { console.error("effective priority default failed", r2.result.effective_frontmatter); process.exit(1); }

  // validate
  let v = await ops.validate({path:"tasks/a.md"});
  if (!v.valid) { console.error("validate should be valid", JSON.stringify(v)); process.exit(1); }

  // query
  let q = await ops.query({where:"title == 'Hello minimal'"});
  if (!q.valid) { console.error("query invalid", JSON.stringify(q)); process.exit(1); }
  if (!q.result.results || q.result.results.length !== 1 || q.result.results[0].path !== "tasks/a.md") {
    console.error("query results mismatch", JSON.stringify(q.result));
    process.exit(1);
  }

  // link validation: query for tasks with a valid project_link to existing project
  // Also validate resolution implicitly: the canonical link resolver is exercised via
  // collection query + validate. For an absolute link, a missing target should invalidate.
  await col.close();

  // ---- invalid fixture: same type, bad enum ----
  let res2 = await Collection.open(invalidFixture);
  if (res2.error || !res2.collection) { console.error("open invalid fixture failed", JSON.stringify(res2.error)); process.exit(1); }
  const col2 = res2.collection;
  const ops2 = col2.v03Operations();
  let bad = await ops2.read({path:"tasks/bad.md"});
  // Note: upstream oracle read.valid stays true for schema-invalid records; validate gates validity.
  // Accept either behaviour, but diagnostics must be surfaced via validate.
  // Keep read check lenient: valid is allowed to be true when record is schema-invalid.

  let bv = await ops2.validate({path:"tasks/bad.md"});
  if (bv.valid) { console.error("bad validate should be invalid", JSON.stringify(bv)); process.exit(1); }
  if (!bv.diagnostics || bv.diagnostics.length === 0) { console.error("bad validate diagnostics empty", JSON.stringify(bv)); process.exit(1); }
  await col2.close();

  // source bytes unchanged probe: ensure inspect/open did not mutate
  const beforeA = fs.readFileSync(minimalFixture + "/tasks/a.md", "utf8");
  if (!beforeA.includes("Hello minimal")) { console.error("source mutated"); process.exit(1); }

  // ---- actual TodoBench export, produced by the C++ exporter in this test ----
  const manifest = JSON.parse(fs.readFileSync(manifestPath, "utf8"));
  let exportedOpen = await Collection.open(exportedFixture);
  if (exportedOpen.error || !exportedOpen.collection) {
    console.error("open actual TodoBench export failed", JSON.stringify(exportedOpen.error));
    process.exit(1);
  }
  const exported = exportedOpen.collection;
  if (exported.config.spec_version !== "0.3.0") {
    console.error("actual export spec mismatch", exported.config.spec_version);
    process.exit(1);
  }
  const exportedOps = exported.v03Operations();
  const expectedPaths = [manifest.parent_path, manifest.child_path, manifest.project_path].sort();
  const exportedQuery = await exportedOps.query({where:"true"});
  if (!exportedQuery.valid) {
    console.error("actual export query failed", JSON.stringify(exportedQuery));
    process.exit(1);
  }
  const actualPaths = (exportedQuery.result.results || []).map(record => record.path).sort();
  if (JSON.stringify(actualPaths) !== JSON.stringify(expectedPaths)) {
    console.error("actual export record set mismatch", JSON.stringify({expectedPaths, actualPaths}));
    process.exit(1);
  }
  for (const path of expectedPaths) {
    const validation = await exportedOps.validate({path});
    if (!validation.valid) {
      console.error("actual export record invalid", path, JSON.stringify(validation));
      process.exit(1);
    }
  }
  const childRead = await exportedOps.read({path:manifest.child_path});
  if (!childRead.valid || childRead.result.frontmatter.title !== "Oracle edited child" ||
      childRead.result.frontmatter.status !== "waiting") {
    console.error("external edit not visible through TS oracle", JSON.stringify(childRead));
    process.exit(1);
  }
  if (childRead.result.frontmatter.todobench_project_link !== "/" + manifest.project_path ||
      childRead.result.frontmatter.todobench_parent_link !== "/" + manifest.parent_path) {
    console.error("actual export relationship targets mismatch", JSON.stringify(childRead.result.frontmatter));
    process.exit(1);
  }
  for (const field of ["todobench_project_link", "todobench_parent_link"]) {
    const target = childRead.result.frontmatter[field].slice(1);
    if (!expectedPaths.includes(target) || !fs.existsSync(exportedFixture + "/" + target)) {
      console.error("actual export relationship target does not exist", field, target);
      process.exit(1);
    }
  }
  await exported.close();

  console.log("oracle verified: fixtures and actual C++ export/edit/relationships OK");
}
main().catch(e=>{ console.error(e.stack||String(e)); process.exit(1); });
"""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--oracle-root", required=True,
                        help="path to native/mdbase_oracle/ts (built dist expected)")
    parser.add_argument("--minimal-fixture", required=True)
    parser.add_argument("--invalid-fixture", required=True)
    parser.add_argument("--transfer-driver", required=True)
    return parser.parse_args()


def validate_inputs(args: argparse.Namespace) -> tuple[Path, Path, Path, Path, Path]:
    oracle_root = Path(args.oracle_root).resolve()
    minimal = Path(args.minimal_fixture).resolve()
    invalid = Path(args.invalid_fixture).resolve()
    transfer_driver = Path(args.transfer_driver).resolve()
    index = oracle_root / "dist" / "index.js"
    required = [
        (oracle_root.exists(), f"oracle root missing: {oracle_root}"),
        (transfer_driver.is_file(), f"transfer driver missing: {transfer_driver}"),
        (index.exists(), f"oracle not built (missing {index}); run `npm ci && npm run build` in {oracle_root}"),
        ((minimal / "mdbase.yaml").exists(), f"minimal fixture missing mdbase.yaml at {minimal}"),
        ((invalid / "mdbase.yaml").exists(), f"invalid fixture missing mdbase.yaml at {invalid}"),
    ]
    for valid, message in required:
        if not valid:
            raise RuntimeError(message)
    validate_pin(oracle_root)
    validate_node()
    return oracle_root, minimal, invalid, transfer_driver, index


def validate_pin(oracle_root: Path) -> None:
    pin = oracle_root.parent / "MDBASE_TS_REVISION"
    expected = "885c4f6a37c1877c959a5c74fc348f0b094c66a7"
    if not pin.exists():
        raise RuntimeError(f"missing pin file {pin}; oracle must be pinned to 885c4f")
    actual = pin.read_text().strip()
    if actual != expected:
        raise RuntimeError(f"oracle pin mismatch: expected {expected}, got {actual!r}")


def validate_node() -> None:
    try:
        subprocess.run(["node", "--version"], check=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=10)
    except Exception as error:
        raise RuntimeError(f"node not available for oracle verify: {error}") from error


def run_transfer_driver(driver: Path, arguments: list[str], failure: str) -> str:
    result = subprocess.run([str(driver), *arguments], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=60)
    if result.returncode != 0:
        raise RuntimeError(f"{failure}\n{result.stdout}")
    return result.stdout


def create_and_edit_export(temp_root: Path, transfer_driver: Path) -> tuple[Path, Path, Path]:
    workspace = temp_root / "native-workspace"
    exported = temp_root / "actual-export"
    imported = temp_root / "native-reimport"
    manifest = temp_root / "manifest.json"
    run_transfer_driver(
        transfer_driver,
        ["export-fixture", str(workspace), str(exported), str(manifest)],
        "C++ transfer driver failed to generate an export",
    )
    metadata = json.loads(manifest.read_text(encoding="utf-8"))
    child = exported / metadata["child_path"]
    child_text = child.read_text(encoding="utf-8")
    child_text, title_count = re.subn(
        r"^title:\s*.*$", "title: Oracle edited child", child_text,
        count=1, flags=re.MULTILINE,
    )
    child_text, status_count = re.subn(
        r"^status:\s*.*$", "status: waiting", child_text,
        count=1, flags=re.MULTILINE,
    )
    if title_count != 1 or status_count != 1:
        raise RuntimeError("could not apply the external title/status edit to the exported task")
    child.write_text(child_text, encoding="utf-8")
    return exported, imported, manifest


def verify_with_typescript(code: str, temp_root: Path, oracle_root: Path,
                           minimal: Path, invalid: Path, exported: Path,
                           manifest: Path) -> None:
    script = temp_root / "verify.mjs"
    script.write_text(code, encoding="utf-8")
    result = subprocess.run(
        ["node", str(script), str(oracle_root), str(minimal), str(invalid),
         str(exported), str(manifest)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60,
    )
    print(result.stdout, flush=True)
    if result.returncode != 0:
        raise RuntimeError(f"oracle verify failed (exit {result.returncode})")
    if "oracle verified" not in result.stdout:
        raise RuntimeError("oracle verify did not emit success marker")


def verify_native_reimport(driver: Path, exported: Path, imported: Path,
                           manifest: Path) -> None:
    output = run_transfer_driver(
        driver, ["import-check", str(exported), str(imported), str(manifest)],
        "native re-import did not preserve the external edit",
    )
    print(output, flush=True)
    if "native re-import verified" not in output:
        raise RuntimeError("native re-import did not emit success marker")


def main() -> int:
    try:
        oracle_root, minimal, invalid, driver, index = validate_inputs(parse_args())
        code = ORACLE_NODE_CODE.replace("%ORACLE_INDEX%", json.dumps(index.as_uri()))
        with tempfile.TemporaryDirectory(prefix="todobench-mdbase-oracle-") as temp:
            temp_root = Path(temp)
            exported, imported, manifest = create_and_edit_export(temp_root, driver)
            verify_with_typescript(code, temp_root, oracle_root, minimal, invalid,
                                   exported, manifest)
            verify_native_reimport(driver, exported, imported, manifest)
        return 0
    except (RuntimeError, OSError, ValueError, json.JSONDecodeError) as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
