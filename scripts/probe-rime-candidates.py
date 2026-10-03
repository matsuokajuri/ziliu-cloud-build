#!/usr/bin/env python3
"""Collect unselected candidate lists from the packaged, real librime runtime."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import re
from datetime import datetime, timezone
from pathlib import Path
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PAYLOAD = ROOT / "build/release/public-alpha-20260923/new-package/payload"
DEFAULT_DLL = DEFAULT_PAYLOAD / "rime.dll"
DEFAULT_INPUT = ROOT / "tests/data/contextual-ranking-pilot.json"
LIMIT = 200
SHARED_INPUTS = (
    "default.yaml",
    "default.custom.yaml",
    "rime_ice.schema.yaml",
    "rime_ice.custom.yaml",
    "rime_ice.dict.yaml",
    "ziliu_phrase.txt",
    "ziliu_private.schema.yaml",
)


class Traits(ctypes.Structure):
    _fields_ = [
        ("data_size", ctypes.c_int),
        ("shared_data_dir", ctypes.c_char_p),
        ("user_data_dir", ctypes.c_char_p),
        ("distribution_name", ctypes.c_char_p),
        ("distribution_code_name", ctypes.c_char_p),
        ("distribution_version", ctypes.c_char_p),
        ("app_name", ctypes.c_char_p),
        ("modules", ctypes.POINTER(ctypes.c_char_p)),
        ("min_log_level", ctypes.c_int),
        ("log_dir", ctypes.c_char_p),
        ("prebuilt_data_dir", ctypes.c_char_p),
        ("staging_dir", ctypes.c_char_p),
    ]


class Composition(ctypes.Structure):
    _fields_ = [
        ("length", ctypes.c_int),
        ("cursor_pos", ctypes.c_int),
        ("sel_start", ctypes.c_int),
        ("sel_end", ctypes.c_int),
        ("preedit", ctypes.c_char_p),
    ]


class Candidate(ctypes.Structure):
    _fields_ = [
        ("text", ctypes.c_char_p),
        ("comment", ctypes.c_char_p),
        ("reserved", ctypes.c_void_p),
    ]


class Menu(ctypes.Structure):
    _fields_ = [
        ("page_size", ctypes.c_int),
        ("page_no", ctypes.c_int),
        ("is_last_page", ctypes.c_int),
        ("highlighted_candidate_index", ctypes.c_int),
        ("num_candidates", ctypes.c_int),
        ("candidates", ctypes.POINTER(Candidate)),
        ("select_keys", ctypes.c_char_p),
    ]


class Context(ctypes.Structure):
    _fields_ = [
        ("data_size", ctypes.c_int),
        ("composition", Composition),
        ("menu", Menu),
        ("commit_text_preview", ctypes.c_char_p),
        ("select_labels", ctypes.POINTER(ctypes.c_char_p)),
    ]


class CandidateIterator(ctypes.Structure):
    _fields_ = [
        ("ptr", ctypes.c_void_p),
        ("index", ctypes.c_int),
        ("candidate", Candidate),
    ]


class Status(ctypes.Structure):
    _fields_ = [
        ("data_size", ctypes.c_int),
        ("schema_id", ctypes.c_char_p),
        ("schema_name", ctypes.c_char_p),
        ("is_disabled", ctypes.c_int),
        ("is_composing", ctypes.c_int),
        ("is_ascii_mode", ctypes.c_int),
        ("is_full_shape", ctypes.c_int),
        ("is_simplified", ctypes.c_int),
        ("is_traditional", ctypes.c_int),
        ("is_ascii_punct", ctypes.c_int),
    ]


class ApiHeader(ctypes.Structure):
    # RimeApi starts with a 32-bit data_size and then pointer-aligned function
    # slots. get_input is the 70th function pointer in the librime 1.x ABI.
    _fields_ = [("data_size", ctypes.c_int), ("function_slots", ctypes.c_void_p * 70)]


API_GET_INPUT_INDEX = 69


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def rime_data_size(structure: type[ctypes.Structure]) -> int:
    return ctypes.sizeof(structure) - ctypes.sizeof(ctypes.c_int)


def shared_input_hashes(shared_data: Path) -> dict[str, str]:
    return {name: sha256(shared_data / name) for name in SHARED_INPUTS if (shared_data / name).is_file()}


def learning_settings(effective_schema: Path) -> dict[str, Any]:
    text = effective_schema.read_text(encoding="utf-8")
    translator = re.search(r"(?ms)^translator:\s*\n((?:^[ \t].*\n|^\s*\n)*)", text)
    body = translator.group(1) if translator else ""
    user_dict = re.search(r"(?m)^  user_dict:\s*[\"']?(\w+)[\"']?\s*$", body)
    learning = re.search(r"(?m)^  enable_user_dict:\s*true\s*$", body)
    core_length = re.search(r"(?m)^  core_word_length:\s*(\d+)\s*$", body)
    max_length = re.search(r"(?m)^  max_word_length:\s*(\d+)\s*$", body)
    return {
        "schema_path": str(effective_schema),
        "schema_sha256": sha256(effective_schema),
        "user_dict": user_dict.group(1) if user_dict else None,
        "enable_user_dict": bool(learning),
        "core_word_length": int(core_length.group(1)) if core_length else None,
        "max_word_length": int(max_length.group(1)) if max_length else None,
        "normal_learning_enabled": bool(learning and user_dict and user_dict.group(1) == "rime_ice"),
    }


def validate_run_dir(run_dir: Path, artifact_root: Path, build_root: Path) -> Path:
    artifact_root = artifact_root.resolve()
    run_dir = run_dir.resolve()
    if not run_dir.is_relative_to(artifact_root) or run_dir == artifact_root:
        raise ValueError(f"run directory must be a child of {artifact_root}")
    for ancestor in (build_root.resolve(), artifact_root, *run_dir.parents):
        if ancestor.exists():
            attributes = getattr(ancestor.stat(), "st_file_attributes", 0)
            if ancestor.is_symlink() or (attributes & 0x400):
                raise ValueError(f"reparse/symlink path is not allowed in run directory: {ancestor}")
    return run_dir


def validate_cases(path: Path) -> list[dict[str, Any]]:
    cases = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(cases, list) or not cases:
        raise ValueError("input must be a non-empty JSON array")
    seen: set[str] = set()
    for case in cases:
        if not isinstance(case, dict):
            raise ValueError("each case must be a JSON object")
        if not isinstance(case.get("id"), str) or not case["id"]:
            raise ValueError("each case requires a non-empty string id")
        if case["id"] in seen:
            raise ValueError(f"duplicate case id: {case['id']}")
        seen.add(case["id"])
        if not isinstance(case.get("input"), str) or not case["input"]:
            raise ValueError(f"case {case['id']} requires a non-empty input")
        if any(ord(char) > 127 for char in case["input"]):
            raise ValueError(f"case {case['id']} input must be ASCII keystrokes")
        if not isinstance(case.get("expected"), list) or not all(
            isinstance(value, str) for value in case["expected"]
        ):
            raise ValueError(f"case {case['id']} expected must be a string array")
    return cases


def source_identity(payload_data: Path) -> dict[str, Any]:
    upstream = ROOT / "third_party/rime-ice"
    overlay = ROOT / "data/ziliu"
    relative_paths = [
        "default.custom.yaml",
        "rime_ice.custom.yaml",
        "rime_ice.schema.yaml",
        "ziliu_phrase.txt",
        "ziliu_private.schema.yaml",
    ]
    files: list[dict[str, Any]] = []
    for relative in relative_paths:
        upstream_path = upstream / relative
        overlay_path = overlay / relative
        package_path = payload_data / relative
        source_path = overlay_path if overlay_path.is_file() else upstream_path
        item: dict[str, Any] = {"path": relative}
        if upstream_path.is_file():
            item["upstream_sha256"] = sha256(upstream_path)
        if overlay_path.is_file():
            item["overlay_sha256"] = sha256(overlay_path)
        if source_path.is_file():
            item["selected_source"] = "ziliu_overlay" if overlay_path.is_file() else "rime_ice"
            item["selected_source_sha256"] = sha256(source_path)
        if package_path.is_file():
            package_hash = sha256(package_path)
            item["package_sha256"] = package_hash
            if source_path.is_file():
                if package_hash == sha256(source_path):
                    item["identity"] = "identical"
                elif source_path.suffix in {".txt", ".yaml"} and package_path.read_bytes().replace(b"\r\n", b"\n") == source_path.read_bytes().replace(b"\r\n", b"\n"):
                    item["identity"] = "identical_after_line_ending_normalization"
                else:
                    item["identity"] = "different"
            else:
                item["identity"] = "package_only"
        else:
            item["identity"] = "missing_from_package"
        files.append(item)
    summary = {
        "identical": sum(item["identity"] == "identical" for item in files),
        "identical_after_line_ending_normalization": sum(item["identity"] == "identical_after_line_ending_normalization" for item in files),
        "different": sum(item["identity"] == "different" for item in files),
        "missing_from_package": sum(item["identity"] == "missing_from_package" for item in files),
    }
    return {"source_commit": git_head(), "files": files, "summary": summary}


def git_head() -> str | None:
    try:
        import subprocess

        result = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
        return result.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None


def bind(dll: Any, name: str, result: Any, *args: Any) -> Any:
    function = getattr(dll, name)
    function.restype = result
    function.argtypes = list(args)
    return function


def configure_api(dll: Any) -> dict[str, Any]:
    get_api = bind(dll, "rime_get_api", ctypes.POINTER(ApiHeader))
    api_header = get_api()
    if not api_header or api_header.contents.data_size < ctypes.sizeof(ApiHeader):
        raise RuntimeError("librime returned an incompatible RimeApi table")
    get_input_address = api_header.contents.function_slots[API_GET_INPUT_INDEX]
    if not get_input_address:
        raise RuntimeError("librime RimeApi.get_input is unavailable")
    get_input = ctypes.CFUNCTYPE(ctypes.c_char_p, ctypes.c_size_t)(get_input_address)
    return {
        "setup": bind(dll, "RimeSetup", None, ctypes.POINTER(Traits)),
        "deployer_initialize": bind(dll, "RimeDeployerInitialize", None, ctypes.POINTER(Traits)),
        "initialize": bind(dll, "RimeInitialize", None, ctypes.POINTER(Traits)),
        "finalize": bind(dll, "RimeFinalize", None),
        "deploy": bind(dll, "RimeDeployWorkspace", ctypes.c_int),
        "prebuild": bind(dll, "RimePrebuildAllSchemas", ctypes.c_int),
        "create_session": bind(dll, "RimeCreateSession", ctypes.c_size_t),
        "destroy_session": bind(dll, "RimeDestroySession", ctypes.c_int, ctypes.c_size_t),
        "select_schema": bind(dll, "RimeSelectSchema", ctypes.c_int, ctypes.c_size_t, ctypes.c_char_p),
        "process_key": bind(dll, "RimeProcessKey", ctypes.c_int, ctypes.c_size_t, ctypes.c_int, ctypes.c_int),
        "get_input": get_input,
        "get_context": bind(dll, "RimeGetContext", ctypes.c_int, ctypes.c_size_t, ctypes.POINTER(Context)),
        "free_context": bind(dll, "RimeFreeContext", ctypes.c_int, ctypes.POINTER(Context)),
        "get_status": bind(dll, "RimeGetStatus", ctypes.c_int, ctypes.c_size_t, ctypes.POINTER(Status)),
        "free_status": bind(dll, "RimeFreeStatus", ctypes.c_int, ctypes.POINTER(Status)),
        "candidate_begin": bind(dll, "RimeCandidateListBegin", ctypes.c_int, ctypes.c_size_t, ctypes.POINTER(CandidateIterator)),
        "candidate_next": bind(dll, "RimeCandidateListNext", ctypes.c_int, ctypes.POINTER(CandidateIterator)),
        "candidate_end": bind(dll, "RimeCandidateListEnd", None, ctypes.POINTER(CandidateIterator)),
    }


def collect_case(api: dict[str, Any], case: dict[str, Any]) -> dict[str, Any]:
    result = {key: value for key, value in case.items() if key != "context"}
    result["context"] = case.get("context")
    session = api["create_session"]()
    if not session:
        raise RuntimeError(f"RimeCreateSession failed for {case['id']}")
    try:
        if not api["select_schema"](session, b"rime_ice"):
            raise RuntimeError(f"could not select rime_ice for {case['id']}")
        failed_key: str | None = None
        for key in case["input"]:
            if not api["process_key"](session, ord(key), 0):
                failed_key = key
                break

        raw_input_value = api["get_input"](session)
        context = Context()
        context.data_size = rime_data_size(Context)
        raw_preedit: str | None = None
        page_size: int | None = None
        if api["get_context"](session, ctypes.byref(context)):
            try:
                if context.composition.preedit:
                    raw_preedit = context.composition.preedit.decode("utf-8")
                if context.menu.page_size:
                    page_size = context.menu.page_size
            finally:
                api["free_context"](ctypes.byref(context))

        status = Status()
        status.data_size = rime_data_size(Status)
        switches: dict[str, Any] | None = None
        if api["get_status"](session, ctypes.byref(status)):
            try:
                switches = {
                    "schema_id": status.schema_id.decode("utf-8") if status.schema_id else None,
                    "ascii_mode": bool(status.is_ascii_mode),
                    "simplification": bool(status.is_simplified),
                }
            finally:
                api["free_status"](ctypes.byref(status))

        candidates: list[dict[str, Any]] = []
        iterator = CandidateIterator()
        has_menu = bool(api["candidate_begin"](session, ctypes.byref(iterator)))
        try:
            while has_menu and len(candidates) < LIMIT + 1:
                if not api["candidate_next"](ctypes.byref(iterator)):
                    break
                text = iterator.candidate.text.decode("utf-8") if iterator.candidate.text else ""
                comment = iterator.candidate.comment.decode("utf-8") if iterator.candidate.comment else None
                candidates.append({"source_index": iterator.index, "text": text, "comment": comment})
        finally:
            if has_menu:
                api["candidate_end"](ctypes.byref(iterator))

        truncated = len(candidates) > LIMIT
        if truncated:
            candidates = candidates[:LIMIT]
        result.update(
            {
                "status": "input_error" if failed_key is not None else "ok",
                "failed_key": failed_key,
                "raw_input": raw_input_value.decode("utf-8") if raw_input_value else None,
                "raw_preedit": raw_preedit,
                "switches": switches,
                "page_size": page_size,
                "candidate_count": len(candidates),
                "truncated": truncated,
                "candidates": candidates,
            }
        )
        return result
    finally:
        api["destroy_session"](session)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT)
    parser.add_argument("--payload", type=Path, default=DEFAULT_PAYLOAD)
    parser.add_argument("--dll", type=Path, default=DEFAULT_DLL)
    parser.add_argument("--run-dir", type=Path, help="new, non-existing artifact directory")
    args = parser.parse_args()

    input_path = args.input.resolve()
    payload = args.payload.resolve()
    dll_path = args.dll.resolve()
    shared_data = payload / "data/rime"
    schema_path = shared_data / "rime_ice.schema.yaml"
    for path in (input_path, shared_data, schema_path, dll_path):
        if not path.exists():
            raise FileNotFoundError(path)
    cases = validate_cases(input_path)

    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    artifact_root = (ROOT / "build/contextual-ranking-pilot").resolve()
    run_dir = validate_run_dir(args.run_dir or artifact_root / timestamp, artifact_root, ROOT / "build")
    if run_dir.exists():
        raise FileExistsError(f"refusing to reuse existing run directory: {run_dir}")
    run_dir.mkdir(parents=True)
    user_dir = run_dir / "user-data"
    stage_dir = run_dir / "staging"
    prebuilt_dir = run_dir / "prebuilt"
    log_dir = run_dir / "logs"
    for directory in (user_dir, stage_dir, prebuilt_dir, log_dir):
        directory.mkdir()
        if not directory.resolve().is_relative_to(run_dir):
            raise RuntimeError(f"isolation path escaped run directory: {directory}")
        if any(directory.iterdir()):
            raise RuntimeError(f"isolated directory was not pristine: {directory}")

    dll_dirs: list[Any] = []
    for directory in (dll_path.parent, dll_path.parent.parent / "bin", payload):
        if directory.is_dir() and hasattr(os, "add_dll_directory"):
            dll_dirs.append(os.add_dll_directory(str(directory)))

    # The DLL's user, staging, prebuilt, and log paths all remain inside this
    # newly-created run directory. Shared data is read from the immutable package.
    encoded_paths = {
        "shared": str(shared_data).encode("utf-8"),
        "user": str(user_dir).encode("utf-8"),
        "staging": str(stage_dir).encode("utf-8"),
        "prebuilt": str(prebuilt_dir).encode("utf-8"),
        "logs": str(log_dir).encode("utf-8"),
        "app": b"rime.ziliu-contextual-ranking-pilot",
        "distribution": b"Ziliu contextual-ranking pilot",
        "code_name": b"ziliu-contextual-ranking-pilot",
        "version": b"1",
    }
    traits = Traits()
    traits.data_size = rime_data_size(Traits)
    traits.shared_data_dir = encoded_paths["shared"]
    traits.user_data_dir = encoded_paths["user"]
    traits.distribution_name = encoded_paths["distribution"]
    traits.distribution_code_name = encoded_paths["code_name"]
    traits.distribution_version = encoded_paths["version"]
    traits.app_name = encoded_paths["app"]
    traits.min_log_level = 2
    traits.log_dir = encoded_paths["logs"]
    traits.prebuilt_data_dir = encoded_paths["prebuilt"]
    traits.staging_dir = encoded_paths["staging"]

    manifest: dict[str, Any] = {
        "runtime_dll": str(dll_path),
        "runtime_dll_sha256": sha256(dll_path),
        "package_dll_matches_cached_runtime": sha256(dll_path) == sha256(ROOT / ".cache/librime-runtime/dist/lib/rime.dll"),
        "package_root": str(payload),
        "shared_data_dir": str(shared_data),
        "schema": "rime_ice",
        "schema_path": str(schema_path),
        "schema_sha256": sha256(schema_path),
        "input_path": str(input_path),
        "input_sha256": sha256(input_path),
        "source_identity": source_identity(shared_data),
        "shared_input_hashes_before": shared_input_hashes(shared_data),
        "run_dir": str(run_dir),
    }
    flags = {
        "real_rime": True,
        "stub": False,
        "context_fed_to_rime": False,
        "candidate_selection_calls": 0,
        "commit_calls": 0,
        "isolated_user_data": True,
        "isolated_staging_and_prebuilt": True,
        "normal_learning_configuration": False,
        "network_calls_from_probe": False,
    }

    dll = ctypes.WinDLL(str(dll_path))
    api = configure_api(dll)
    initialized = False
    try:
        api["setup"](ctypes.byref(traits))
        api["deployer_initialize"](ctypes.byref(traits))
        initialized = True
        if not api["deploy"]():
            raise RuntimeError("RimeDeployWorkspace failed; see run-dir logs")
        if not api["prebuild"]():
            raise RuntimeError("RimePrebuildAllSchemas failed; see run-dir logs")
        effective_schema = stage_dir / "rime_ice.schema.yaml"
        effective_learning = learning_settings(effective_schema)
        flags["normal_learning_configuration"] = effective_learning["normal_learning_enabled"]
        if not flags["normal_learning_configuration"]:
            raise RuntimeError("effective deployed rime_ice schema does not enable its normal user dictionary")
        api["initialize"](ctypes.byref(traits))
        output_cases = [collect_case(api, case) for case in cases]
    finally:
        if initialized:
            api["finalize"]()
        for dll_dir in dll_dirs:
            dll_dir.close()

    if sha256(input_path) != manifest["input_sha256"]:
        raise RuntimeError("input JSON changed during probe")
    manifest["shared_input_hashes_after"] = shared_input_hashes(shared_data)
    if manifest["shared_input_hashes_after"] != manifest["shared_input_hashes_before"]:
        raise RuntimeError("packaged shared Rime inputs changed during probe")
    manifest["effective_learning_configuration"] = effective_learning
    output = {
        "schema_version": 1,
        "manifest": manifest,
        "input_sha256": manifest["input_sha256"],
        "flags": flags,
        "case_count": len(output_cases),
        "enumeration_limit": LIMIT,
        "cases": output_cases,
    }
    result_path = run_dir / "results.json"
    result_path.write_text(json.dumps(output, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"results": str(result_path), "case_count": len(output_cases), "flags": flags}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"probe failed: {exc}", file=sys.stderr)
        raise
