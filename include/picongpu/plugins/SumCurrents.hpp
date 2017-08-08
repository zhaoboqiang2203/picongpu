/* Copyright 2013-2017 Axel Huebl, Felix Schmitt, Heiko Burau, Rene Widera,
 *                     Felix Schmitt, Benjamin Worpitz, Richard Pausch
 *
 * This file is part of PIConGPU.
 *
 * PIConGPU is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PIConGPU is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PIConGPU.
 * If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "picongpu/simulation_defines.hpp"

#include "picongpu/fields/FieldJ.hpp"

#include <pmacc/dimensions/DataSpaceOperations.hpp>
#include "picongpu/plugins/ILightweightPlugin.hpp"
#include <pmacc/memory/shared/Allocate.hpp>
#include <pmacc/dataManagement/DataConnector.hpp>
#include <pmacc/nvidia/atomic.hpp>

#include <iostream>


namespace picongpu
{
using namespace pmacc;

namespace po = boost::program_options;

typedef FieldJ::DataBoxType J_DataBox;

struct KernelSumCurrents
{
    template<class Mapping>
    DINLINE void operator()(J_DataBox fieldJ, float3_X* gCurrent, Mapping mapper) const
    {
        typedef typename Mapping::SuperCellSize SuperCellSize;

        PMACC_SMEM( sh_sumJ, float3_X );

        const DataSpace<simDim > threadIndex(threadIdx);
        const int linearThreadIdx = DataSpaceOperations<simDim>::template map<SuperCellSize > (threadIndex);

        if (linearThreadIdx == 0)
        {
            sh_sumJ = float3_X::create(0.0);
        }

        __syncthreads();


        const DataSpace<simDim> superCellIdx(mapper.getSuperCellIndex(DataSpace<simDim > (blockIdx)));
        const DataSpace<simDim> cell(superCellIdx * SuperCellSize::toRT() + threadIndex);

        const float3_X myJ = fieldJ(cell);

        nvidia::atomicAdd(&(sh_sumJ.x()), myJ.x());
        nvidia::atomicAdd(&(sh_sumJ.y()), myJ.y());
        nvidia::atomicAdd(&(sh_sumJ.z()), myJ.z());

        __syncthreads();

        if (linearThreadIdx == 0)
        {
            nvidia::atomicAdd(&(gCurrent->x()), sh_sumJ.x());
            nvidia::atomicAdd(&(gCurrent->y()), sh_sumJ.y());
            nvidia::atomicAdd(&(gCurrent->z()), sh_sumJ.z());
        }
    }
};

class SumCurrents : public ILightweightPlugin
{
private:
    MappingDesc *cellDescription;
    uint32_t notifyPeriod;

    GridBuffer<float3_X, DIM1> *sumcurrents;

public:

    SumCurrents() :
    cellDescription(nullptr),
    notifyPeriod(0)
    {

        Environment<>::get().PluginConnector().registerPlugin(this);
    }

    virtual ~SumCurrents()
    {

    }

    void notify(uint32_t currentStep)
    {
        const int rank = Environment<simDim>::get().GridController().getGlobalRank();
        const float3_X gCurrent = getSumCurrents();

        // gCurrent is just j
        // j = I/A
#if(SIMDIM==DIM3)
        const float3_X realCurrent(
                                   gCurrent.x() * CELL_HEIGHT * CELL_DEPTH,
                                   gCurrent.y() * CELL_WIDTH * CELL_DEPTH,
                                   gCurrent.z() * CELL_WIDTH * CELL_HEIGHT);
#elif(SIMDIM==DIM2)
        const float3_X realCurrent(
                                   gCurrent.x() * CELL_HEIGHT,
                                   gCurrent.y() * CELL_WIDTH,
                                   gCurrent.z() * CELL_WIDTH * CELL_HEIGHT);
#endif
        float3_64 realCurrent_SI(
                                 float_64(realCurrent.x()) * (UNIT_CHARGE / UNIT_TIME),
                                 float_64(realCurrent.y()) * (UNIT_CHARGE / UNIT_TIME),
                                 float_64(realCurrent.z()) * (UNIT_CHARGE / UNIT_TIME));

        /*FORMAT OUTPUT*/
        typedef std::numeric_limits< float_64 > dbl;

        std::cout.precision(dbl::digits10);
        if (math::abs(gCurrent.x()) + math::abs(gCurrent.y()) + math::abs(gCurrent.z()) != float_X(0.0))
            std::cout << "[ANALYSIS] [" << rank << "] [COUNTER] [SumCurrents] [" << currentStep
            << std::scientific << "] " <<
            realCurrent_SI << " Abs:" << math::abs(realCurrent_SI) << std::endl;
    }

    void pluginRegisterHelp(po::options_description& desc)
    {
        desc.add_options()
            ("sumcurr.period", po::value<uint32_t > (&notifyPeriod), "enable plugin [for each n-th step]");
    }

    std::string pluginGetName() const
    {
        return "SumCurrents";
    }

    void setMappingDescription(MappingDesc *cellDescription)
    {
        this->cellDescription = cellDescription;
    }

private:

    void pluginLoad()
    {
        if (notifyPeriod > 0)
        {
            sumcurrents = new GridBuffer<float3_X, DIM1 > (DataSpace<DIM1 > (1)); //create one int on gpu und host

            Environment<>::get().PluginConnector().setNotificationPeriod(this, notifyPeriod);
        }
    }

    void pluginUnload()
    {
        if (notifyPeriod > 0)
        {
            __delete(sumcurrents);
        }
    }

    float3_X getSumCurrents()
    {
        DataConnector &dc = Environment<>::get().DataConnector();
        auto fieldJ = dc.get< FieldJ >( FieldJ::getName(), true );

        sumcurrents->getDeviceBuffer().setValue(float3_X::create(0.0));
        auto block = MappingDesc::SuperCellSize::toRT();

        AreaMapping<CORE + BORDER, MappingDesc> mapper(*cellDescription);
        PMACC_KERNEL(KernelSumCurrents{})
            (mapper.getGridDim(), block)
            (fieldJ->getDeviceDataBox(),
             sumcurrents->getDeviceBuffer().getBasePointer(),
             mapper);

        dc.releaseData( FieldJ::getName() );

        sumcurrents->deviceToHost();
        return sumcurrents->getHostBuffer().getDataBox()[0];
    }

};

}

