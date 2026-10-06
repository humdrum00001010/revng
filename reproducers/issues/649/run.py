#!/usr/bin/env python3
"""Run supported normal whole-binary quick/project commands without selectors."""
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
    parser.error('Explicitly classify a different producer; do not call it the pinned pristine image')
destination = BASE / 'results' / args.label
destination.mkdir(parents=True, exist_ok=False)
steps = []


def record(path):
    data = path.read_bytes()
    return {'file': path.relative_to(BASE).as_posix(), 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def command(name, argv):
    result = subprocess.run(argv, capture_output=True, text=True)
    public = [s.replace(str(BASE), '${CASE_DIR}') for s in argv]
    (destination / (name + '.stdout.log')).write_text(result.stdout.replace(str(BASE), '${CASE_DIR}'))
    (destination / (name + '.stderr.log')).write_text(result.stderr.replace(str(BASE), '${CASE_DIR}'))
    steps.append({'name': name, 'argv': public, 'exit': result.returncode})
    return result


inspect = command('image-identity', [args.docker, 'image', 'inspect', args.image, '--format', '{{.Id}}'])
if inspect.returncode:
    raise SystemExit(inspect.returncode)
cases = []
for stem in ['export_matrix', 'shared_ordinal_alias']:
    case = destination / stem
    case.mkdir()
    script = (f'revng --version; '
              f'revng quick artifact emit-recompilable-archive /case/build/{stem}.dll -o /case/results/{args.label}/{stem}/quick.tar.gz && '
              f'revng quick analyze /case/build/{stem}.dll -o /case/results/{args.label}/{stem}/quick-model.yml && '
              f'mkdir /case/results/{args.label}/{stem}/project && cd /case/results/{args.label}/{stem}/project && '
              f'revng project init /case/build/{stem}.dll && '
              'revng project artifact emit-recompilable-archive -o project.tar.gz')
    argv = [args.docker, 'run', '--platform', 'linux/amd64', '--network', 'none', '--cpus', '1',
            '--memory', '4g', '--memory-swap', '4g', '--mount', f'type=bind,src={BASE},dst=/case',
            '--workdir', '/case', '--entrypoint', '/bin/bash', args.image, '-lc', script]
    result = command(stem + '-whole-supported-paths', argv)
    truth = json.loads((BASE / 'build' / (stem + '.truth.json')).read_text())
    outcome = {'fixture': stem, 'input': record(BASE / 'build' / (stem + '.dll')),
               'complete_export_truth': truth, 'pipeline_exit': result.returncode,
               'baseline_class': args.baseline_class,
               'manual_model_or_prototype_input': False, 'function_selection': False}
    if result.returncode == 0:
        models = [('quick', case / 'quick-model.yml'), ('project', case / 'project/revng.yml')]
        outcome['models'] = []
        for mode, path in models:
            model = yaml.safe_load(path.read_text())
            roots = model.get('Functions', [])
            outcome['models'].append({'mode': mode, 'artifact': record(path), 'function_count': len(roots),
                                      'all_function_entries': [row.get('Entry') for row in roots]})
        outcome['archives'] = []
        for mode, path in [('quick', case / 'quick.tar.gz'), ('project', case / 'project/project.tar.gz')]:
            with tarfile.open(path) as archive:
                source = archive.extractfile('decompiled/functions.c').read()
            outcome['archives'].append({'mode': mode, 'artifact': record(path), 'functions_C_bytes': len(source),
                                        'functions_C_sha256': hashlib.sha256(source).hexdigest()})
            (case / (mode + '-functions.c')).write_bytes(source)
        counts = [row['function_count'] for row in outcome['models']]
        outcome['classification'] = 'reproduced_supported_path' if counts == [0, 0] else 'not_reproduced'
    else:
        outcome['classification'] = 'inconclusive_earlier_failure'
    cases.append(outcome)
receipt = {'issue': 649, 'image': args.image, 'actual_image_id': inspect.stdout.strip(),
           'baseline_class': args.baseline_class, 'supported_whole_binary_paths_only': True,
           'commands': steps, 'cases': cases, 'private_inputs': False,
           'runtime_execution_performed': False, 'publication_approved': False}
(destination / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps({'cases': [(c['fixture'], c['classification']) for c in cases],
                  'receipt': 'results/' + args.label + '/receipt.json'}))
