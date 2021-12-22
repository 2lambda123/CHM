//
// Canadian Hydrological Model - The Canadian Hydrological Model (CHM) is a novel
// modular unstructured mesh based approach for hydrological modelling
// Copyright (C) 2018 Christopher Marsh
//
// This file is part of Canadian Hydrological Model.
//
// Canadian Hydrological Model is free software: you can redistribute it and/or
// modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Canadian Hydrological Model is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Canadian Hydrological Model.  If not, see
// <http://www.gnu.org/licenses/>.
//

#include "fsm.hpp"
REGISTER_MODULE_CPP(FSM);

FSM::FSM(config_file cfg)
    : module_base("FSM", parallel::data, cfg)
{
    depends("solar_el");
    depends("ilwr");
    depends("rh");
    depends("t");
    depends("p_snow");
    depends("p_rain");
    depends("U_2m_above_srf");

    depends("iswr_direct");
    depends("iswr_diffuse");

    optional("iswr_subcanopy");
    optional("rh_subcanopy");
    optional("ta_subcanopy");
    optional("ilwr_subcanopy");
    optional("iswr_subcanopy");
    optional("p_subcanopy");
    optional("drift_mass");

    // Optional avalanche variables
    optional("delta_avalanche_snowdepth");
    optional("delta_avalanche_mass");

    provides("swe");
    provides("snowdepthavg");
    provides("snowdepthavg_vert");
    provides("E");
    provides("H");
    provides("sum_snowpack_subl");
    provides("subl");
    provides("snow_albedo");

}

void FSM::init(mesh& domain)
{
    //Canopy, snow and soil layers
    __layers_MOD_fvg1 = 0.5; // Fraction of vegetation in upper canopy layer
    __layers_MOD_zsub = 1.5; // Subcanopy wind speed diagnostic height (m)

//    __layers_MOD_ncnpy = 2; // Number of canopy layers
    __layers_MOD_nsmax = 3; // Maximum number of snow layers
    __layers_MOD_nsoil = 4; // Number of soil layers

    __soilprops_MOD_b = 7.63; // Clapp-Hornberger exponent
    __soilprops_MOD_hcap_soil = 2.3e6; // Volumetric heat capacity of dry soil (J/K/m^3)
    __soilprops_MOD_hcon_soil = 0.11; // Thermal conductivity of dry soil (W/m/K)
    __soilprops_MOD_sathh = 0.41; // Saturated soil water pressure (m)
    __soilprops_MOD_vcrit = 0.26; // Volumetric soil moisture at critical point
    __soilprops_MOD_vsat = 0.27; // Volumetric soil moisture at saturation

    allocate();

//    __layers_MOD_Dzsnow[0] = 0.1;
//    __layers_MOD_Dzsnow[1] = 0.2;
//    __layers_MOD_Dzsnow[2] = 0.4;


    #pragma omp parallel for
    for (size_t i = 0; i < domain->size_faces(); i++)
    {
        auto face = domain->face(i);
        auto& d = face->make_module_data<data>(ID);

        d.veg.alb0 = 0.2;
        d.veg.vegh = 0;
        d.veg.VAI = 0;

        d.diag.sum_snowpack_subl = 0;
    }
}
void FSM::run(mesh_elem& face)
{
    if(is_water(face))
    {
        set_all_nan_on_skip(face);
        return;
    }


    // met data
    float zT = 2; // m
    float zU = 2;
    float Ps = mio::Atmosphere::stdAirPressure(face->get_z()); // Pa

    float dt = (float)global_param->dt();


    float rh = -9999;
    if(has_optional("rh_subcanopy")) {
        rh = (*face)["rh_subcanopy"_s];
    } else {
        rh = (*face)["rh"_s];
    }

    float Rf = 0; // rainfall rate
    float Sf = 0;  // snowfall rate
    if(has_optional("p_subcanopy"))
    {
        Rf = (*face)["p_rain_subcanopy"_s]  / dt; // rainfall rate
        Sf = (*face)["p_snow_subcanopy"_s] / dt; // snowfall rate
    } else {
        Rf = (*face)["p_rain"_s]  / dt; // rainfall rate
        Sf = (*face)["p_snow"_s] / dt; // snowfall rate
    }

    // drop if v. small mass value
    float p_cutoff = 0.1f/dt; //mm/dt
    if( Rf < p_cutoff && Rf > -p_cutoff)
        Rf = 0;
    if( Sf < p_cutoff && Sf > -p_cutoff)
        Sf = 0;

    auto& d = face->get_module_data<data>(ID);

    float elev = (float)(*face)["solar_el"_s];
    float ilwr = -9999;
    if(has_optional("ilwr_subcanopy")) {
        ilwr = (float)(*face)["ilwr_subcanopy"_s];
    } else {
        ilwr = (float)(*face)["ilwr"_s];
    }


    float Sdiff = 0;
    float Sdir = 0;
    if(has_optional("iswr_subcanopy"))
    {
        // TODO: double check this is correct understanding. I don't believe canopy is giving us a diffuse through canopy
        Sdiff = (float)(*face)["iswr_diffuse"_s];
        Sdir  = (float)(*face)["iswr_subcanopy"_s];
    }
    else
    {
        Sdiff = (float)(*face)["iswr_diffuse"_s];
        Sdir = (float)(*face)["iswr_direct"_s];
    }


    float t = -9999;
    if(has_optional("ta_subcanopy")) {
        t = (float)(*face)["ta_subcanopy"_s];
    } else {
        t = (float)(*face)["t"_s];
    }
    t += 273.15;

    float tc = (float)(t - 273.15);
    float Qs = __constants_MOD_eps * (__constants_MOD_e0 / Ps) *
               exp((float)17.5043 * tc / ((float)241.3 + tc));
    float Qa = (rh/ (float)100.0) * Qs; // specific humidity

    float U = (float)(*face)["U_2m_above_srf"_s];

    //blowing snow
    float trans = 0;
    if(has_optional("drift_mass"))
    {
        double mass = (*face)["drift_mass"_s]; //kg/m^2  (mm)

        mass = is_nan(mass) ? 0 : mass;

        // drop if v. small mass value
//        if( mass < 0.1 && mass > -0.1)
//            mass = 0;

        trans = mass / dt;
    }

    // If snow avalanche variables are available
    if(has_optional("delta_avalanche_mass"))
    {
        double delta_avalanche_swe = (*face)["delta_avalanche_mass"_s];

        // delta_avalanche_swe is m^3 of swe. So ----> m^3 / m^2 * 1000 kg/m^3 = kg/m^2
        delta_avalanche_swe = delta_avalanche_swe / face->get_area() * 1000.0;
        trans = trans + delta_avalanche_swe/dt;

    }

    // FSM has removal as positive and deposition as negative
    trans = -trans;

    fsm2_timestep(
        // Driving variables
        &dt, &elev, &zT, &zU,
        &ilwr, &Ps, &Qa, &Rf, &Sdiff, &Sdir, &Sf, &t, &trans, &U,

        // Vegetation characteristics
         &d.veg.alb0, &d.veg.vegh, &d.veg.VAI,

        // State variables
        &d.state.albs, &d.state.Tsrf, d.state.Dsnw, &d.state.Nsnow, d.state.Qcan,
        d.state.Rgrn, d.state.Sice, d.state.Sliq, d.state.Sveg, d.state.Tcan, d.state.Tsnow,
        d.state.Tsoil, d.state.Tveg, d.state.Vsmc,

        // Diagnostics
        &d.diag.H, &d.diag.LE, &d.diag.LWout, &d.diag.LWsub, &d.diag.Melt,
        &d.diag.Roff, &d.diag.snd, &d.diag.snw, &d.diag.subl, &d.diag.svg,
        &d.diag.SWout, &d.diag.SWsub, &d.diag.Usub,  d.diag.Wflx
        );

    (*face)["swe"_s] = d.diag.snw;
    (*face)["snowdepthavg"_s] = d.diag.snd;
    (*face)["snowdepthavg_vert"_s] = d.diag.snd/std::max(0.001,cos(face->slope()));

    (*face)["H"_s] = d.diag.H;
    (*face)["E"_s] = d.diag.LE;
    (*face)["subl"_s] = d.diag.subl;

    d.diag.sum_snowpack_subl += d.diag.subl * global_param->dt();

    (*face)["sum_snowpack_subl"_s] = d.diag.sum_snowpack_subl;
    (*face)["snow_albedo"] = d.state.albs;
}


FSM::~FSM()
{


}
