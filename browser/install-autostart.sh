#!/usr/bin/env sh
set -eu

uname_s=$(uname -s 2>/dev/null || printf unknown)

case "$uname_s" in
    MINGW*|MSYS*|CYGWIN*)
        if command -v cygpath >/dev/null 2>&1; then
            host_unix="${1:-${MINGW_PREFIX:-/ucrt64}/bin/efrits-nfc-bridge.exe}"
            host_win=$(cygpath -w "$host_unix")
        else
            printf '%s\n' "cygpath est requis sous MSYS2." >&2
            exit 1
        fi
        command="powershell.exe -NoProfile -WindowStyle Hidden -Command \"Start-Process -WindowStyle Hidden -FilePath '$host_win'\""
        # MSYS2 convertit normalement les arguments de la forme /v, /t, /d et /f
        # en chemins Unix->Windows avant d'appeler un executable natif. REG.EXE les
        # attend au contraire tels quels comme options; sans cette exclusion il repond
        # simplement "ERREUR : syntaxe incorrecte".
        MSYS2_ARG_CONV_EXCL='*' MSYS_NO_PATHCONV=1 \
            reg.exe add 'HKCU\Software\Microsoft\Windows\CurrentVersion\Run' \
            /v EFRITSNfcBridge /t REG_SZ /d "$command" /f >/dev/null

        # make install replaces the executable on disk but Windows keeps an
        # already running bridge in memory. Stop it explicitly so an upgrade
        # cannot leave the browser connected to an older protocol version.
        powershell.exe -NoProfile -Command \
            "Get-Process efrits-nfc-bridge -ErrorAction SilentlyContinue | Stop-Process -Force" \
            >/dev/null 2>&1 || true
        sleep 1
        powershell.exe -NoProfile -WindowStyle Hidden -Command \
            "Start-Process -WindowStyle Hidden -FilePath '$host_win'" >/dev/null 2>&1 || true
        printf '%s\n' "Pont NFC installé au démarrage Windows: $host_win"
        ;;
    Linux*)
        host="${1:-$HOME/.local/bin/efrits-nfc-bridge}"
        mkdir -p "$HOME/.config/systemd/user"
        cat > "$HOME/.config/systemd/user/efrits-nfc-bridge.service" <<UNIT
[Unit]
Description=EFRITS NFC browser bridge
After=graphical-session.target

[Service]
Type=simple
ExecStart=$host
Restart=on-failure
RestartSec=2

[Install]
WantedBy=default.target
UNIT
        if command -v systemctl >/dev/null 2>&1; then
            systemctl --user daemon-reload
            systemctl --user enable --now efrits-nfc-bridge.service
            printf '%s\n' "Pont NFC activé via systemd --user."
        else
            mkdir -p "$HOME/.config/autostart"
            cat > "$HOME/.config/autostart/efrits-nfc-bridge.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=EFRITS NFC Bridge
Exec=$host
X-GNOME-Autostart-enabled=true
NoDisplay=true
DESKTOP
            "$host" >/dev/null 2>&1 &
            printf '%s\n' "Pont NFC activé via autostart desktop."
        fi
        ;;
    *)
        printf 'Système non pris en charge automatiquement: %s\n' "$uname_s" >&2
        exit 1
        ;;
esac
