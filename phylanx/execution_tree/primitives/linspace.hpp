//   Copyright (c) 2018 R. Tohid
//
//   Distributed under the Boost Software License, Version 1.0. (See accompanying
//   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(PHYLANX_PRIMITIVES_LINSPACE_JAN_22_2018_0931AM)
#define PHYLANX_PRIMITIVES_LINSPACE_JAN_22_2018_0931AM

#include <phylanx/config.hpp>
#include <phylanx/execution_tree/primitives/base_primitive.hpp>
#include <phylanx/execution_tree/primitives/primitive_component_base.hpp>

#include <hpx/lcos/future.hpp>

#include <string>
#include <vector>

namespace phylanx { namespace execution_tree { namespace primitives
{
    /**
     * @brief Linear space of evenly distributed numbers over the given interval.
     *
     * @author R. Tohid
     * @version 0.0.1
     * @date 2018
     */
    class linspace : public primitive_component_base
    {
    public:
        static match_pattern_type const match_data;

        /**
         * @brief Default constructor.
         */
        linspace() = default;

        /**
         * @brief Creates a linear space of evenly spaced numbers over the given interval.
         *
         * @param args Is a vector with exactly three elements (in order):
         *
         * start: the first value of the sequence.\n
         * stop: the last value of the sequence. It will be ignored if the number of
         * samples is less than 2.\n
         * num_samples: number of samples in the sequence.
         */
        linspace(std::vector<primitive_argument_type>&& args);

        hpx::future<primitive_argument_type> eval(
            std::vector<primitive_argument_type> const& args) const override;
    };

    PHYLANX_EXPORT primitive create_linspace(hpx::id_type const& locality,
        std::vector<primitive_argument_type>&& operands,
        std::string const& name = "");
}}}

#endif