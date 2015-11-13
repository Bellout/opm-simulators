/*
  Copyright 2014, 2015 SINTEF ICT, Applied Mathematics.
  Copyright 2014, 2015 Statoil ASA.

  This file is part of the Open Porous Media Project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OPM_RATECONVERTER_HPP_HEADER_INCLUDED
#define OPM_RATECONVERTER_HPP_HEADER_INCLUDED

#include <opm/autodiff/BlackoilPropsAdInterface.hpp>

#include <opm/core/props/BlackoilPhases.hpp>
#include <opm/core/simulator/BlackoilState.hpp>
#include <opm/core/utility/RegionMapping.hpp>
#include <opm/core/linalg/ParallelIstlInformation.hpp>

#include <Eigen/Core>

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
/**
 * \file
 * Facility for converting component rates at surface conditions to
 * phase (voidage) rates at reservoir conditions.
 *
 * This uses the average hydrocarbon pressure to define fluid
 * properties.  The facility is intended to support Reservoir Voidage
 * rates only ('RESV').
 */

namespace Opm {
    namespace RateConverter {
        /**
         * Convenience tools for implementing the rate conversion
         * facility.
         */
        namespace Details {
            namespace Select {
                template <class RegionID, bool>
                struct RegionIDParameter
                {
                    using type =
                        typename std::remove_reference<RegionID>::type &;
                };

                template <class RegionID>
                struct RegionIDParameter<RegionID, true>
                {
                    using type = RegionID;
                };
            } // Select

            /**
             * \brief Computes the temperature, pressure, and counter increment.
             *
             * In a parallel run only cells owned contribute to the cell average.
             * \tparam is_parallel Whether this is a parallel run.
             */
             template<bool is_parallel>
            struct AverageIncrementCalculator
            {
                /**
                 * \brief Computes the temperature, pressure, and counter increment.
                 * \param pressure    The pressure.
                 * \param temperature The temperature.
                 * \param cell        The current cell index.
                 * \param ownership   A vector indicating whether a cell is owned
                 *                    by this process (value 1), or not (value 0).
                 * \param cell        The cell index.
                 */
                std::tuple<double, double, int>
                operator()(const std::vector<double>& pressure,
                           const std::vector<double>& temperature,
                           const std::vector<double>& ownership,
                           std::size_t cell){
                    if ( ownership[cell] )
                    {
                        return std::make_tuple(pressure[cell],
                                               temperature[cell], 1);
                    }
                    else
                    {
                        return std::make_tuple(0, 0, 0);
                    }
                }
            };
            template<>
            struct AverageIncrementCalculator<false>
            {
                std::tuple<double, double, int>
                operator()(const std::vector<double>& pressure,
                           const std::vector<double>& temperature,
                           const std::vector<double>&,
                           std::size_t cell){
                    return std::make_tuple(pressure[cell],
                                           temperature[cell], 1);
                }
            };
            /**
             * Provide mapping from Region IDs to user-specified collection
             * of per-region attributes.
             *
             * \tparam RegionId Region identifier type.  Must be hashable by
             * \code std::hash<> \endcode.  Typically a built-in integer
             * type--e.g., \c int.
             *
             * \tparam Attributes User-defined type that represents
             * collection of attributes that have meaning in a per-region
             * aggregate sense.  Must be copy-constructible.
             */
            template <typename RegionId, class Attributes>
            class RegionAttributes
            {
            public:
                /**
                 * Expose \c RegionId as a vocabulary type for use in query
                 * methods.
                 */
                using RegionID =
                    typename Select::RegionIDParameter
                    <RegionId, std::is_integral<RegionId>::value>::type;

                /**
                 * Constructor.
                 *
                 * \tparam RMap Class type that implements the RegionMapping
                 * protocol.  Typically an instantiation of \code
                 * Opm::RegionMapping<> \endcode.
                 *
                 * \param[in] rmap Specific region mapping that provides
                 * reverse lookup from regions to cells.
                 *
                 * \param[in] attr Pre-constructed initialiser for \c
                 * Attributes.
                 */
                template <class RMap>
                RegionAttributes(const RMap&       rmap,
                                 const Attributes& attr)
                {
                    using VT = typename AttributeMap::value_type;

                    for (const auto& r : rmap.activeRegions()) {
                        auto v = std::unique_ptr<Value>(new Value(attr));

                        const auto stat = attr_.insert(VT(r, std::move(v)));

                        if (stat.second) {
                            // New value inserted.
                            const auto& cells = rmap.cells(r);

                            assert (! cells.empty());

                            // Region's representative cell.
                            stat.first->second->cell_ = cells[0];
                        }
                    }
                }

                /**
                 * Retrieve representative cell in region.
                 *
                 * \param[in] reg Specific region.
                 *
                 * \return Representative cell in region \p reg.
                 */
                int cell(const RegionID reg) const
                {
                    return this->find(reg).cell_;
                }

                /**
                 * Request read-only access to region's attributes.
                 *
                 * \param[in] reg Specific region.
                 *
                 * \return Read-only access to region \p reg's per-region
                 * attributes.
                 */
                const Attributes& attributes(const RegionID reg) const
                {
                    return this->find(reg).attr_;
                }

                /**
                 * Request modifiable access to region's attributes.
                 *
                 * \param[in] reg Specific region.
                 *
                 * \return Read-write access to region \p reg's per-region
                 * attributes.
                 */
                Attributes& attributes(const RegionID reg)
                {
                    return this->find(reg).attr_;
                }

            private:
                /**
                 * Aggregate per-region attributes along with region's
                 * representative cell.
                 */
                struct Value {
                    Value(const Attributes& attr)
                        : attr_(attr)
                        , cell_(-1)
                    {}

                    Attributes attr_;
                    int        cell_;
                };

                using ID =
                    typename std::remove_reference<RegionId>::type;

                using AttributeMap =
                    std::unordered_map<ID, std::unique_ptr<Value>>;

                AttributeMap attr_;

                /**
                 * Read-only access to region's properties.
                 */
                const Value& find(const RegionID reg) const
                {
                    const auto& i = attr_.find(reg);

                    if (i == attr_.end()) {
                        throw std::invalid_argument("Unknown region ID");
                    }

                    return *i->second;
                }

                /**
                 * Read-write access to region's properties.
                 */
                Value& find(const RegionID reg)
                {
                    const auto& i = attr_.find(reg);

                    if (i == attr_.end()) {
                        throw std::invalid_argument("Unknown region ID");
                    }

                    return *i->second;
                }
            };

            /**
             * Convenience functions for querying presence/absence of
             * active phases.
             */
            namespace PhaseUsed {
                /**
                 * Active water predicate.
                 *
                 * \param[in] pu Active phase object.
                 *
                 * \return Whether or not water is an active phase.
                 */
                inline bool
                water(const PhaseUsage& pu)
                {
                    return pu.phase_used[ BlackoilPhases::Aqua ] != 0;
                }

                /**
                 * Active oil predicate.
                 *
                 * \param[in] pu Active phase object.
                 *
                 * \return Whether or not oil is an active phase.
                 */
                inline bool
                oil(const PhaseUsage& pu)
                {
                    return pu.phase_used[ BlackoilPhases::Liquid ] != 0;
                }

                /**
                 * Active gas predicate.
                 *
                 * \param[in] pu Active phase object.
                 *
                 * \return Whether or not gas is an active phase.
                 */
                inline bool
                gas(const PhaseUsage& pu)
                {
                    return pu.phase_used[ BlackoilPhases::Vapour ] != 0;
                }
            } // namespace PhaseUsed

            /**
             * Convenience functions for querying numerical IDs
             * ("positions") of active phases.
             */
            namespace PhasePos {
                /**
                 * Numerical ID of active water phase.
                 *
                 * \param[in] pu Active phase object.
                 *
                 * \return Non-negative index/position of water if
                 * active, -1 if not.
                 */
                inline int
                water(const PhaseUsage& pu)
                {
                    int p = -1;

                    if (PhaseUsed::water(pu)) {
                        p = pu.phase_pos[ BlackoilPhases::Aqua ];
                    }

                    return p;
                }

                /**
                 * Numerical ID of active oil phase.
                 *
                 * \param[in] pu Active phase object.
                 *
                 * \return Non-negative index/position of oil if
                 * active, -1 if not.
                 */
                inline int
                oil(const PhaseUsage& pu)
                {
                    int p = -1;

                    if (PhaseUsed::oil(pu)) {
                        p = pu.phase_pos[ BlackoilPhases::Liquid ];
                    }

                    return p;
                }

                /**
                 * Numerical ID of active gas phase.
                 *
                 * \param[in] pu Active phase object.
                 *
                 * \return Non-negative index/position of gas if
                 * active, -1 if not.
                 */
                inline int
                gas(const PhaseUsage& pu)
                {
                    int p = -1;

                    if (PhaseUsed::gas(pu)) {
                        p = pu.phase_pos[ BlackoilPhases::Vapour ];
                    }

                    return p;
                }
            } // namespace PhasePos
        } // namespace Details

        /**
         * Convert component rates at surface conditions to phase
         * (voidage) rates at reservoir conditions.
         *
         * The conversion uses fluid properties evaluated at average
         * hydrocarbon pressure in regions or field.
         *
         * \tparam Property Fluid property object.  Expected to
         * feature the formation volume factor functions of the
         * BlackoilPropsAdInterface.
         *
         * \tparam Region Type of a forward region mapping.  Expected
         * to provide indexed access through \code operator[]()
         * \endcode as well as inner types \c value_type, \c
         * size_type, and \c const_iterator.  Typically \code
         * std::vector<int> \endcode.
         */
        template <class Property, class Region>
        class SurfaceToReservoirVoidage {
        public:
            /**
             * Constructor.
             *
             * \param[in] props Fluid property object.
             *
             * \param[in] region Forward region mapping.  Often
             * corresponds to the "FIPNUM" mapping of an ECLIPSE input
             * deck.
             */
            SurfaceToReservoirVoidage(const Property& props,
                                      const Region&   region)
                : props_(props)
                , rmap_ (region)
                , attr_ (rmap_, Attributes(props_.numPhases()))
            {}

            /**
             * Compute average hydrocarbon pressure and maximum
             * dissolution and evaporation at average hydrocarbon
             * pressure in all regions in field.
             *
             * Fluid properties are evaluated at average hydrocarbon
             * pressure for purpose of conversion from surface rate to
             * reservoir voidage rate.
             *
             * \param[in] state Dynamic reservoir state.
             * \param[in] any  The information and communication utilities
             *                 about/of the parallelization. in any parallel
             *                 it wraps a ParallelISTLInformation. Parameter
             *                 is optional.
             */
            void
            defineState(const BlackoilState& state,
                        const boost::any& info = boost::any())
            {
#if HAVE_MPI
                if( info.type() == typeid(ParallelISTLInformation) )
                {
                    const auto& ownership =
                        boost::any_cast<const ParallelISTLInformation&>(info)
                        .updateOwnerMask(state.pressure());
                    calcAverages<true>(state, info, ownership);
                }
                else
#endif
                {
                    std::vector<double> dummyOwnership; // not actually used
                    calcAverages<false>(state, info, dummyOwnership);
                }   
                calcRmax();
            }
            /**
             * Region identifier.
             *
             * Integral type.
             */
            typedef typename RegionMapping<Region>::RegionId RegionId;

            /**
             * Compute coefficients for surface-to-reservoir voidage
             * conversion.
             *
             * \tparam Input Type representing contiguous collection
             * of component rates at surface conditions.  Must support
             * direct indexing through \code operator[]()\endcode.
             *
             * \tparam Coeff Type representing contiguous collection
             * of surface-to-reservoir conversion coefficients.  Must
             * support direct indexing through \code operator[]()
             * \endcode.
             *
             * \param[in] in Single tuple of active component rates at
             * surface conditions.
             *
             * \param[in] r Fluid-in-place region to which the
             * component rates correspond.
             *
             * \param[out] coeff Surface-to-reservoir conversion
             * coefficients for all active phases, corresponding to
             * input rates \c in in region \c r.
             */
            template <class Input,
                      class Coeff>
            void
            calcCoeff(const Input& in, const RegionId r, Coeff& coeff) const
            {
                using V = typename Property::V;

                const auto& pu = props_.phaseUsage();
                const auto& ra = attr_.attributes(r);

                const auto  p  = this->constant(ra.pressure);
                const auto  T  = this->constant(ra.temperature);
                const auto  c  = this->getRegCell(r);

                const int   iw = Details::PhasePos::water(pu);
                const int   io = Details::PhasePos::oil  (pu);
                const int   ig = Details::PhasePos::gas  (pu);

                std::fill(& coeff[0], & coeff[0] + props_.numPhases(), 0.0);

                if (Details::PhaseUsed::water(pu)) {
                    // q[w]_r = q[w]_s / bw

                    const V bw = props_.bWat(p, T, c).value();

                    coeff[iw] = 1.0 / bw(0);
                }

                const Miscibility& m = calcMiscibility(in, r);

                // Determinant of 'R' matrix
                const double detR = 1.0 - (m.rs(0) * m.rv(0));

                if (Details::PhaseUsed::oil(pu)) {
                    // q[o]_r = 1/(bo * (1 - rs*rv)) * (q[o]_s - rv*q[g]_s)

                    const auto   rs  = this->constant(m.rs);
                    const V      bo  = props_.bOil(p, T, rs, m.cond, c).value();
                    const double den = bo(0) * detR;

                    coeff[io] += 1.0 / den;

                    if (Details::PhaseUsed::gas(pu)) {
                        coeff[ig] -= m.rv(0) / den;
                    }
                }

                if (Details::PhaseUsed::gas(pu)) {
                    // q[g]_r = 1/(bg * (1 - rs*rv)) * (q[g]_s - rs*q[o]_s)

                    const auto   rv  = this->constant(m.rv);
                    const V      bg  = props_.bGas(p, T, rv, m.cond, c).value();
                    const double den = bg(0) * detR;

                    coeff[ig] += 1.0 / den;

                    if (Details::PhaseUsed::oil(pu)) {
                        coeff[io] -= m.rs(0) / den;
                    }
                }
            }

        private:
            /**
             * Fluid property object.
             */
            const Property&  props_;

            /**
             * "Fluid-in-place" region mapping (forward and reverse).
             */
            const RegionMapping<Region> rmap_;

            /**
             * Derived property attributes for each active region.
             */
            struct Attributes {
                Attributes(const int np)
                    : pressure   (0.0)
                    , temperature(0.0)
                    , Rmax       (Eigen::ArrayXd::Zero(np, 1))
                {}

                double         pressure;
                double         temperature;
                Eigen::ArrayXd Rmax;
            };

            Details::RegionAttributes<RegionId, Attributes> attr_;

            /**
             * Aggregate structure defining fluid miscibility
             * conditions in single region with particular input
             * surface rates.
             */
            struct Miscibility {
                Miscibility()
                    : rs  (1)
                    , rv  (1)
                    , cond(1)
                {
                    rs << 0.0;
                    rv << 0.0;
                }

                /**
                 * Dissolved gas-oil ratio at particular component oil
                 * and gas rates at surface conditions.
                 *
                 * Limited by "RSmax" at average hydrocarbon pressure
                 * in region.
                 */
                typename Property::V rs;

                /**
                 * Evaporated oil-gas ratio at particular component oil
                 * and gas rates at surface conditions.
                 *
                 * Limited by "RVmax" at average hydrocarbon pressure
                 * in region.
                 */
                typename Property::V rv;

                /**
                 * Fluid condition in representative region cell.
                 *
                 * Needed for purpose of FVF evaluation.
                 */
                std::vector<PhasePresence> cond;
            };

            /**
             * Compute average hydrocarbon pressure and temperatures in all
             * regions.
             *
             * \param[in] state       Dynamic reservoir state.
             * \param[in] info        The information and communication utilities
             *                        about/of the parallelization.
             * \param[in] ownership   In a parallel run this is vector containing
             *                        1 for every owned unknown, zero othercase.
             *                        If it is no such run then it is not used.
             * \tparam    is_parallel True if the runi is parallel. In this case
             *                        info has to contain a ParallelISTLInformation
             *                        object.
             */
            template<bool is_parallel>
            void
            calcAverages(const BlackoilState& state, const boost::any& info,
                         const std::vector<double>& ownerShip)
            {
                const auto& press = state.pressure();
                const auto& temp  = state.temperature();

                for (const auto& reg : rmap_.activeRegions()) {
                    auto& ra = attr_.attributes(reg);
                    auto& p  = ra.pressure;
                    auto& T  = ra.temperature;

                    std::size_t n = 0;
                    p = T = 0.0;
                    for (const auto& cell : rmap_.cells(reg)) {
                        auto increment = Details::
                            AverageIncrementCalculator<is_parallel>()(press, temp,
                                                                      ownerShip,
                                                                      cell);
                        p += std::get<0>(increment);
                        T += std::get<1>(increment);
                        n += std::get<2>(increment);
                    }
                    std::size_t global_n = n;
                    double global_p = p;
                    double global_T = T;
#if HAVE_MPI
                    if ( is_parallel )
                    {
                        const auto& real_info = boost::any_cast<const ParallelISTLInformation&>(info);
                        global_n = real_info.communicator().sum(n);
                        global_p = real_info.communicator().sum(p);
                        global_T = real_info.communicator().sum(T);
                    }
#endif
                    p = global_p / global_n;
                    T = global_T / global_n;
                }
            }
            /**
             * Compute maximum dissolution and evaporation ratios at
             * average hydrocarbon pressure.
             *
             * Uses the pressure value computed by averagePressure()
             * and must therefore be called *after* that method.
             */
            void
            calcRmax()
            {
                const PhaseUsage& pu = props_.phaseUsage();

                if (Details::PhaseUsed::oil(pu) &&
                    Details::PhaseUsed::gas(pu))
                {
                    const Eigen::ArrayXd::Index
                        io = Details::PhasePos::oil(pu),
                        ig = Details::PhasePos::gas(pu);

                    // Note: Intentionally does not take capillary
                    // pressure into account.  This facility uses the
                    // average *hydrocarbon* pressure rather than
                    // average phase pressure.

                    for (const auto& reg : rmap_.activeRegions()) {
                        auto& ra = attr_.attributes(reg);

                        const auto c = this->getRegCell(reg);
                        const auto p = this->constant(ra.pressure);
                        const auto T = this->constant(ra.temperature);

                        auto& Rmax = ra.Rmax;
                        Rmax.row(io) = props_.rsSat(p, T, c).value();
                        Rmax.row(ig) = props_.rvSat(p, T, c).value();
                    }
                }
            }

            /**
             * Compute fluid conditions in particular region for a
             * given set of component rates at surface conditions.
             *
             * \tparam Input Type representing collection of (active)
             * component rates at surface conditions.  Must support
             * direct indexing through \code operator[]()\endcode.
             *
             * \param[in] in Single tuple of active component rates at
             * surface conditions.
             *
             * \param[in] r Fluid-in-place region to which the
             * component rates correspond.
             *
             * \return Fluid conditions in region \c r corresponding
             * to surface component rates \c in.
             */
            template <class Input>
            Miscibility
            calcMiscibility(const Input& in, const RegionId r) const
            {
                const auto& pu   = props_.phaseUsage();
                const auto& attr = attr_.attributes(r);

                const int io = Details::PhasePos::oil(pu);
                const int ig = Details::PhasePos::gas(pu);

                Miscibility m;
                PhasePresence& cond = m.cond[0];

                if (Details::PhaseUsed::water(pu)) {
                    cond.setFreeWater();
                }

                if (Details::PhaseUsed::oil(pu)) {
                    cond.setFreeOil();

                    if (Details::PhaseUsed::gas(pu)) {
                        const double rsmax = attr.Rmax(io);
                        const double rs =
                            (0.0 < std::abs(in[io]))
                            ? in[ig] / in[io]
                            : (0.0 < std::abs(in[ig])) ? rsmax : 0.0;

                        if (rsmax < rs) {
                            cond.setFreeGas();
                        }

                        m.rs(0) = std::min(rs, rsmax);
                    }
                }

                if (Details::PhaseUsed::gas(pu)) {
                    if (! Details::PhaseUsed::oil(pu)) {
                        // Oil *NOT* active -- not really supported.
                        cond.setFreeGas();
                    }

                    if (Details::PhaseUsed::oil(pu)) {
                        const double rvmax = attr.Rmax(ig);
                        const double rv =
                            (0.0 < std::abs(in[ig]))
                            ? (in[io] / in[ig])
                            : (0.0 < std::abs(in[io])) ? rvmax : 0.0;

                        m.rv(0) = std::min(rv, rvmax);
                    }
                }

                return m;
            }

            /**
             * Conversion from \code Property::V \endcode to (constant)
             * \code Property::ADB \endcode (zero derivatives).
             */
            typename Property::ADB
            constant(const typename Property::V& x) const
            {
                return Property::ADB::constant(x);
            }

            /**
             * Conversion from \c double to (constant) \code Property::ADB
             * \endcode (zero derivatives).
             */
            typename Property::ADB
            constant(const double x) const
            {
                typename Property::V y(1);
                y << x;

                return this->constant(y);
            }

            /**
             * Retrieve representative cell of region
             *
             * \param[in] r Particular region.
             *
             * \return Representative cell of region \c r.
             */
            typename Property::Cells
            getRegCell(const RegionId r) const
            {
                typename Property::Cells c(1, this->attr_.cell(r));

                return c;
            }
        };
    } // namespace RateConverter
} // namespace Opm

#endif  /* OPM_RATECONVERTER_HPP_HEADER_INCLUDED */
