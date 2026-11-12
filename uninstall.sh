#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Removes what install.sh put into your home directory.
#
#   ./uninstall.sh                 remove the plugins and the Kdenlive descriptions
#   ./uninstall.sh --flatpak-env   also take the plugin directory out of the
#                                  Kdenlive Flatpak's FREI0R_PATH override again
#   ./uninstall.sh --dry-run       only show what would be done
#
# Projects that use the cube will show the effect as missing afterwards.
set -eu

APP=org.kde.kdenlive
plugin_dir="${CUBE3D_PLUGIN_DIR:-$HOME/.frei0r-1/lib}"
flatpak_data="$HOME/.var/app/$APP/data/kdenlive"
native_data="${XDG_DATA_HOME:-$HOME/.local/share}/kdenlive"

dry=0
flatpak_env=0
for arg in "$@"; do
    case "$arg" in
    --dry-run) dry=1 ;;
    --flatpak-env) flatpak_env=1 ;;
    -h | --help)
        sed -n '3,12p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "unknown option: $arg" >&2
        exit 2
        ;;
    esac
done

remove() {
    [ -e "$1" ] || return 0
    if [ "$dry" = 1 ]; then
        echo "  would remove: $1"
    else
        rm -f "$1"
        echo "  removed $1"
    fi
}

remove "$plugin_dir/cube3d.so"
remove "$plugin_dir/cube3d_transition.so"
remove "$HOME/.local/bin/cube3d-faces"
for data in "$flatpak_data" "$native_data"; do
    remove "$data/effects/frei0r_cube3d.xml"
    remove "$data/effects/cube3d_gridcell.xml"
    for t in frei0r_cube3d_transition cube3d_turn_right cube3d_tip_up cube3d_tip_down cube3d_flip \
        cube3d_composition cube3d_composition_out; do
        remove "$data/transitions/$t.xml"
    done
done
# leave the directories unless empty: other things may live there
if [ "$dry" = 1 ]; then
    [ -d "$plugin_dir" ] && echo "  would remove $plugin_dir if it is empty"
else
    rmdir "$plugin_dir" 2>/dev/null || true
fi

if [ "$flatpak_env" = 1 ]; then
    if ! command -v flatpak >/dev/null 2>&1; then
        echo "--flatpak-env: 'flatpak' cannot be run from this shell; nothing changed there." >&2
        exit 1
    fi
    default=$(flatpak info --show-metadata "$APP" 2>/dev/null | sed -n 's/^FREI0R_PATH=//p')
    default=${default:-/app/lib/frei0r-1}
    current=$(flatpak override --user --show "$APP" 2>/dev/null | sed -n 's/^FREI0R_PATH=//p')
    case ":$current:" in
    *":$plugin_dir:"*)
        # Take only our directory out. The variable must keep a value: removing
        # it would hide the frei0r effects that Kdenlive itself ships.
        rest=$(printf '%s\n' "$current" | tr ':' '\n' | grep -vxF "$plugin_dir" | paste -sd: -)
        rest=${rest:-$default}
        if [ "$dry" = 1 ]; then
            echo "  would run: flatpak override --user --env=FREI0R_PATH=$rest $APP"
        else
            flatpak override --user "--env=FREI0R_PATH=$rest" "$APP"
            echo "  Flatpak: FREI0R_PATH override is now $rest"
        fi
        ;;
    *)
        [ -n "$current" ] && echo "  Flatpak: FREI0R_PATH override ($current) does not contain $plugin_dir, left alone"
        ;;
    esac
fi
echo "Done."
