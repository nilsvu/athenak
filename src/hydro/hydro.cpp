//========================================================================================
// AthenaXXX astrophysical plasma code
// Copyright(C) 2020 James M. Stone <jmstone@ias.edu> and the Athena code team
// Licensed under the 3-clause BSD License (the "LICENSE")
//========================================================================================
//! \file hydro.cpp
//  \brief implementation of functions in class Hydro

#include <iostream>

#include "athena.hpp"
#include "athena_arrays.hpp"
#include "parameter_input.hpp"
#include "tasklist/task_list.hpp"
#include "mesh/mesh.hpp"
#include "hydro/hydro.hpp"
#include "hydro/eos/eos.hpp"

namespace hydro {
//----------------------------------------------------------------------------------------
// constructor, initializes data structures and parameters

Hydro::Hydro(Mesh *pm, ParameterInput *pin, int gid) :
  pmesh_(pm), my_mbgid_(gid)
{
  // construct EOS object (no default)
  {std::string eqn_of_state   = pin->GetString("hydro","eos");
  if (eqn_of_state.compare("adiabatic") == 0) {
    peos = new AdiabaticHydro(pmesh_, pin, my_mbgid_);
    nhydro = 5;
  } else if (eqn_of_state.compare("isothermal") == 0) {
    peos = new IsothermalHydro(pmesh_, pin, my_mbgid_);
    nhydro = 4;
  } else {
    std::cout << "### FATAL ERROR in " << __FILE__ << " at line " << __LINE__ << std::endl
              << "<hydro> eos = '" << eqn_of_state << "' not implemented"
              << std::endl;
    std::exit(EXIT_FAILURE);
  }} // extra brace to limit scope of string

  // set time-evolution option (no default)
  {std::string evolution_t = pin->GetString("hydro","evolution");
  if (evolution_t.compare("static") == 0) {
    hydro_evol = HydroEvolution::hydro_static;
  } else if (evolution_t.compare("kinematic") == 0) {
    hydro_evol = HydroEvolution::kinematic;
  } else if (evolution_t.compare("dynamic") == 0) {
    hydro_evol = HydroEvolution::hydro_dynamic;
  } else if (evolution_t.compare("none") == 0) {
    hydro_evol = HydroEvolution::no_evolution;
  } else {
    std::cout << "### FATAL ERROR in " << __FILE__ << " at line " << __LINE__ << std::endl
              << "<hydro> evolution = '" << evolution_t << "' not implemented"
              << std::endl;
    std::exit(EXIT_FAILURE);
  }} // extra brace to limit scope of string

  // allocate memory for conserved and primitive variables
  MeshBlock *pmb = pmesh_->FindMeshBlock(my_mbgid_);
  int ncells1 = pmb->mb_cells.nx1 + 2*(pmb->mb_cells.ng);
  int ncells2 = (pmb->mb_cells.nx2 > 1)? (pmb->mb_cells.nx2 + 2*(pmb->mb_cells.ng)) : 1;
  int ncells3 = (pmb->mb_cells.nx3 > 1)? (pmb->mb_cells.nx3 + 2*(pmb->mb_cells.ng)) : 1;
  u0.SetSize(nhydro, ncells3, ncells2, ncells1);
  w0.SetSize(nhydro, ncells3, ncells2, ncells1);

  // for time-evolving problems, continue to construct methods, allocate arrays
  if (hydro_evol != HydroEvolution::no_evolution) {

    // allocate reconstruction method (default PLM)
    {std::string recon_method = pin->GetOrAddString("hydro","reconstruct","plm");
    if (recon_method.compare("dc") == 0) {
      precon = new DonorCell(pin, nhydro, ncells1);
    } else if (recon_method.compare("plm") == 0) {
      precon = new PiecewiseLinear(pin, nhydro, ncells1);
    } else if (recon_method.compare("ppm") == 0) {
      precon = new PiecewiseParabolic(pin, nhydro, ncells1);
    } else {
      std::cout << "### FATAL ERROR in " << __FILE__ << " at line " << __LINE__
                << std::endl << "<hydro> recon = '" << recon_method
                << "' not implemented" << std::endl;
      std::exit(EXIT_FAILURE);
    }} // extra brace to limit scope of string

    // allocate Riemann solver object (default depends on EOS and dynamics)
    {std::string rsolver;
    if (peos->adiabatic_eos) {
      rsolver = pin->GetOrAddString("hydro","rsolver","hllc");
    } else {
      rsolver = pin->GetOrAddString("hydro","rsolver","hlle"); 
    }
    // always make solver=advection for kinematic problems
    if (rsolver.compare("advection") == 0 || hydro_evol == HydroEvolution::kinematic) {
      prsolver = new Advection(pmesh_, pin, my_mbgid_);
    } else if (rsolver.compare("llf") == 0) {
      prsolver = new LLF(pmesh_, pin, my_mbgid_);
    } else if (rsolver.compare("hlle") == 0) {
      prsolver = new HLLE(pmesh_, pin, my_mbgid_);
    } else if (rsolver.compare("hllc") == 0) {
      if (!(peos->adiabatic_eos)) {
        std::cout << "### FATAL ERROR in " << __FILE__ << " at line " << __LINE__
                  << std::endl << "<hydro> rsolver = '" << rsolver
                  << "' cannot be used with isothermal EOS" << std::endl;
        std::exit(EXIT_FAILURE);
      } else {
        prsolver = new HLLC(pmesh_, pin, my_mbgid_);
      }
    } else if (rsolver.compare("roe") == 0) {
      prsolver = new Roe(pmesh_, pin, my_mbgid_);
    } else {
      std::cout << "### FATAL ERROR in " << __FILE__ << " at line " << __LINE__
                << std::endl << "<hydro> rsolver = '" << rsolver << "' not implemented"
                << std::endl;
      std::exit(EXIT_FAILURE);
    }} // extra brace to limit scope of string

    // allocate registers, flux divergence, scratch arrays for time-dep probs
    u1.SetSize(nhydro, ncells3, ncells2, ncells1);
    divf.SetSize(nhydro, ncells3, ncells2, ncells1);
    w_.SetSize(nhydro, ncells1);
    wl_.SetSize(nhydro, ncells1);
    wl_jp1.SetSize(nhydro, ncells1);
    wl_kp1.SetSize(nhydro, ncells1);
    wr_.SetSize(nhydro, ncells1);
    uflux_.SetSize(nhydro, ncells1);
  }
}

//----------------------------------------------------------------------------------------
//! \fn  void Hydro::HydroAddTasks
//  \brief

void Hydro::HydroAddTasks(TaskList &tl, TaskID start, std::vector<TaskID> &added) {

  auto hydro_copycons = tl.AddTask(&Hydro::CopyConserved, this, start);
  auto hydro_divflux  = tl.AddTask(&Hydro::HydroDivFlux, this, hydro_copycons);
  auto hydro_update  = tl.AddTask(&Hydro::HydroUpdate, this, hydro_divflux);
  auto hydro_send  = tl.AddTask(&Hydro::HydroSend, this, hydro_update);
  auto hydro_newdt  = tl.AddTask(&Hydro::NewTimeStep, this, hydro_send);
  auto hydro_recv  = tl.AddTask(&Hydro::HydroReceive, this, hydro_newdt);
//  auto phy_bval  = tl.AddTask(&Hydro::PhysicalBoundaryValues, this, hydro_recv);
  auto hydro_con2prim  = tl.AddTask(&Hydro::ConToPrim, this, hydro_recv);

  added.emplace_back(hydro_copycons);
  added.emplace_back(hydro_divflux);
  added.emplace_back(hydro_update);
  added.emplace_back(hydro_send);
  added.emplace_back(hydro_newdt);
  added.emplace_back(hydro_recv);
  added.emplace_back(hydro_con2prim);

  return;
}

//----------------------------------------------------------------------------------------
//! \fn  void Hydro::HydroSend
//  \brief

TaskStatus Hydro::HydroSend(Driver *pdrive, int stage) 
{
  MeshBlock* pmb = pmesh_->FindMeshBlock(my_mbgid_);
  TaskStatus tstat;
  tstat = pmb->pbvals->SendCellCenteredVariables(u0, nhydro);
  return tstat;
}

//----------------------------------------------------------------------------------------
//! \fn  void Hydro::HydroReceive
//  \brief

TaskStatus Hydro::HydroReceive(Driver *pdrive, int stage)
{
  MeshBlock* pmb = pmesh_->FindMeshBlock(my_mbgid_);
  TaskStatus tstat;
  tstat = pmb->pbvals->ReceiveCellCenteredVariables(u0, nhydro);
  return tstat;
}

//----------------------------------------------------------------------------------------
//! \fn  void Hydro::ConToPrim
//  \brief

TaskStatus Hydro::ConToPrim(Driver *pdrive, int stage)
{
  peos->ConservedToPrimitive(u0, w0);
  return TaskStatus::complete;
}

} // namespace hydro
