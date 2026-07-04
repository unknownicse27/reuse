# ReuSE - Experiments
## Run ReuSE
### Testing Benchmarks
After installing the program from the benchmark directory, you can run ReuSE with that benchmark. For more information about running ReuSE, you can access the README.md file in the parent directory (/reuse).

```bash
/reuse/benchmarks $ reuse -t 36000 -d ReuSE_TEST ../benchmarks/grep/3.10/obj-llvm/src/grep.bc ../benchmarks/grep/3.10/obj-gcov/src/grep
```
Format : reuse -t <time_budget> -d <output_dir> <path_to_bc_file(llvm)> <path_to_exec_file(gcov)>


## Analyzing Results
### Branch Coverage
When the experiment is completed, ReuSE provides a line graph showing how many branches were covered in each time budget section through the 'report_coverage.py' program. If you run the command below, ReuSE returns the graph by creating a 'coverage_result.png' file in the same directory.
```
/reuse/benchmarks$ python3 calculate_coverage.py --benchmark grep ReuSE_TEST
usage: calculate_coverage.py [-h] [--benchmark STR] [--graph PATH] [--budget TIME] [DIRS ...]
```

If you want to return multiple results in a single graph, just list the names of the directories, such as:
```
/reuse/benchmarks$ python3 calculate_coverage.py --benchmark grep ReuSE_TEST1 ReuSE_TEST2 ReuSE_TEST3 ...
```

### Bug-Finding
ReuSE also provides the "report_bugs.py" program to extract test cases that cause system errors among those generated through the experiment. When you execute the command below, ReuSE automatically detects bug-triggering test cases. As a result of execution, ReuSE returns the test case causing the bug, its arguments, the system crash signal, and the location (file name and line) of the code where the bug occurs.
```
/reuse/benchmarks$ python3 report_bugs.py --benchmark grep ReuSE
```

Similar to branch coverage, bug-finding also allows you to search multiple directories at once by simply listing the directories.

```
/reuse/benchmarks$ python3 report_bugs.py --benchmark grep ReuSE_TEST1 ReuSE_TEST2 ...
```


### Options of Reporting Programs
+ /benchmarks/calculate_coverage.py

| Option | Description |
|:------:|:------------|
| `-h, --help`  | Show help message and exit |
| `--benchmark` | Name of benchmark & verison |
| `DIRS`        | Names of directories to draw figure |

+ /benchmarks/report_bugs.py
```
/reuse/benchmarks$ python3 report_bugs.py --help
usage: report_bugs.py [-h] [--benchmark STR] [--table PATH] [DIRS ...]
```
| Option | Description |
|:------:|:------------|
| `-h, --help`  | Show this help message and exit |
| `--benchmark` | Name of benchmark & verison |
| `--table`     | Path to save bug table graph |
| `DIRS`        | Name of directory to detect bugs |
