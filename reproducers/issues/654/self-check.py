#!/usr/bin/env python3
"""Build and check only the authored fixture/control in bounded fresh containers."""
import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile
import tarfile
import uuid
from datetime import datetime, timezone

ROOT = pathlib.Path(__file__).resolve().parent
DEFAULT_BASELINE = "revng/revng@sha256:1446ae6f9ba9f2f907af69bb6c3954093327dc0e515fad76f1de8610df4a0fce"
EXPECTED_DLL = "7b592d48fa2d49b4bd603ac14d21ba0db8bb71fb3e6d4a4fe177afd8279bf744"


def digest(path):
    data = path.read_bytes()
    return {"file": path.name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def write_receipt(receipt):
    (ROOT / "self-check.json").write_text(json.dumps(receipt, indent=2) + "\n")


def public_text(text):
    # Actual commands use bind mounts; publish only portable public mount labels.
    text = text.replace(str(ROOT), "${PACKAGE}")
    if str(ROOT.parent) in text or str(pathlib.Path.home()) in text:
        raise RuntimeError("An output contains a host workspace path; review locally before sharing")
    return text


def run_recorded(command, public_command, receipt, label):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    stdout, stderr = public_text(result.stdout), public_text(result.stderr)
    logs = ROOT / "logs"
    logs.mkdir(exist_ok=True)
    for name, value in (("stdout", stdout), ("stderr", stderr)):
        (logs / (label + "." + name + ".log")).write_text(value)
    row = {"case": label, "command": public_command, "exit_code": result.returncode,
           "stdout": digest(logs / (label + ".stdout.log")),
           "stderr": digest(logs / (label + ".stderr.log"))}
    receipt["runs"].append(row)
    write_receipt(receipt)
    return row, stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", default=DEFAULT_BASELINE)
    parser.add_argument("--fixed-image")
    args = parser.parse_args()
    receipt = {"schema": "issue654-authored-C-control-self-check.v1",
               "observed_utc": datetime.now(timezone.utc).isoformat(),
               "status": "pending", "fixture": digest(ROOT / "public-test.dll"),
               "control_source": digest(ROOT / "occupy-low-page.c"),
               "binary_alone_failure_claimed": False,
               "control_changes_only_host_anonymous_page_layout": True,
               "private_inputs": False, "function_selection": False,
               "manual_model_or_ABI_override": False,
               "resources": {"cpus": 1, "memory_gib": 2, "pids": 128,
                             "seconds_per_run": 90, "sequential": True}, "runs": []}
    if receipt["fixture"]["sha256"] != EXPECTED_DLL:
        receipt["status"] = "fixture_hash_mismatch"
        write_receipt(receipt)
        return 1
    available = subprocess.run(["docker", "info", "--format", "{{.ServerVersion}}"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if available.returncode:
        receipt["status"] = "blocked_runtime_unavailable"
        receipt["runtime_observation"] = "Docker API unavailable; no container was launched"
        write_receipt(receipt)
        return 2

    common = ["docker", "run", "--rm", "--pull", "never", "--platform", "linux/amd64",
              "--network", "none", "--cpus", "1", "--memory", "2g", "--memory-swap", "2g",
              "--pids-limit", "128", "--ulimit", "core=0"]
    fixture_mount = "type=bind,source=" + str(ROOT) + ",target=/fixture"
    compile_command = common + ["--name", "issue654-build-" + uuid.uuid4().hex[:8],
                                "--mount", fixture_mount, "--workdir", "/fixture", args.baseline,
                                "bash", "-c", "source /revng/environment; exec timeout --signal=TERM --kill-after=5 90 env CONTROL_CC=/revng/root/lib64/llvm/llvm/bin/clang ./build-control.sh"]
    shown = [part.replace(str(ROOT), "${PACKAGE}") for part in compile_command]
    row, _ = run_recorded(compile_command, shown, receipt, "build-control")
    if row["exit_code"]:
        receipt["status"] = "control_build_failed"
        write_receipt(receipt)
        return 1
    receipt["control_library"] = digest(ROOT / "occupy-low-page.so")

    comparisons = [("official-baseline", args.baseline)]
    if args.fixed_image:
        comparisons.append(("source-fixed", args.fixed_image))
    for profile, image in comparisons:
        for layout in ("natural", "controlled"):
            label = profile + "-" + layout
            with tempfile.TemporaryDirectory(prefix="issue654-public-") as scratch:
                project = pathlib.Path(scratch) / "project"
                project.mkdir()
                command = common + ["--name", "issue654-" + uuid.uuid4().hex[:8],
                                    "--mount", fixture_mount + ",readonly",
                                    "--mount", "type=bind,source=" + str(project) + ",target=/work",
                                    "--workdir", "/work", image, "bash", "/fixture/run-case.sh", layout]
                shown = [part.replace(str(ROOT), "${PACKAGE}").replace(str(project), "${FRESH_PROJECT}") for part in command]
                row, stderr = run_recorded(command, shown, receipt, label)
                row["profile"], row["layout"] = profile, layout
                row["IDT_allocation_assertion"] = "env->idt.base != -1" in stderr
                if row["exit_code"] == 0:
                    archive = project / "recompilable.tar.gz"
                    row["archive"] = digest(archive)
                    with tarfile.open(archive, "r:gz") as stream:
                        members = sorted(member.name for member in stream.getmembers()
                                         if member.isfile())
                        source = stream.extractfile("decompiled/functions.c").read()
                    row["archive_members"] = members
                    row["generated_C_sha256"] = hashlib.sha256(source).hexdigest()
                    row["input_DLL_sha256_after_run"] = digest(ROOT / "public-test.dll")["sha256"]
                expected = 134 if profile == "official-baseline" and layout == "controlled" else 0
                row["expected_exit"] = expected
                row["matched_expected"] = row["exit_code"] == expected
                if expected == 134:
                    row["matched_expected"] &= row["IDT_allocation_assertion"]
                else:
                    row["matched_expected"] &= row.get("input_DLL_sha256_after_run") == EXPECTED_DLL
                    row["matched_expected"] &= "decompiled/functions.c" in row.get("archive_members", [])
                write_receipt(receipt)
                if not row["matched_expected"]:
                    receipt["status"] = "not_reproduced" if row["exit_code"] in (0, 125) else "inconclusive_earlier_failure"
                    write_receipt(receipt)
                    return 1
    receipt["status"] = "reproduced_supported_path_with_environment_control"
    receipt["successful_generated_C_agrees"] = len({r["generated_C_sha256"] for r in receipt["runs"] if "generated_C_sha256" in r}) == 1
    write_receipt(receipt)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
