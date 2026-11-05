#!/usr/bin/env bash
# Fail if the plugin cannot be distributed as a standalone .so:
#   - no RPATH/RUNPATH (build-machine paths),
#   - only standard system libraries as dependencies,
#   - glibc requirement no newer than MAX_GLIBC.
set -euo pipefail

SO="${1:?usage: check-portable.sh <plugin.so>}"
MAX_GLIBC="${MAX_GLIBC:-2.28}"
ALLOWED_LIBS="libz.so.1 libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2"

status=0

if readelf -d "${SO}" | grep -qE '\((RPATH|RUNPATH)\)'; then
    echo "FAIL: ${SO} embeds an RPATH/RUNPATH:" >&2
    readelf -d "${SO}" | grep -E '\((RPATH|RUNPATH)\)' >&2
    status=1
fi

for lib in $(readelf -d "${SO}" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
    if [[ " ${ALLOWED_LIBS} " != *" ${lib} "* ]]; then
        echo "FAIL: ${SO} depends on non-standard library ${lib}" >&2
        status=1
    fi
done

glibc=$(objdump -T "${SO}" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -V -u | tail -1)
if [ "$(printf '%s\n%s\n' "${glibc}" "${MAX_GLIBC}" | sort -V | tail -1)" != "${MAX_GLIBC}" ]; then
    echo "FAIL: ${SO} requires glibc ${glibc} (> ${MAX_GLIBC})" >&2
    status=1
fi

if [ "${status}" -eq 0 ]; then
    echo "Portable: ${SO} (glibc >= ${glibc}, no RPATH, standard libs only)"
fi
exit "${status}"
