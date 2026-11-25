#!/bin/bash
# 在一個發行版的乾淨容器裡編譯並跑完整測試（一次一個）。用法：bash run-distro.sh ubuntu22.04|ubuntu24.04|ubuntu26.04|debian13|rocky9.6|fedora44 [結果資料夾]
# 需要 ~/wm/dist/<發行版>/ 裡有打好的 libpomelo 套件；容器用完即丟。
D=$1; RES=${2:-$HOME/.tmp/archres}
WM=$(cd "$(dirname "$0")/../.." && pwd)
case $D in
  ubuntu22.04) IMG=ubuntu:22.04; K=deb ;; ubuntu24.04) IMG=ubuntu:24.04; K=deb ;; ubuntu26.04) IMG=ubuntu:26.04; K=deb ;;
  debian13) IMG=debian:13; K=deb ;; rocky9.6) IMG=rockylinux/rockylinux:9.6; K=rpm ;; fedora44) IMG=fedora:44; K=rpm ;;
  *) echo "不認得 $D" >&2; exit 2 ;;
esac
mkdir -p "$RES"
docker run --rm --name "archtest-$D" --tmpfs /small:size=8m -v "$WM/pomelo-archive":/src:ro -v "$WM/dist/$D":/dist:ro -v "$RES":/res "$IMG" bash /src/test/in-container.sh $K $D
