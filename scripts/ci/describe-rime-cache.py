#!/usr/bin/env python3
"""Read bounded disposable CI caches and emit fingerprints, never admission results.

Usage: describe-rime-cache.py TEST_STATE_ROOT OUTPUT_JSON
This tool neither changes caches nor chooses/updates the compiled-profile pin.
The C++ checker and the original CTest exit status remain the only test gate.
"""
import hashlib
import json
import os
import re
import stat
import struct
import sys
import zlib
from pathlib import Path

FILES = (
    "default.yaml", "melt_eng.prism.bin", "melt_eng.reverse.bin",
    "melt_eng.schema.yaml", "melt_eng.table.bin", "radical_pinyin.prism.bin",
    "radical_pinyin.reverse.bin", "radical_pinyin.schema.yaml",
    "radical_pinyin.table.bin", "rime_ice.prism.bin", "rime_ice.reverse.bin",
    "rime_ice.schema.yaml", "rime_ice.table.bin", "ziliu_private.schema.yaml",
)
YAML_FILES = tuple(name for name in FILES if name.endswith(".yaml"))
PRISM_BINDINGS = (
    ("melt_eng.prism.bin", ("melt_eng.schema.yaml",)),
    ("radical_pinyin.prism.bin", ("radical_pinyin.schema.yaml",)),
    ("rime_ice.prism.bin", ("rime_ice.schema.yaml", "ziliu_private.schema.yaml")),
)
MAX_RUNS = 12
MAX_ENTRIES = 4096
MAX_FILE_BYTES = 128 * 1024 * 1024
MAX_TOTAL_BYTES = 256 * 1024 * 1024
PRISM_HEADER_BYTES = 32 + 7 * 4 + 256
RUN_NAME = re.compile(r"run-[0-9]+-[0-9]+\Z")
RESOURCE_KEY = re.compile(rb"[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*\Z")


class DiagnosticError(Exception):
    """Only fixed, non-sensitive error codes may leave this tool."""


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def is_link(info):
    return stat.S_ISLNK(info.st_mode) or bool(
        getattr(info, "st_file_attributes", 0)
        & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    )


def safe_directory(path):
    path = Path(os.path.abspath(path))
    for component in reversed((path, *path.parents)):
        info = component.lstat()
        if is_link(info) or not stat.S_ISDIR(info.st_mode):
            raise DiagnosticError("unsafe_directory")
    return path


def bounded_entries(path, limit):
    result = []
    with os.scandir(path) as entries:
        for entry in entries:
            if len(result) == limit:
                raise DiagnosticError("entry_limit")
            result.append(entry.name)
    return result


def read_regular(path, remaining):
    before = path.lstat()
    if is_link(before) or not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
        raise DiagnosticError("unsafe_file")
    if before.st_size > MAX_FILE_BYTES or before.st_size > remaining:
        raise DiagnosticError("byte_limit")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    with os.fdopen(descriptor, "rb") as stream:
        opened = os.fstat(stream.fileno())
        if is_link(opened) or not stat.S_ISREG(opened.st_mode) or (
            opened.st_dev, opened.st_ino, opened.st_size
        ) != (before.st_dev, before.st_ino, before.st_size):
            raise DiagnosticError("file_changed")
        data = stream.read(min(MAX_FILE_BYTES, remaining) + 1)
        after = os.fstat(stream.fileno())
    current = path.lstat()
    if len(data) != before.st_size or is_link(current) or (
        after.st_size, after.st_mtime_ns, current.st_dev, current.st_ino
    ) != (before.st_size, before.st_mtime_ns, before.st_dev, before.st_ino):
        raise DiagnosticError("file_changed")
    return data


def strict_lines(data):
    if not data:
        raise DiagnosticError("malformed_yaml")
    lines = []
    begin = 0
    ending = None
    while begin < len(data):
        newline = data.find(b"\n", begin)
        if newline < 0:
            line = data[begin:]
            if b"\r" in line:
                raise DiagnosticError("malformed_yaml")
            lines.append((begin, line, len(data)))
            break
        crlf = newline > begin and data[newline - 1] == 13
        if ending is not None and crlf != ending:
            raise DiagnosticError("malformed_yaml")
        ending = crlf
        content_end = newline - int(crlf)
        line = data[begin:content_end]
        if b"\r" in line:
            raise DiagnosticError("malformed_yaml")
        lines.append((begin, line, newline + 1))
        begin = newline + 1
    if ending is None or not lines:
        raise DiagnosticError("malformed_yaml")
    return lines


def normalize_yaml(data):
    """Mirror NormalizeCompiledYaml byte semantics; do not parse/re-emit YAML."""
    lines = strict_lines(data)
    if len(lines) < 5 or [line[1] for line in lines[:3]] != [
        b"__build_info:", b"  rime_version: 1.17.0", b"  timestamps:"
    ]:
        raise DiagnosticError("malformed_yaml")
    values = []
    previous = b""
    body = None
    for index in range(3, len(lines)):
        offset, line, _ = lines[index]
        if line.startswith(b"    "):
            entry = line[4:]
            separator = entry.find(b": ")
            if separator < 0:
                raise DiagnosticError("malformed_yaml")
            key, value = entry[:separator], entry[separator + 2:]
            if (not RESOURCE_KEY.fullmatch(key)
                    or any(part in (b".", b"..") for part in key.split(b"/"))
                    or (previous and key <= previous)
                    or not re.fullmatch(rb"[0-9]+", value)
                    or len(value.lstrip(b"0")) > 20
                    or int(value.lstrip(b"0") or b"0") > (1 << 64) - 1):
                raise DiagnosticError("malformed_yaml")
            previous = key
            values.append((offset + 4 + separator + 2, len(value)))
            continue
        if not line or line[:1] in (b" ", b"\t"):
            raise DiagnosticError("malformed_yaml")
        body = index
        break
    if not values or body is None:
        raise DiagnosticError("malformed_yaml")
    has_root = False
    for _, line, _ in lines[body:]:
        if not line or line[:1] in (b" ", b"\t"):
            continue
        colon = line.find(b":")
        if colon <= 0 or line[:colon] == b"__build_info":
            raise DiagnosticError("malformed_yaml")
        has_root = True
    if not has_root:
        raise DiagnosticError("malformed_yaml")
    normalized = data
    for offset, length in reversed(values):
        normalized = normalized[:offset] + b"0" + normalized[offset + length:]
    return normalized


def rime_crc32(data):
    # Boost crc_optimal initial remainder 0, final xor 0xffffffff. zlib's
    # externally supplied seed is already xor-adjusted, unlike Boost's seed.
    return zlib.crc32(data, 0xffffffff) & 0xffffffff


def prism_header_valid(data):
    prefix = b"Rime::Prism/4.0"
    return (len(data) >= PRISM_HEADER_BYTES
            and data[:len(prefix)] == prefix
            and data[len(prefix):32] == b"\0" * (32 - len(prefix)))


def aggregate(files):
    records = b"".join(
        name.encode("ascii") + b" " + sha256(files[name]).encode("ascii") + b"\n"
        for name in FILES
    )
    return sha256(records)


def newline_stats(data):
    crlf = data.count(b"\r\n")
    return {"crlf": crlf, "lf_only": data.count(b"\n") - crlf,
            "cr_only": data.count(b"\r") - crlf,
            "terminal": "crlf" if data.endswith(b"\r\n") else
                        "lf" if data.endswith(b"\n") else "none"}


def describe_files(raw):
    normalized = {}
    result = {"status": "fingerprints_only", "files": [], "prism_bindings": []}
    malformed = False
    for name in FILES:
        data = raw[name]
        entry = {"name": name, "bytes": len(data), "raw_sha256": sha256(data)}
        if name in YAML_FILES:
            entry["newlines"] = newline_stats(data)
            try:
                normalized[name] = normalize_yaml(data)
                lines = strict_lines(normalized[name])
                body = next(offset for offset, line, _ in lines[3:]
                            if not line.startswith(b"    "))
                entry["timestamp_resource_keys"] = [
                    line[4:].split(b": ", 1)[0].decode("ascii")
                    for offset, line, _ in lines[3:] if offset < body
                ]
                entry["body_line_count"] = sum(offset >= body for offset, _, _ in lines)
                entry["normalized_header_sha256"] = sha256(normalized[name][:body])
                entry["body_sha256"] = sha256(normalized[name][body:])
            except DiagnosticError:
                entry["error"] = "malformed_yaml"
                malformed = True
        elif name.endswith(".prism.bin"):
            if not prism_header_valid(data):
                entry["error"] = "invalid_prism_header"
                malformed = True
            else:
                normalized[name] = data[:36] + b"\0" * 4 + data[40:]
                entry["dictionary_crc32"] = int.from_bytes(data[32:36], "little")
                if name == "melt_eng.prism.bin":
                    fields = struct.unpack("<5I2i", data[32:60])
                    entry["prism_header"] = dict(zip((
                        "dictionary_crc32", "schema_crc32", "num_syllables",
                        "num_spellings", "double_array_units",
                        "double_array_relative_offset", "spelling_map_relative_offset"
                    ), fields))
                    entry["normalized_block_fingerprints"] = {
                        "block_bytes": 4096,
                        "sha256": [sha256(normalized[name][offset:offset + 4096])
                                   for offset in range(0, len(data), 4096)]
                    }
        else:
            normalized[name] = data
        if name in normalized:
            entry["normalized_sha256"] = sha256(normalized[name])
            entry["normalized_bytes"] = len(normalized[name])
        result["files"].append(entry)
    for name, schemas in PRISM_BINDINGS:
        data = raw[name]
        recorded = int.from_bytes(data[36:40], "little") if len(data) >= 40 else None
        candidates = [{"name": schema, "raw_crc32": rime_crc32(raw[schema])}
                      for schema in schemas]
        matches = recorded is not None and any(
            recorded == item["raw_crc32"] for item in candidates)
        result["prism_bindings"].append({"name": name,
            "recorded_schema_crc32": recorded, "schemas": candidates,
            "matches": matches})
        if not matches:
            malformed = True
    if malformed:
        result["status"] = "invalid_artifact"
        return result
    result["normalized_aggregate_sha256"] = aggregate(normalized)
    variants = {}
    for ending_name, ending in (("lf", b"\n"), ("crlf", b"\r\n")):
        for terminal in ("preserve", "present", "absent"):
            files = dict(normalized)
            for name in YAML_FILES:
                data = normalized[name].replace(b"\r\n", b"\n")
                if terminal == "present" and not data.endswith(b"\n"):
                    data += b"\n"
                elif terminal == "absent":
                    data = data.rstrip(b"\n")
                files[name] = data.replace(b"\n", ending)
            variants[f"{ending_name}_terminal_{terminal}"] = aggregate(files)
    result["hypothetical_yaml_variant_aggregates_NOT_ADMISSION"] = variants
    return result


def describe_cache(path):
    try:
        path = safe_directory(path)
        names = bounded_entries(path, len(FILES) + 1)
        if len(names) != len(FILES) or set(names) != set(FILES):
            return {"status": "inventory_mismatch", "entry_count": len(names),
                    "expected_file_count": len(FILES)}
        raw = {}
        remaining = MAX_TOTAL_BYTES
        for name in FILES:
            raw[name] = read_regular(path / name, remaining)
            remaining -= len(raw[name])
        return describe_files(raw)
    except DiagnosticError as error:
        return {"status": str(error)}
    except OSError:
        return {"status": "read_failure"}


def describe_root(root):
    report = {"purpose": "diagnostic_only_not_admission", "max_runs": MAX_RUNS,
              "expected_file_count": len(FILES), "runs": []}
    try:
        root = safe_directory(root)
        names = bounded_entries(root, MAX_ENTRIES)
        runs = sorted(name for name in names if RUN_NAME.fullmatch(name))
        report["matching_run_count"] = len(runs)
        report["omitted_run_count"] = max(0, len(runs) - MAX_RUNS)
        for name in runs[:MAX_RUNS]:
            report["runs"].append({"run": name, **describe_cache(root / name / "build")})
        report["status"] = "fingerprints_only"
    except DiagnosticError as error:
        report["status"] = str(error)
    except OSError:
        report["status"] = "read_failure"
    return report


def main(arguments):
    if len(arguments) != 2:
        print("Usage: describe-rime-cache.py TEST_STATE_ROOT OUTPUT_JSON", file=sys.stderr)
        return 2
    report = describe_root(Path(arguments[0]))
    try:
        output = Path(os.path.abspath(arguments[1]))
        safe_directory(output.parent)
        # A fresh evidence file prevents following an existing link or truncating
        # another file. Reruns must use a new evidence destination.
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0)
        with os.fdopen(os.open(output, flags, 0o600), "w", encoding="utf-8", newline="\n") as stream:
            json.dump(report, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except (OSError, DiagnosticError):
        print("Cache diagnostic output could not be created safely.", file=sys.stderr)
        return 1
    print(f"Diagnostic-only cache fingerprints: {len(report['runs'])} run(s); original CTest gate unchanged.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
