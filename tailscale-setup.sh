#!/usr/bin/env bash
set -Eeuo pipefail

# ==========================================
# Tailscale Setup for Ubuntu (DojoPaaS)
# ==========================================

# Funnel のローカル転送先ポート
FUNNEL_PORT="${FUNNEL_PORT:-8000}"
SSH_USER="${SSH_USER:-ubuntu}"

log() {
  echo
  echo "==> $*"
}

fail() {
  echo "エラー: $*" >&2
  exit 1
}

# FUNNEL_PORT は CLI に渡す前に検証する。
if [[ ! "$FUNNEL_PORT" =~ ^[0-9]{1,5}$ ]]; then
  fail "FUNNEL_PORT は1〜65535の数字で指定してください"
fi

FUNNEL_PORT_NUM=$((10#$FUNNEL_PORT))
if (( FUNNEL_PORT_NUM < 1 || FUNNEL_PORT_NUM > 65535 )); then
  fail "FUNNEL_PORT は1〜65535で指定してください"
fi

# 1. 実行環境の確認
if [[ "$EUID" -ne 0 ]]; then
  fail "root権限で実行してください（例: sudo ./tailscale-setup.sh）"
fi

if [[ ! -f /etc/os-release ]]; then
  fail "OSを確認できません"
fi

# shellcheck disable=SC1091
source /etc/os-release

if [[ "${ID:-}" != "ubuntu" ]]; then
  fail "Ubuntu専用スクリプトです"
fi

if ! command -v systemctl >/dev/null 2>&1; then
  fail "systemctl が見つかりません。systemdが有効なUbuntuで実行してください"
fi

# 2. Tailscaleのインストール
if ! command -v tailscale >/dev/null 2>&1; then
  log "Tailscaleをインストール"

  apt-get update
  apt-get install -y curl ca-certificates

  install_script="$(mktemp /tmp/install-tailscale.XXXXXX.sh)"
  trap 'rm -f -- "$install_script"' EXIT

  curl -fsSL \
    --proto '=https' \
    --tlsv1.2 \
    https://tailscale.com/install.sh \
    -o "$install_script"

  sh "$install_script"
else
  log "Tailscaleはインストール済み"
fi

# 3. デーモン起動・自動起動
log "tailscaledを有効化"
systemctl enable --now tailscaled

tailscale_running() {
  tailscale status --json 2>/dev/null \
    | grep '"BackendState"[[:space:]]*:[[:space:]]*"Running"' >/dev/null
}

# 4. Tailscale認証
if ! tailscale_running; then
  log "Tailscaleの認証を開始"
  echo "表示されるURLをブラウザで開き、このサーバーをtailnetへ追加してください"
  tailscale up
fi

if ! tailscale_running; then
  fail "TailscaleがRunning状態になっていません"
fi

# 5. Tailscale SSHの有効化
log "Tailscale SSHを有効化"
tailscale set --ssh

# 6. Funnelの有効化
log "Tailscale Funnelを有効化"
echo "転送先: http://127.0.0.1:${FUNNEL_PORT_NUM}"
echo "注意: Funnelはインターネット全体へ公開されます"

tailscale funnel \
  --bg \
  "http://127.0.0.1:${FUNNEL_PORT_NUM}"

# 7. セットアップ結果
log "セットアップ完了"

echo
echo "Tailscale IP:"
tailscale ip -4

echo
echo "接続状態:"
tailscale status

echo
echo "Funnel:"
tailscale funnel status

echo
echo "SSH接続例:"
echo "ssh ${SSH_USER}@$(tailscale ip -4)"
