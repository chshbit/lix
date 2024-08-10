// SPDX-FileCopyrightText: 2024 Nix and Lix Authors
//
// SPDX-License-Identifier: LGPL-2.1-only

#pragma once
///@file

#include "nixexpr.hh"
#include "eval.hh"

#include <string>
#include <map>

namespace nix {

void printValueAsXML(EvalState & state, bool strict, bool location,
    Value & v, std::ostream & out, NixStringContext & context, const PosIdx pos);

}
