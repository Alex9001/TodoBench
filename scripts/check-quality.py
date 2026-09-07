#!/usr/bin/env python3
"""Run TodoBench's blocking complexity checks."""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

GCC_ONLY_FLAGS = {"-mno-direct-extern-access"}

ROOT = Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    return parser.parse_args()


def _is_lizard_module(interpreter: str) -> bool:
    probe = subprocess.run(
        [interpreter, "-m", "lizard", "--help"],
        capture_output=True,
        text=True,
        check=False,
    )
    output = (probe.stdout + probe.stderr).lower()
    return probe.returncode == 0 and "cyclomatic" in output


def _python_candidates() -> list[str]:
    candidates = []
    configured = os.environ.get("LIZARD_PYTHON")
    if configured:
        candidates.append(configured)
    candidates.append(sys.executable)
    venv_unix = ROOT / ".venv" / "bin" / "python"
    venv_windows = ROOT / ".venv" / "Scripts" / "python.exe"
    if venv_unix.exists():
        candidates.append(str(venv_unix))
    if venv_windows.exists():
        candidates.append(str(venv_windows))
    for directory in ("/usr/bin", "/usr/local/bin", str(Path.home() / ".local/bin")):
        if not os.path.isdir(directory):
            continue
        for path in sorted(Path(directory).glob("python*")):
            if path.is_file() and os.access(path, os.X_OK) and re.fullmatch(r"python(?:[0-9]+(?:\\.[0-9]+)*)?", path.name):
                candidates.append(str(path))
    return list(dict.fromkeys(candidates))


def require_lizard() -> list[str]:
    candidates = _python_candidates()
    for interpreter in candidates:
        if _is_lizard_module(interpreter):
            return [interpreter, "-m", "lizard"]

    executable = shutil.which("lizard")
    if executable is not None:
        help_result = subprocess.run([executable, "--help"], capture_output=True, text=True, check=False)
        if "cyclomatic" in (help_result.stdout + help_result.stderr).lower():
            return [executable]

    raise RuntimeError(
        "required Python lizard module/entry point not found; install lizard in one of the Python interpreters "
        + ", ".join(candidates)
    )


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(f"required tool not found: {name}")
    return path


def run(command: list[str], *, cwd: Path = ROOT) -> subprocess.CompletedProcess[str]:
    print("$", " ".join(command))
    return subprocess.run(command, cwd=cwd, text=True, check=False)


def source_files() -> list[Path]:
    files = sorted((ROOT / "native" / "src").rglob("*.cpp"))
    files.extend(sorted((ROOT / "native" / "src").rglob("*.h")))
    files.extend(sorted((ROOT / "native" / "tests").glob("test_*.cpp")))
    files.append(ROOT / "scripts" / "check-quality.py")
    files = [path for path in files if path.exists()]
    if not files:
        raise RuntimeError("source discovery returned no handwritten application functions")
    return files


def verify_lizard_boundary(lizard: list[str]) -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "boundary.cpp"
        path.write_text(
            "int at_limit(int value) {\n"
            + "".join(f"    if (value == {index}) return {index};\n" for index in range(14))
            + "    return -1;\n}\n",
            encoding="utf-8",
        )
        passed = run([*lizard, "-l", "cpp", "-C", "15", "-i", "0", "-w", str(path)]).returncode == 0
        if not passed:
            raise RuntimeError("cyclomatic boundary fixture unexpectedly failed")


def verify_lizard_rejection(lizard: list[str]) -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "failure.cpp"
        path.write_text(
            "int above_limit(int value) {\n"
            + "".join(f"    if (value == {index}) return {index};\n" for index in range(15))
            + "    return -1;\n}\n",
            encoding="utf-8",
        )
        rejected = run([*lizard, "-l", "cpp", "-C", "15", "-i", "0", "-w", str(path)]).returncode != 0
        if not rejected:
            raise RuntimeError("cyclomatic rejection fixture unexpectedly passed")


def verify_cognitive_rejection(clang_tidy: str) -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "cognitive_failure.cpp"
        body = "int above_cognitive_limit(int value) {\n"
        for index in range(8):
            body += "    " * (index + 1) + f"if (value > {index}) {{\n"
        body += "    " * 9 + "return value;\n"
        body += "    " * 8 + "}\n" * 8 + "    return -1;\n}\n"
        path.write_text(body, encoding="utf-8")
        result = run([
            clang_tidy,
            "--checks=-*,readability-function-cognitive-complexity",
            "--warnings-as-errors=readability-function-cognitive-complexity",
            str(path),
            "--",
            "-std=c++20",
        ])
        if result.returncode == 0:
            raise RuntimeError("cognitive rejection fixture unexpectedly passed")


def _without_gcc_only_flags(parts: list[str]) -> list[str]:
    return [part for part in parts if part not in GCC_ONLY_FLAGS]


def filtered_compile_database(build_dir: Path, destination: Path) -> None:
    compile_db = build_dir / "compile_commands.json"
    if not compile_db.exists():
        raise RuntimeError(f"compilation database not found: {compile_db}")
    entries = json.loads(compile_db.read_text(encoding="utf-8"))
    filtered = []
    for entry in entries:
        item = dict(entry)
        if "command" in item:
            item["command"] = shlex.join(_without_gcc_only_flags(shlex.split(item["command"])))
        if "arguments" in item:
            item["arguments"] = _without_gcc_only_flags(list(item["arguments"]))
        filtered.append(item)
    destination.write_text(json.dumps(filtered), encoding="utf-8")


def run_clang_tidy(clang_tidy: str, build_dir: Path, files: list[Path]) -> None:
    with tempfile.TemporaryDirectory() as directory:
        compile_dir = Path(directory)
        filtered_compile_database(build_dir, compile_dir / "compile_commands.json")
        command = [clang_tidy, "-p", str(compile_dir), "--config-file", str(ROOT / ".clang-tidy")]
        command.extend(str(path.relative_to(ROOT)) for path in files if path.suffix == ".cpp")
        result = run(command)
        if result.returncode != 0:
            raise RuntimeError("clang-tidy cognitive complexity gate failed")


def main() -> int:
    try:
        args = parse_args()
        lizard = require_lizard()
        clang_tidy = require_tool("clang-tidy")
        files = source_files()
        print(f"Discovered {len(files)} handwritten source/script files")
        lizard_result = run([
            *lizard,
            "--no-gitignore",
            "-l",
            "cpp",
            "-l",
            "python",
            "-C",
            "15",
            "-i",
            "0",
            "-w",
            "native/src",
            "native/tests",
            "scripts",
        ])
        if lizard_result.returncode != 0:
            raise RuntimeError("Lizard cyclomatic complexity gate failed")
        verify_lizard_boundary(lizard)
        verify_lizard_rejection(lizard)
        verify_cognitive_rejection(clang_tidy)
        run_clang_tidy(clang_tidy, args.build_dir, files)
    except RuntimeError as error:
        print(f"quality gate: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
