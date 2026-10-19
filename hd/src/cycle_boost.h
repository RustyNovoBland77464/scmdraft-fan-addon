// Cycle-boost search shares isolated trials, parallelism and joint validation with cargo boost.
#pragma once
#include "return_boost.h"
namespace cycle_boost {
using Rule = return_boost::Rule;
struct Plan : return_boost::Plan {
    Plan(const bwgame::state &st, std::vector<mining_boost::Assignment> a,
         std::vector<Rule> other = {}, boost_compute::Executor *pool = nullptr)
        : return_boost::Plan(st, std::move(a), std::move(other), pool, true) {}
};
}
