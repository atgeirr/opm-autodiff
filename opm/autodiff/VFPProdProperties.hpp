/*
  Copyright 2015 SINTEF ICT, Applied Mathematics.

  This file is part of the Open Porous Media project (OPM).

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

#ifndef OPM_AUTODIFF_VFPPRODPROPERTIES_HPP_
#define OPM_AUTODIFF_VFPPRODPROPERTIES_HPP_

#include <opm/parser/eclipse/EclipseState/Schedule/VFPProdTable.hpp>
#include <opm/core/wells.h>
// TODO: this one not sure should be here.
#include <opm/core/props/BlackoilPhases.hpp>
#include <opm/material/densead/Math.hpp>
#include <opm/material/densead/Evaluation.hpp>
#include <opm/autodiff/VFPHelpers.hpp>

#include <vector>
#include <map>


namespace Opm {

template <class Scalar>
class AutoDiffBlock;

/**
 * Class which linearly interpolates BHP as a function of rate, tubing head pressure,
 * water fraction, gas fraction, and artificial lift for production VFP tables, and similarly
 * the BHP as a function of the rate and tubing head pressure.
 */
class VFPProdProperties {
public:
    typedef AutoDiffBlock<double> ADB;

    /**
     * Empty constructor
     */
    VFPProdProperties();

    /**
     * Constructor
     * Takes *no* ownership of data.
     * @param prod_table A *single* VFPPROD table
     */
    explicit VFPProdProperties(const VFPProdTable* prod_table);

    /**
     * Constructor
     * Takes *no* ownership of data.
     * @param prod_tables A map of different VFPPROD tables.
     */
    explicit VFPProdProperties(const std::map<int, std::shared_ptr<const VFPProdTable> >& prod_tables);

    /**
     * Linear interpolation of bhp as function of the input parameters.
     * @param table_id Table number to use
     * @param wells Wells structure with information about wells in qs
     * @param qs Flow quantities
     * @param thp Tubing head pressure
     * @param alq Artificial lift or other parameter
     *
     * @return The bottom hole pressure, interpolated/extrapolated linearly using
     * the above parameters from the values in the input table.
     */
    ADB bhp(const std::vector<int>& table_id,
            const Wells& wells,
            const ADB& qs,
            const ADB& thp,
            const ADB& alq) const;

    /**
     * Linear interpolation of bhp as a function of the input parameters given as ADBs
     * Each entry corresponds typically to one well.
     * @param table_id Table number to use. A negative entry (e.g., -1)
     *                 will indicate that no table is used, and the corresponding
     *                 BHP will be calculated as a constant -1e100.
     * @param aqua Water phase
     * @param liquid Oil phase
     * @param vapour Gas phase
     * @param thp Tubing head pressure
     * @param alq Artificial lift or other parameter
     *
     * @return The bottom hole pressure, interpolated/extrapolated linearly using
     * the above parameters from the values in the input table, for each entry in the
     * input ADB objects.
     */
    ADB bhp(const std::vector<int>& table_id,
            const ADB& aqua,
            const ADB& liquid,
            const ADB& vapour,
            const ADB& thp,
            const ADB& alq) const;


    /**
     * Linear interpolation of bhp as a function of the input parameters given as
     * Evalutions
     * Each entry corresponds typically to one well.
     * @param table_id Table number to use. A negative entry (e.g., -1)
     *                 will indicate that no table is used, and the corresponding
     *                 BHP will be calculated as a constant -1e100.
     * @param aqua Water phase
     * @param liquid Oil phase
     * @param vapour Gas phase
     * @param thp Tubing head pressure
     * @param alq Artificial lift or other parameter
     *
     * @return The bottom hole pressure, interpolated/extrapolated linearly using
     * the above parameters from the values in the input table, for each entry in the
     * input ADB objects.
     */
    template <class EvalWell>
    EvalWell bhp(const int table_id,
                 const EvalWell& aqua,
                 const EvalWell& liquid,
                 const EvalWell& vapour,
                 const double& thp,
                 const double& alq) const {

        //Get the table
        const VFPProdTable* table = detail::getTable(m_tables, table_id);
        EvalWell bhp = 0.0;

        //Find interpolation variables
        EvalWell flo = detail::getFlo(aqua, liquid, vapour, table->getFloType());
        EvalWell wfr = detail::getWFR(aqua, liquid, vapour, table->getWFRType());
        EvalWell gfr = detail::getGFR(aqua, liquid, vapour, table->getGFRType());

        //Compute the BHP for each well independently
        if (table != nullptr) {
            //First, find the values to interpolate between
            //Value of FLO is negative in OPM for producers, but positive in VFP table
            auto flo_i = detail::findInterpData(-flo.value(), table->getFloAxis());
            auto thp_i = detail::findInterpData( thp, table->getTHPAxis()); // assume constant
            auto wfr_i = detail::findInterpData( wfr.value(), table->getWFRAxis());
            auto gfr_i = detail::findInterpData( gfr.value(), table->getGFRAxis());
            auto alq_i = detail::findInterpData( alq, table->getALQAxis()); //assume constant

            detail::VFPEvaluation bhp_val = detail::interpolate(table->getTable(), flo_i, thp_i, wfr_i, gfr_i, alq_i);

            bhp = (bhp_val.dwfr * wfr) + (bhp_val.dgfr * gfr) - (bhp_val.dflo * flo);
            bhp.setValue(bhp_val.value);
        }
        else {
            bhp.setValue(-1e100); //Signal that this value has not been calculated properly, due to "missing" table
        }
        return bhp;
    }


    // TODO: different name to avoid name conflicts
    double bhpfra(const int table_id,
                 const double& flo,
                 const double& wfr,
                 const double& gfr,
                 const double& thp,
                 const double& alq) const {

        //Get the table
        const VFPProdTable* table = detail::getTable(m_tables, table_id);

        //Compute the BHP for each well independently
        //First, find the values to interpolate between
        //Value of FLO is negative in OPM for producers, but positive in VFP table
        auto flo_i = detail::findInterpData(-flo, table->getFloAxis());
        auto thp_i = detail::findInterpData( thp, table->getTHPAxis()); // assume constant
        auto wfr_i = detail::findInterpData( wfr, table->getWFRAxis());
        auto gfr_i = detail::findInterpData( gfr, table->getGFRAxis());
        auto alq_i = detail::findInterpData( alq, table->getALQAxis()); //assume constant

        detail::VFPEvaluation bhp_val = detail::interpolate(table->getTable(), flo_i, thp_i, wfr_i, gfr_i, alq_i);

        return bhp_val.value;
    }

    // TODO: if not template, we should move to a cpp file
    void calculateRatesBhpWithTHPTarget(const std::vector<double>& rates1,
                                        const std::vector<double>& rates2,
                                        const double bhp1, // this one will be used as fixed fractions, in the middle
                                        const double bhp2,
                                        const double bhp_end, // higher end for producers, lower end for injectors
                                        const double dp,
                                        const int table_id,
                                        const double thp,
                                        const double alq,
                                        std::vector<double>& return_rates,
                                        double& return_bhp) const
    {
        assert(table_id > 0);

        const VFPProdTable* table = detail::getTable(m_tables, table_id);

        const int Water = BlackoilPhases::Aqua;
        const int Oil = BlackoilPhases::Liquid;
        const int Gas = BlackoilPhases::Vapour;

        // FLO is the rate
        // implement based on producer first
        const double aqua1 = rates1[Water];
        const double liquid1 = rates1[Oil];
        const double vapour1 = rates1[Gas];
        const double flo_rate1 = detail::getFlo(aqua1, liquid1, vapour1, table->getFloType());

        const double aqua2 = rates2[Water];
        const double liquid2 = rates2[Oil];
        const double vapour2 = rates2[Gas];
        const double flo_rate2 = detail::getFlo(aqua2, liquid2, vapour2, table->getFloType());

        std::cout << "flo_rate1 is " << flo_rate1 << std::endl;
        std::cout << " bhp1 is " << bhp1 << std::endl;

        std::cout << "flo_rate2 is " << flo_rate2 << std::endl;
        std::cout << " bhp2 is " << bhp2 << std::endl;


        const double wfr = detail::getWFR(aqua1, liquid1, vapour1, table->getWFRType());
        const double gfr = detail::getGFR(aqua1, liquid1, vapour1, table->getGFRType());

        const int sample_number = 100;
        std::vector<double> bhp_samples(sample_number);
        std::vector<double> rate_samples(sample_number, 0.);

        // rate sampling interval
        const double rate_iterval = flo_rate2 / (sample_number - 1);
        for (int i = 0; i < sample_number; ++i) {
            rate_samples[i] =  i * rate_iterval;
        }

        // based on all the rate samples, let us calculate the bhp_samples
        for (int i = 0; i < sample_number; ++i) {
            bhp_samples[i] = bhpfra(table_id, rate_samples[i], wfr, gfr, thp, alq) - dp;
        }

        std::cout << " the rate and bhp samples " << std::endl;
        for (int i = 0; i < sample_number; ++i) {
            std::cout << rate_samples[i] << " " << bhp_samples[i] << std::endl;
        }

        // interface needs to be resigned
        const bool found = detail::findIntersectionForBhp(rate_samples, bhp_samples, flo_rate1, flo_rate2, bhp1, bhp2, return_bhp);

        if (!found) {
            std::cout << " COULD NOT find an Intersection point, the well might need to be closed " << std::endl;
            std::cin.ignore();
        }
    }


    /**
     * Linear interpolation of bhp as a function of the input parameters
     * @param table_id Table number to use
     * @param aqua Water phase
     * @param liquid Oil phase
     * @param vapour Gas phase
     * @param thp Tubing head pressure
     * @param alq Artificial lift or other parameter
     *
     * @return The bottom hole pressure, interpolated/extrapolated linearly using
     * the above parameters from the values in the input table.
     */
    double bhp(int table_id,
            const double& aqua,
            const double& liquid,
            const double& vapour,
            const double& thp,
            const double& alq) const;

    /**
     * Linear interpolation of thp as a function of the input parameters
     * @param table_id Table number to use
     * @param aqua Water phase
     * @param liquid Oil phase
     * @param vapour Gas phase
     * @param bhp Bottom hole pressure
     * @param alq Artificial lift or other parameter
     *
     * @return The tubing hole pressure, interpolated/extrapolated linearly using
     * the above parameters from the values in the input table.
     */
    double thp(int table_id,
            const double& aqua,
            const double& liquid,
            const double& vapour,
            const double& bhp,
            const double& alq) const;

    /**
     * Returns the table associated with the ID, or throws an exception if
     * the table does not exist
     */
    const VFPProdTable* getTable(const int table_id) const;

    /**
     * Returns true if no vfp tables are in the current map
     */
    bool empty() const {
        return m_tables.empty();
    }

private:
    // Map which connects the table number with the table itself
    std::map<int, const VFPProdTable*> m_tables;
};




} //namespace


#endif /* OPM_AUTODIFF_VFPPRODPROPERTIES_HPP_ */
