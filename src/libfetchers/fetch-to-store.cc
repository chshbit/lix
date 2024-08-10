// SPDX-FileCopyrightText: 2024 Nix and Lix Authors
//
// SPDX-License-Identifier: LGPL-2.1-only

#include "fetch-to-store.hh"
#include "fetchers.hh"
#include "cache.hh"

namespace nix {

StorePath fetchToStore(
    Store & store,
    const SourcePath & path,
    std::string_view name,
    FileIngestionMethod method,
    PathFilter * filter,
    RepairFlag repair)
{
    Activity act(*logger, lvlChatty, actUnknown, fmt("copying '%s' to the store", path));

    auto filter2 = filter ? *filter : defaultPathFilter;

    return
        settings.readOnlyMode
        ? store.computeStorePathForPath(name, path.path.abs(), method, htSHA256, filter2).first
        : store.addToStore(name, path.path.abs(), method, htSHA256, filter2, repair);
}


}
