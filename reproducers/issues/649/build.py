#!/usr/bin/env python3
"""Build complete public C-derived PE fixtures using Clang/LLD 17."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
from pe_exports import PE

BASE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--llvm-bin', type=Path)
args = parser.parse_args()


def tool(name):
    candidate = args.llvm_bin / name if args.llvm_bin else None
    value = str(candidate) if candidate and candidate.exists() else shutil.which(name)
    if value is None:
        raise RuntimeError('Missing LLVM tool: ' + name)
    return value


def record(path):
    data = path.read_bytes()
    return {'file': path.relative_to(BASE).as_posix(), 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


steps = []
(BASE / 'build').mkdir(exist_ok=True)


def run(name, argv):
    result = subprocess.run(argv, cwd=BASE, capture_output=True, text=True)
    public_argv = [Path(argv[0]).name, *argv[1:]]
    public_stdout = result.stdout.replace(str(BASE), '${CASE_DIR}')
    public_stderr = result.stderr.replace(str(BASE), '${CASE_DIR}')
    (BASE / 'build' / (name + '.stdout.log')).write_text(public_stdout)
    (BASE / 'build' / (name + '.stderr.log')).write_text(public_stderr)
    steps.append({'name': name, 'argv': public_argv, 'exit': result.returncode})
    if result.returncode:
        raise RuntimeError(name + ': ' + public_stderr)
    return public_stdout


version = run('clang-version', [tool('clang'), '--version'])
if 'version 17.' not in version:
    raise RuntimeError('Use LLVM 17 for this pinned fixture')
run('compile', [tool('clang'), '--target=i686-pc-windows-msvc', '-O2', '-ffreestanding',
                '-c', 'src/export_matrix.c', '-o', 'build/export_matrix.obj'])
run('link-whole-DLL', [tool('lld-link'), '/DLL', '/NOENTRY', '/NODEFAULTLIB', '/MACHINE:X86',
                       '/OPT:NOICF', '/OPT:NOREF', '/timestamp:0', '/DEF:src/export_matrix.def',
                       '/OUT:build/export_matrix.dll', 'build/export_matrix.obj'])
original = BASE / 'build/export_matrix.dll'
pe = PE(original)
rows, names = pe.exports()
truth = pe.truth()
assert truth['expected_executable_function_roots'] == 3
assert truth['entrypoint_RVA'] == 0 and truth['COFF_symbol_count'] == 0
assert any(row['forwarder'] for row in rows) and any(row['zero_RVA_hole'] for row in rows)
assert any(row['names'] and not row['executable'] and not row['forwarder'] for row in rows)
(BASE / 'build/export_matrix.truth.json').write_text(json.dumps(truth, indent=2) + '\n')
# Author a complete standard PE alias-table variant: two names refer to one
# EAT index. This changes only this independently authored synthetic DLL.
alpha = next(row for row in rows if 'alpha' in row['names'])
alias = next(row for row in rows if 'alias_alpha' in row['names'])
alias_name = next(row for row in names if row['name'] == 'alias_alpha')
shared = bytearray(original.read_bytes())
struct.pack_into('<H', shared, alias_name['ordinal_field_offset'], alpha['index'])
struct.pack_into('<I', shared, alias['EAT_field_offset'], 0)
shared_path = BASE / 'build/shared_ordinal_alias.dll'
shared_path.write_bytes(shared)
shared_truth = PE(shared_path).truth()
assert shared_truth['names_by_executable_RVA'][hex(alpha['rva'])] == ['alias_alpha', 'alpha']
assert shared_truth['expected_executable_function_roots'] == 3
(BASE / 'build/shared_ordinal_alias.truth.json').write_text(json.dumps(shared_truth, indent=2) + '\n')
for filename in ['export_matrix.dll', 'shared_ordinal_alias.dll']:
    run(filename + '-complete-PE', [tool('llvm-readobj'), '--file-headers', '--sections', '--coff-exports', 'build/' + filename])
receipt = {'issue': 649, 'authorship': 'independently authored complete C and PE export table fixtures',
           'private_inputs': False, 'manual_model_or_prototype_input': False, 'function_selection': False,
           'compiler_version': version, 'steps': steps,
           'inputs': [record(BASE / 'src/export_matrix.c'), record(BASE / 'src/export_matrix.def')],
           'binaries': [record(original), record(shared_path)],
           'full_export_truth': [record(BASE / 'build/export_matrix.truth.json'), record(BASE / 'build/shared_ordinal_alias.truth.json')]}
(BASE / 'build/build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps({'built': receipt['binaries'], 'expected_unique_code_roots_each': 3}))
