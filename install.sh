#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Installs the cube3d plugins and their Kdenlive descriptions for the current
# user. Nothing is written outside your home directory and no root is needed.
#
#   ./install.sh                  copy the files
#   ./install.sh --flatpak-env    also let the Kdenlive Flatpak look for frei0r
#                                 plugins in your home directory (see below)
#   ./install.sh --dry-run        only show what would be done
#
# What goes where
#   ~/.frei0r-1/lib/cube3d.so, cube3d_transition.so
#       the per-user frei0r plugin directory. A Kdenlive from your distribution's
#       packages (and any other MLT program) finds them there by itself.
#   <Kdenlive data dir>/effects/*.xml, transitions/*.xml
#       names, ranges and layout of the parameters as Kdenlive shows them.
#       Flatpak: ~/.var/app/org.kde.kdenlive/data/kdenlive
#       native:  ~/.local/share/kdenlive
#
# The Flatpak and --flatpak-env
#   The Kdenlive Flatpak starts with FREI0R_PATH=/app/lib/frei0r-1, which makes
#   MLT ignore the per-user directory. --flatpak-env adds that directory to the
#   variable with a per-user Flatpak override, e.g.
#       flatpak override --user --env=FREI0R_PATH=/app/lib/frei0r-1:$HOME/.frei0r-1/lib org.kde.kdenlive
#   ./uninstall.sh --flatpak-env takes it out again. (Do not undo it with
#   "flatpak override --unset-env=FREI0R_PATH": that removes the variable
#   altogether and with it every frei0r effect Kdenlive ships.)
set -eu

APP=org.kde.kdenlive
here=$(cd "$(dirname "$0")" && pwd)
build="$here/build"
plugin_dir="${CUBE3D_PLUGIN_DIR:-$HOME/.frei0r-1/lib}"
flatpak_home="$HOME/.var/app/$APP"
flatpak_data="$flatpak_home/data/kdenlive"
native_data="${XDG_DATA_HOME:-$HOME/.local/share}/kdenlive"

dry=0
flatpak_env=0
for arg in "$@"; do
    case "$arg" in
    --dry-run) dry=1 ;;
    --flatpak-env) flatpak_env=1 ;;
    -h | --help)
        sed -n '3,29p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "unknown option: $arg" >&2
        exit 2
        ;;
    esac
done

run() {
    if [ "$dry" = 1 ]; then
        echo "  would run: $*"
    else
        "$@"
    fi
}

for f in cube3d.so cube3d_transition.so kdenlive/effects/frei0r_cube3d.xml; do
    [ -f "$build/$f" ] || {
        echo "build/$f is missing - run 'make' first" >&2
        exit 1
    }
done

# Is the Flatpak there, and can it be talked to from this shell?
flatpak_cmd=0
if command -v flatpak >/dev/null 2>&1 && flatpak info "$APP" >/dev/null 2>&1; then
    flatpak_cmd=1
fi
flatpak_present=$flatpak_cmd
[ -d "$flatpak_home" ] && flatpak_present=1
if [ "$flatpak_env" = 1 ] && [ "$flatpak_cmd" = 0 ]; then
    echo "--flatpak-env: cannot run 'flatpak' for $APP from this shell." >&2
    echo "Run this script where flatpak works, or set the override yourself (see --help)." >&2
    exit 1
fi

echo "Plugins -> $plugin_dir"
run mkdir -p "$plugin_dir"
# install into place under a temporary name: a running Kdenlive may have the old file mapped
for so in cube3d.so cube3d_transition.so; do
    run cp "$build/$so" "$plugin_dir/.$so.new"
    run mv -f "$plugin_dir/.$so.new" "$plugin_dir/$so"
done

# the helper that turns a project's bin clips and zones into pickable face files
tool_dir="$HOME/.local/bin"
echo "cube3d-faces -> $tool_dir"
run mkdir -p "$tool_dir"
run cp "$here/tools/cube3d-faces" "$tool_dir/cube3d-faces"
run chmod +x "$tool_dir/cube3d-faces"

install_xml() {
    echo "Kdenlive descriptions -> $1"
    run mkdir -p "$1/effects" "$1/transitions"
    for x in "$build"/kdenlive/effects/*.xml; do
        run cp "$x" "$1/effects/"
    done
    for x in "$build"/kdenlive/transitions/*.xml; do
        run cp "$x" "$1/transitions/"
    done
}

[ "$flatpak_present" = 1 ] && install_xml "$flatpak_data"
if command -v kdenlive >/dev/null 2>&1 || [ -d "$native_data" ] || [ "$flatpak_present" = 0 ]; then
    install_xml "$native_data"
fi

status=0
if [ "$flatpak_cmd" = 1 ]; then
    default=$(flatpak info --show-metadata "$APP" 2>/dev/null | sed -n 's/^FREI0R_PATH=//p')
    current=$(flatpak override --user --show "$APP" 2>/dev/null | sed -n 's/^FREI0R_PATH=//p')
    base=${current:-${default:-/app/lib/frei0r-1}}
    case ":$base:" in
    *":$plugin_dir:"*)
        echo "Flatpak: Kdenlive already looks in $plugin_dir"
        visible=1
        ;;
    *)
        visible=0
        if [ "$flatpak_env" = 1 ]; then
            echo "Flatpak: letting Kdenlive look in $plugin_dir"
            run flatpak override --user "--env=FREI0R_PATH=$base:$plugin_dir" "$APP"
            visible=1
        else
            cat <<EOT

The Kdenlive Flatpak does not look in $plugin_dir yet, so it will not see the
cube. Either run this script again with --flatpak-env, or run:

    flatpak override --user "--env=FREI0R_PATH=$base:$plugin_dir" $APP

EOT
        fi
        ;;
    esac
    # A plugin built against a newer C library than the Flatpak's is skipped by
    # MLT without a word, so ask Kdenlive's own melt whether it can see ours.
    if [ "$visible" = 1 ] && [ "$dry" = 0 ]; then
        seen=$(cd "$HOME" && flatpak run --command=sh "$APP" -c \
            'melt -query filters 2>/dev/null; melt -query transitions 2>/dev/null' 2>/dev/null |
            grep -c 'frei0r\.cube3d' || true)
        if [ "$seen" = 2 ]; then
            echo "Flatpak: Kdenlive's MLT lists frei0r.cube3d and frei0r.cube3d_transition"
        else
            cat >&2 <<EOT

WARNING: Kdenlive's MLT does not list the plugins although they are installed.
Most likely they were built on a system whose C library is newer than the one
inside the Flatpak. Build them in an older environment (a container with an
older distribution will do) and install again.
EOT
            status=1
        fi
    fi
elif [ "$flatpak_present" = 1 ]; then
    cat <<EOT

The Kdenlive Flatpak is installed but 'flatpak' cannot be run from this shell,
so its settings were not checked. It needs this once (run it on the host):

    flatpak override --user --env=FREI0R_PATH=/app/lib/frei0r-1:$plugin_dir $APP

EOT
fi

echo "Done. Restart Kdenlive; look for \"3D Cube\" in the Effects and in the Compositions/Transitions lists."
exit $status
