#!/usr/bin/env sh
set -eu

say() { printf '%s\n' "$*"; }
die() { printf 'Erreur: %s\n' "$*" >&2; exit 1; }

uname_s=$(uname -s 2>/dev/null || printf unknown)

case "$uname_s" in
    Linux*)
        [ -r /etc/os-release ] || die "distribution Linux non identifiee; ce script gere les Debian-like."
        # shellcheck disable=SC1091
        . /etc/os-release
        family="${ID:-} ${ID_LIKE:-}"
        case "$family" in
            *debian*|*ubuntu*) ;;
            *) die "distribution non Debian-like (${ID:-inconnue}). Installez make, un compilateur C, pkg-config, libpcsclite-dev et pcscd." ;;
        esac

        if [ "$(id -u)" -eq 0 ]; then
            SUDO=""
        elif command -v sudo >/dev/null 2>&1; then
            SUDO="sudo"
        else
            die "sudo est requis pour installer les paquets (ou relancez ce script en root)."
        fi

        say "Installation des dependances Debian/Ubuntu..."
        $SUDO apt-get update
        $SUDO apt-get install -y build-essential pkg-config libpcsclite-dev pcscd pcsc-tools

        if command -v systemctl >/dev/null 2>&1; then
            $SUDO systemctl enable --now pcscd.socket >/dev/null 2>&1 || \
            $SUDO systemctl enable --now pcscd.service >/dev/null 2>&1 || true
        fi

        say "Dependances installees. Compilez avec: make"
        ;;

    MINGW*|MSYS*|CYGWIN*)
        command -v pacman >/dev/null 2>&1 || die "pacman introuvable: lancez ce script depuis MSYS2."

        case "${MSYSTEM:-}" in
            UCRT64)
                compiler_pkg="mingw-w64-ucrt-x86_64-gcc"
                ;;
            MINGW64)
                compiler_pkg="mingw-w64-x86_64-gcc"
                ;;
            CLANG64)
                compiler_pkg="mingw-w64-clang-x86_64-clang"
                ;;
            *)
                compiler_pkg="mingw-w64-ucrt-x86_64-gcc"
                say "Shell MSYS2 ${MSYSTEM:-MSYS} detecte: installation de la toolchain UCRT64."
                say "Pour compiler facilement, ouvrez ensuite le shell 'MSYS2 UCRT64'."
                ;;
        esac

        say "Installation des dependances MSYS2..."
        pacman -S --needed --noconfirm make "$compiler_pkg"
        say "Dependances installees. Compilez avec: make"
        ;;

    *)
        die "systeme non pris en charge automatiquement: $uname_s"
        ;;
esac
