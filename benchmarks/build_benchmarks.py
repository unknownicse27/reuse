import os
import json
import argparse

versions = {
    "bison" : ["3.6", "3.7", "3.8"],    "csplit"  : ["9.7", "9.8", "9.9"],          "diff"    : ["3.10", "3.11", "3.12"],
    "du"    : ["9.7", "9.8", "9.9"],    "find"    : ["4.8.0", "4.9.0", "4.10.0"],   "gawk"    : ["5.3.0", "5.3.1", "5.3.2"], 
    "grep"  : ["3.10", "3.11", "3.12"], "ls"      : ["9.7", "9.8", "9.9"],          "m4"      : ["1.4.18", "1.4.19", "1.4.20"], 
    "make"  : ["4.2", "4.3", "4.4"],    "objcopy" : ["2.43", "2.44", "2.45"],       "objdump" : ["2.43", "2.44", "2.45"],
    "patch" : ["2.6", "2.7", "2.8"],    "sqlite3" : ["3.51.0", "3.51.1", "3.51.2"], "xorriso" : ["1.5.2", "1.5.4", "1.5.6"]
}


def unzip_tar_xz(src_file):
    os.system(f"tar -xf {src_file}")
    src_dir = src_file.replace(".tar.xz", "")
    return src_dir

def unzip_tar_gz(src_file):
    os.system(f"tar -zxvf {src_file}")
    src_dir = src_file.replace(".tar.gz", "")
    return src_dir

def build_sqlite(running_dir, benchmark, n_cores):
    with open(f"{running_dir}/sqlite_config.json", "r") as sqlite_f:
        sqlite_config = json.load(sqlite_f)
    os.chdir(f"{running_dir}/{benchmark}")
    for ver, config in sqlite_config.items():
        os.system(f"./build_sqlite.sh --year {config['year']} --amalgamation {config['amalgamation']} --n-core {n_cores}")
    os.chdir(running_dir)


# def build_gnu(running_dir, benchmark, gnu_config, n_cores):



running_dir = os.getcwd()

parser = argparse.ArgumentParser(description="Build benchmark cores and LLVM bitcode.")
parser.add_argument("benchmarks", nargs="+", choices=sorted(list(versions.keys()) + ['all']),
                    metavar="benchmark", help="benchmark(s) to build")
parser.add_argument("--n-core", type=int, default=1, dest="n_cores",
                    help="number of gcov cores to build per version (default: 1)")
args = parser.parse_args()

n_cores = args.n_cores
if 'all' in args.benchmarks:
    benchmarks = list(versions.keys())
else:
    benchmarks = args.benchmarks

with open(f"{running_dir}/gnu_config.json") as gnu_f:
    gnu_config = json.load(gnu_f)

for benchmark in benchmarks:
    if not os.path.exists(f"{running_dir}/{benchmark}"):
        os.mkdir(f"{running_dir}/{benchmark}")

    if benchmark in ["sqlite3"]:
        os.system(f"cp {running_dir}/build_sqlite.sh {running_dir}/{benchmark}")
        build_sqlite(running_dir, benchmark, n_cores)
    else:
        os.chdir(f"{running_dir}/{benchmark}")
        for ver, src_url in gnu_config[benchmark].items():
            src_file = src_url[src_url.rfind('/') + 1:]
            if not os.path.exists(src_file):
                os.system(f"wget {src_url}")
            if src_url.endswith(".tar.gz"):
                src_dir = unzip_tar_gz(src_file)
            elif src_url.endswith(".tar.xz"):
                src_dir = unzip_tar_xz(src_file)
            os.system(f"mv {src_dir} {ver}")

            os.chdir(f"{running_dir}/{benchmark}/{ver}")
            if benchmark in ["find", "sed", "xorriso"]:
                src = benchmark
            elif benchmark in ["objcopy", "objdump"]:
                src = "binutils"
            elif benchmark in ["gawk", "make"]:
                src = ''
            else:
                src = "src"
            print(f"Building Cores of {ver}")
            for i in range(n_cores):
                if os.path.exists(f"{running_dir}/{benchmark}/{ver}/obj-gcov{i+1}"):
                    continue
                print(f"Building {i+1}-th Core")
                os.system(f"mkdir {running_dir}/{benchmark}/{ver}/obj-gcov{i+1}")
                os.chdir(f"{running_dir}/{benchmark}/{ver}/obj-gcov{i+1}")
                os.system('../configure --disable-nls CC=/usr/bin/gcc CXX=/usr/bin/g++ GCOV=/usr/bin/gcov CFLAGS="-g -O2 -fprofile-arcs -ftest-coverage -fprofile-abs-path" CXXFLAGS="-g -O2 -fprofile-arcs -ftest-coverage -fprofile-abs-path" LDFLAGS="--coverage"')
                os.system("make -j$(nproc)")
                os.chdir(f"{running_dir}/{benchmark}/{ver}")

            if os.path.exists(f"{running_dir}/{benchmark}/{ver}/obj-llvm"):
                continue
            print(f"Building LLVM Directory")
            os.system(f"mkdir obj-llvm")
            os.chdir(f"{running_dir}/{benchmark}/{ver}/obj-llvm")
            os.system('CC=wllvm ../configure --disable-nls CFLAGS="-O1 -Xclang -disable-llvm-passes -D__NO_STRING_INLINES  -D_FORTIFY_SOURCE=0 -U__OPTIMIZE__"')
            os.system("make -j$(nproc)")
            os.chdir(f"{running_dir}/{benchmark}/{ver}/obj-llvm/{src}")
            os.system("find . -executable -type f | xargs -I '{}' extract-bc '{}'")
            os.system("find . -name '*.bc' -exec opt -strip-debug {} -o {} \\;")
            os.chdir(f"{running_dir}/{benchmark}")
        os.chdir(f"{running_dir}")
