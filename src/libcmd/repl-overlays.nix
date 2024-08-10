# SPDX-FileCopyrightText: 2024 Nix and Lix Authors
#
# SPDX-License-Identifier: LGPL-2.1-only

info: initial: functions:
let
  final = builtins.foldl' (prev: function: prev // (function info final prev)) initial functions;
in
final
