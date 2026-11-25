# 測試共用函式（bash）。用法：. lib.sh；PA 是 pomelo-archive 的執行檔路徑（要能 --list --extract …）
PASS=0; FAIL=0; SKIP=0
FAILED=()
ok()   { PASS=$((PASS+1)); [ -n "$VERBOSE" ] && echo "  ok   $1"; return 0; }
bad()  { FAIL=$((FAIL+1)); FAILED+=("$1"); echo "  FAIL $1${2:+ — $2}"; return 0; }
skip() { SKIP=$((SKIP+1)); echo "  skip $1${2:+ — $2}"; return 0; }
have() { command -v "$1" >/dev/null 2>&1; }
# check 名稱 指令…：指令成功才算過
check() { local n=$1; shift; if "$@" >/dev/null 2>&1; then ok "$n"; else bad "$n"; fi; }
# expect_rc 名稱 預期結束碼 指令…
expect_rc() { local n=$1 want=$2; shift 2; "$@" >/dev/null 2>&1; local rc=$?; if [ "$rc" = "$want" ]; then ok "$n"; else bad "$n" "結束碼 $rc，預期 $want"; fi; }
# 兩棵樹是否一樣：內容、權限（檔案）、符號連結
tree_eq() {
  diff -r --no-dereference "$1" "$2" >/dev/null 2>&1 || return 1
  [ "$(cd "$1" && find . -type f -printf '%P %m\n' | sort)" = "$(cd "$2" && find . -type f -printf '%P %m\n' | sort)" ] || return 2
  [ "$(cd "$1" && find . -type l -printf '%P -> %l\n' | sort)" = "$(cd "$2" && find . -type l -printf '%P -> %l\n' | sort)" ] || return 3
}
summary() {
  echo "結果：通過 $PASS、失敗 $FAIL、略過 $SKIP"
  [ "$FAIL" -eq 0 ]
}
