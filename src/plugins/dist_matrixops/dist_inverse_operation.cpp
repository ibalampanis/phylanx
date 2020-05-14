// Copyright (c) 2020 Rory Hector
// Copyright (c) 2018-2020 Hartmut Kaiser
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// To do:
// Handle cases of non-dense matrices
// Reduce fetches and overhead used for distributed computation

#include <phylanx/config.hpp>
#include <phylanx/execution_tree/annotation.hpp>
#include <phylanx/execution_tree/localities_annotation.hpp>
#include <phylanx/execution_tree/locality_annotation.hpp>
#include <phylanx/execution_tree/meta_annotation.hpp>
#include <phylanx/execution_tree/primitives/node_data_helpers.hpp>
#include <phylanx/execution_tree/tiling_annotations.hpp>
#include <phylanx/ir/node_data.hpp>
#include <phylanx/plugins/dist_matrixops/dist_inverse_operation.hpp>
#include <phylanx/plugins/dist_matrixops/tile_calculation_helper.hpp>
#include <phylanx/util/detail/bad_swap.hpp>

#include <hpx/errors/throw_exception.hpp>
#include <hpx/include/lcos.hpp>
#include <hpx/include/naming.hpp>
#include <hpx/include/util.hpp>
#include <hpx/lcos/future.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <blaze/Math.h>
#include <blaze_tensor/Math.h>

#include <phylanx/util/distributed_matrix.hpp>

// This is just boilerplate
namespace phylanx { namespace dist_matrixops { namespace primitives {
    // Declare the help_string details, just for explaining what
    // a primitive is doing, if needed.
    constexpr char const* const help_string = R"(
        inverse_d(matrix)
        Args:

            blaze dynamic matrix

        Returns:

            the inverse of the matrix
        )";

    // For the plugin file, you need to tell Phylanx these things about
    // your primtiive in a tuple
    // (1) name of the primitive, e.g. inverse_d
    // (2) a description of how the primitive can be invoked in the compiler
    //        _k means the primitive should be invoked with k arguments
    // (3) a creation function (defined in header)
    // (4) a connection between the name and the type (always the same)
    // (5) describes the usage of the primitive
    execution_tree::match_pattern_type const dist_inverse::match_data = {
        hpx::util::make_tuple("inverse_d", std::vector<std::string>{R"(
			inverse_d(
				_1_matrix
				)
			)"},
            &create_dist_inverse,
            &execution_tree::create_primitive<dist_inverse>, help_string)};

    // Pretty much boilerplate, just change the name, or may need to use
    // std::vector<primitive_arguments_type>&& for multiple args.
    // Passes the arguments along to the constructor of the base class.
    // Stores the three arguments as members.
    // Name = name of primitive instance, Codename = file used to generate it
    dist_inverse::dist_inverse(
        execution_tree::primitive_arguments_type&& operands,
        std::string const& name, std::string const& codename)
      : primitive_component_base(std::move(operands), name, codename)
    {
    }

    // find the first column belonging to a locality
    std::size_t getStartCol(std::size_t id, std::size_t n, std::size_t numLocs)
    {
        std::size_t startCol = id * (n / numLocs) + std::min(id, n % numLocs);
        return startCol;
    }

    // find the final column belonging to a locality
    std::size_t getEndCol(std::size_t id, std::size_t n, std::size_t numLocs)
    {
        std::size_t endCol;
        if (id == numLocs - 1)
            endCol = n;
        else
            endCol = getStartCol(id + 1, n, numLocs);
        return endCol;
    }

    // find the locality who holds a given column locally
    std::size_t findOwningLoc(
        std::size_t n, std::size_t numLocs, std::size_t col)
    {
        std::size_t id;
        if (col < (n % numLocs) * (n / numLocs + 1))
            id = col / ((n / numLocs) + 1);
        else
            id = (n % numLocs) +
                ((col - (n % numLocs) * (n / numLocs + 1)) / (n / numLocs));
        return id;
    }

    // This is where the computation of the inverse should be performed.
    template <typename T>
    execution_tree::primitive_argument_type dist_inverse::distGaussInv(
        ir::node_data<T>&& arg,
        execution_tree::localities_information&& lhs_localities) const
    {
        if (lhs_localities.num_dimensions() != 2)
        {
            HPX_THROW_EXCEPTION(hpx::bad_parameter,
                "dist_inverse::distGaussInv",
                generate_error_message("the input must be a 2d matrix"));
        }

        //std::cout << makeplus1(0) << std::endl;
        //std::cout << makeplus1(1) << std::endl;
        //std::cout << makeplus1(2) << std::endl;

        std::size_t thisLocalityID = lhs_localities.locality_.locality_id_;
        //std::cout << "This is locality " << thisLocalityID << std::endl;

        //std::cout << "Data belonging to this locality:" << std::endl;
        //std::cout << arg.matrix() << std::endl;

        std::size_t numLocalities = lhs_localities.locality_.num_localities_;
        //std::cout << "Total number of localities = " << numLocalities
        //          << std::endl;

        //std::cout << "The localities information is as follows: " << std::endl;
        //std::cout << "   Columns = " << lhs_localities.columns() << std::endl;
        //std::cout << "   Rows = " << lhs_localities.rows() << std::endl;
        //std::cout << "   Size = " << lhs_localities.size() << std::endl;

        //std::cout << "Tile information: " << std::endl;
        //std::size_t numTilesSeen = 1;
        //for (auto const& lhsTile : lhs_localities.tiles_)
        //{
            //std::cout << "    "
            //          << "Tile " << numTilesSeen
            //          << " Dimension = " << lhsTile.dimension() << std::endl;
            //std::cout << "    "
            //          << "Tile " << numTilesSeen
            //          << " Span Name 1 = " << lhsTile.get_span_name(1)
            //          << std::endl;
            //numTilesSeen++;
        //}

        util::distributed_matrix<T> lhs_data(lhs_localities.annotation_.name_,
            arg.matrix(), lhs_localities.locality_.num_localities_,
            lhs_localities.locality_.locality_id_);

        // Gets the span of the columns, i.e. startCol, endCol+1
        execution_tree::tiling_span const& lhs_span =
            lhs_localities.get_span(1);

        // Create a matrix of the proper size initialized to zeros
        // of type T
        blaze::DynamicMatrix<T> result_matrix(
            arg.dimension(0), lhs_localities.columns(), T{0});

        // Turn result matrix from a matrix of zeros into an identity matrix
        for (int j = 0; j<result_matrix.rows(); j++)
            result_matrix(j, j) = 1.0;

        // std::cout << result_matrix << std::endl;

        // Generic barrier. Give each barrier a unique identifier
        if (numLocalities > 1)
        {
            hpx::lcos::barrier b("barrier1_" + lhs_localities.annotation_.name_,
                lhs_localities.locality_.num_localities_,
                lhs_localities.locality_.locality_id_);
            b.wait();
        }

        auto myMatrix = arg.matrix();
        std::size_t numRows = myMatrix.rows();
        std::size_t numCols = myMatrix.columns();

        // Define some additional information on where
        // columns lie in the full input
        std::size_t startCol = thisLocalityID * (numRows / numLocalities) +
            std::min(thisLocalityID, numRows % numLocalities);
        std::size_t endCol = startCol + numCols - 1;

        // Definition of this loc's part of a nxn double row-major identity matrix
        blaze::DynamicMatrix<double> invMatrix =
            blaze::submatrix(blaze::IdentityMatrix<double>(numRows), 0,
                startCol, arg.matrix().rows(), arg.matrix().columns());

            // For swapping rows back into place
            std::vector<std::tuple<std::size_t, std::size_t>> swappedRows;

            // Do gaussian elimination to get upper triangular
            // matrix with 1's across diagonal
            for (std::int64_t current_row = 0; current_row < numRows;
                 current_row++)
            {
                //// Swaps current row with nearest subsequent row such that
                //// after swapping A[i][i] != 0.
                //if ((*lhs_data)(current_row, current_row) ==
                //    0)    // if A[i][i] = 0,
                //{
                //    bool rowFound = false;
                //    std::size_t checkOffset = 1;
                //    while (current_row + checkOffset < numRows && !rowFound)
                //    {
                //        if ((*lhs_data)(
                //                (current_row + checkOffset), current_row) != 0)
                //        {
                //            swap(column(myMatrix, current_row),
                //                column(myMatrix,
                //                    (current_row + checkOffset) % numRows));
                //            swappedRows.emplace_back(
                //                std::tuple<std::size_t, std::size_t>(
                //                    current_row,
                //                    (current_row + checkOffset) % numRows));
                //            rowFound = true;
                //        }
                //        else
                //            checkOffset++;
                //    }

                //    // swap row with nearest subsequent row such that after
                //    // swapping A[i][i] != 0
                //    // if fails, inverse does not exist
                //    if (!rowFound)
                //    {
                //        HPX_THROW_EXCEPTION(hpx::bad_parameter,
                //            "gauss_inverse::eval",
                //            generate_error_message("inverse does not exist"));
                //    }
                //    current_row--;    // After swapping, make sure to retry this row
                //}
                //else    // the inversion has not already failed
                //{




                     //if (numLocalities > 1)
                     //{
                     //    hpx::lcos::barrier b9(
                     //        "barrierz_" + lhs_localities.annotation_.name_,
                     //        lhs_localities.locality_.num_localities_,
                     //        lhs_localities.locality_.locality_id_);
                     //    b9.wait();
                     //}

                     // Find the locality that owns the pivot element then get the pivot
                     std::size_t ownid1 = findOwningLoc(numRows, numLocalities, current_row);
                     std::size_t localIndexOffset = current_row -
                         getStartCol(ownid1, numRows, numLocalities);
                     auto pulledElement =
                         lhs_data
                                .fetch(ownid1, current_row, current_row + 1,
                                 localIndexOffset, localIndexOffset + 1)
                         .get();
                     double scale = pulledElement(0, 0);

                     // Removing this barrier causes errors
                     if (numLocalities > 1)
                     {
                         hpx::lcos::barrier b9(
                             "barrierz_" + lhs_localities.annotation_.name_,
                             lhs_localities.locality_.num_localities_,
                             lhs_localities.locality_.locality_id_);
                         b9.wait();
                     }

                    for (std::int64_t col = 0; col < numCols; col++)
                    {

                        (*lhs_data)(current_row, col) =
                            (*lhs_data)(current_row, col) / scale;
                        invMatrix(current_row, col) =
                            invMatrix(current_row, col) / scale;
                    }
                    if (current_row < numRows - 1)
                    {
                        for (std::int64_t nextRow = current_row + 1;
                             nextRow < numRows; nextRow++)
                        {
                            //if (numLocalities > 1)
                            //{
                            //    hpx::lcos::barrier b3("barrierc_" +
                            //            lhs_localities.annotation_.name_,
                            //        lhs_localities.locality_.num_localities_,
                            //        lhs_localities.locality_.locality_id_);
                            //    b3.wait();
                            //}

                            // Find the locality that owns the pivot element then get the pivot
                            std::size_t ownid2 = findOwningLoc(
                                numRows, numLocalities, current_row);
                            std::size_t localIndexOffset2 = current_row -
                                getStartCol(ownid2, numRows, numLocalities);
                            auto pulledElement =
                                lhs_data
                                    .fetch(ownid2, nextRow, nextRow + 1,
                                        localIndexOffset2, localIndexOffset2 + 1)
                                    .get();
                            double factor = pulledElement(0, 0);

                            // Removing this barrier causes errors
                            if (numLocalities > 1)
                            {
                                hpx::lcos::barrier b9("barrierz_" +
                                        lhs_localities.annotation_.name_,
                                    lhs_localities.locality_.num_localities_,
                                    lhs_localities.locality_.locality_id_);
                                b9.wait();
                            }


                            for (std::int64_t nextCol = 0;
                                 nextCol < numCols; nextCol++)
                            {
                                (*lhs_data)(nextRow, nextCol) =
                                    (*lhs_data)(nextRow, nextCol) -
                                    (factor *
                                        (*lhs_data)(current_row, nextCol));
                                invMatrix(nextRow, nextCol) =
                                    invMatrix(nextRow, nextCol) -
                                    (factor * invMatrix(current_row, nextCol));
                            }
                            if (numLocalities > 1)
                            {
                                hpx::lcos::barrier b2("barrierb" +
                                        std::to_string(nextRow) +
                                        "_" +
                                        lhs_localities.annotation_.name_,
                                    lhs_localities.locality_.num_localities_,
                                    lhs_localities.locality_.locality_id_);
                                b2.wait();
                            }

                        }
                    }
               // }

               hpx::lcos::barrier b5("barriere" + std::to_string(current_row) +
                        "_" + lhs_localities.annotation_.name_,
                    lhs_localities.locality_.num_localities_,
                    lhs_localities.locality_.locality_id_);
                b5.wait();

            }


            // Back substitution phase, going from bottom to top
            // in matrix zeroing out columns except diagonal
            for (std::int64_t zeroCol = numRows - 1; zeroCol > 0; zeroCol--)
            {
                for (std::int64_t row = zeroCol - 1; row >= 0; row--)
                {
                    // Find the locality that owns the pivot element then get the pivot
                    std::size_t ownid3 =
                        findOwningLoc(numRows, numLocalities, zeroCol);
                    std::size_t localIndexOffset3 =
                        zeroCol -
                        getStartCol(ownid3, numRows, numLocalities);
                    auto pulledElement =
                        lhs_data
                            .fetch(ownid3, row, row + 1,
                                localIndexOffset3, localIndexOffset3 + 1)
                            .get();
                    double factor = pulledElement(0, 0);

                    // Removing this barrier causes errors
                    if (numLocalities > 1)
                    {
                        hpx::lcos::barrier b9(
                            "barrierz_" + lhs_localities.annotation_.name_,
                            lhs_localities.locality_.num_localities_,
                            lhs_localities.locality_.locality_id_);
                        b9.wait();
                    }


                    for (std::int64_t col = 0; col < numCols; col++)
                    {
                        myMatrix(row, col) = myMatrix(row, col) -
                            (factor * myMatrix(zeroCol, col));
                        invMatrix(row, col) = invMatrix(row, col) -
                            (factor * invMatrix(zeroCol, col));
                    }
                }
            }

            // Swap rows back into place
            for (std::int64_t i = swappedRows.size() - 1; i >= 0; i--)
            {
                swap(row(invMatrix, std::get<0>(swappedRows[i])),
                    row(invMatrix, std::get<1>(swappedRows[i])));
            }

            // std::cout << invMatrix << std::endl;

            // Prepare the output 
            execution_tree::primitive_argument_type result =
                execution_tree::primitive_argument_type{invMatrix};
            execution_tree::annotation ann{ir::range("tile",
                ir::range("rows", static_cast<std::int64_t>(0),
                    static_cast<std::int64_t>(numRows)),
                ir::range("columns", static_cast<std::int64_t>(startCol),
                    static_cast<std::int64_t>(endCol+1)))};
            // Generate new tiling annotation for the result vector
            execution_tree::tiling_information_2d tile_info(
                ann, name_, codename_);

            ++lhs_localities.annotation_.generation_;

            auto locality_ann = lhs_localities.locality_.as_annotation();
            result.set_annotation(
                execution_tree::localities_annotation(locality_ann,
                    tile_info.as_annotation(name_, codename_),
                    lhs_localities.annotation_, name_, codename_),
                name_, codename_);

        return result;
    }

    // This is where the computation of the inverse should be performed.
    execution_tree::primitive_argument_type dist_inverse::distGaussInv(
        execution_tree::primitive_argument_type&& lhs) const
    {
        using namespace execution_tree;

        execution_tree::localities_information lhs_localities =
            extract_localities_information(lhs, name_, codename_);

        switch (extract_numeric_value_dimension(lhs, name_, codename_))
        {
        case 2:
            // Do the inverse operation
            return distGaussInv(
                extract_numeric_value(std::move(lhs), name_, codename_),
                std::move(lhs_localities));
        default:
            HPX_THROW_EXCEPTION(hpx::bad_parameter,
                " dist_inverse::distGaussInv",
                generate_error_message("left hand side operand has unsupported "
                                       "number of dimensions"));
        }
    }

    // Call the evaluation function
    // Petty much boilerplate, just gotta change names and stuff
    // if the constructor was called without an operand, just pass back
    // the args to the function, otherwise, pass the operands
    hpx::future<execution_tree::primitive_argument_type> dist_inverse::eval(
        execution_tree::primitive_arguments_type const& operands,
        execution_tree::primitive_arguments_type const& args,
        execution_tree::eval_context ctx) const

    {
        using namespace execution_tree;

        // Check to make sure there is exactly one item to invert
        if (operands.size() != 1)
        {
            HPX_THROW_EXCEPTION(hpx::bad_parameter, "dist_inverse::eval",
                generate_error_message(
                    "the gaussian inverse operation primitive requires"
                    "exactly one operand"));
        }

        // Check if there are no valid operands
        if (!valid(operands[0]))
        {
            HPX_THROW_EXCEPTION(hpx::bad_parameter, "dist_inverse::eval",
                generate_error_message(
                    "the gaussian_inverse_operation primitive requires that "
                    "the arguments given by the operands array is valid"));
        }

        // Get a future to the result of the actual computation
        auto this_ = this->shared_from_this();
        return hpx::dataflow(hpx::launch::sync,
            hpx::util::unwrapping(
                [this_ = std::move(this_)](primitive_arguments_type&& args)
                    -> primitive_argument_type {
                    return this_->distGaussInv(std::move(args[0]));
                }),
            execution_tree::primitives::detail::map_operands(operands,
                execution_tree::functional::value_operand{}, args, name_,
                codename_, std::move(ctx)));
    }
}}}  
