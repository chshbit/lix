#include "names.hh"
#include "primops.hh"

namespace nix {

std::shared_ptr<RegexCache> makeRegexCache()
{
    return std::make_shared<RegexCache>();
}

/**
 * builtins.baseNameOf
 */

static void prim_baseNameOf(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    NixStringContext context;
    v.mkString(
        baseNameOf(*state.coerceToString(
            pos,
            *args[0],
            context,
            "while evaluating the first argument passed to builtins.baseNameOf",
            false,
            false
        )),
        context
    );
}

static RegisterPrimOp primop_baseNameOf({
    .name = "baseNameOf",
    .args = {"s"},
    .doc = R"(
      Return the *base name* of the string *s*, that is, everything
      following the final slash in the string. This is similar to the GNU
      `basename` command.
    )",
    .fun = prim_baseNameOf,
});

/**
 * builtins.compareVersions
 */

static void prim_compareVersions(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto version1 = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the first argument passed to builtins.compareVersions"
    );
    auto version2 = state.forceStringNoCtx(
        *args[1], pos, "while evaluating the second argument passed to builtins.compareVersions"
    );
    v.mkInt(compareVersions(version1, version2));
}

static RegisterPrimOp primop_compareVersions({
    .name = "__compareVersions",
    .args = {"s1", "s2"},
    .doc = R"(
      Compare two strings representing versions and return `-1` if
      version *s1* is older than version *s2*, `0` if they are the same,
      and `1` if *s1* is newer than *s2*. The version comparison
      algorithm is the same as the one used by [`nix-env
      -u`](../command-ref/nix-env.md#operation---upgrade).
    )",
    .fun = prim_compareVersions,
});

/**
 * builtins.concatStringsSep
 */

static void prim_concatStringsSep(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    NixStringContext context;

    auto sep = state.forceString(
        *args[0],
        context,
        pos,
        "while evaluating the first argument (the separator string) passed to "
        "builtins.concatStringsSep"
    );
    state.forceList(
        *args[1],
        pos,
        "while evaluating the second argument (the list of strings to concat) passed to "
        "builtins.concatStringsSep"
    );

    std::string res;
    res.reserve((args[1]->listSize() + 32) * sep.size());
    bool first = true;

    for (auto elem : args[1]->listItems()) {
        if (first) {
            first = false;
        } else {
            res += sep;
        }
        res += *state.coerceToString(
            pos,
            *elem,
            context,
            "while evaluating one element of the list of strings to concat passed to "
            "builtins.concatStringsSep"
        );
    }

    v.mkString(res, context);
}

static RegisterPrimOp primop_concatStringsSep(
    {.name = "__concatStringsSep",
     .args = {"separator", "list"},
     .doc = R"(
      Concatenate a list of strings with a separator between each
      element, e.g. `concatStringsSep "/" ["usr" "local" "bin"] ==
      "usr/local/bin"`.
    )",
     .fun = prim_concatStringsSep}
);

/**
 * builtins.match
 */

void prim_match(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto re = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the first argument passed to builtins.match"
    );

    try {

        auto regex = state.regexCache->get(re);

        NixStringContext context;
        const auto str = state.forceString(
            *args[1], context, pos, "while evaluating the second argument passed to builtins.match"
        );

        std::cmatch match;
        if (!std::regex_match(str.begin(), str.end(), match, regex)) {
            v.mkNull();
            return;
        }

        // the first match is the whole string
        const size_t len = match.size() - 1;
        state.mkList(v, len);
        for (size_t i = 0; i < len; ++i) {
            if (!match[i + 1].matched) {
                (v.listElems()[i] = state.allocValue())->mkNull();
            } else {
                (v.listElems()[i] = state.allocValue())->mkString(match[i + 1].str());
            }
        }

    } catch (std::regex_error & e) {
        if (e.code() == std::regex_constants::error_space) {
            // limit is _GLIBCXX_REGEX_STATE_LIMIT for libstdc++
            state.error<EvalError>("memory limit exceeded by regular expression '%s'", re)
                .atPos(pos)
                .debugThrow();
        } else {
            state.error<EvalError>("invalid regular expression '%s'", re).atPos(pos).debugThrow();
        }
    }
}

static RegisterPrimOp primop_match({
    .name = "__match",
    .args = {"regex", "str"},
    .doc = R"s(
      Returns a list if the [extended POSIX regular
      expression](http://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap09.html#tag_09_04)
      *regex* matches *str* precisely, otherwise returns `null`. Each item
      in the list is a regex group.

      ```nix
      builtins.match "ab" "abc"
      ```

      Evaluates to `null`.

      ```nix
      builtins.match "abc" "abc"
      ```

      Evaluates to `[ ]`.

      ```nix
      builtins.match "a(b)(c)" "abc"
      ```

      Evaluates to `[ "b" "c" ]`.

      ```nix
      builtins.match "[[:space:]]+([[:upper:]]+)[[:space:]]+" "  FOO   "
      ```

      Evaluates to `[ "FOO" ]`.
    )s",
    .fun = prim_match,
});

/**
 * builtins.parseDrvName
 */

static void prim_parseDrvName(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto name = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the first argument passed to builtins.parseDrvName"
    );
    DrvName parsed(name);
    auto attrs = state.buildBindings(2);
    attrs.alloc(state.sName).mkString(parsed.name);
    attrs.alloc("version").mkString(parsed.version);
    v.mkAttrs(attrs);
}

static RegisterPrimOp primop_parseDrvName({
    .name = "__parseDrvName",
    .args = {"s"},
    .doc = R"(
      Split the string *s* into a package name and version. The package
      name is everything up to but not including the first dash not followed
      by a letter, and the version is everything following that dash. The
      result is returned in a set `{ name, version }`. Thus,
      `builtins.parseDrvName "nix-0.12pre12876"` returns `{ name =
      "nix"; version = "0.12pre12876"; }`.
    )",
    .fun = prim_parseDrvName,
});

/**
 * builtins.replaceStrings
 */

static void prim_replaceStrings(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceList(
        *args[0], pos, "while evaluating the first argument passed to builtins.replaceStrings"
    );
    state.forceList(
        *args[1], pos, "while evaluating the second argument passed to builtins.replaceStrings"
    );
    if (args[0]->listSize() != args[1]->listSize()) {
        state
            .error<EvalError>(
                "'from' and 'to' arguments passed to builtins.replaceStrings have different lengths"
            )
            .atPos(pos)
            .debugThrow();
    }

    std::vector<std::string> from;
    from.reserve(args[0]->listSize());
    for (auto elem : args[0]->listItems()) {
        from.emplace_back(state.forceString(
            *elem,
            pos,
            "while evaluating one of the strings to replace passed to builtins.replaceStrings"
        ));
    }

    std::unordered_map<size_t, std::string> cache;
    auto to = args[1]->listItems();

    NixStringContext context;
    auto s = state.forceString(
        *args[2],
        context,
        pos,
        "while evaluating the third argument passed to builtins.replaceStrings"
    );

    std::string res;
    // Loops one past last character to handle the case where 'from' contains an empty string.
    for (size_t p = 0; p <= s.size();) {
        bool found = false;
        auto i = from.begin();
        auto j = to.begin();
        size_t j_index = 0;
        for (; i != from.end(); ++i, ++j, ++j_index) {
            if (s.compare(p, i->size(), *i) == 0) {
                found = true;
                auto v = cache.find(j_index);
                if (v == cache.end()) {
                    NixStringContext ctx;
                    auto ts = state.forceString(
                        **j,
                        ctx,
                        pos,
                        "while evaluating one of the replacement strings passed to "
                        "builtins.replaceStrings"
                    );
                    v = (cache.emplace(j_index, ts)).first;
                    for (auto & path : ctx) {
                        context.insert(path);
                    }
                }
                res += v->second;
                if (i->empty()) {
                    if (p < s.size()) {
                        res += s[p];
                    }
                    p++;
                } else {
                    p += i->size();
                }
                break;
            }
        }
        if (!found) {
            if (p < s.size()) {
                res += s[p];
            }
            p++;
        }
    }

    v.mkString(res, context);
}

static RegisterPrimOp primop_replaceStrings({
    .name = "__replaceStrings",
    .args = {"from", "to", "s"},
    .doc = R"(
      Given string *s*, replace every occurrence of the strings in *from*
      with the corresponding string in *to*.

      The argument *to* is lazy, that is, it is only evaluated when its corresponding pattern in *from* is matched in the string *s*

      Example:

      ```nix
      builtins.replaceStrings ["oo" "a"] ["a" "i"] "foobar"
      ```

      evaluates to `"fabir"`.
    )",
    .fun = prim_replaceStrings,
});

/**
 * builtins.split
 */

void prim_split(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto re = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the first argument passed to builtins.split"
    );

    try {

        auto regex = state.regexCache->get(re);

        NixStringContext context;
        const auto str = state.forceString(
            *args[1], context, pos, "while evaluating the second argument passed to builtins.split"
        );

        auto begin = std::cregex_iterator(str.begin(), str.end(), regex);
        auto end = std::cregex_iterator();

        // Any matches results are surrounded by non-matching results.
        const size_t len = std::distance(begin, end);
        state.mkList(v, 2 * len + 1);
        size_t idx = 0;

        if (len == 0) {
            v.listElems()[idx++] = args[1];
            return;
        }

        for (auto i = begin; i != end; ++i) {
            assert(idx <= 2 * len + 1 - 3);
            auto match = *i;

            // Add a string for non-matched characters.
            (v.listElems()[idx++] = state.allocValue())->mkString(match.prefix().str());

            // Add a list for matched substrings.
            const size_t slen = match.size() - 1;
            auto elem = v.listElems()[idx++] = state.allocValue();

            // Start at 1, beacause the first match is the whole string.
            state.mkList(*elem, slen);
            for (size_t si = 0; si < slen; ++si) {
                if (!match[si + 1].matched) {
                    (elem->listElems()[si] = state.allocValue())->mkNull();
                } else {
                    (elem->listElems()[si] = state.allocValue())->mkString(match[si + 1].str());
                }
            }

            // Add a string for non-matched suffix characters.
            if (idx == 2 * len) {
                (v.listElems()[idx++] = state.allocValue())->mkString(match.suffix().str());
            }
        }

        assert(idx == 2 * len + 1);

    } catch (std::regex_error & e) {
        if (e.code() == std::regex_constants::error_space) {
            // limit is _GLIBCXX_REGEX_STATE_LIMIT for libstdc++
            state.error<EvalError>("memory limit exceeded by regular expression '%s'", re)
                .atPos(pos)
                .debugThrow();
        } else {
            state.error<EvalError>("invalid regular expression '%s'", re).atPos(pos).debugThrow();
        }
    }
}

static RegisterPrimOp primop_split({
    .name = "__split",
    .args = {"regex", "str"},
    .doc = R"s(
      Returns a list composed of non matched strings interleaved with the
      lists of the [extended POSIX regular
      expression](http://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap09.html#tag_09_04)
      *regex* matches of *str*. Each item in the lists of matched
      sequences is a regex group.

      ```nix
      builtins.split "(a)b" "abc"
      ```

      Evaluates to `[ "" [ "a" ] "c" ]`.

      ```nix
      builtins.split "([ac])" "abc"
      ```

      Evaluates to `[ "" [ "a" ] "b" [ "c" ] "" ]`.

      ```nix
      builtins.split "(a)|(c)" "abc"
      ```

      Evaluates to `[ "" [ "a" null ] "b" [ null "c" ] "" ]`.

      ```nix
      builtins.split "([[:upper:]]+)" " FOO "
      ```

      Evaluates to `[ " " [ "FOO" ] " " ]`.
    )s",
    .fun = prim_split,
});

/**
 * builtins.splitVersion
 */

static void prim_splitVersion(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto version = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the first argument passed to builtins.splitVersion"
    );
    auto iter = version.cbegin();
    Strings components;
    while (iter != version.cend()) {
        auto component = nextComponent(iter, version.cend());
        if (component.empty()) {
            break;
        }
        components.emplace_back(component);
    }
    state.mkList(v, components.size());
    for (const auto & [n, component] : enumerate(components)) {
        (v.listElems()[n] = state.allocValue())->mkString(std::move(component));
    }
}

static RegisterPrimOp primop_splitVersion({
    .name = "__splitVersion",
    .args = {"s"},
    .doc = R"(
      Split a string representing a version into its components, by the
      same version splitting logic underlying the version comparison in
      [`nix-env -u`](../command-ref/nix-env.md#operation---upgrade).
    )",
    .fun = prim_splitVersion,
});

/**
 * builtins.stringLength
 */

static void prim_stringLength(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    NixStringContext context;
    auto s = state.coerceToString(
        pos, *args[0], context, "while evaluating the argument passed to builtins.stringLength"
    );
    v.mkInt(s->size());
}

static RegisterPrimOp primop_stringLength({
    .name = "__stringLength",
    .args = {"e"},
    .doc = R"(
      Return the length of the string *e*. If *e* is not a string,
      evaluation is aborted.
    )",
    .fun = prim_stringLength,
});

/**
 * builtins.substring
 */

static void prim_substring(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    int start = state.forceInt(
        *args[0],
        pos,
        "while evaluating the first argument (the start offset) passed to builtins.substring"
    );

    if (start < 0) {
        state.error<EvalError>("negative start position in 'substring'").atPos(pos).debugThrow();
    }

    int len = state.forceInt(
        *args[1],
        pos,
        "while evaluating the second argument (the substring length) passed to builtins.substring"
    );

    // Special-case on empty substring to avoid O(n) strlen
    // This allows for the use of empty substrings to efficently capture string context
    if (len == 0) {
        state.forceValue(*args[2], pos);
        if (args[2]->type() == nString) {
            v.mkString("", args[2]->string.context);
            return;
        }
    }

    NixStringContext context;
    auto s = state.coerceToString(
        pos,
        *args[2],
        context,
        "while evaluating the third argument (the string) passed to builtins.substring"
    );

    v.mkString((unsigned int) start >= s->size() ? "" : s->substr(start, len), context);
}

static RegisterPrimOp primop_substring({
    .name = "__substring",
    .args = {"start", "len", "s"},
    .doc = R"(
      Return the substring of *s* from character position *start*
      (zero-based) up to but not including *start + len*. If *start* is
      greater than the length of the string, an empty string is returned,
      and if *start + len* lies beyond the end of the string, only the
      substring up to the end of the string is returned. *start* must be
      non-negative. For example,

      ```nix
      builtins.substring 0 3 "nixos"
      ```

      evaluates to `"nix"`.
    )",
    .fun = prim_substring,
});

/**
 * builtins.toString
 */

static void prim_toString(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    NixStringContext context;
    auto s = state.coerceToString(
        pos,
        *args[0],
        context,
        "while evaluating the first argument passed to builtins.toString",
        true,
        false
    );
    v.mkString(*s, context);
}

static RegisterPrimOp primop_toString({
    .name = "toString",
    .args = {"e"},
    .doc = R"(
      Convert the expression *e* to a string. *e* can be:

        - A string (in which case the string is returned unmodified).

        - A path (e.g., `toString /foo/bar` yields `"/foo/bar"`.

        - A set containing `{ __toString = self: ...; }` or `{ outPath = ...; }`.

        - An integer.

        - A list, in which case the string representations of its elements
          are joined with spaces.

        - A Boolean (`false` yields `""`, `true` yields `"1"`).

        - `null`, which yields the empty string.
    )",
    .fun = prim_toString,
});

}
