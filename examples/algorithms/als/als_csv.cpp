//   Copyright (c) 2018 Shahrzad Shirzad
//
//   Distributed under the Boost Software License, Version 1.0.0. (See accompanying
//   file LICENSE_1_0.0.txt or copy at http://www.boost.org/LICENSE_1_0.0.txt)

#include <phylanx/phylanx.hpp>
#include <hpx/hpx_init.hpp>

#include <iostream>

#include <blaze/Math.h>
#include <hpx/program_options.hpp>
#include <cstdint>
#include <string>

///////////////////////////////////////////////////////////////////////////////
char const* const read_x_code = R"(
    //
    // Read input-data from given CSV file
    //
    define(read_x, filepath, row_start, row_stop, col_start, col_stop,
        slice(file_read_csv(filepath), make_list(row_start , row_stop),
              make_list(col_start , col_stop))
    )
    read_x
)";


char const* const als_code = R"(
    //
    // Alternating Least squares algorithm
    //
    define(__als, ratings, regularization, num_factors, iterations, alpha, enable_output,
        block(
            define(num_users, shape(ratings, 0)),
            define(num_items, shape(ratings, 1)),
            define(comp_r, __ne(ratings,0.0,true)),
            define(conf, alpha * ratings),

            define(conf_u, constant(0.0, make_list(num_items))),
            define(conf_i, constant(0.0,make_list(num_users))),

            define(c_u, constant(0.0, make_list(num_items, num_items))),
            define(c_i, constant(0.0, make_list(num_users, num_users))),
            define(p_u, constant(0.0, make_list(num_items))),
            define(p_i, constant(0.0, make_list(num_users))),


            define(X, random(make_list(num_users, num_factors))),
            define(Y, random(make_list(num_items, num_factors))),
            define(I_f, identity(num_factors)),
            define(I_i, identity(num_items)),
            define(I_u, identity(num_users)),
            define(k, 0),
            define(i, 0),
            define(u, 0),

            define(XtX, dot(transpose(X), X) + regularization * I_f),
            define(YtY, dot(transpose(Y), Y) + regularization * I_f),
            define(A, constant(0.0, make_list(num_factors, num_factors))),
            define(b, constant(0.0, make_list(num_factors))),

            while(k < iterations,
                block(
                    if(enable_output,
                            block(
                                    cout("iteration ",k),
                                    cout("X: ", X),
                                    cout("Y: ", Y)
                            )
                    ),
                    

                    while(u < num_users,
                        block(
                            store(conf_u, slice_row(conf, u)),
                            store(c_u, diag(conf_u)),
                            store(p_u, __ne(conf_u,0.0,true)),
                            store(A, dot(dot(transpose(Y), c_u), Y)+ YtY),
                            store(b, dot(dot(transpose(Y), (c_u + I_i)), p_u)),
                            store(slice_row(X, u), dot(inverse(A), b)),
                            store(u, u + 1)
                        )
                    ),
                    store(u, 0),

                    store(XtX, dot(transpose(X), X) + regularization * I_f),

                    while(i < num_items,
                        block(
                            store(conf_i, slice_column(conf, i)),
                            store(c_i, diag(conf_i)),
                            store(p_i, __ne(conf_i, 0.0, true)),
                            store(A, dot(dot(transpose(X), c_i),X) + XtX),
                            store(b, dot(dot(transpose(X), (c_i + I_u)), p_i)),
                            store(slice_row(Y, i), dot(inverse(A), b)),
                            store(i, i + 1)
                        )
                    ),
                    store(i, 0),

                    store(YtY, dot(transpose(Y), Y) + regularization * I_f),

                    store(k, k + 1)
                )
            ),
            //list(comp_r, dot(X, transpose(Y)))
            list(X, Y, comp_r, dot(X, transpose(Y)))
        )
    )
    __als
)";

int hpx_main(hpx::program_options::variables_map& vm)
{
    if (vm.count("data_csv") == 0)
    {
        std::cerr << "Please specify '--data_csv=data-file'";
        return hpx::finalize();
    }

    // evaluate generated execution tree
    auto row_start = static_cast<int64_t>(0);
    auto col_start = static_cast<int64_t>(0);
    auto row_stop = vm["row_stop"].as<std::int64_t>();
    auto col_stop = vm["col_stop"].as<std::int64_t>();

    auto regularization = vm["regularization"].as<double>();
    auto iterations = vm["iterations"].as<int64_t>();
    auto num_factors = vm["factors"].as<int64_t>();
    auto alpha = vm["alpha"].as<double>();
    auto filepath = vm["data_csv"].as<std::string>();

    bool enable_output = vm.count("enable_output") != 0;
    //bool enable_output = 1;

    // compile the given code
    phylanx::execution_tree::compiler::function_list snippets;
    auto const& code_read_x =
        phylanx::execution_tree::compile("read_x", read_x_code, snippets);
    auto read_x = code_read_x.run();

    auto ratings = read_x(filepath, row_start, row_stop, col_start, col_stop);

    auto const& code_als = phylanx::execution_tree::compile(als_code, snippets);
    auto als = code_als.run();

    hpx::evaluate_active_counters(true, "start");
    hpx::util::high_resolution_timer t;
    auto result = als(
        ratings, regularization, num_factors, iterations, alpha, enable_output);
    auto time_diff = t.elapsed();
    std::cout << t.elapsed();

    hpx::evaluate_active_counters(true, " finish");

    auto result_r = phylanx::execution_tree::extract_list_value(result);
    auto it = result_r.begin();
    std::cout << "X: \n"
              << phylanx::execution_tree::extract_numeric_value(*it++)
              << "\n Y: \n"
              << phylanx::execution_tree::extract_numeric_value(*it++)
              << "\n True rating: \n"
              << phylanx::execution_tree::extract_numeric_value(*it++)
              << "\n Our rating: \n"
              << phylanx::execution_tree::extract_numeric_value(*it)
              << std::endl;

    return hpx::finalize();
}

int main(int argc, char* argv[])
{
    // command line handling
    hpx::program_options::options_description desc("usage: als [options]");
    desc.add_options()
            ("enable_output,e", "enable progress output (default: false)")
            ("iterations,i",
             hpx::program_options::value<std::int64_t>()->default_value(5),
             "number of iterations (default: 10.0)")
            ("factors,f",
             hpx::program_options::value<std::int64_t>()->default_value(10),
             "number of factors (default: 10)")
            ("alpha,a",
             hpx::program_options::value<double>()->default_value(40),
             "alpha (default: 40)")
            ("regularization,r",
             hpx::program_options::value<double>()->default_value(0.01),
             "regularization (default: 0.1)")
            ("data_csv",
             hpx::program_options::value<std::string>(),
             "file name for reading data")
            ("row_stop",
             hpx::program_options::value<std::int64_t>()->default_value(200),
             "row_stop (default: 10)")
            ("col_stop",
             hpx::program_options::value<std::int64_t>()->default_value(400),
             "col_stop (default: 100)")
            ;
    return hpx::init(desc, argc, argv);
}
