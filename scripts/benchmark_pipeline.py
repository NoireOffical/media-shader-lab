#!/usr/bin/env python3
"""Run reproducible decode and GPU-pipeline benchmarks for Media Shader Lab."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Optional


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INPUT = ROOT / "assets/videos/big_buck_bunny_720p_20s.mp4"
NUMBER_PATTERN = re.compile(r"(?:^|\s)([a-z_]+)=([^\s]+)")


def portable_path(path: Path) -> str:
    resolved = path.resolve()
    return str(resolved.relative_to(ROOT)) if resolved.is_relative_to(ROOT) else str(resolved)


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Compare software decode, VideoToolbox CPU transfer/IOSurface "
            "zero-copy, and synchronous/PBO GPU transfers."
        )
    )
    parser.add_argument("--binary", type=Path, default=ROOT / "build/media_shader_lab")
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--frames", type=int, default=300)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--filter", default="edge")
    parser.add_argument(
        "--encoder",
        choices=("auto", "libx265", "hevc_videotoolbox"),
        default="auto",
    )
    parser.add_argument("--quality", action="store_true")
    parser.add_argument("--decode-only", action="store_true")
    parser.add_argument("--pipeline-only", action="store_true")
    parser.add_argument("--timeout", type=int, default=300)
    parsed = parser.parse_args()
    if parsed.frames <= 0 or parsed.repeats <= 0 or parsed.timeout <= 0:
        parser.error("--frames, --repeats, and --timeout must be positive")
    if parsed.decode_only and parsed.pipeline_only:
        parser.error("--decode-only and --pipeline-only cannot be combined")
    return parsed


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def command_output(command: list[str]) -> str:
    try:
        return subprocess.run(
            command,
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def machine_metadata(input_path: Path, binary: Path, args: argparse.Namespace) -> dict[str, Any]:
    metadata: dict[str, Any] = {
        "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "git_commit": command_output(["git", "rev-parse", "HEAD"]),
        "git_dirty": bool(command_output(["git", "status", "--porcelain"])),
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "processor": platform.processor() or "unknown",
        "python": platform.python_version(),
        "input": portable_path(input_path),
        "input_sha256": sha256(input_path),
        "binary": portable_path(binary),
        "frames_per_run": args.frames,
        "repeats": args.repeats,
        "filter": args.filter,
        "quality_enabled": args.quality,
    }
    if platform.system() == "Darwin":
        metadata["mac_model"] = command_output(["sysctl", "-n", "hw.model"])
        metadata["cpu_brand"] = command_output(
            ["sysctl", "-n", "machdep.cpu.brand_string"]
        )
        metadata["macos_version"] = platform.mac_ver()[0]
    return metadata


def parse_key_values(text: str) -> dict[str, Any]:
    values: dict[str, Any] = {}
    for key, raw in NUMBER_PATTERN.findall(text):
        if raw in ("true", "false"):
            values[key] = raw == "true"
            continue
        try:
            values[key] = float(raw) if any(mark in raw for mark in ".eE") else int(raw)
        except ValueError:
            values[key] = raw
    return values


def run_process(command: list[str], log_path: Path, timeout: int) -> tuple[int, float, str]:
    started = time.perf_counter()
    try:
        completed = subprocess.run(
            command,
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=timeout,
            env={**os.environ, "GLFW_COCOA_MENUBAR": "0"},
        )
        elapsed = time.perf_counter() - started
        combined = completed.stdout + completed.stderr
        log_path.write_text(combined, encoding="utf-8")
        return completed.returncode, elapsed, combined
    except subprocess.TimeoutExpired as error:
        elapsed = time.perf_counter() - started
        combined = (error.stdout or "") + (error.stderr or "")
        if isinstance(combined, bytes):
            combined = combined.decode("utf-8", errors="replace")
        combined += f"\nbenchmark timeout after {timeout} seconds\n"
        log_path.write_text(combined, encoding="utf-8")
        return 124, elapsed, combined


def case_definitions(include_decode: bool, include_pipeline: bool) -> list[dict[str, Any]]:
    cases: list[dict[str, Any]] = []
    if include_decode:
        cases.extend(
            [
                {"name": "decode_software", "kind": "decode", "args": ["--decoder", "software"]},
                {
                    "name": "decode_videotoolbox_cpu_transfer",
                    "kind": "decode",
                    "darwin": True,
                    "args": ["--decoder", "videotoolbox", "--no-zero-copy"],
                },
                {
                    "name": "decode_videotoolbox_zero_copy",
                    "kind": "decode",
                    "darwin": True,
                    "expects_zero_copy": True,
                    "args": ["--decoder", "videotoolbox"],
                },
            ]
        )
    if include_pipeline:
        cases.extend(
            [
                {
                    "name": "pipeline_software_sync",
                    "kind": "pipeline",
                    "args": ["--decoder", "software", "--no-pbo"],
                },
                {
                    "name": "pipeline_software_pbo",
                    "kind": "pipeline",
                    "args": ["--decoder", "software"],
                },
                {
                    "name": "pipeline_videotoolbox_cpu_transfer",
                    "kind": "pipeline",
                    "darwin": True,
                    "args": ["--decoder", "videotoolbox", "--no-zero-copy"],
                },
                {
                    "name": "pipeline_videotoolbox_zero_copy",
                    "kind": "pipeline",
                    "darwin": True,
                    "expects_zero_copy": True,
                    "args": ["--decoder", "videotoolbox"],
                },
            ]
        )
    return cases


def numeric_medians(runs: list[dict[str, Any]]) -> dict[str, float]:
    keys = sorted({key for run in runs for key, value in run.items() if isinstance(value, (int, float)) and not isinstance(value, bool)})
    return {
        key: statistics.median(
            float(run[key]) for run in runs if isinstance(run.get(key), (int, float)) and not isinstance(run.get(key), bool)
        )
        for key in keys
    }


def percent_change(candidate: float, baseline: float) -> Optional[float]:
    return (candidate / baseline - 1.0) * 100.0 if baseline else None


def comparisons(aggregates: dict[str, dict[str, Any]]) -> dict[str, Any]:
    output: dict[str, Any] = {}

    def add(name: str, baseline: str, candidate: str, metric: str) -> None:
        if baseline not in aggregates or candidate not in aggregates:
            return
        base = aggregates[baseline].get(metric)
        changed = aggregates[candidate].get(metric)
        if isinstance(base, (int, float)) and isinstance(changed, (int, float)):
            output[name] = {
                "metric": metric,
                "baseline": baseline,
                "candidate": candidate,
                "baseline_value": base,
                "candidate_value": changed,
                "percent_change": percent_change(float(changed), float(base)),
            }

    add("videotoolbox_zero_copy_vs_cpu_transfer_decode", "decode_videotoolbox_cpu_transfer", "decode_videotoolbox_zero_copy", "wall_fps")
    add("pbo_vs_sync_pipeline", "pipeline_software_sync", "pipeline_software_pbo", "encoding_fps")
    add("videotoolbox_zero_copy_vs_cpu_transfer_pipeline", "pipeline_videotoolbox_cpu_transfer", "pipeline_videotoolbox_zero_copy", "encoding_fps")
    return output


def main() -> int:
    args = arguments()
    binary = args.binary.resolve()
    input_path = args.input.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise SystemExit(f"benchmark binary is missing or not executable: {binary}")
    if not input_path.is_file():
        raise SystemExit(f"benchmark input is missing: {input_path}")

    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    output_dir = (args.output_dir or ROOT / "build/benchmarks" / stamp).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    encoder = args.encoder
    if encoder == "auto":
        encoder = "hevc_videotoolbox" if platform.system() == "Darwin" else "libx265"

    include_decode = not args.pipeline_only
    include_pipeline = not args.decode_only
    results: list[dict[str, Any]] = []
    required_failure = False
    for definition in case_definitions(include_decode, include_pipeline):
        if definition.get("darwin") and platform.system() != "Darwin":
            results.append({
                "name": definition["name"],
                "kind": definition["kind"],
                "status": "skipped",
                "reason": "VideoToolbox is only available on macOS",
                "runs": [],
            })
            continue

        case_runs: list[dict[str, Any]] = []
        status = "passed"
        reason = ""
        for repeat in range(1, args.repeats + 1):
            stem = f"{definition['name']}-run-{repeat}"
            command = [
                portable_path(binary),
                "--input", portable_path(input_path),
                "--no-sync",
                "--max-frames", str(args.frames),
                *definition["args"],
            ]
            metrics_path = output_dir / f"{stem}.metrics.json"
            if definition["kind"] == "decode":
                command.extend(["--headless", "--metrics-output", portable_path(metrics_path)])
            else:
                output_path = output_dir / f"{stem}.mp4"
                command.extend([
                    "--filter", args.filter,
                    "--output", portable_path(output_path),
                    "--encoder", encoder,
                    "--no-audio",
                ])
                if encoder == "libx265":
                    command.extend(["--preset", "ultrafast", "--crf", "24"])
                else:
                    command.extend(["--bitrate-kbps", "6000"])
                if args.quality:
                    command.extend(["--quality-output", portable_path(output_dir / f"{stem}.quality.json")])

            return_code, elapsed, text = run_process(
                command, output_dir / f"{stem}.log", args.timeout
            )
            if return_code != 0:
                status = "skipped" if definition.get("darwin") else "failed"
                reason = next(
                    (line.strip() for line in reversed(text.splitlines()) if line.strip()),
                    f"process exited with {return_code}",
                )
                if status == "failed":
                    required_failure = True
                break

            run: dict[str, Any] = {
                "repeat": repeat,
                "wall_seconds_external": elapsed,
                "command": [str(Path(item).relative_to(ROOT)) if Path(item).is_absolute() and Path(item).is_relative_to(ROOT) else item for item in command],
            }
            run.update(parse_key_values(text))
            if metrics_path.exists():
                run.update(json.loads(metrics_path.read_text(encoding="utf-8")))
            if definition.get("expects_zero_copy") and not run.get("decoder_zero_copy"):
                status = "fallback"
                reason = "VideoToolbox ran, but the input surface was not eligible for IOSurface zero-copy"
            case_runs.append(run)

        result: dict[str, Any] = {
            "name": definition["name"],
            "kind": definition["kind"],
            "status": status,
            "runs": case_runs,
        }
        if reason:
            result["reason"] = reason
        if case_runs:
            result["median"] = numeric_medians(case_runs)
        results.append(result)
        print(f"{definition['name']}: {status}")

    aggregates = {
        result["name"]: result["median"]
        for result in results
        if result.get("status") == "passed" and "median" in result
    }
    report = {
        "schema_version": 1,
        "environment": machine_metadata(input_path, binary, args),
        "encoder": encoder,
        "cases": results,
        "comparisons": comparisons(aggregates),
    }
    json_path = output_dir / "benchmark.json"
    json_path.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    csv_path = output_dir / "benchmark.csv"
    metric_names = sorted({key for values in aggregates.values() for key in values})
    with csv_path.open("w", newline="", encoding="utf-8") as destination:
        writer = csv.DictWriter(destination, fieldnames=["case", "status", *metric_names], extrasaction="ignore")
        writer.writeheader()
        for result in results:
            writer.writerow({"case": result["name"], "status": result["status"], **result.get("median", {})})

    print(f"JSON report: {json_path}")
    print(f"CSV summary: {csv_path}")
    return 1 if required_failure else 0


if __name__ == "__main__":
    sys.exit(main())
