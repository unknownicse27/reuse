# ReuSE

Refining Query Results for Efficient Symbolic Execution Across Software Releases

#
### Build ReuSE
1) Clone our source code. 
```bash
$ git clone https://github.com/unknownicse27/reuse.git
```

2) Build ReuSE with Dockerfile. This command builds ReuSE and a benchmark (grep-3.10~12).
```bash
$ cd reuse
/reuse $ docker build -t reuse .
```
ReuSE is implemented on Ubuntu-22.04 and KLEE-3.1.


3) Connect to Docker using the command below.
```bash
/reuse $ docker run -it --ulimit='stack=-1:-1' reuse
```

If you want to build ReuSE locally, follow the link to also install the [Z3 solver](https://github.com/Z3Prover/z3/blob/master/README.md).

### Run ReuSE
Now, you can run ReuSE for three consecutive program releases with the following code. (e.g. grep-3.10 ~ 12).
```bash
/reuse/benchmarks $ reuse -t 3600 -d ReuSE_TEST ../benchmarks/grep/3.10/obj-llvm/src/grep.bc ../benchmarks/grep/3.10/obj-gcov1/src/grep
```
Format : reuse -t <time_budget> -d <output_dir> <path_to_bc_file(llvm)> <path_to_exec_file(gcov)>
+ -t : Time Budget (seconds)
+ -d : Output Directory


Then, you will see logs as follows.
```bash
[INFO] ReuSE : Coverage will be recorded at "ReuSE_TEST/coverage.csv" at every iteration.
[INFO] ReuSE : All configuration loaded. Start testing.
[INFO] ReuSE : Iteration: 1 Iteration budget: 120 Total budget: 3600 Time elapsed: 141 Used argument:  Coverage: 1711
[INFO] ReuSE : Iteration: 2 Iteration budget: 120 Total budget: 3600 Time elapsed: 283 Used argument: -G Coverage: 2481
```

When the time budget expires without error, you can see the following output.
```bash
[INFO] ReuSE : Iteration: 24 Iteration budget: 120 Total budget: 3600 Time elapsed: 3341 Used argument: -l Coverage: 3167 
[INFO] ReuSE : Iteration: 25 Iteration budget: 120 Total budget: 3600 Time elapsed: 3479 Used argument: -q Coverage: 3168
[INFO] ReuSE : Iteration: 26 Iteration budget: 120 Total budget: 3600 Time elapsed: 3625 Used argument: -c Coverage: 3169 
[INFO] ReuSE : Testing done. Achieve 3169 coverage.
```


## Reporting Results
### Branch Coverage
If you want to get results about how many branches ReuSE has covered, run the following command.
```bash
# Needs 'matplotlib' package
/reuse/benchmarks $ python3 report_coverage.py --benchmark grep-3.4 ReuSE_TEST 
```

And if you want to compare multiple results in a graph, just list the directory names as: 
```bash
/reuse/benchmarks $ python3 report_coverage.py --benchmark grep-3.4 ReuSE_TEST1 ReuSE_TEST2 ...
```


### Bug Finding
If you want to check information about what bugs ReuSE has found, run the following command.
```bash
/reuse/experiments $ python3 report_bugs.py --benchmark grep ReuSE_TEST
```
+ Modify the tested versions within the .py file.

After executing the command, you will get a bug report named "bug_report.txt".
```bash
/reuse/experiments $ cat bug_report.txt
# Example from bison-3.8
Used argument      : "./bison" "A" "-T"
Crashed signal     : ['CRASHED signal 11']
File error occured : File: ../../lib/quotearg.c
Line error occured : 393
```


## Usage
```
$ orbis --help
usage: orbis [-h] [--klee KLEE] [--klee-replay KLEE_REPLAY]
             [--gen-bout GEN_BOUT] [--gcov GCOV] [--init-budget INT] [--n-testcases FLOAT]
             [--init-args STR] [-d OUTPUT_DIR] [--src-depth SRC_DEPTH]
             [-t INT] [-p STR]
             [llvm_bc] [gcov_obj]
```


### Optional Arguments
| Option | Description |
|:------:|:------------|
| `-h, --help` | show help message |
| `-d, --output-dir` | Directory where experiment results are saved |
| `--unsat-core` | Decide whether to extract the UNSAT core |


### Executable Settings
| Option | Description |
|:------:|:------------|
| `--klee` | Path to "klee" executable |
| `--klee-replay` | Path to "klee-replay" executable |
| `--kleaver` | Path to "kleaver" executable |
| `--ktest-tool` | Path to "ktest-tool" executable |
| `--gcov` | Path to "gcov" executable |


### Hyperparameters
| Option | Description |
|:------:|:------------|
| `--gcov-depth` | Depth from the obj-gcov directory to the directory where the gcov file was created |
| `--num-resample` | Initial symbolic argument formats |
| `--repetition` | Number of releases to test consecutively (list releases in klee.py) |

### Required Arguments
| Option | Description |
|:------:|:------------|
| `-t, --budget` | Time budget for each release |
| `llvm_bc` | .bc file in obj-llvm directory |
| `gcov_obj` | executable file in obj-gcov# directory |

## Usage of Other Programs
### /benchmarks/report_bugs.py
```
/orbis/benchmarks$ python3 report_bugs.py --help
usage: report_bugs.py [-h] [--benchmark STR] [--table PATH] [DIRS ...]
```
| Option | Description |
|:------:|:------------|
| `-h, --help`  | Show this help message and exit |
| `--benchmark` | Name of benchmark & verison |
| `--table`     | Path to save bug table graph |
| `DIRS`        | Name of directory to detect bugs |


### /benchmarks/report_coverage.py
```
/orbis/benchmarks$ python3 report_coverage.py --help
usage: report_coverage.py [-h] [--benchmark STR] [--graph PATH] [--budget TIME] [DIRS ...]
```
| Option | Description |
|:------:|:------------|
| `-h, --help`  | Show help message and exit |
| `--benchmark` | Name of benchmark & verison |
| `--graph`     | Path to save coverage graph |
| `--budget`    | Time budget of the coverage graph |
| `DIRS`        | Names of directories to draw figure |


## Source Code Structure
This section describes the structure of the directory and  files. Some less-important files may be omitted.
```
.
├── benchmarks                    <Testing & reporting directory>
    ├── build_benchmarks.py       ├── Python file to build target programs
    ├── build_sqlite.sh           ├── Script file to build sqlite-amalgamation (called when using build_benchmarks.py)
    ├── gnu_config.json           ├── JSON file with the version and download link of the GNU program to be built
    └── sqlite_config.py          └── JSON file with the version and amalgamation name of the SQLite program to be built
├── experiments                   <Directory to run ReuSE & save results>
    ├── calculate_coverage.py     ├── Directory of option-related path conditions for the program
    └── report_bugs.py            └── Directory of program options
├── klee                          <KLEE-3.1-based symbolic execution tool compatible with ReuSE>
└── reuse                         <Main source code directory>
    ├── bin.py                    ├── Main function of ReuSE
    ├── filter.py                 ├── Filter duplicate query results
    ├── klee.py                   ├── Run symbolic execution (e.g., KLEE)
    ├── resample.py               ├── Re-sample the assignment of a feasible query that accepts multi-assignment
    └── unsat_core.py             └── Extract core constraints of infeasible query data
    
```


## Data Availability
If you want to access data about the experiments of ParaSuit, you can download it at the following URL:
https://github.com/anonymousase26/orbis/releases/tag/experimental_result

Download the following file from the URL
+ reuse_main_experiments.zip

You can download the data files by clicking file or running the following commands.

```
$ wget https://github.com/anonymousase26/orbis/releases/download/experimental_result/reuse_main_experiments.zip
$ unzip reuse_main_experiments.zip
```

You can access the test-case directories for 6 programs: xorriso, sqlite, gcal, find, csplit, and ls.

Also, in each test directory, you will see the following files:
+ iteration-* : Iterations that used different option arguments and seed files.
   + info : Log file that expresses the KLEE command for the iteration.
   + test*.ktest : Generated test-cases for the target program. You can use the klee-replay to try each test-case.
+ coverage : Log file of elapsed time, accumulated coverage, coverage of each iteration, and used option arguments.
