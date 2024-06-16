import json
import subprocess
import itertools
import textwrap
from pathlib import Path
import tempfile
import hashlib
import datetime

from . import environment
from .environment import RelengEnvironment
from . import keys
from . import docker
from .version import VERSION, RELEASE_NAME, MAJOR
from .gitutils import verify_are_on_tag, git_preconditions
from . import release_notes

$RAISE_SUBPROC_ERROR = True
$XONSH_SHOW_TRACEBACK = True

GCROOTS_DIR = Path('./release/gcroots')
BUILT_GCROOTS_DIR = Path('./release/gcroots-build')
DRVS_TXT = Path('./release/drvs.txt')
ARTIFACTS = Path('./release/artifacts')
MANUAL = Path('./release/manual')

RELENG_MSG = "Release created with releng/create_release.xsh"

BUILD_CORES = 16
MAX_JOBS = 2


def setup_creds(env: RelengEnvironment):
    key = keys.get_ephemeral_key(env)
    $AWS_SECRET_ACCESS_KEY = key.secret_key
    $AWS_ACCESS_KEY_ID = key.id
    $AWS_DEFAULT_REGION = 'garage'
    $AWS_ENDPOINT_URL = environment.S3_ENDPOINT


def official_release_commit_tag(force_tag=False):
    print('[+] Setting officialRelease in flake.nix and tagging')
    prev_branch = $(git symbolic-ref --short HEAD).strip()

    git switch --detach
    sed -i 's/officialRelease = false/officialRelease = true/' flake.nix
    git add flake.nix
    message = f'release: {VERSION} "{RELEASE_NAME}"\n\nRelease produced with releng/create_release.xsh'
    git commit -m @(message)
    git tag @(['-f'] if force_tag else []) -a -m @(message) @(VERSION)

    return prev_branch


def merge_to_release(prev_branch):
    git switch @(prev_branch)
    # Create a merge back into the release branch so that git tools understand
    # that the release branch contains the tag, without the release commit
    # actually influencing the tree.
    merge_msg = textwrap.dedent("""\
        release: merge release {VERSION} back to mainline

        This merge commit returns to the previous state prior to the release but leaves the tag in the branch history.
        {RELENG_MSG}
    """).format(VERSION=VERSION, RELENG_MSG=RELENG_MSG)
    git merge -m @(merge_msg) -s ours @(VERSION)


def realise(paths: list[str]):
    args = [
        '--realise',
        '--max-jobs',
        MAX_JOBS,
        '--cores',
        BUILD_CORES,
        '--log-format',
        'bar-with-logs',
        '--add-root',
        BUILT_GCROOTS_DIR
    ]
    nix-store @(args) @(paths)


def eval_jobs(build_profile):
    nej_output = $(nix-eval-jobs --workers 4 --gc-roots-dir @(GCROOTS_DIR) --force-recurse --flake f'.#release-jobs.{build_profile}')
    return [json.loads(s) for s in nej_output.strip().split('\n')]


def upload_drv_paths_and_outputs(env: RelengEnvironment, paths: list[str]):
    proc = subprocess.Popen([
            'nix',
            'copy',
            '-v',
            '--to',
            env.cache_store_uri(),
            '--stdin',
        ],
        stdin=subprocess.PIPE,
        env=__xonsh__.env.detype(),
    )

    proc.stdin.write('\n'.join(itertools.chain(paths, x + '^*' for x in paths)).encode())
    proc.stdin.close()
    rv = proc.wait()
    if rv != 0:
        raise subprocess.CalledProcessError(rv, proc.args)


def make_manifest(eval_result):
    manifest = {vs['system']: vs['outputs']['out'] for vs in eval_result}
    def manifest_line(system, out):
        return f'  {system} = "{out}";'

    manifest_text = textwrap.dedent("""\
        # This file was generated by releng/create_release.xsh in Lix
        {{
          {lines}
        }}
    """).format(lines='\n'.join(manifest_line(s, p) for (s, p) in manifest.items()))

    return manifest_text


def make_git_tarball(to: Path):
    git archive --verbose --prefix=lix-@(VERSION)/ --format=tar.gz -o @(to) @(VERSION)


def confirm(prompt, expected):
    resp = input(prompt)

    if resp != expected:
        raise ValueError('Unconfirmed')


def sha256_file(f: Path):
    hasher = hashlib.sha256()

    with open(f, 'rb') as h:
        while data := h.read(1024 * 1024):
            hasher.update(data)

    return hasher.hexdigest()


def make_artifacts_dir(eval_result, d: Path):
    d.mkdir(exist_ok=True, parents=True)
    version_dir = d / 'lix' / f'lix-{VERSION}'
    version_dir.mkdir(exist_ok=True, parents=True)

    tarballs_drv = next(p for p in eval_result if p['attr'] == 'tarballs')
    cp --no-preserve=mode -r @(tarballs_drv['outputs']['out'])/* @(version_dir)

    # FIXME: upgrade-nix searches for manifest.nix at root, which is rather annoying
    with open(d / 'manifest.nix', 'w') as h:
        h.write(make_manifest(eval_result))

    with open(version_dir / 'manifest.nix', 'w') as h:
        h.write(make_manifest(eval_result))

    print('[+] Make sources tarball')

    filename = f'lix-{VERSION}.tar.gz'
    git_tarball = version_dir / filename
    make_git_tarball(git_tarball)

    file_hash = sha256_file(git_tarball)

    print(f'Hash: {file_hash}')
    with open(version_dir / f'{filename}.sha256', 'w') as h:
        h.write(file_hash)


def prepare_release_notes():
    rl_path = release_notes.build_release_notes_to_file()

    commit_msg = textwrap.dedent("""\
        release: release notes for {VERSION}

        {RELENG_MSG}
    """).format(VERSION=VERSION, RELENG_MSG=RELENG_MSG)

    git add @(rl_path) @(release_notes.SUMMARY)
    git rm --ignore-unmatch 'doc/manual/rl-next/*.md'

    git commit -m @(commit_msg)


def upload_artifacts(env: RelengEnvironment, noconfirm=False, no_check_git=False, force_push_tag=False):
    if not no_check_git:
        verify_are_on_tag()
        git_preconditions()
    assert 'AWS_SECRET_ACCESS_KEY' in __xonsh__.env

    tree @(ARTIFACTS)

    env_part = f'environment {env.name}'
    not noconfirm and confirm(
        f'Would you like to release {ARTIFACTS} as {VERSION} in {env.colour(env_part)}? Type "I want to release this to {env.name}" to confirm\n',
        f'I want to release this to {env.name}'
    )

    docker_images = list((ARTIFACTS / f'lix/lix-{VERSION}').glob(f'lix-{VERSION}-docker-image-*.tar.gz'))
    assert docker_images

    print('[+] Upload to cache')
    with open(DRVS_TXT) as fh:
        upload_drv_paths_and_outputs(env, [x.strip() for x in fh.readlines() if x])

    print('[+] Upload docker images')
    for target in env.docker_targets:
        docker.upload_docker_images(target, docker_images)

    print('[+] Upload to release bucket')
    aws s3 cp --recursive @(ARTIFACTS)/ @(env.releases_bucket)/
    print('[+] Upload manual')
    upload_manual(env)

    print('[+] git push tag')
    git push @(['-f'] if force_push_tag else []) @(env.git_repo) f'{VERSION}:refs/tags/{VERSION}'


def do_tag_merge(force_tag=False, no_check_git=False):
    if not no_check_git:
        git_preconditions()
    prev_branch = official_release_commit_tag(force_tag=force_tag)
    merge_to_release(prev_branch)
    git switch --detach @(VERSION)


def build_manual(eval_result):
    (drv, manual) = next((x['drvPath'], x['outputs']['doc']) for x in eval_result if x['attr'] == 'build.x86_64-linux')
    print('[+] Building manual')
    realise([drv])

    cp --no-preserve=mode -T -vr @(manual)/share/doc/nix/manual @(MANUAL)


def upload_manual(env: RelengEnvironment):
    stable = json.loads($(nix eval --json '.#nix.officialRelease'))
    if stable:
        version = MAJOR
    else:
        version = 'nightly'

    print('[+] aws s3 sync manual')
    aws s3 sync @(MANUAL)/ @(env.docs_bucket)/manual/lix/@(version)/
    if stable:
        aws s3 sync @(MANUAL)/ @(env.docs_bucket)/manual/lix/stable/


def build_artifacts(build_profile, no_check_git=False):
    rm -rf release/
    if not no_check_git:
        verify_are_on_tag()
        git_preconditions()

    print('[+] Evaluating')
    eval_result = eval_jobs(build_profile)
    drv_paths = [x['drvPath'] for x in eval_result]

    print('[+] Building')
    realise(drv_paths)
    build_manual(eval_result)

    with open(DRVS_TXT, 'w') as fh:
        # don't bother putting the release tarballs themselves because they are duplicate and huge
        fh.write('\n'.join(x['drvPath'] for x in eval_result if x['attr'] != 'lix-release-tarballs'))

    make_artifacts_dir(eval_result, ARTIFACTS)
    print(f'[+] Done! See {ARTIFACTS}')
