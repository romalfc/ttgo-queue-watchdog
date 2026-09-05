Import("env")

import subprocess


def get_build_hash():
    project_dir = env.subst("$PROJECT_DIR")
    try:
        commit_hash = subprocess.check_output(
            ["git", "rev-parse", "--short=12", "HEAD"],
            cwd=project_dir,
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
        dirty = subprocess.call(
            ["git", "diff", "--quiet", "--ignore-submodules", "HEAD", "--"],
            cwd=project_dir,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ) != 0
        return commit_hash + ("-dirty" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


build_hash = get_build_hash()
env.AppendUnique(CPPDEFINES=[("BUILD_HASH", '\\"%s\\"' % build_hash)])
print("Build hash: %s" % build_hash)