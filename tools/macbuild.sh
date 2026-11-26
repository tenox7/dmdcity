#!/bin/sh
# Build the host side on the Mac, for tests with fakedmd.py.
set -e
H=$(cd "$(dirname "$0")/../host" && pwd)
B=$H/../machost; mkdir -p $B; cd $B
for f in $H/*.c; do cp $f .; done
# macOS libc declares sys_nerr/sys_errlist const: rename the K&R externs
sed -i '' -e 's/extern int sys_nerr;/extern int x_nerr;/' -e 's/extern char \*sys_errlist\[\];/extern char *x_errlist[];/' \
  -e 's/errno < sys_nerr/errno < x_nerr/' -e 's/return sys_errlist\[errno\]/return x_errlist[errno]/' s_fileio.c
echo 'int x_nerr = 0; char *x_errlist[1];' > xerr.c
CF="-std=gnu89 -w -Wno-return-mismatch -Wno-implicit-function-declaration -Wno-implicit-int -DIS_INTEL -I$H/headers -O"
for f in *.c; do clang $CF -c $f; done
clang -o mcity *.o -lm
echo built $B/mcity
