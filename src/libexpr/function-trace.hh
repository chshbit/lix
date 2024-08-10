// SPDX-FileCopyrightText: 2024 Nix and Lix Authors
//
// SPDX-License-Identifier: LGPL-2.1-only

#pragma once
///@file

#include "eval.hh"

#include <chrono>

namespace nix {

struct FunctionCallTrace
{
    const Pos pos;
    FunctionCallTrace(const Pos & pos);
    ~FunctionCallTrace();
};
}
