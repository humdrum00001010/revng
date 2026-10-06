#!/usr/bin/env python3
"""Run standard whole-binary analysis; classify failures without custom DLA input."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
import yaml

BASE = Path(__file__).resolve().parent
OFFICIAL = 'revng/revng@sha256:1446ae6f9ba9f2f907af69bb6c3954093327dc0e515fad76f1de8610df4a0fce'
parser = argparse.ArgumentParser()
parser.add_argument('--image', default=OFFICIAL)
parser.add_argument('--label', default='pristine-pinned-upstream')
parser.add_argument('--baseline-class', choices=['pristine_pinned_upstream', 'pristine_current_upstream', 'modified_producer_disclosed'], default='pristine_pinned_upstream')
parser.add_argument('--docker', default='docker')
args = parser.parse_args()
if not re.fullmatch(r'[A-Za-z0-9_-]+', args.label):
    parser.error('Use a simple public result label')
if args.image != OFFICIAL and args.baseline_class == 'pristine_pinned_upstream':
    parser.error('Disclose modified/different producer classification')
destination = BASE / 'results' / args.label
destination.mkdir(parents=True, exist_ok=False)
steps = []


def record(path):
    data = path.read_bytes()
    return {'file': path.relative_to(BASE).as_posix(), 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def command(name, argv):
    result = subprocess.run(argv, capture_output=True, text=True)
    stdout, stderr = result.stdout.replace(str(BASE), '${CASE_DIR}'), result.stderr.replace(str(BASE), '${CASE_DIR}')
    (destination / (name + '.stdout.log')).write_text(stdout)
    (destination / (name + '.stderr.log')).write_text(stderr)
    steps.append({'name': name, 'argv': [s.replace(str(BASE), '${CASE_DIR}') for s in argv], 'exit': result.returncode})
    return result, stdout + '\n' + stderr


inspect, _ = command('image-identity', [args.docker, 'image', 'inspect', args.image, '--format', '{{.Id}}'])
if inspect.returncode:
    raise SystemExit(inspect.returncode)
cases = []
for mode in ['raw', 'debug']:
    case = destination / mode
    case.mkdir()
    script = (f'revng --version; '
              f'revng quick artifact emit-recompilable-archive /case/build/{mode}.elf -o /case/results/{args.label}/{mode}/quick.tar.gz && '
              f'revng quick analyze /case/build/{mode}.elf -o /case/results/{args.label}/{mode}/quick-model.yml')
    argv = [args.docker, 'run', '--platform', 'linux/amd64', '--network', 'none', '--cpus', '1',
            '--memory', '4g', '--memory-swap', '4g', '--mount', f'type=bind,src={BASE},dst=/case',
            '--workdir', '/case', '--entrypoint', '/bin/bash', args.image, '-lc', script]
    result, log = command(mode + '-whole-supported-path', argv)
    outcome = {'fixture': mode, 'binary': record(BASE / 'build' / (mode + '.elf')),
               'pipeline_exit': result.returncode, 'baseline_class': args.baseline_class,
               'private_inputs': False, 'manual_model_or_prototype_input': False,
               'function_selection': False, 'direct_DLA_graph_or_IR_input': False,
               'target_new_model_width_loss_proven': False,
               'model_owned_distinct_aggregate_pointee_condition_proven': False}
    if result.returncode:
        outcome['classification'] = 'inconclusive_earlier_failure'
        outcome['existing_upstream_large_scalar_assertion_observed'] = (
            'MergedScalar->Size == PointerSize' in log)
        outcome['scope_note'] = 'A failure before automatic model output does not reproduce the separately reported backend model-width loss.'
    else:
        model_path = case / 'quick-model.yml'
        model = yaml.safe_load(model_path.read_text())
        outcome['automatic_model'] = record(model_path)
        outcome['all_function_count'] = len(model.get('Functions', []))
        outcome['all_TypeDefinitions'] = model.get('TypeDefinitions', [])
        with tarfile.open(case / 'quick.tar.gz') as archive:
            generated = archive.extractfile('decompiled/types-and-globals.h').read()
            functions = archive.extractfile('decompiled/functions.c').read()
        (case / 'types-and-globals.h').write_bytes(generated)
        (case / 'functions.c').write_bytes(functions)
        outcome['classification'] = 'not_reproduced'
        outcome['scope_note'] = 'Supported C-to-binary pipeline completed. Inspect inferred pointer/scalar views; success alone does not establish that the direct-graph target condition occurred.'
    cases.append(outcome)
receipt = {'issue': 648, 'image': args.image, 'actual_image_id': inspect.stdout.strip(),
           'baseline_class': args.baseline_class, 'commands': steps, 'cases': cases,
           'source_report_existing_fix': 'b482edd3d3b77e134bf21979c36f59e0a7502c95',
           'source_report_existing_model_initialization_fix': 'c634c39387932bc9f0267c0455c737cf281214c5',
           'new_width_change_requires_separate_supported_binary_evidence': True,
           'direct_graph_test_is_not_binary_reproduction': True,
           'runtime_execution_performed': False, 'publication_approved': False}
(destination / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps({'cases': [(c['fixture'], c['classification']) for c in cases],
                  'receipt': 'results/' + args.label + '/receipt.json'}))
