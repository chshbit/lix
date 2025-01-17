#pragma once
///@file

#include "error.hh"
#include "types.hh"

namespace nix {

/**
 * The list of available deprecated features.
 *
 * Reminder: New deprecated features should start out with a warning without throwing an error.
 * See the developer documentation for details.
 */
enum struct DeprecatedFeature
{
    #include "deprecated-features.gen.inc"
    NumDepFeatures, // number of available deprecated features, do not use
};

enum struct DeprecatedFeatures {};

inline DeprecatedFeatures operator| (DeprecatedFeatures a, DeprecatedFeatures b) {
    return static_cast<DeprecatedFeatures>(static_cast<size_t>(a) | static_cast<size_t>(b));
}

inline DeprecatedFeatures operator| (DeprecatedFeatures a, DeprecatedFeature b) {
    return a | static_cast<DeprecatedFeatures>(1 << static_cast<size_t>(b));
}

inline DeprecatedFeatures operator& (DeprecatedFeatures a, DeprecatedFeature b) {
    return static_cast<DeprecatedFeatures>(static_cast<size_t>(a) & (1 << static_cast<size_t>(b)));
}

/**
 * Just because writing `DeprecatedFeature::UrlLiterals` is way too long
 */
using Dep = DeprecatedFeature;

/**
 * Parse a deprecated feature (enum value) from its name. Deprecated
 * feature flag names are hyphenated and do not contain spaces.
 */
const std::optional<DeprecatedFeature> parseDeprecatedFeature(
        const std::string_view & name);

/**
 * Show the name of a deprecated feature. This is the opposite of
 * parseDeprecatedFeature().
 */
std::string_view showDeprecatedFeature(const DeprecatedFeature);

/**
 * Shorthand for `str << showDeprecatedFeature(feature)`.
 */
std::ostream & operator<<(
        std::ostream & str,
        const DeprecatedFeature & feature);

/**
 * Parse a set of strings to the corresponding set of deprecated
 * features, ignoring (but warning for) any unknown feature.
 */
DeprecatedFeatures parseDeprecatedFeatures(const std::set<std::string> &);

/**
 * A deprecated feature used for some
 * operation, but was not enabled.
 */
class MissingDeprecatedFeature : public Error
{
public:
    /**
     * The deprecated feature that was required but not enabled.
     */
    DeprecatedFeature missingFeature;

    MissingDeprecatedFeature(DeprecatedFeature missingFeature);
};

}
