#!/bin/bash
# Enroll and verify two fingers, one at a time, with prompts that say which
# finger is being asked for. The driver cannot do this itself: fprintd runs as
# a system service with no terminal, so its "waiting for a finger" lines only
# reach the journal. Everything user-facing has to come from here.
#
#   ./tools/two-fingers.sh
#   FINGER1=right-thumb-finger FINGER2=left-index-finger ./tools/two-fingers.sh
#
# Nothing here needs root; fprintd is reached over D-Bus as the current user.
set -u

FINGER1=${FINGER1:-right-index-finger}
FINGER2=${FINGER2:-right-middle-finger}
NAME1=${NAME1:-"第 1 根（右食指）"}
NAME2=${NAME2:-"第 2 根（右中指）"}
STAGES=10

WANT=$USER

line() { printf '%s\n' "----------------------------------------------------------------"; }
head2() {
    line
    printf '  %s\n' "$1"
    shift
    for l in "$@"; do printf '  %s\n' "$l"; done
    line
}

# fprintd keeps the device claimed for a moment after a client dies; every
# second attempt otherwise fails with AlreadyInUse and looks like a device bug.
clear_claim() {
    local out
    out=$(fprintd-list "$WANT" 2>&1 >/dev/null)
    case $out in
    *AlreadyInUse*)
        printf '  设备仍被上一次会话占用，正在重启 fprintd...\n'
        sudo systemctl restart fprintd || return 1
        sleep 1
        ;;
    esac
    return 0
}

ask() {
    printf '\n>>> %s\n    准备好后按 Enter：' "$1"
    read -r _
}

# Reads fprintd's progress lines and turns them into a running count, because
# fprintd itself only says "stage-passed" with no number and no finger name.
enroll_finger() {
    local finger=$1 who=$2 n=0 done_ok=0 rc
    local tmp
    tmp=$(mktemp)

    clear_claim
    head2 "现在录入 ${who}：${finger}" \
          "共 ${STAGES} 次按压，每次按下 1-2 秒后抬起" \
          "每次抬起后等 2-3 秒（设备要重新握手），再按下一次" \
          "只按这一根手指，位置尽量和上一次重合"

    fprintd-enroll -f "$finger" "$WANT" 2>&1 | while IFS= read -r l; do
        case $l in
        *"Enroll result: enroll-stage-passed"*)
            n=$((n + 1))
            printf '  [%2d/%d] %s 已接受 —— 抬起，等 2 秒，再用这一根按下去\n' \
                   "$n" "$STAGES" "$who" ;;
        *"Enroll result: enroll-completed"*)
            printf '  [完成] 这根手指录好了\n'
            echo ok >"$tmp" ;;
        *"Enroll result: enroll-retry-scan"*)
            printf '  [重按] %s：这一帧没通过 —— 还是这一根，按实一点\n' "$who" ;;
        *"Enroll result: enroll-finger-not-centered"*)
            printf '  [重按] 手指偏了 —— 指腹对准传感器中心\n' ;;
        *"Enroll result: enroll-data-full"*)
            printf '  [重按] 数据满了 —— 换一处指腹\n' ;;
        *"Enroll result: "*)
            printf '  [结果] %s\n' "${l#*Enroll result: }" ;;
        *"Enrolling "*)
            printf '  目标：%s\n' "${l#Enrolling }" ;;
        *"Using device"*) : ;;
        *AlreadyInUse*)
            printf '  [错误] 设备被占用，请运行 sudo systemctl restart fprintd 后重试\n' ;;
        *) printf '  %s\n' "$l" ;;
        esac
    done

    done_ok=$(cat "$tmp" 2>/dev/null)
    rm -f "$tmp"
    [ "$done_ok" = ok ]
}

verify_finger() {
    local finger=$1 who=$2 press=$3 n=0 verdict=none
    local tmp
    tmp=$(mktemp)

    clear_claim
    head2 "现在验证 ${who}：${finger}" \
          "把 ${press} 按在传感器上，按住 1-2 秒后抬起" \
          "如果一次没通过，fprintd 还会再给两次机会"

    fprintd-verify -f "$finger" "$WANT" 2>&1 | while IFS= read -r l; do
        case $l in
        *"Verify result: verify-match"*)
            printf '  [通过] 匹配成功\n'
            echo match >"$tmp" ;;
        *"Verify result: verify-no-match"*)
            printf '  [未通过] 不匹配\n'
            echo nomatch >"$tmp" ;;
        *"Verify result: verify-retry-scan"*)
            n=$((n + 1))
            printf '  [重按] 第 %d 次没读出有效图像，再按一次\n' "$n" ;;
        *"Verify started"*)
            printf '  传感器已就绪，正在等手指 —— 现在按下去\n' ;;
        *"Verifying:"*)
            printf '  目标：%s\n' "${l#*Verifying: }" ;;
        *"Using device"*) : ;;
        *AlreadyInUse*)
            printf '  [错误] 设备被占用，请运行 sudo systemctl restart fprintd 后重试\n' ;;
        *) printf '  %s\n' "$l" ;;
        esac
    done

    verdict=$(cat "$tmp" 2>/dev/null)
    rm -f "$tmp"
    printf '  => %s\n' "${verdict:-unknown}"
}

main() {
    head2 "两根手指：录入 + 分别验证" \
          "第 1 根：${NAME1}  ->  ${FINGER1}" \
          "第 2 根：${NAME2}  ->  ${FINGER2}" \
          "全程 5 步，每步开始前会停下来等你按 Enter"

    printf '\n>>> 这会删掉 %s 现有的全部指纹模板，按 Enter 继续：' "$WANT"
    if ! read -r confirm; then
        exit 1
    fi
    [ -z "$confirm" ] || { echo "已取消"; exit 1; }

    printf '  重启 fprintd，清掉上一次可能残留的 device claim（可能要输 sudo 密码）\n'
    sudo systemctl restart fprintd || printf '  !! 重启失败，仍继续尝试\n'
    sleep 1

    fprintd-delete "$WANT" >/dev/null 2>&1
    printf '\n  已清空旧模板\n'

    ask "第 1 步：录入 ${NAME1}"
    enroll_finger "$FINGER1" "$NAME1" || printf '  !! 第 1 根没有录完\n'

    ask "第 2 步：录入 ${NAME2}"
    enroll_finger "$FINGER2" "$NAME2" || printf '  !! 第 2 根没有录完\n'

    line
    fprintd-list "$WANT"

    ask "第 3 步：验证 ${NAME1}（按 ${NAME1}）"
    verify_finger "$FINGER1" "$NAME1" "${NAME1}"

    ask "第 4 步：验证 ${NAME2}（按 ${NAME2}）"
    verify_finger "$FINGER2" "$NAME2" "${NAME2}"

    ask "第 5 步（安全性）：验证 ${NAME1}，但请按 ${NAME2}，应当不通过"
    verify_finger "$FINGER1" "$NAME1" "${NAME2}"

    line
    printf '  说明：第 5 步如果显示"匹配成功"，说明两根手指分不开，\n'
    printf '        这套阈值就不能用于解锁。分数在 journal 里：\n'
    printf '        sudo journalctl -u fprintd --since "10 min ago" | grep score\n'
    line
}

main "$@"
