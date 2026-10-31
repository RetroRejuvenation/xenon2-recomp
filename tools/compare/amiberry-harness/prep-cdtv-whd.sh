#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
# Lay out the host directory that run-cdtv-whd.sh boots as DH0: (WHDLoad + the Xenon 2 CDTV install + a startup-sequence).
# usage: prep-cdtv-whd.sh DH0_DIR
#   env WHDLOAD=<the WHDLoad executable, from the WHDLoad user package>
#       INSTALL=<unpacked WHDLoad install of Xenon 2 CDTV: a directory holding Xenon2CDTV.Slave and data/ with the
#                game's files (the 'Xenon2CDTV' folder of the install archives)>
# All inputs are user-supplied; nothing is downloaded and nothing is copied into the repository.
set -eu
DH0=$1
: "${WHDLOAD:?set WHDLOAD to the WHDLoad executable}" "${INSTALL:?set INSTALL to the Xenon2CDTV install directory}"
[ -f "$INSTALL/Xenon2CDTV.Slave" ] || { echo "no Xenon2CDTV.Slave in $INSTALL" >&2; exit 2; }
mkdir -p "$DH0/C" "$DH0/S"
cp "$WHDLOAD" "$DH0/C/WHDLoad"
rm -rf "$DH0/Xenon2CDTV"
cp -R "$INSTALL" "$DH0/Xenon2CDTV"
# WHDLoad locates the game's 'data' directory relative to the current directory, hence the cd.
printf 'cd dh0:Xenon2CDTV\ndh0:C/WHDLoad Xenon2CDTV.Slave PAL NOREQ\n' > "$DH0/S/startup-sequence"
echo "prepared $DH0"
