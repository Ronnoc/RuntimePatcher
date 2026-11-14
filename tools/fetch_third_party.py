#!/usr/bin/env python3
"""Fetches the pinned header-only dependencies used to cross-compile the subsdk9 module.

  third_party/musl          musl v1.2.5 (libc headers)
  third_party/llvm-project  llvm 23.1.5, sparse: libcxx/include, libcxx/src, libcxx/vendor

zstd for the host (CMake) build is fetched by CMake itself (FetchContent).
"""
import os
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "third_party")


def run(*cmd):
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)


def main():
    os.makedirs(ROOT, exist_ok=True)
    musl = os.path.join(ROOT, "musl")
    if not os.path.isdir(musl):
        run("git", "clone", "--depth", "1", "--branch", "v1.2.5", "https://git.musl-libc.org/git/musl", musl)
    llvm = os.path.join(ROOT, "llvm-project")
    if not os.path.isdir(llvm):
        run("git", "clone", "--depth", "1", "--filter=blob:none", "--sparse", "--branch", "llvmorg-23.1.5",
            "https://github.com/llvm/llvm-project.git", llvm)
        run("git", "-C", llvm, "sparse-checkout", "set", "libcxx/include", "libcxx/src", "libcxx/vendor")
    print("[+] third_party ready")


if __name__ == "__main__":
    sys.exit(main())
