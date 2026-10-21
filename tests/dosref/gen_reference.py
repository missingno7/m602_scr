#!/usr/bin/env python3
"""Regenerate the DOS reference traces in tests/dosref/*.trace.

Compiles the reconstructed SAVER.PAS from the m602_recon matching
decompilation, unchanged, with Turbo Pascal 6.01 together with SAVENV.PAS and
MOVEREF.PAS from this directory, runs it under MS-DOS Player and stores one
trace per scenario.  Work directory: build/dosref.

    python tests/dosref/gen_reference.py <m602_recon> [--tp DIR] [--msdos EXE]
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

# name: width, frames, initial StepY, initial Cycle, Randomize time h m s hund
SCENARIOS = {
    "w80": (80, 2000, 0, 1, 13, 7, 42, 17),
    "w107": (107, 2000, 0, 1, 8, 30, 5, 99),
    "w140_stepy1_cycle0": (140, 1500, 1, 0, 23, 59, 59, 99),
    "w41_stepy1_cycle3": (41, 800, 1, 3, 0, 0, 0, 0),
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("recon", type=Path, help="m602_recon checkout (reads src/SAVER.PAS)")
    ap.add_argument("--tp", type=Path, default=Path(r"C:\tools\tp601"), help="Turbo Pascal 6.01 directory")
    ap.add_argument("--msdos", type=Path, default=Path(r"C:\tools\nmlgcdos\msdos.exe"), help="MS-DOS Player")
    args = ap.parse_args()

    work = ROOT / "build" / "dosref"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    shutil.copy(args.recon / "src" / "SAVER.PAS", work)
    for name in ("SAVENV.PAS", "MOVEREF.PAS"):
        shutil.copy(HERE / name, work)

    def run(*command: str) -> str:
        proc = subprocess.run([str(args.msdos), "-e", *command], cwd=work, stdin=subprocess.DEVNULL,
                              env=dict(os.environ, MSDOS_PATH=str(args.tp)),
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
        text = proc.stdout.decode("latin-1")
        if proc.returncode != 0:
            raise SystemExit(f"{command[0]} failed ({proc.returncode}):\n{text}")
        return text

    log = run(str(args.tp / "TPC.EXE"), "/M", f"/U{args.tp / 'tv'}", "MOVEREF.PAS")
    if not (work / "MOVEREF.EXE").exists():
        raise SystemExit(log)

    for name, params in SCENARIOS.items():
        lines = [line for line in run("MOVEREF.EXE", *map(str, params)).splitlines() if line.strip()]
        if len(lines) != params[1]:
            raise SystemExit(f"{name}: expected {params[1]} frames, got {len(lines)}")
        header = "# " + " ".join(map(str, params)) + \
                 "  (width frames stepy cycle hour minute second hundredths)\n"
        (HERE / f"{name}.trace").write_text(header + "\n".join(lines) + "\n", encoding="ascii")
        print(f"{name}: {len(lines)} frames")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
