#include "primops.hh"
#include "eval-inline.hh"
#include "eval-settings.hh"
#include "extra-primops.hh"
#include "store-api.hh"
#include "fetchers.hh"
#include "filetransfer.hh"
#include "registry.hh"
#include "url.hh"

#include <ctime>
#include <iomanip>
#include <regex>

namespace nix {

void emitTreeAttrs(
    EvalState & state,
    const fetchers::Tree & tree,
    const fetchers::Input & input,
    Value & v,
    bool emptyRevFallback,
    bool forceDirty)
{
    assert(input.isLocked());

    auto attrs = state.buildBindings(10);


    state.mkStorePathString(tree.storePath, attrs.alloc(state.sOutPath));

    // FIXME: support arbitrary input attributes.

    auto narHash = input.getNarHash();
    assert(narHash);
    attrs.alloc("narHash").mkString(narHash->to_string(Base::SRI, true));

    if (input.getType() == "git")
        attrs.alloc("submodules").mkBool(
            fetchers::maybeGetBoolAttr(input.attrs, "submodules").value_or(false));

    if (!forceDirty) {

        if (auto rev = input.getRev()) {
            attrs.alloc("rev").mkString(rev->gitRev());
            attrs.alloc("shortRev").mkString(rev->gitShortRev());
        } else if (emptyRevFallback) {
            // Backwards compat for `builtins.fetchGit`: dirty repos return an empty sha1 as rev
            auto emptyHash = Hash(HashType::SHA1);
            attrs.alloc("rev").mkString(emptyHash.gitRev());
            attrs.alloc("shortRev").mkString(emptyHash.gitShortRev());
        }

        if (auto revCount = input.getRevCount())
            attrs.alloc("revCount").mkInt(*revCount);
        else if (emptyRevFallback)
            attrs.alloc("revCount").mkInt(0);

    }

    if (auto dirtyRev = fetchers::maybeGetStrAttr(input.attrs, "dirtyRev")) {
        attrs.alloc("dirtyRev").mkString(*dirtyRev);
        attrs.alloc("dirtyShortRev").mkString(*fetchers::maybeGetStrAttr(input.attrs, "dirtyShortRev"));
    }

    if (auto lastModified = input.getLastModified()) {
        attrs.alloc("lastModified").mkInt(*lastModified);
        attrs.alloc("lastModifiedDate").mkString(
            fmt("%s", std::put_time(std::gmtime(&*lastModified), "%Y%m%d%H%M%S")));
    }

    v.mkAttrs(attrs);
}

std::string fixURI(std::string uri, EvalState & state, const std::string & defaultScheme = "file")
{
    state.checkURI(uri);
    if (uri.find("://") == std::string::npos) {
        const auto p = ParsedURL {
            .scheme = defaultScheme,
            .authority = "",
            .path = uri
        };
        return p.to_string();
    } else {
        return uri;
    }
}

std::string fixURIForGit(std::string uri, EvalState & state)
{
    /* Detects scp-style uris (e.g. git@github.com:NixOS/nix) and fixes
     * them by removing the `:` and assuming a scheme of `ssh://`
     * */
    static std::regex scp_uri("([^/]*)@(.*):(.*)");
    if (uri[0] != '/' && std::regex_match(uri, scp_uri))
        return fixURI(std::regex_replace(uri, scp_uri, "$1@$2/$3"), state, "ssh");
    else
        return fixURI(uri, state);
}

struct FetchTreeParams {
    bool emptyRevFallback = false;
    bool allowNameArgument = false;
};

static void fetchTree(
    EvalState & state,
    const PosIdx pos,
    Value * * args,
    Value & v,
    std::optional<std::string> type,
    const FetchTreeParams & params = FetchTreeParams{}
) {
    fetchers::Input input;
    NixStringContext context;

    state.forceValue(*args[0], pos);

    if (args[0]->type() == nAttrs) {
        state.forceAttrs(*args[0], pos, "while evaluating the argument passed to builtins.fetchTree");

        fetchers::Attrs attrs;

        if (auto aType = args[0]->attrs->get(state.sType)) {
            if (type)
                state.error<EvalError>(
                    "unexpected attribute 'type'"
                ).atPos(pos).debugThrow();
            type = state.forceStringNoCtx(*aType->value, aType->pos, "while evaluating the `type` attribute passed to builtins.fetchTree");
        } else if (!type)
            state.error<EvalError>(
                "attribute 'type' is missing in call to 'fetchTree'"
            ).atPos(pos).debugThrow();

        attrs.emplace("type", type.value());

        for (auto & attr : *args[0]->attrs) {
            if (attr.name == state.sType) continue;
            state.forceValue(*attr.value, attr.pos);
            if (attr.value->type() == nPath || attr.value->type() == nString) {
                auto s = state.coerceToString(attr.pos, *attr.value, context, "", false, false).toOwned();
                attrs.emplace(state.symbols[attr.name],
                    state.symbols[attr.name] == "url"
                    ? type == "git"
                      ? fixURIForGit(s, state)
                      : fixURI(s, state)
                    : s);
            }
            else if (attr.value->type() == nBool)
                attrs.emplace(state.symbols[attr.name], Explicit<bool>{attr.value->boolean});
            else if (attr.value->type() == nInt) {
                auto intValue = attr.value->integer.value;

                if (intValue < 0) {
                    state.error<EvalError>("negative value given for fetchTree attr %1%: %2%", state.symbols[attr.name], intValue).atPos(pos).debugThrow();
                }
                unsigned long asUnsigned = intValue;

                attrs.emplace(state.symbols[attr.name], asUnsigned);
            } else
                state.error<TypeError>("fetchTree argument '%s' is %s while a string, Boolean or integer is expected",
                    state.symbols[attr.name], showType(*attr.value)).debugThrow();
        }

        if (!params.allowNameArgument)
            if (auto nameIter = attrs.find("name"); nameIter != attrs.end())
                state.error<EvalError>(
                    "attribute 'name' isn’t supported in call to 'fetchTree'"
                ).atPos(pos).debugThrow();

        input = fetchers::Input::fromAttrs(std::move(attrs));
    } else {
        auto url = state.coerceToString(pos, *args[0], context,
                "while evaluating the first argument passed to the fetcher",
                false, false).toOwned();

        if (type == "git") {
            fetchers::Attrs attrs;
            attrs.emplace("type", "git");
            attrs.emplace("url", fixURIForGit(url, state));
            input = fetchers::Input::fromAttrs(std::move(attrs));
        } else {
            input = fetchers::Input::fromURL(fixURI(url, state));
        }
    }

    if (!evalSettings.pureEval && !input.isDirect())
        input = lookupInRegistries(state.store, input).first;

    if (evalSettings.pureEval && !input.isLocked()) {
        state.error<EvalError>("in pure evaluation mode, 'fetchTree' requires a locked input").atPos(pos).debugThrow();
    }

    auto [tree, input2] = input.fetch(state.store);

    state.allowPath(tree.storePath);

    emitTreeAttrs(state, tree, input2, v, params.emptyRevFallback, false);
}

static void prim_fetchTree(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    experimentalFeatureSettings.require(Xp::Flakes);
    fetchTree(state, pos, args, v, std::nullopt, FetchTreeParams { .allowNameArgument = false });
}

// FIXME: document
static RegisterPrimOp primop_fetchTree({
    .name = "fetchTree",
    .arity = 1,
    .fun = prim_fetchTree
});

static void fetch(EvalState & state, const PosIdx pos, Value * * args, Value & v,
    const std::string & who, bool unpack, std::string name)
{
    std::optional<std::string> url;
    std::optional<Hash> expectedHash;

    state.forceValue(*args[0], pos);

    if (args[0]->type() == nAttrs) {

        for (auto & attr : *args[0]->attrs) {
            std::string_view n(state.symbols[attr.name]);
            if (n == "url")
                url = state.forceStringNoCtx(*attr.value, attr.pos, "while evaluating the url we should fetch");
            else if (n == "sha256")
                expectedHash = newHashAllowEmpty(state.forceStringNoCtx(*attr.value, attr.pos, "while evaluating the sha256 of the content we should fetch"), HashType::SHA256);
            else if (n == "name")
                name = state.forceStringNoCtx(*attr.value, attr.pos, "while evaluating the name of the content we should fetch");
            else
                state.error<EvalError>("unsupported argument '%s' to '%s'", n, who)
                .atPos(pos).debugThrow();
        }

        if (!url)
            state.error<EvalError>(
                "'url' argument required").atPos(pos).debugThrow();
    } else
        url = state.forceStringNoCtx(*args[0], pos, "while evaluating the url we should fetch");

    if (who == "fetchTarball")
        url = evalSettings.resolvePseudoUrl(*url);

    state.checkURI(*url);

    if (name == "")
        name = baseNameOf(*url);

    if (evalSettings.pureEval && !expectedHash)
        state.error<EvalError>("in pure evaluation mode, '%s' requires a 'sha256' argument", who).atPos(pos).debugThrow();

    // early exit if pinned and already in the store
    if (expectedHash && expectedHash->type == HashType::SHA256) {
        auto expectedPath = state.store->makeFixedOutputPath(
            name,
            FixedOutputInfo {
                .method = unpack ? FileIngestionMethod::Recursive : FileIngestionMethod::Flat,
                .hash = *expectedHash,
                .references = {}
            });

        if (state.store->isValidPath(expectedPath)) {
            state.allowAndSetStorePathString(expectedPath, v);
            return;
        }
    }

    // TODO: fetching may fail, yet the path may be substitutable.
    //       https://github.com/NixOS/nix/issues/4313
    auto storePath =
        unpack
        ? fetchers::downloadTarball(state.store, *url, name, (bool) expectedHash).tree.storePath
        : fetchers::downloadFile(state.store, *url, name, (bool) expectedHash).storePath;

    if (expectedHash) {
        auto hash = unpack
            ? state.store->queryPathInfo(storePath)->narHash
            : hashFile(HashType::SHA256, state.store->toRealPath(storePath));
        if (hash != *expectedHash) {
            state.error<EvalError>(
                "hash mismatch in file downloaded from '%s':\n  specified: %s\n  got:       %s",
                *url,
                expectedHash->to_string(Base::Base32, true),
                hash.to_string(Base::Base32, true)
            ).withExitStatus(102)
            .debugThrow();
        }
    }

    state.allowAndSetStorePathString(storePath, v);
}

void prim_fetchurl(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    fetch(state, pos, args, v, "fetchurl", false, "");
}

void prim_fetchTarball(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    fetch(state, pos, args, v, "fetchTarball", true, "source");
}

void prim_fetchGit(EvalState & state, const PosIdx pos, Value * * args, Value & v)
{
    fetchTree(state, pos, args, v, "git", FetchTreeParams { .emptyRevFallback = true, .allowNameArgument = true });
}

}
