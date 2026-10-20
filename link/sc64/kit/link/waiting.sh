#!/bin/bash
# whether Halo 64 Link listens on port 6466 yet (exit 0: yes): read from
# the kernel's tables, so nothing connects to it
grep -qE ':1942 0+:0000 0A' /proc/net/tcp /proc/net/tcp6 2>/dev/null
