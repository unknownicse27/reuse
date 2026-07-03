#!/bin/bash
set -e

# ---------- defaults ----------
NUM_MAKE_CORES=5
YEAR=""
AMALGAMATION=""
VERSION=""

usage() {
    cat <<EOF
Usage: $0 --year YEAR --amalgamation NAME [--n-core N] [--version VER]

  -y, --year          release year on sqlite.org (e.g., 2023)
  -a, --amalgamation  amalgamation name or zip (e.g., sqlite-amalgamation-3420000[.zip])
  -n, --n-core        number of gcov build copies (default: 5)
  -v, --version       working directory name (default: derived from amalgamation number)
EOF
    exit "${1:-1}"
}

# ---------- parse args ----------
while [ $# -gt 0 ]; do
    case "$1" in
        -y|--year)          YEAR="$2";           shift 2 ;;
        -a|--amalgamation)  AMALGAMATION="$2";   shift 2 ;;
        -n|--n-core)        NUM_MAKE_CORES="$2"; shift 2 ;;
        -v|--version)       VERSION="$2";        shift 2 ;;
        -h|--help)          usage 0 ;;
        *) echo "Unknown argument: $1"; usage ;;
    esac
done

# ---------- validate ----------
if [ -z "$YEAR" ] || [ -z "$AMALGAMATION" ]; then
    echo "Error: --year and --amalgamation are required"
    usage
fi

AMALGAMATION="${AMALGAMATION%.zip}"   # strip trailing .zip if given
ZIP="${AMALGAMATION}.zip"
URL="https://www.sqlite.org/${YEAR}/${ZIP}"

# ---------- derive version dir if not given ----------
if [ -z "$VERSION" ]; then
    num="${AMALGAMATION##*-}"   # sqlite-amalgamation-3420000 -> 3420000
    if [[ "$num" =~ ^[0-9]{7}$ ]]; then
        VERSION="$((10#${num:0:1})).$((10#${num:1:2})).$((10#${num:3:2}))"
    else
        echo "Could not derive version from '$AMALGAMATION'; pass --version explicitly"
        exit 1
    fi
fi

echo "[INFO] version=$VERSION  amalgamation=$AMALGAMATION  year=$YEAR  cores=$NUM_MAKE_CORES"

# ########################## sqlite ##########################
mkdir -p "$VERSION"
cd "$VERSION"

if [ ! -f "$ZIP" ]; then
    wget "$URL"
fi

if [ ! -d "$AMALGAMATION" ]; then
    unzip "$ZIP"
fi

# ---------- obj-llvm ----------
if [ -d obj-llvm ]; then
    echo "[SKIP] obj-llvm already exists"
else
    cp -r "$AMALGAMATION" obj-llvm
    cd obj-llvm
    wllvm -g -O1 -Xclang -disable-llvm-passes -D__NO_STRING_INLINES -D_FORTIFY_SOURCE=0 -U__OPTIMIZE__ -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_DEPRECATED -DSQLITE_DEFAULT_PAGE_SIZE=512 -DSQLITE_DEFAULT_CACHE_SIZE=10 -DSQLITE_DISABLE_INTRINSIC -DSQLITE_DISABLE_LFS -DYYSTACKDEPTH=20 -DSQLITE_OMIT_LOOKASIDE -DSQLITE_OMIT_WAL -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_DEFAULT_LOOKASIDE='64,5' -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -I. shell.c sqlite3.c -o sqlite3 -lm
    find . -executable -type f | xargs -I '{}' extract-bc '{}'
    cd ..
fi

# ---------- obj-gcov ----------
for i in $(seq 1 "$NUM_MAKE_CORES") ; do
    if [ -d obj-gcov${i} ]; then
        echo "[SKIP] obj-gcov${i} already exists"
        continue
    fi

    cp -r "$AMALGAMATION" obj-gcov${i}
    cd obj-gcov${i}
    gcc -g -fprofile-arcs -ftest-coverage -O0 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_DEPRECATED -DSQLITE_DEFAULT_PAGE_SIZE=512 -DSQLITE_DEFAULT_CACHE_SIZE=10 -DSQLITE_DISABLE_INTRINSIC -DSQLITE_DISABLE_LFS -DYYSTACKDEPTH=20 -DSQLITE_OMIT_LOOKASIDE -DSQLITE_OMIT_WAL -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_DEFAULT_LOOKASIDE='64,5' -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -I. shell.c sqlite3.c -o sqlite3 -lm
    cd ..
done