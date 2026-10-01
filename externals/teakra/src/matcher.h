#pragma once

#include <algorithm>
#include <vector>
#include "common_types.h"
#include "crash.h"

struct Rejector {
    u16 mask;
    u16 unexpected;
    bool Rejects(u16 instruction) const {
        return (instruction & mask) == unexpected;
    }
};

template <typename Visitor>
class Matcher {
public:
    using visitor_type = Visitor;
    using handler_return_type = typename Visitor::instruction_return_type;
    using handler_function = handler_return_type (*)(Visitor&, u16, u16);
    // Returns the handler specialized for one opcode (see MatcherCreator::CreateFixed).
    using fixed_handler_lookup = handler_function (*)(u16);
    // Returns true if one of an opcode's operands names a register that changes the program
    // flow or the interrupt state (see IsFlowRegister).
    using flow_register_check = bool (*)(u16);

    Matcher(const char* const name, u16 mask, u16 expected, bool expanded, handler_function func,
            flow_register_check flow_register = nullptr, fixed_handler_lookup fixed = nullptr)
        : name{name}, mask{mask}, expected{expected}, expanded{expanded}, fn{func},
          flow_register{flow_register}, fixed{fixed} {}

    static Matcher AllMatcher(handler_function func) {
        return Matcher("*", 0, 0, false, func);
    }

    const char* GetName() const {
        return name;
    }

    bool NeedExpansion() const {
        return expanded;
    }

    bool Matches(u16 instruction) const {
        return (instruction & mask) == expected &&
               std::none_of(rejectors.begin(), rejectors.end(),
                            [instruction](const Rejector& rejector) {
                                return rejector.Rejects(instruction);
                            });
    }

    Matcher Except(Rejector rejector) const {
        Matcher new_matcher(*this);
        new_matcher.rejectors.push_back(rejector);
        return new_matcher;
    }

    handler_return_type call(Visitor& v, u16 instruction, u16 instruction_expansion = 0) const {
        ASSERT(Matches(instruction));
        return fn(v, instruction, instruction_expansion);
    }

    bool UsesFlowRegister(u16 instruction) const {
        return flow_register && flow_register(instruction);
    }

    // Returns the handler for `instruction`, or one specialized for it if the matcher has them.
    handler_function GetHandler(u16 instruction) const {
        return fixed ? fixed(instruction) : fn;
    }

private:
    const char* name;
    u16 mask;
    u16 expected;
    bool expanded;
    handler_function fn;
    flow_register_check flow_register;
    fixed_handler_lookup fixed;
    std::vector<Rejector> rejectors;
};
