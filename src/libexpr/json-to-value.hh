// SPDX-FileCopyrightText: 2024 Nix and Lix Authors
//
// SPDX-License-Identifier: LGPL-2.1-only

#pragma once
///@file

#include "error.hh"

#include <string>

namespace nix {

class EvalState;
struct Value;

MakeError(JSONParseError, Error);

void parseJSON(EvalState & state, const std::string_view & s, Value & v);

}
