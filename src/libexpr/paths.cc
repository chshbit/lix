// SPDX-FileCopyrightText: 2024 Nix and Lix Authors
//
// SPDX-License-Identifier: LGPL-2.1-only

#include "eval.hh"

namespace nix {

SourcePath EvalState::rootPath(CanonPath path)
{
    return path;
}

}
