"""Compile tracked shaders (optionally SDK rows) and audit their containers.

Reports refusals, timeouts and unresolved checks separately. No SDK is required
by CI. Allowances key source/profile/record/paramno/code/register/offset/slot,
with a card and an exact count; added findings and obsolete allowances fail.
"""
import argparse
import collections
import csv
import hashlib
import json
import pathlib
import subprocess

from uniform_container_check import check_container


def issue_key(row, issue):
    return (row['source'], row['profile'], issue['record'], issue['paramno'], issue['code'],
            issue.get('register'), issue.get('offset'), issue.get('slot'))


def allowance_delta(findings, entries):
    """Exact evidence, not a wildcard for everything with the same diagnostic."""
    allowed = {}
    for entry in entries:
        if not entry.get('card', '').startswith('t_') or not entry.get('reason'):
            raise ValueError('allowance requires card and reason')
        key = issue_key(entry, entry)
        if key in allowed or not isinstance(entry['count'], int) or entry['count'] <= 0:
            raise ValueError('duplicate or invalid allowance %r' % (key,))
        allowed[key] = entry['count']
    return [dict(key=key, actual=findings[key], allowed=allowed.get(key, 0))
            for key in sorted(findings.keys() | allowed.keys(), key=repr)
            if findings[key] != allowed.get(key, 0)]


def run_census(args):
    root = pathlib.Path(__file__).resolve().parents[2]
    out = pathlib.Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=True)
    files = subprocess.check_output(['git', '-c', f'safe.directory={root.as_posix()}', 'ls-files', '--',
        'tools/rsx-cg-compiler/tests/shaders', 'tests/regression/shader-differential/shaders',
        'samples/*/shaders'], cwd=root, text=True).splitlines()
    rows = []
    for name in sorted(files):
        path = pathlib.Path(name)
        if path.suffix.lower() not in ('.cg', '.fcg', '.vcg'):
            continue
        profile = 'sce_vp_rsx' if path.name.endswith('_v.cg') or path.suffix == '.vcg' else 'sce_fp_rsx'
        rows.append(dict(source=name, profile=profile, path=str(root / path), group='tracked', includes=[]))
    if len(rows) < 100:
        raise ValueError('tracked corpus enumeration collapsed: %d shaders' % len(rows))
    if args.sdk_csv:
        sdk = pathlib.Path(args.sdk_root)
        with open(args.sdk_csv, newline='') as source:
            for row in csv.reader(source):
                name, profile = row[:2]
                if profile not in ('sce_fp_rsx', 'sce_vp_rsx'):
                    raise ValueError('unsupported SDK row profile ' + profile)
                rows.append(dict(source=name, profile=profile, path=str(sdk / name), group='sdk', includes=[
                    str(sdk / 'samples/tutorial/DeferredShading/include'),
                    str(sdk / 'samples/tutorial/SpuGraphics/SpuRender/common/include')]))
    else:
        print('SKIPPED SDK corpus: --sdk-csv/--sdk-root not supplied', flush=True)
    outcomes = []
    findings = collections.Counter()
    for index, row in enumerate(rows, 1):
        stem = hashlib.sha256((row['group'] + row['source'] + row['profile']).encode()).hexdigest()[:24]
        binary = out / (stem + '.bin')
        cmd = [args.compiler, '-p', row['profile']]
        for include in row['includes']:
            cmd += ['-I', include]
        cmd += ['--emit-container', str(binary), row['path']]
        try:
            run = subprocess.run(cmd, capture_output=True, timeout=args.timeout, cwd=root)
            code = run.returncode
            (out / (stem + '.log')).write_bytes(run.stdout + run.stderr)
        except subprocess.TimeoutExpired:
            code = 124
        item = dict(source=row['source'], group=row['group'], profile=row['profile'], compiler_exit=code)
        if code == 0:
            data = binary.read_bytes()
            report = check_container(data, expected_profile={'sce_vp_rsx': 7003, 'sce_fp_rsx': 7004}[row['profile']])
            item.update(sha256=hashlib.sha256(data).hexdigest(), report=report)
            for issue in report['issues']:
                findings[issue_key(row, issue)] += 1
        outcomes.append(item)
        if index % 25 == 0 or index == len(rows):
            print('%d/%d compiled; accepted=%d findings=%d' %
                  (index, len(rows), sum(x['compiler_exit'] == 0 for x in outcomes), sum(findings.values())), flush=True)
        # Incremental durable record: interrupted runs are visibly incomplete.
        with open(out / 'outcomes.jsonl', 'a', encoding='utf-8') as log:
            log.write(json.dumps(item) + '\n')
    entries = []
    for path in args.allowlist:
        entries.extend(json.loads(pathlib.Path(path).read_text()))
    delta = allowance_delta(findings, entries)
    summary = dict(total=len(rows), outcomes=dict(collections.Counter(x['compiler_exit'] for x in outcomes)),
                   findings=sum(findings.values()), allowlist_delta=delta,
                   coverage=dict(collections.Counter(check['status'] for x in outcomes
                                 for check in x.get('report', {}).get('checks', []))))
    (out / 'summary.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps({k: v for k, v in summary.items() if k != 'allowlist_delta'}), flush=True)
    print('allowlist differences: %d (details in summary.json)' % len(delta), flush=True)
    return int(bool(delta) or any(x['compiler_exit'] not in (0, 1) for x in outcomes))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler')
    parser.add_argument('--output', required=True)
    parser.add_argument('--allowlist', action='append', default=[])
    parser.add_argument('--sdk-csv')
    parser.add_argument('--sdk-root')
    parser.add_argument('--timeout', type=float, default=20)
    args = parser.parse_args()
    if bool(args.sdk_csv) != bool(args.sdk_root):
        parser.error('--sdk-csv and --sdk-root must be supplied together')
    if (pathlib.Path(args.output) / 'outcomes.jsonl').exists():
        parser.error('output already contains a census; choose a fresh directory')
    return run_census(args)


if __name__ == '__main__':
    raise SystemExit(main())
