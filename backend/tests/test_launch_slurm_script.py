"""scripts/launch_backend_slurm.sh: the site profiles and the job script.

The launcher is run for real, with stand-ins for sbatch, squeue, ssh and the
container runtime on PATH, so what is checked is the job script it writes: the
runtime it calls, the flags, the environment it hands the container. The job
script is then run once (against a stand-in runtime that records what it was
given) to check the translation to SINGULARITYENV_ and the variables kept
under --cleanenv. Nothing here needs SLURM or a container runtime.
"""
import os
import stat
import subprocess
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.skipif(sys.platform == "win32", reason="bash scripts")

REPO = Path(__file__).resolve().parents[2]
LAUNCHER = REPO / "scripts" / "launch_backend_slurm.sh"

STUBS = {
    # The job script is copied before the launcher removes it.
    "sbatch": 'cp "${@: -1}" "$STUB_OUT/job.sh"; printf "%s\\n" "$@" > "$STUB_OUT/sbatch.args"; echo 4242',
    "squeue": 'case "$*" in *%N*) echo lrdn0001;; *) echo 4242;; esac',
    "sacct": "true",
    "ssh": "exit 0",
    "srun": "exit 0",
    "hostname": 'echo "${STUB_HOSTNAME:-login.example.org}"',
    # `pull FILE REF` writes the file; `run ...` records its arguments and environment.
    "runtime": (
        'if [ "$1" = pull ]; then echo "$2 $3" >> "$STUB_OUT/pulls"; sleep "${STUB_PULL_SLEEP:-0}"; echo sif > "$2"; exit 0; fi\n'
        'printf "%s\\n" "$@" > "$STUB_OUT/run.args"; env > "$STUB_OUT/run.env"'
    ),
}


def _install(bindir: Path, runtimes, stubs=None):
    bindir.mkdir()
    for name, body in {**STUBS, **(stubs or {})}.items():
        for n in runtimes if name == "runtime" else [name]:
            f = bindir / n
            f.write_text("#!/bin/bash\n" + body + "\n")
            f.chmod(f.stat().st_mode | stat.S_IEXEC)


@pytest.fixture
def run_launcher(tmp_path):
    def run(*args, runtimes=("apptainer",), env=None, stubs=None):
        out = tmp_path / "out"
        out.mkdir(exist_ok=True)
        bindir = tmp_path / f"bin{len(list(tmp_path.glob('bin*')))}"
        _install(bindir, runtimes, stubs)
        home = tmp_path / "home"
        home.mkdir(exist_ok=True)
        sif = tmp_path / "backend.sif"
        sif.write_text("sif")
        e = {
            "PATH": f"{bindir}:{os.environ['PATH']}",
            "HOME": str(home),  # the launcher writes ~/.visivo_token without a workspace
            "STUB_OUT": str(out),
            "VISIVO_ASSIGN_TIMEOUT": "4",
        }
        e.update(env or {})
        argv = ["bash", str(LAUNCHER)]
        if "--image" not in args:
            argv += ["--sif", str(sif)]
        r = subprocess.run(argv + list(args), env=e, capture_output=True, text=True, timeout=60)
        return r, out

    return run


def _job(out: Path) -> str:
    return (out / "job.sh").read_text()


def _run_job(tmp_path: Path, out: Path, extra_env=None):
    """Run the written job script against the stand-in runtime; return (argv, environment seen)."""
    bindir = next(tmp_path.glob("bin*"))
    env = {
        "PATH": f"{bindir}:{os.environ['PATH']}",
        "HOME": str(tmp_path / "home"),
        "STUB_OUT": str(out),
        "SLURM_JOB_ID": "4242",
        "REQUESTS_CA_BUNDLE": "/host/ca.pem",
    }
    env.update(extra_env or {})
    p = subprocess.run(["bash", str(out / "job.sh")], env=env, capture_output=True, text=True, timeout=60)
    assert p.returncode == 0, p.stdout + p.stderr
    args = (out / "run.args").read_text().split("\n")
    seen = dict(
        line.split("=", 1) for line in (out / "run.env").read_text().splitlines() if "=" in line
    )
    return args, seen


def test_generic_site_keeps_the_pleiadi_behaviour(run_launcher):
    r, out = run_launcher("--site", "generic")
    assert r.returncode == 0, r.stdout + r.stderr
    job = _job(out)
    assert "#SBATCH --partition=gpu\n" in job
    assert "apptainer run --nv  " in job            # no --cleanenv
    assert "cleanenv" not in job
    assert "export APPTAINERENV_LD_LIBRARY_PATH=/usr/lib64" in job
    assert "EGL_BIND=(" not in job
    assert "Log file:" in r.stdout


def test_pleiadi_site_is_the_generic_one(run_launcher):
    r, out = run_launcher("--site", "pleiadi")
    assert r.returncode == 0, r.stdout + r.stderr
    assert "#SBATCH --partition=gpu\n" in _job(out)
    assert "cleanenv" not in _job(out)


def test_leonardo_profile(run_launcher, tmp_path):
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    r, out = run_launcher(
        "--site", "leonardo", "--account", "ACCT_X", "--qos", "boost_qos_dbg",
        runtimes=("singularity",),
        env={"CINECA_SCRATCH": str(scratch)},
    )
    assert r.returncode == 0, r.stdout + r.stderr
    job = _job(out)
    real = scratch.resolve()
    sbatch_args = (out / "sbatch.args").read_text()
    assert "#SBATCH --partition=boost_usr_prod\n" in job
    assert "--account=ACCT_X" in sbatch_args
    assert "--qos=boost_qos_dbg" in sbatch_args
    assert "singularity run --nv --cleanenv" in job   # the runtime found, not apptainer
    assert "apptainer run" not in job
    assert "export APPTAINERENV_LD_LIBRARY_PATH" not in job
    assert "EGL_BIND=(--bind /usr/share/glvnd/egl_vendor.d:/usr/share/glvnd/egl_vendor.d)" in job
    assert '"${EGL_BIND[@]}"' in job
    assert "10_nvidia.json" in job                    # the vendor file the env var names
    # Logs, token and caches on scratch, and the data root defaults there.
    assert f"APPTAINERENV_VISIVO_DATA_ROOT={real}\n" in job
    assert f"APPTAINERENV_VISIVO_EXPORTS_DIR={real}/visivo/exports" in job
    assert (real / "visivo" / "token").is_file()
    assert f"Log file:      {real}/visivo/logs/visivo_backend_4242.log" in r.stdout
    # The account is an sbatch option, not part of the job script.
    assert "ACCT_X" not in job


def test_leonardo_needs_an_account(run_launcher):
    r, _ = run_launcher("--site", "leonardo")
    assert r.returncode == 1
    assert "--account" in r.stderr


def test_site_is_detected_from_the_host_name(run_launcher, tmp_path):
    r, out = run_launcher(
        "--account", "A", env={"STUB_HOSTNAME": "login01.leonardo.local", "WORK": str(tmp_path)}
    )
    assert r.returncode == 0, r.stdout + r.stderr
    assert "Site profile: leonardo (detected)" in r.stdout
    assert "boost_usr_prod" in _job(out)


@pytest.mark.parametrize("host", [
    "leonardo-dev.example.org", "my-leonardo-box", "login.example.org",
])
def test_other_hosts_are_generic(run_launcher, host):
    r, out = run_launcher("--account", "A", env={"STUB_HOSTNAME": host})
    assert r.returncode == 0, r.stdout + r.stderr
    assert "Site profile: generic (detected)" in r.stdout
    assert "boost_usr_prod" not in _job(out)


@pytest.mark.parametrize("host", [
    "login01-ext.leonardo.cineca.it", "LOGIN02.LEONARDO.LOCAL", "lrdn0042",
])
def test_cineca_leonardo_hosts_are_detected(run_launcher, tmp_path, host):
    r, _ = run_launcher("--account", "A", env={"STUB_HOSTNAME": host, "WORK": str(tmp_path)})
    assert r.returncode == 0, r.stdout + r.stderr
    assert "Site profile: leonardo (detected)" in r.stdout


def test_options_beat_the_profile(run_launcher, tmp_path):
    data = tmp_path / "data"
    data.mkdir()
    r, out = run_launcher(
        "--site", "leonardo", "--account", "A", "--partition", "lrd_all_viz", "--no-cleanenv",
        "--data-root", str(data), "--workspace", str(tmp_path / "ws"),
        env={"CINECA_SCRATCH": str(tmp_path)},
    )
    assert r.returncode == 0, r.stdout + r.stderr
    job = _job(out)
    assert "#SBATCH --partition=lrd_all_viz\n" in job
    assert "cleanenv" not in job
    assert f"APPTAINERENV_VISIVO_DATA_ROOT={data}\n" in job
    assert (tmp_path / "ws" / "token").is_file()


def test_unknown_site_is_refused(run_launcher):
    r, _ = run_launcher("--site", "nowhere")
    assert r.returncode == 1 and "unknown site" in r.stderr


def test_image_is_pulled_once_to_the_workspace(run_launcher, tmp_path):
    ref = "docker://ghcr.io/example/visivo-backend:1.2.3-egl"
    ws = tmp_path / "ws"
    r, out = run_launcher("--image", ref, "--workspace", str(ws))
    assert r.returncode == 0, r.stdout + r.stderr
    pulls = (out / "pulls").read_text().splitlines()
    assert len(pulls) == 1 and pulls[0].endswith(ref)
    sif = next((ws / "images").glob("*.sif"))
    assert str(sif) in _job(out)
    assert not list((ws / "images").glob("*.pulling.*"))
    # A second run reuses it.
    r2, out2 = run_launcher("--image", ref, "--workspace", str(ws))
    assert r2.returncode == 0, r2.stdout + r2.stderr
    assert len((out2 / "pulls").read_text().splitlines()) == 1
    assert "already pulled" in r2.stdout


def test_backend_src_is_named_and_checked(run_launcher, tmp_path):
    app = tmp_path / "app"
    app.mkdir()
    (app / "__init__.py").write_text("")
    r, out = run_launcher("--backend-src", str(app))
    assert r.returncode == 0, r.stdout + r.stderr
    assert f"Overlaying /app/app with host source: {app.resolve()}" in r.stdout
    assert f"--bind {app.resolve()}:/app/app" in _job(out)
    assert str(app.resolve()) in _job(out).split("Backend source:")[1].split("\n")[0]
    # backend/ instead of backend/app would break the image's package.
    bad = tmp_path / "backend"
    bad.mkdir()
    r2, _ = run_launcher("--backend-src", str(bad))
    assert r2.returncode == 1 and "backend/app" in r2.stderr


def test_job_script_runs_with_singularity_and_cleanenv(run_launcher, tmp_path):
    """Run the written job script against a stand-in runtime."""
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    r, out = run_launcher(
        "--site", "leonardo", "--account", "A", "--token", "tok123",
        runtimes=("singularity",), env={"CINECA_SCRATCH": str(scratch)},
    )
    assert r.returncode == 0, r.stdout + r.stderr
    args, seen = _run_job(tmp_path, out, {
        "SLURM_MEM_PER_NODE": "65536",
        "CUDA_VISIBLE_DEVICES": "0",
        "APPTAINERENV_VISIVO_RENDER_MAX_SESSIONS": "4",   # an operator's own setting
    })
    assert args[0] == "run" and "--cleanenv" in args and "--nv" in args
    assert seen["SINGULARITYENV_VISIVO_TOKEN"] == "tok123"
    assert seen["SINGULARITYENV_PORT"] == "8000"
    assert seen["SINGULARITYENV_SLURM_JOB_ID"] == "4242"
    assert seen["SINGULARITYENV_SLURM_MEM_PER_NODE"] == "65536"
    assert seen["SINGULARITYENV_CUDA_VISIBLE_DEVICES"] == "0"
    assert seen["SINGULARITYENV_VISIVO_RENDER_MAX_SESSIONS"] == "4"
    assert seen["SINGULARITYENV___EGL_VENDOR_LIBRARY_FILENAMES"].endswith("10_nvidia.json")
    # The host's own variables are not forwarded by name.
    assert "SINGULARITYENV_REQUESTS_CA_BUNDLE" not in seen


def test_leonardo_without_scratch_variables_warns(run_launcher):
    # VISIVO_LOGIN_SHELL marks the re-run under a login shell as already done.
    r, out = run_launcher(
        "--site", "leonardo", "--account", "A",
        env={"VISIVO_LOGIN_SHELL": "1", "CINECA_SCRATCH": "", "SCRATCH": "", "WORK": ""},
    )
    assert r.returncode == 0, r.stdout + r.stderr
    assert "no scratch area found" in r.stderr


def test_cleanenv_forwards_the_operators_visivo_settings(run_launcher, tmp_path):
    """VISIVO_* and VTK_EGL_DEVICE_INDEX exported at submit time reach the container, plainly named."""
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    r, out = run_launcher(
        "--site", "leonardo", "--account", "A", "--token", "tok123",
        runtimes=("singularity",),
        env={
            "CINECA_SCRATCH": str(scratch),
            "VISIVO_RENDER_MAX_SESSIONS": "3",
            "VISIVO_HEAVY_TIMEOUT_S": "90",
            "VISIVO_LLM_URL": "http://llm host/v1 ; echo $HOME",   # quoting survives
            "VTK_EGL_DEVICE_INDEX": "1",
            "VISIVO_SITE": "leonardo",                             # the launcher's own option
        },
    )
    assert r.returncode == 0, r.stdout + r.stderr
    # The values are in the script, so the job's own environment is not needed.
    args, seen = _run_job(tmp_path, out)
    assert "--cleanenv" in args
    assert seen["SINGULARITYENV_VISIVO_RENDER_MAX_SESSIONS"] == "3"
    assert seen["SINGULARITYENV_VISIVO_HEAVY_TIMEOUT_S"] == "90"
    assert seen["SINGULARITYENV_VISIVO_LLM_URL"] == "http://llm host/v1 ; echo $HOME"
    assert seen["SINGULARITYENV_VTK_EGL_DEVICE_INDEX"] == "1"
    assert seen["SINGULARITYENV_VISIVO_TOKEN"] == "tok123"        # the launcher's token
    assert "SINGULARITYENV_VISIVO_SITE" not in seen
    assert "SINGULARITYENV_REQUESTS_CA_BUNDLE" not in seen


@pytest.mark.parametrize("site,runtime,cleanenv", [
    ("leonardo", "singularity", True),
    ("pleiadi", "apptainer", False),
    ("generic", "apptainer", False),
])
def test_cleanenv_flag_follows_the_site(run_launcher, tmp_path, site, runtime, cleanenv):
    r, out = run_launcher(
        "--site", site, "--account", "A", runtimes=(runtime,),
        env={"CINECA_SCRATCH": str(tmp_path), "VISIVO_RENDER_MAX_SESSIONS": "3"},
    )
    assert r.returncode == 0, r.stdout + r.stderr
    args, _ = _run_job(tmp_path, out)
    assert ("--cleanenv" in args) is cleanenv


def test_other_sites_job_script_has_no_forwarding_block(run_launcher):
    r, out = run_launcher("--site", "generic", env={"VISIVO_RENDER_MAX_SESSIONS": "3"})
    assert r.returncode == 0, r.stdout + r.stderr
    assert "APPTAINERENV_VISIVO_RENDER_MAX_SESSIONS" not in _job(out)


# ── The launcher as the client runs it: from standard input ──────────────────

def _bare_env(tmp_path, **extra):
    home = tmp_path / "home"
    home.mkdir(exist_ok=True)
    e = {"PATH": os.environ["PATH"], "HOME": str(home), "VISIVO_ASSIGN_TIMEOUT": "4"}
    e.update(extra)
    return e


def test_stdin_mode_needs_an_image(tmp_path):
    """From a temporary copy marked as stdin mode, the repository's default image is not looked for."""
    copy = tmp_path / "tmp" / "visivo-launcher.abc"
    copy.parent.mkdir()
    copy.write_text(LAUNCHER.read_text())
    # A default image that would be found if the path were derived from the script's place.
    (tmp_path / "apptainer").mkdir()
    (tmp_path / "apptainer" / "backend.sif").write_text("sif")
    r = subprocess.run(["bash", str(copy), "--site", "generic"],
                       env=_bare_env(tmp_path, VISIVO_LAUNCHER_STDIN="1"),
                       capture_output=True, text=True, timeout=30)
    assert r.returncode == 1 and "no container image" in r.stderr, r.stdout + r.stderr


def test_script_from_a_bare_pipe_needs_an_image(tmp_path):
    """`bash -s` (the script on stdin, $0 = bash) is stdin mode without any marker."""
    r = subprocess.run(["bash", "-s", "--", "--site", "generic"], input=LAUNCHER.read_text(),
                       env=_bare_env(tmp_path), capture_output=True, text=True, timeout=30)
    assert r.returncode == 1 and "no container image" in r.stderr, r.stdout + r.stderr


def test_a_clone_run_keeps_its_default_image_path(run_launcher, tmp_path):
    """The developer path: the script's own place is searched for apptainer/backend.sif, as before."""
    r = subprocess.run(["bash", str(LAUNCHER), "--site", "generic"],
                       env=_bare_env(tmp_path), capture_output=True, text=True, timeout=30)
    assert r.returncode == 1 and "container image not found" in r.stdout
    assert "apptainer/backend.sif" in r.stdout


def test_leonardo_reruns_a_temporary_copy_under_a_login_shell(tmp_path):
    """No scratch variables over ssh: the copy the client runs is run again with `bash -l`, which reads them."""
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    home = tmp_path / "home"
    home.mkdir()
    (home / ".bash_profile").write_text(f"export CINECA_SCRATCH={scratch}\n")
    copy = tmp_path / "visivo-launcher.xyz"
    copy.write_text(LAUNCHER.read_text())
    bindir = tmp_path / "bin"
    _install(bindir, ("singularity",))
    sif = tmp_path / "backend.sif"
    sif.write_text("sif")
    out = tmp_path / "out"
    out.mkdir()
    env = {"PATH": f"{bindir}:{os.environ['PATH']}", "HOME": str(home), "STUB_OUT": str(out),
           "VISIVO_ASSIGN_TIMEOUT": "4", "VISIVO_LAUNCHER_STDIN": "1"}
    r = subprocess.run(["bash", str(copy), "--site", "leonardo", "--account", "A", "--sif", str(sif)],
                       env=env, capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stdout + r.stderr
    assert f"Scratch area: {scratch.resolve()}" in r.stdout
    assert "no scratch area found" not in r.stderr


def test_a_bare_pipe_cannot_be_rerun_and_says_so(tmp_path):
    scratch_less = _bare_env(tmp_path, CINECA_SCRATCH="", SCRATCH="", WORK="")
    r = subprocess.run(["bash", "-s", "--", "--site", "leonardo", "--account", "A", "--sif", "/nonexistent"],
                       input=LAUNCHER.read_text(), env=scratch_less, capture_output=True, text=True, timeout=30)
    assert "is not a file" in r.stderr
    assert "no scratch area found" in r.stderr


# ── The image: where it goes, and how its pull is shown ──────────────────────

def test_image_dir_is_where_the_image_goes(run_launcher, tmp_path):
    shared = tmp_path / "shared"
    ref = "docker://ghcr.io/example/visivo-backend:1.2.3-egl"
    r, out = run_launcher("--image", ref, "--workspace", str(tmp_path / "ws"), "--image-dir", str(shared))
    assert r.returncode == 0, r.stdout + r.stderr
    assert next(shared.glob("*.sif"))
    assert not (tmp_path / "ws" / "images").exists()


def test_leonardo_pulls_into_the_projects_work_area(run_launcher, tmp_path):
    scratch = tmp_path / "scratch"
    work = tmp_path / "work"
    scratch.mkdir()
    work.mkdir()
    ref = "docker://ghcr.io/example/visivo-backend:1.2.3-egl"
    r, out = run_launcher("--site", "leonardo", "--account", "A", "--image", ref, runtimes=("singularity",),
                          env={"CINECA_SCRATCH": str(scratch), "WORK": str(work)})
    assert r.returncode == 0, r.stdout + r.stderr
    assert next((work / "visivo" / "images").glob("*.sif"))
    assert not (scratch / "visivo" / "images").exists()


def test_the_pull_reports_its_progress(run_launcher, tmp_path):
    ref = "docker://ghcr.io/example/visivo-backend:1.2.3-egl"
    r, out = run_launcher("--image", ref, "--workspace", str(tmp_path / "ws"),
                          env={"STUB_PULL_SLEEP": "3", "VISIVO_PULL_REPORT_EVERY": "1"})
    assert r.returncode == 0, r.stdout + r.stderr
    assert "[visivo] Pull: 1 s" in r.stdout or "[visivo] Pull: 2 s" in r.stdout
    assert "[visivo] Pull: done in" in r.stdout


# ── Waiting in the queue ──────────────────────────────────────────────────────

# PENDING (with a reason) for the first `STUB_PENDING` polls, then a node.
QUEUE_STUBS = {
    "squeue": (
        'case "$*" in *%N*) n=$(cat "$STUB_OUT/n" 2>/dev/null || echo 0); n=$((n+1)); echo $n > "$STUB_OUT/n";'
        ' if [ "$n" -le "${STUB_PENDING:-0}" ]; then echo "|PENDING|Priority"; else echo "lrdn0001|RUNNING|None"; fi;;'
        ' *) echo 4242;; esac'
    ),
}


def test_the_queue_state_and_reason_are_reported(run_launcher):
    r, out = run_launcher("--assign-timeout", "30", stubs=QUEUE_STUBS, env={"STUB_PENDING": "3"})
    assert r.returncode == 0, r.stdout + r.stderr
    lines = [ln for ln in r.stdout.splitlines() if "Queue:" in ln]
    # Said when it changes, not at every poll.
    assert len(lines) == 1 and "PENDING (Priority)" in lines[0]
    assert "Compute node: lrdn0001" in r.stdout


def test_no_node_within_the_assign_timeout_is_an_error_that_names_the_job(run_launcher):
    r, _ = run_launcher("--assign-timeout", "3", stubs=QUEUE_STUBS, env={"STUB_PENDING": "1000"})
    assert r.returncode == 1
    assert "no compute node assigned to job 4242 within 3 s" in r.stderr
    assert "scancel 4242" in r.stderr


def test_the_assign_timeout_zero_waits_for_the_node(run_launcher):
    r, _ = run_launcher("--assign-timeout", "0", stubs=QUEUE_STUBS, env={"STUB_PENDING": "3"})
    assert r.returncode == 0, r.stdout + r.stderr


def test_the_timeouts_must_be_numbers(run_launcher):
    r, _ = run_launcher("--assign-timeout", "soon")
    assert r.returncode == 1 and "ASSIGN_TIMEOUT must be a whole number" in r.stderr


def test_the_ready_timeout_is_an_option(run_launcher):
    r, _ = run_launcher("--ready-timeout", "5", stubs={"ssh": "exit 1"})
    assert r.returncode == 1 and "did not become ready within 5 seconds" in r.stdout


def test_a_job_that_vanishes_ends_even_without_a_time_limit(tmp_path):
    """No queue entry and no accounting record: a minute of it (shortened here) ends the wait."""
    copy = tmp_path / "launcher.sh"
    copy.write_text(LAUNCHER.read_text().replace("SECONDS - GONE_SINCE >= 60", "SECONDS - GONE_SINCE >= 2"))
    bindir = tmp_path / "bin"
    _install(bindir, ("apptainer",), {"squeue": 'case "$*" in *%N*) ;; *) echo 4242;; esac'})
    out = tmp_path / "out"
    out.mkdir()
    sif = tmp_path / "backend.sif"
    sif.write_text("sif")
    env = _bare_env(tmp_path, STUB_OUT=str(out))
    env["PATH"] = f"{bindir}:{os.environ['PATH']}"
    r = subprocess.run(["bash", str(copy), "--sif", str(sif), "--assign-timeout", "0"],
                       env=env, capture_output=True, text=True, timeout=60)
    assert r.returncode == 1 and "no longer in the queue" in r.stderr, r.stdout + r.stderr
