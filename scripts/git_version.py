"""Injection de la version Git dans include/git_version.h (interface_base.md §7.1).

Extrait `git describe --tags --always --dirty` et génère le header GIT_VERSION.
Le fichier est régénéré uniquement si son contenu change (builds incrémentaux).
"""
import os
import subprocess

Import("env")

PROJECT_DIR = env["PROJECT_DIR"]
HEADER_PATH = os.path.join(PROJECT_DIR, "include", "git_version.h")
FALLBACK = "v0.1.0-dev"


def _git_version():
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=PROJECT_DIR,
            stderr=subprocess.DEVNULL,
            timeout=5,
        )
        version = out.decode("utf-8").strip()
        return version if version else FALLBACK
    except Exception:
        return FALLBACK


def _build_header(version):
    return (
        "/* Généré au build par scripts/git_version.py — NE PAS ÉDITER. */\n"
        "#ifndef GIT_VERSION_H\n"
        "#define GIT_VERSION_H\n"
        '#define GIT_VERSION "%s"\n'
        "#endif /* GIT_VERSION_H */\n" % version
    )


version = _git_version()
content = _build_header(version)

if not os.path.exists(HEADER_PATH):
    with open(HEADER_PATH, "w") as f:
        f.write(content)
    print("Version Git : %s (header généré)" % version)
else:
    with open(HEADER_PATH, "r") as f:
        if f.read() != content:
            with open(HEADER_PATH, "w") as f:
                f.write(content)
            print("Version Git : %s (header mis à jour)" % version)
