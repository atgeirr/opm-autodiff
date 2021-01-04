/*
  Copyright 2019 SINTEF Digital, Mathematics and Cybernetics.
  Copyright 2019 Dr. Blatt - HPC-Simulation-Software & Services.

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

#pragma once

#include <opm/simulators/linalg/twolevelmethodcpr.hh>


namespace Opm
{
    template <class Communication>
    void extendCommunicatorWithWells(const Communication& comm,
                                     std::shared_ptr<Communication>& commRW,
                                     const int nw)
    //                                    std::vector<int>& welldofs)
    {
        // used for extending the coarse communicator pattern
        using IndexSet = typename Communication::ParallelIndexSet;
        using LocalIndex = typename IndexSet::LocalIndex;
        const IndexSet& indset = comm.indexSet(); 
        IndexSet& indset_rw = commRW->indexSet(); 
        int max_nw = comm.communicator().max(nw)+1;
        int num_proc = comm.communicator().size()+1;
        int loc_max = 0;
        int count = 0;
        indset_rw.beginResize();
        for (auto ind=indset.begin(), indend=indset.end(); ind!= indend; ++ind) {
            // int loc_ind = ind->local();
            indset_rw.add(ind->global(),
                          LocalIndex(ind->local(),ind->local().attribute(),true));
            loc_max = std::max(loc_max,ind->global());
            assert(count == ind->local());
            count +=1;
        }
        int global_max = comm.communicator().max(loc_max)+1;
        //used append the welldofs at the end
        for (int i=0; i < nw; ++i){
            // need to set unique global number
            int local_ind = count;
            int v = global_max + max_nw*num_proc + i;
            // set to true to not have problems with higher levels if growing of domains is used
            indset_rw.add(v, LocalIndex(local_ind, Dune::OwnerOverlapCopyAttributeSet::owner, true));
            //welldofs.push_back(local_ind);
            local_ind +=1;       
        }
        indset.endResize();
        // assume same communication pattern
        commRW->remoteIndices().setNeighbours(comm.remoteIndices().getNeighbours());
        commRW->remoteIndices().template rebuild<true>();
    }
    
    template <class FineOperator, class CoarseOperator, class Communication, bool transpose = false>
class PressureBhpTransferPolicy : public Dune::Amg::LevelTransferPolicyCpr<FineOperator, CoarseOperator>
{
public:
    typedef Dune::Amg::LevelTransferPolicyCpr<FineOperator, CoarseOperator> ParentType;
    typedef Communication ParallelInformation;
    typedef typename FineOperator::domain_type FineVectorType;

public:
    PressureBhpTransferPolicy(const Communication& comm, const FineVectorType& weights, const boost::property_tree::ptree& prm)
        : communication_(&const_cast<Communication&>(comm))
        , weights_(weights)
        , prm_(prm)
        , pressure_var_index_(prm_.get<int>("pressureindex"))
        , bhp_var_index_(prm_.get<int>("bhpindex"))
                                                    
    {
    }

    virtual void createCoarseLevelSystem(const FineOperator& fineOperator) override
    {
        using CoarseMatrix = typename CoarseOperator::matrix_type;
        const auto& fineLevelMatrix = fineOperator.getmat();
        const auto& nw = fineOperator.getNumberOfExtraEquations();
        if(prm_.get<int>("add_wells")){    
            coarseLevelMatrix_.reset(new CoarseMatrix(fineLevelMatrix.N()+nw, fineLevelMatrix.M()+nw, CoarseMatrix::implicit));
        }else{
            coarseLevelMatrix_.reset(new CoarseMatrix(fineLevelMatrix.N()+nw, fineLevelMatrix.M()+nw, CoarseMatrix::row_wise));
        }
        auto createIter = coarseLevelMatrix_->createbegin();
        for (const auto& row : fineLevelMatrix) {
            for (auto col = row.begin(), cend = row.end(); col != cend; ++col) {
                createIter.insert(col.index());
            }
            ++createIter;
        }
#if DUNE_VERSION_NEWER(DUNE_ISTL, 2, 7)
        coarseLevelCommunication_ = std::make_shared<Communication>(communication_->communicator(),communication_->category(),false);//ParallelInformation pinfo;
#else    
        coarseLevelCommunication_ = std::make_shared<Communication>(communication_->communicator(),communication_->getSolverCategory(),false);//ParallelInformation pinfo;
#endif    
        if(prm_.get<bool>("add_wells")){            
            fineOperator.addWellPressureEquationStruct(coarseLevelMatrix_);
            coarseLevelMatrix_->compress();//all elemenst should be set
            extendCommunicatorWithWells(communication_,
                                        coarseLevelCommunication_,
                                        nw);
        }
        calculateCoarseEntries(fineOperator);
                
        this->lhs_.resize(this->coarseLevelMatrix_->M());
        this->rhs_.resize(this->coarseLevelMatrix_->N());
        using OperatorArgs = typename Dune::Amg::ConstructionTraits<CoarseOperator>::Arguments;
#if DUNE_VERSION_NEWER(DUNE_ISTL, 2, 7)
        OperatorArgs oargs(coarseLevelMatrix_, *coarseLevelCommunication_);
        this->operator_ = Dune::Amg::ConstructionTraits<CoarseOperator>::construct(oargs);
#else
        OperatorArgs oargs(*coarseLevelMatrix_, *coarseLevelCommunication_);
        this->operator_.reset(Dune::Amg::ConstructionTraits<CoarseOperator>::construct(oargs));
#endif
    }

    virtual void calculateCoarseEntries(const FineOperator& fineOperator) override
    {
        const auto& fineMatrix = fineOperator.getmat();
        *coarseLevelMatrix_ = 0;
        auto rowCoarse = coarseLevelMatrix_->begin();
        for (auto row = fineMatrix.begin(), rowEnd = fineMatrix.end(); row != rowEnd; ++row, ++rowCoarse) {
            assert(row.index() == rowCoarse.index());
            auto entryCoarse = rowCoarse->begin();
            for (auto entry = row->begin(), entryEnd = row->end(); entry != entryEnd; ++entry, ++entryCoarse) {
                assert(entry.index() == entryCoarse.index());
                double matrix_el = 0;
                if (transpose) {
                    const auto& bw = weights_[entry.index()];
                    for (size_t i = 0; i < bw.size(); ++i) {
                        matrix_el += (*entry)[pressure_var_index_][i] * bw[i];
                    }
                } else {
                    const auto& bw = weights_[row.index()];
                    for (size_t i = 0; i < bw.size(); ++i) {
                        matrix_el += (*entry)[i][pressure_var_index_] * bw[i];
                    }
                }
                (*entryCoarse) = matrix_el;
            }
        }
        if(prm_.get<bool>("add_wells")){
            assert(transpose==false);// not implemented
            fineOperator.addWellPressureEquations(coarseLevelMatrix_, weights_);
        }
        assert(rowCoarse == coarseLevelMatrix_->end());
    }

    virtual void moveToCoarseLevel(const typename ParentType::FineRangeType& fine) override
    {
        //NB we iterate over fine assumming welldofs is at the end
        // Set coarse vector to zero
        this->rhs_ = 0;

        auto end = fine.end(), begin = fine.begin();

        for (auto block = begin; block != end; ++block) {
            const auto& bw = weights_[block.index()];
            double rhs_el = 0.0;
            if (transpose) {
                rhs_el = (*block)[pressure_var_index_];
            } else {
                for (size_t i = 0; i < block->size(); ++i) {
                    rhs_el += (*block)[i] * bw[i];
                }
            }
            this->rhs_[block - begin] = rhs_el;
        }

        this->lhs_ = 0;
    }

    virtual void moveToFineLevel(typename ParentType::FineDomainType& fine) override
    {
        //NB we iterate over fine assumming welldofs is at the end
        auto end = fine.end(), begin = fine.begin();

        for (auto block = begin; block != end; ++block) {
            if (transpose) {
                const auto& bw = weights_[block.index()];
                for (size_t i = 0; i < block->size(); ++i) {
                    (*block)[i] = this->lhs_[block - begin] * bw[i];
                }
            } else {
                (*block)[pressure_var_index_] = this->lhs_[block - begin];
            }
        }
    }

    virtual PressureBhpTransferPolicy* clone() const override
    {
        return new PressureBhpTransferPolicy(*this);
    }

    const Communication& getCoarseLevelCommunication() const
    {
        return *coarseLevelCommunication_;
    }

private:
    Communication* communication_;
    const FineVectorType& weights_;
    boost::property_tree::ptree prm_;
    const int pressure_var_index_;
    const int bhp_var_index_;
    std::shared_ptr<Communication> coarseLevelCommunication_;
    std::shared_ptr<typename CoarseOperator::matrix_type> coarseLevelMatrix_;
    
};

} // namespace Opm

