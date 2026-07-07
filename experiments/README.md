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
When the experiment is completed, ReuSE provides a line graph showing how many branches were covered in each release through the 'calculate_coverage.py' script. If you run the command below, ReuSE returns the coverage in the terminal output.
```
/reuse/experiments$ python3 calculate_coverage.py --program grep ReuSE_TEST 
```

If you want to return multiple results, just list the names of the directories, such as:
```
/reuse/experiments $ python3 calculate_coverage.py --program grep ReuSE_TEST1 ReuSE_TEST2 ...
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
```
/reuse/experiments$ python3 calculate_coverage.py --help
usage: calculate_coverage.py [-h] --program PROGRAM output_dirs [output_dirs ...]
```
| Option | Description |
|:------:|:------------|
| `-h, --help`  | Show this help message and exit |
| `--program`   | Name of tested program |
| `output_dirs` | Output directories to calculate branch coverage |

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
