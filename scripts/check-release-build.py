#!/usr/bin/env python3
"""Authorize revalidation of a draft only after its exact native builds passed."""
import json
import os
from pathlib import Path
import subprocess
import sys

run_id = sys.argv[1]
assert run_id.isdigit(), 'Expected a GitHub Actions run number'
run = json.loads(subprocess.check_output(['gh', 'run', 'view', run_id, '--json',
                                         'headSha,jobs,status,workflowName,event'], text=True))
assert run['workflowName'] == 'Release' and run['event'] == 'workflow_dispatch'
assert run['status'] == 'completed', 'Build run is still active'
required = {
    'checks / required-gate',
    'package (ubuntu-22.04, linux-x86_64)',
    'package (windows-2025, windows-x64)',
    'package (macos-15-intel, macos-x86_64)',
    'package (macos-14, macos-arm64)',
}
passed = {job['name'] for job in run['jobs'] if job['conclusion'] == 'success'}
assert required <= passed, 'Required build jobs did not pass: ' + str(required - passed)
version = subprocess.check_output([sys.executable, 'scripts/version.py'], text=True).strip()
release = json.loads(subprocess.check_output(['gh', 'release', 'view', 'v' + version,
                                              '--json', 'isDraft,targetCommitish'], text=True))
assert release['isDraft'], 'Only unpublished drafts can be resumed'
assert release['targetCommitish'] == run['headSha'], 'Draft does not match the tested build'
with Path(os.environ['GITHUB_OUTPUT']).open('a') as target:
    target.write('version=' + version + '\ncommit=' + run['headSha'] + '\n')
print('Verified native build gates for', run['headSha'], 'in run', run_id)
