# 小柚瀏覽器效能比較：量測腳本

結果與說明見 docs/小柚瀏覽器效能比較.md。在專案根目錄執行：

```
docker build -t pomelo-perf pomelo-browser/bench
docker run --rm -v "$PWD":/src -w /src pomelo-perf sh -c 'make -C libpomelo && make -C pomelo-browser pomelo-browser \
    POMELO_CFLAGS="-I../libpomelo $(pkg-config --cflags cairo)" POMELO_LIBS="-L../libpomelo -lpomelo -lX11 -lXext -lXrandr -lcairo"'
docker run --rm --shm-size=1g -v "$PWD":/src -w /src pomelo-perf dbus-run-session -- pomelo-browser/bench/perf.sh
```

- `perf.sh`：啟動時間（視窗出現、網頁顯示完成）與記憶體（同一個 session 所有行程的 PSS），每項 3 次。
  只量其中幾個瀏覽器：`-e BROWSERS="pomelo chromium"`。
- `size.sh`、`size-disk.sh`：在已經裝好 Pomelo 的 Debian 13 上，各瀏覽器多裝幾個套件、要下載多少、佔多少磁碟。
