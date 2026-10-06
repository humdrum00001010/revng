#!/usr/bin/env python3
"""Build the public ELF and record the unmodified official quick CLI run."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import time


ROOT = Path(__file__).resolve().parent
IMAGE = "revng/revng@sha256:1446ae6f9ba9f2f907af69bb6c3954093327dc0e515fad76f1de8610df4a0fce"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--variant", default="official-quick-" + datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
parser.add_argument("--timeout", type=int, default=300)
args = parser.parse_args()
if Path(args.variant).name != args.variant or args.variant in (".", ".."):
    parser.error("--variant must be a directory name")
if args.timeout <= 0:
    parser.error("--timeout must be positive")
out = ROOT / "runs" / args.variant
out.mkdir(parents=True, exist_ok=False)
project = out / "project"
project.mkdir()
llvm_bin = Path(os.environ.get("LLVM_BIN", "/opt/homebrew/opt/llvm@17/bin"))
source = ROOT / "source/program.c"
binary = out / "program.elf"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


result = {
    "issue": 652,
    "official_image": IMAGE,
    "public_source": "source/program.c",
    "public_source_sha256": sha(source),
    "complete_independently_authored_executable": True,
    "function_selectors": False,
    "model_or_metadata_overrides": False,
    "IR_or_Clift_inputs_used": False,
    "helper_module_modifications": False,
    "supported_pipeline": "revng quick artifact emit-recompilable-archive",
    "commands": [],
}


def save():
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    lines = [shlex.join(item["argv"]) for item in result["commands"]]
    (out / "commands.txt").write_text("\n".join(lines) + "\n")


def run(stage, argv, timeout=60):
    started = datetime.datetime.now(datetime.timezone.utc).isoformat()
    with (out / (stage + ".stdout.log")).open("wb") as stdout, (out / (stage + ".stderr.log")).open("wb") as stderr:
        completed = subprocess.run(argv, cwd=ROOT, stdout=stdout, stderr=stderr, timeout=timeout)
    result["commands"].append({"stage": stage, "argv": argv, "cwd": str(ROOT), "exit_code": completed.returncode, "started_at": started})
    save()
    return completed.returncode


for executable in ("clang", "ld.lld", "llvm-readobj", "llvm-objdump"):
    path = llvm_bin / executable
    if run(executable + "-version", [str(path), "--version"]):
        raise SystemExit("Could not obtain " + executable + " version")
    result.setdefault("build_tools", {})[executable] = {"path": str(path), "sha256": sha(path)}

obj = out / "program.o"
build = [str(llvm_bin / "clang"), "--target=i386-unknown-linux-gnu", "-msse", "-O1", "-g", "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fdebug-compilation-dir=/case", "-c", "source/program.c", "-o", str(obj)]
link = [str(llvm_bin / "ld.lld"), "-m", "elf_i386", "--entry=_start", "--build-id=none", "-o", str(binary), str(obj)]
for stage, argv in (("build", build), ("link", link)):
    if run(stage, argv):
        result["classification"] = "build_failed"
        save()
        raise SystemExit(stage + " failed")
result["executable"] = str(binary.relative_to(ROOT))
result["executable_sha256"] = sha(binary)
run("binary-format", [str(llvm_bin / "llvm-readobj"), "--file-headers", "--program-headers", str(binary)])
run("binary-disassembly", [str(llvm_bin / "llvm-objdump"), "--disassemble", str(binary)])
run("official-image-inspect", ["docker", "image", "inspect", IMAGE])
version_argv = ["docker", "run", "--rm", "--platform", "linux/amd64", "--network", "none", "--cpus", "1", "--memory", "1g", "--memory-swap", "1g", IMAGE, "revng", "system-info"]
if run("revng-system-info", version_argv):
    raise SystemExit("Could not obtain official rev.ng component versions")
opt_version_argv = [*version_argv[:-1], "opt", "--version"]
if run("revng-opt-version", opt_version_argv):
    raise SystemExit("Could not obtain the official LLVM version")
component_lines = (out / "revng-system-info.stdout.log").read_text().splitlines()
result["official_revng_component"] = next(line.strip() for line in component_lines if "linux-x86-64/revng/optimized/" in line)

name = "public-repro-652-" + datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" + str(os.getpid())
quick = ["revng", "quick", "artifact", "emit-recompilable-archive", "/inputs/program.elf", "-o", "recompilable.tar.gz"]
argv = ["docker", "run", "--platform", "linux/amd64", "--network", "none", "--name", name, "--cpus", "1", "--memory", "4g", "--memory-swap", "4g", "--mount", "type=bind,src=" + str(binary) + ",dst=/inputs/program.elf,readonly", "--mount", "type=bind,src=" + str(project) + ",dst=/work", "--workdir", "/work", IMAGE, *quick]
result["container_name"] = name
result["revng_command"] = quick
result["quick_timeout_seconds"] = args.timeout
result["quick_started_at"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
save()
started = time.monotonic()
assertion_at = None
stop_reason = None
with (out / "quick.stdout.log").open("wb") as stdout, (out / "quick.stderr.log").open("wb") as stderr:
    process = subprocess.Popen(argv, cwd=ROOT, stdout=stdout, stderr=stderr)
    while process.poll() is None:
        diagnostics = (out / "quick.stderr.log").read_text(errors="replace")
        if "PostInlineHelpersVerifyPass.cpp:28" in diagnostics and "post-inline-helpers-verify: getelementptr in Isolated function" in diagnostics:
            if assertion_at is None:
                assertion_at = time.monotonic()
                print("Observed the native post-inline GEP verifier abort", flush=True)
            if time.monotonic() - assertion_at >= 5:
                stop_reason = "Native fatal assertion observed; owned probe remained running for 5 seconds under Rosetta"
                break
        if time.monotonic() - started >= args.timeout:
            stop_reason = "Owned public probe reached the configured timeout"
            break
        time.sleep(0.5)
    if stop_reason is not None:
        run("quick-pre-stop-state", ["docker", "inspect", name, "--format", "{{json .State}}"])
        run("quick-process-at-stop", ["docker", "top", name, "-eo", "pid,pcpu,etime,args"])
        run("quick-stop", ["docker", "stop", "--time", "5", name], timeout=30)
    exit_code = process.wait(timeout=30)
result["commands"].append({"stage": "quick", "argv": argv, "cwd": str(ROOT), "exit_code": exit_code, "started_at": result["quick_started_at"]})
result["quick_exit_code"] = exit_code
result["quick_elapsed_seconds"] = round(time.monotonic() - started, 3)
result["observer_stop_reason"] = stop_reason
run("quick-terminal-state", ["docker", "inspect", name, "--format", "{{json .State}}"])
state = json.loads((out / "quick-terminal-state.stdout.log").read_text())
result["container_terminal_state"] = state
diagnostics = (out / "quick.stderr.log").read_text(errors="replace")
reproduced = "PostInlineHelpersVerifyPass.cpp:28" in diagnostics and "post-inline-helpers-verify: getelementptr in Isolated function" in diagnostics
result["verifier_failure_reproduced"] = reproduced
result["specific_issue_652_initializer_evidence_cause_established"] = False
result["classification"] = "reproduced" if reproduced else ("not_reproduced" if exit_code == 0 else "inconclusive")
result["classification_scope"] = "Whether the reported post-inline GEP verifier failure occurs on this complete public binary through the normal command"
archive = project / "recompilable.tar.gz"
result["archive_created"] = archive.is_file()
if archive.is_file():
    result["archive_sha256"] = sha(archive)
result["source_unchanged"] = sha(source) == result["public_source_sha256"]
result["binary_unchanged"] = sha(binary) == result["executable_sha256"]
result["quick_stdout_sha256"] = sha(out / "quick.stdout.log")
result["quick_stderr_sha256"] = sha(out / "quick.stderr.log")
save()
print(str(out), result["classification"], "quick exit", exit_code, "OOMKilled", state["OOMKilled"], flush=True)
