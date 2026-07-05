import os
import argparse

import subprocess as sp

from pathlib import Path


parser = argparse.ArgumentParser()

parser.add_argument("--program", required=True, help="Name of tested program.")
parser.add_argument("output_dirs", nargs="+", help="Output directories to calculate branch coverage.")

args = parser.parse_args()

versions = {
    "bison": ["3.6", "3.7", "3.8"],
    "csplit": ["9.7", "9.8", "9.9"],
    "diff": ["3.10", "3.11", "3.12"],
    "du": ["9.7", "9.8", "9.9"],
    "find": ["4.8.0", "4.9.0", "4.10.0"],
    "gawk": ["5.3.0", "5.3.1", "5.3.2"],
    "grep": ["3.10", "3.11", "3.12"],
    "ls": ["9.7", "9.8", "9.9"],
    "m4": ["1.4.18", "1.4.19", "1.4.20"],
    "make": ["4.2", "4.3", "4.4"],
    "objcopy": ["2.43", "2.44", "2.45"],
    "objdump": ["2.43", "2.44", "2.45"],
    "patch": ["2.6", "2.7", "2.8"],
    "sqlite3": ["3.51.0", "3.51.1", "3.51.2"],
    "xorriso": ["1.5.2", "1.5.4", "1.5.6"],
}



def find_all(path, ends):
    # Search for files in the current directory and all sub-directories
    found = []
    for root, dirs, files in os.walk(path):
        for file in files:
            if file.endswith(f'.{ends}'):
                found.append(os.path.join(root, file))
    return found


def clear_gcov(g_path, depth=1):
    # Initialize ".gcda" and ".gcov" files
    for _ in range(depth):
        g_path = g_path[:g_path.rfind('/')]
    gcdas = find_all(g_path, "gcda")
    gcovs = find_all(g_path, "gcov")
    for gcda in gcdas:
        os.system(f"rm -f {gcda}")
    for gcov in gcovs:
        os.system(f"rm -f {gcov}")


running_dir = os.getcwd()
output_dirs = args.output_dirs
program = args.program
replay_bin = f"{running_dir}/../klee/build/bin/klee-replay"

if program in ["xorriso", "find"]:
    src = program
    depth = 1
elif program in ["gawk", "make", "sqlite3"]:
    src = ""
    depth = 0
else:
    src = "src"
    depth = 1

versions = [ver.replace('.', "_") for ver in versions[program]]
testcases = {ver : list() for ver in versions}
for output_dir in output_dirs:
    iterations = [f"{running_dir}/{output_dir}/{it}" for it in os.listdir(f"{running_dir}/{output_dir}") if it.startswith("iteration-")]
    for it in iterations:
        tcs = [f"{it}/{tc}" for tc in os.listdir(it) if tc.endswith(".ktest")]
        it_version = it.split("-")[-2]
        testcases[it_version] = testcases[it_version] + tcs

for ver, tcs in testcases.items():
    target = f"{running_dir}/../benchmarks/{program}/{ver.replace('_', '.')}/obj-gcov1/{src}/{program}"
    target = Path(target).absolute()
    target_dir = Path(target).parent
    original_path = Path().absolute()

    clear_gcov(str(target_dir))
    os.chdir(str(target.parent))
    for testcase in tcs:
        cmd = ' '.join([replay_bin, str(target), str(testcase)])
        process = sp.Popen(cmd, stdout=sp.PIPE, stderr=sp.PIPE, shell=True)
        try:
            _, stderr = process.communicate(timeout=0.1)
        except sp.TimeoutExpired:
            pass
        finally:
            process.kill()

    base = Path()
    for _ in range(depth):
        base = base / '..'
    gcda_pattern = base / '**/*.gcda'
    gcdas = list(target.parent.glob(str(gcda_pattern)))
    gcdas = [gcda.absolute() for gcda in gcdas]

    if len(gcdas) > 0:
        os.chdir(str(target_dir))
        cmd = ["gcov", '-b', *list(map(str, gcdas))]
        cmd = ' '.join(cmd)
        _ = sp.run(cmd, stdout=sp.PIPE, stderr=sp.PIPE, shell=True, check=True)

        base = Path()
        for _ in range(depth):
            if "gawk" in str(target) or "make" in str(target) or "sqlite" in str(target):
                pass
            else:
                base = base / '..'

        gcov_pattern = base / '**/*.gcov'
        gcovs = list(Path().glob(str(gcov_pattern)))
        covered = set()
        for gcov in gcovs:
            try:
                with gcov.open(encoding='UTF-8', errors='replace') as f:
                    file_name = f.readline().strip().split(':')[-1]
                    for i, line in enumerate(f):
                        if ('branch' in line) and ('never' not in line) and ('taken 0%' not in line) and (
                                ":" not in line) and ("returned 0% blocks executed 0%" not in line):
                            bid = f'{file_name} {i}'
                            covered.add(bid)
            except:
                pass
        os.chdir(str(original_path))
    else:
        covered = set()
    print(f"[INFO] ReuSE : Coverage of {program}-{ver.replace('_', '.')} : {len(covered)}")
print(f"[INFO] ReuSE : Calculation done.")
