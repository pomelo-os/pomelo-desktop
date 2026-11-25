#!/bin/sh
# 實際安裝，量磁碟用量的差（$1 = 套件，$2 = minimal 時不裝建議套件）
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null 2>&1
a=$(du -sxm / 2>/dev/null | cut -f1)
apt-get install -y -qq $([ "$2" = minimal ] && echo --no-install-recommends) $1 >/dev/null 2>&1; apt-get clean
b=$(du -sxm / 2>/dev/null | cut -f1)
echo "DISK $3 $2 $((b - a))"
