#pragma once

namespace nix {

/**
 * C++17 std::visit boilerplate
 */
template<class... Ts>
struct overloaded : Ts...
{
    using Ts::operator()...;
};

template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

}
