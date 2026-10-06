#!/usr/bin/env python3
"""Build authored C into complete i386 ELF binaries, with/without debug types."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

BASE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--llvm-bin', type=Path)
args = parser.parse_args()


def tool(name):
    value = str(args.llvm_bin / name) if args.llvm_bin else shutil.which(name)
    if value is None:
        raise RuntimeError('Missing LLVM tool: ' + name)
    return value


def record(path):
    data = path.read_bytes()
    return {'file': path.relative_to(BASE).as_posix(), 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


(BASE / 'build').mkdir(exist_ok=True)
steps = []


def run(name, argv):
    result = subprocess.run(argv, cwd=BASE, capture_output=True, text=True)
    public = [Path(argv[0]).name, *[arg.replace(str(BASE), '${CASE_DIR}') for arg in argv[1:]]]
    (BASE / 'build' / (name + '.stdout.log')).write_text(result.stdout.replace(str(BASE), '${CASE_DIR}'))
    (BASE / 'build' / (name + '.stderr.log')).write_text(result.stderr.replace(str(BASE), '${CASE_DIR}'))
    steps.append({'name': name, 'argv': public, 'exit': result.returncode})
    if result.returncode:
        raise RuntimeError('C/binary build failed: ' + name)
    return result.stdout


version = run('clang-version', [tool('clang'), '--version'])
if 'version 17.' not in version:
    raise RuntimeError('Use pinned Clang/LLD 17')
binaries = []
for mode in ['raw', 'debug']:
    flags = ['--target=i386-linux-gnu', '-O1', '-fno-inline', '-fno-strict-aliasing', '-ffreestanding',
             '-fno-stack-protector', '-fno-pic', '-fno-pie', '-fno-asynchronous-unwind-tables']
    if mode == 'debug':
        flags += ['-g', '-gdwarf-4', '-fdebug-compilation-dir=/source', '-fdebug-prefix-map=' + str(BASE) + '=/source']
    run(mode + '-compile', [tool('clang'), *flags, '-c', 'src/pointer_scalar_views.c', '-o', 'build/' + mode + '.o'])
    run(mode + '-link-complete-ELF', [tool('ld.lld'), '-m', 'elf_i386', '-e', '_start',
                                     '-o', 'build/' + mode + '.elf', 'build/' + mode + '.o'])
    run(mode + '-all-headers-symbols', [tool('llvm-readobj'), '--file-headers', '--sections', '--symbols', 'build/' + mode + '.elf'])
    run(mode + '-full-disassembly', [tool('llvm-objdump'), '-dr', 'build/' + mode + '.elf'])
    binaries.append(record(BASE / 'build' / (mode + '.elf')))
    assert str(BASE).encode() not in (BASE / 'build' / (mode + '.elf')).read_bytes()
receipt = {'issue': 648, 'authorship': 'independently authored complete C and native binary',
           'private_inputs': False, 'manual_model_or_prototype_input': False, 'function_selection': False,
           'direct_DLA_graph_or_IR_input': False, 'compiler_version': version,
           'steps': steps, 'source': record(BASE / 'src/pointer_scalar_views.c'), 'binaries': binaries,
           'source_storage_width': 8, 'pointer_width': 4,
           'models_must_be_generated_by_normal_binary_pipeline': True}
(BASE / 'build/build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps({'built': binaries}))
