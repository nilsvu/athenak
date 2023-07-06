#ifndef EOS_PRIMITIVE_SOLVER_HYD_HPP_
#define EOS_PRIMITIVE_SOLVER_HYD_HPP_
//========================================================================================
// AthenaXXX astrophysical plasma code
// Copyright(C) 2020 James M. Stone <jmstone@ias.edu> and the Athena code team
// Licensed under the 3-clause BSD License (the "LICENSE")
//========================================================================================
//! \file primitive_solver_hyd.hpp
//  \brief Contains the template class for PrimitiveSolverHydro, which is independent
//  of the EquationOfState class used elsewhere in AthenaK.

// C++ headers
#include <string>
#include <float.h>
#include <math.h>
#include <type_traits>
#include <iostream>
#include <sstream>

// PrimitiveSolver headers
#include "eos/primitive-solver/eos.hpp"
#include "eos/primitive-solver/primitive_solver.hpp"
#include "eos/primitive-solver/idealgas.hpp"
#include "eos/primitive-solver/piecewise_polytrope.hpp"
#include "eos/primitive-solver/reset_floor.hpp"

// AthenaK headers
#include "athena.hpp"
#include "mesh/mesh.hpp"
#include "parameter_input.hpp"
#include "adm/adm.hpp"
#include "mhd/mhd.hpp"

template<class EOSPolicy, class ErrorPolicy>
class PrimitiveSolverHydro {
  protected:
    void SetPolicyParams(std::string block, ParameterInput *pin) {
      // Parameters for an ideal gas
      if constexpr(std::is_same_v<Primitive::IdealGas, EOSPolicy>) {
        ps.GetEOSMutable().SetGamma(pin->GetOrAddReal(block, "gamma", 5.0/3.0));
      }
      // Parameters for a piecewise polytrope
      if constexpr(std::is_same_v<Primitive::PiecewisePolytrope, EOSPolicy>) {
        // Find out how many pieces we have; exit it if exceeds the maximum number of
        // polytropes.
        int npieces = pin->GetOrAddInteger(block, "npieces", 1);
        const int N = ps.GetEOS().GetMaxPieces();
        if (npieces > N) {
          std::cout << "### FATAL ERROR in " << __FILE__ << " at line " << __LINE__
                    << std::endl << "<hydro> npieces = " << npieces
                    << " too large; MAX_PIECES = " << N << std::endl;
          std::exit(EXIT_FAILURE);
        }

        // Collect information about the pressure at the first polytrope division,
        // the baryon mass, and the minimum density for the EOS.
        Real P0 = pin->GetOrAddReal(block, "P0", 1.0);
        Real mb_nuc = pin->GetOrAddReal(block, "mb_nuc", 1.0);
        Real rho_min = pin->GetOrAddReal(block, "rho_min", 0.1);

        // Collect each individual polytrope
        Real density_pieces[N];
        Real gamma_pieces[N];
        for (int i = 0; i < npieces; i++) {
          std::stringstream dens_name;
          dens_name << "density" << (i + 1);
          std::stringstream gamma_name;
          gamma_name << "gamma" << (i + 1);
          density_pieces[i] = pin->GetOrAddReal(block, dens_name.str(), 1.0);
          gamma_pieces[i] = pin->GetOrAddReal(block, gamma_name.str(), 5.0/3.0);
        }
        bool result = ps.GetEOSMutable().InitializeFromData(density_pieces, gamma_pieces, 
                                                            rho_min, P0, mb_nuc, npieces);
        if (!result) {
          std::cout << "### FATAL ERROR in " << __FILE__ << " at line " << __LINE__
                    << std::endl << "There was an error while constructing the EOS." << std::endl;
          std::exit(EXIT_FAILURE);
        }
        ps.GetEOSMutable().SetThermalGamma(pin->GetOrAddReal(block, "gamma_thermal", 1.5));
      }
    }
  public:
    Primitive::PrimitiveSolver<EOSPolicy, ErrorPolicy> ps;
    MeshBlockPack* pmy_pack;

    PrimitiveSolverHydro(std::string block, MeshBlockPack *pp, ParameterInput *pin) :
//        pmy_pack(pp), ps{&eos} {
          pmy_pack(pp) {
      ps.GetEOSMutable().SetDensityFloor(pin->GetOrAddReal(block, "dfloor", (FLT_MIN)));
      ps.GetEOSMutable().SetTemperatureFloor(pin->GetOrAddReal(block, "tfloor", (FLT_MIN)));
      ps.GetEOSMutable().SetThreshold(pin->GetOrAddReal(block, "dthreshold", 1.0));
      SetPolicyParams(block, pin);
    }

    // The prim to con function used on the reconstructed states inside the Riemann solver.
    // It also extracts the primitives into a form usable by PrimitiveSolver.
    // TODO: Convert to MHD function
    KOKKOS_INLINE_FUNCTION
    void PrimToConsPt(const ScrArray2D<Real> &w, const ScrArray2D<Real> &brc, 
                      const DvceArray4D<Real> &bx,
                      Real prim_pt[NPRIM], Real cons_pt[NCONS], Real b[NMAG],
                      Real g3d[NSPMETRIC], Real sdetg,
                      const int m, const int k, const int j, const int i,
                      const int &nhyd, const int &nscal,
                      const int ibx, const int iby, const int ibz) const {
      auto &eos = ps.GetEOS();
      Real mb = eos.GetBaryonMass();
      // The magnetic field is densitized, but the PrimToCon call 
      // needs undensitized variables.
      b[ibx] = bx(m, k, j, i)/sdetg;
      b[iby] = brc(iby, i)/sdetg;
      b[ibz] = brc(ibz, i)/sdetg;
      /*b[ibx] = 0.0;
      b[iby] = 0.0;
      b[ibz] = 0.0;*/
      Real prim_pt_old[NPRIM];
      prim_pt[PRH] = prim_pt_old[PRH] = w(IDN, i)/mb;
      prim_pt[PVX] = prim_pt_old[PVX] = w(IVX, i);
      prim_pt[PVY] = prim_pt_old[PVY] = w(IVY, i);
      prim_pt[PVZ] = prim_pt_old[PVZ] = w(IVZ, i);
      for (int n = 0; n < nscal; n++) {
        prim_pt[PYF + n] = prim_pt_old[PYF + n] = w(nhyd + n, i);
      }
      // FIXME: Debug only! Use specific energy to validate other
      // hydro functions before breaking things
      //Real e = w(IDN, i) + w(IEN, i);
      //prim_pt[PTM] = prim_pt_old[PTM] = eos.GetTemperatureFromE(prim_pt[PRH], e, &prim_pt[PYF]);
      //prim_pt[PPR] = prim_pt_old[PPR] = eos.GetPressure(prim_pt[PRH], prim_pt[PTM], &prim_pt[PYF]);
      prim_pt[PPR] = prim_pt_old[PPR] = w(IPR, i);

      // Apply the floor to make sure these values are physical.
      // FIXME: Is this needed if the first-order flux correction is enabled?
      prim_pt[PTM] = prim_pt_old[PTM] = eos.GetTemperatureFromP(prim_pt[PRH], 
                                          prim_pt[PPR], &prim_pt[PYF]);
      bool floored = ps.GetEOS().ApplyPrimitiveFloor(prim_pt[PRH], &prim_pt[PVX],
                                           prim_pt[PPR], prim_pt[PTM], &prim_pt[PYF]);

      ps.PrimToCon(prim_pt, cons_pt, b, g3d);

      // Check for NaNs
      if (CheckForConservedNaNs(cons_pt)) {
        printf("Location: PrimToConsPt\n");
        DumpPrimitiveVars(prim_pt);
      }

      // Densitize the variables
      for (int i = 0; i < nhyd + nscal; i++) {
        cons_pt[i] *= sdetg;
      }
      b[ibx] *= sdetg;
      b[iby] *= sdetg;
      b[ibz] *= sdetg;

      // Copy floored primitives back into the original array.
      // TODO: Check if this is necessary
      if (floored) {
        w(IDN, i) = prim_pt[PRH]*mb;
        w(IVX, i) = prim_pt[PVX];
        w(IVY, i) = prim_pt[PVY];
        w(IVZ, i) = prim_pt[PVZ];
        // FIXME: Debug only! Switch to temperature or pressure after validating.
        //w(IEN, i) = ps.GetEOS().GetEnergy(prim_pt[PRH], prim_pt[PTM], &prim_pt[PYF]) - w(IDN, i);
        w(IPR, i) = prim_pt[PPR];
        for (int n = 0; n < nscal; n++) {
          w(nhyd + n, i) = prim_pt[PYF + n];
        }
      }
    }

    void PrimToCons(DvceArray5D<Real> &prim, DvceArray5D<Real> &bcc, DvceArray5D<Real> &cons,
                    const int il, const int iu, const int jl, const int ju,
                    const int kl, const int ku) {
      auto &indcs = pmy_pack->pmesh->mb_indcs;
      //int &is = indcs.is, &js = indcs.js, &ks = indcs.ks;
      //auto &size = pmy_pack->pmb->mb_size;
      //auto &flat = pmy_pack->pcoord->coord_data.is_minkowski;
      auto &eos_ = ps.GetEOS();
      auto &ps_  = ps;

      auto &adm = pmy_pack->padm->adm;

      int &nhyd = pmy_pack->pmhd->nmhd;
      int &nscal = pmy_pack->pmhd->nscalars;
      int &nmb = pmy_pack->nmb_thispack;

      Real mb = eos_.GetBaryonMass();


      par_for("pshyd_prim2cons", DevExeSpace(), 0, (nmb-1), kl, ku, jl, ju, il, iu,
      KOKKOS_LAMBDA(int m, int k, int j, int i) {
        // Extract metric at a single point
        Real g3d[NSPMETRIC];
        g3d[S11] = adm.g_dd(m, 0, 0, k, j, i);
        g3d[S12] = adm.g_dd(m, 0, 1, k, j, i);
        g3d[S13] = adm.g_dd(m, 0, 2, k, j, i);
        g3d[S22] = adm.g_dd(m, 1, 1, k, j, i);
        g3d[S23] = adm.g_dd(m, 1, 2, k, j, i);
        g3d[S33] = adm.g_dd(m, 2, 2, k, j, i);
        Real sdetg = sqrt(Primitive::GetDeterminant(g3d));

        // The magnetic field is densitized, but the PrimToCon calculation is
        // done with undensitized variables.
        Real b[NMAG] = {bcc(m, IBX, k, j, i)/sdetg, 
                        bcc(m, IBY, k, j, i)/sdetg, 
                        bcc(m, IBZ, k, j, i)/sdetg};

        // Extract primitive variables at a single point
        Real prim_pt[NPRIM], cons_pt[NCONS];
        prim_pt[PRH] = prim(m, IDN, k, j, i)/mb;
        prim_pt[PVX] = prim(m, IVX, k, j, i);
        prim_pt[PVY] = prim(m, IVY, k, j, i);
        prim_pt[PVZ] = prim(m, IVZ, k, j, i);
        for (int n = 0; n < nscal; n++) {
          prim_pt[PYF + n] = prim(m, nhyd + n, k, j, i);
        }
        // FIXME: Debug only! Use specific energy to validate other
        // hydro functions before breaking things.
        //Real e = prim(m, IDN, k, j, i) + prim(m, IEN, k, j, i);
        //prim_pt[PTM] = eos_.GetTemperatureFromE(prim_pt[PRH], e, &prim_pt[PYF]);
        //prim_pt[PPR] = eos_.GetPressure(prim_pt[PRH], prim_pt[PTM], &prim_pt[PYF]);
        prim_pt[PPR] = prim(m, IPR, k, j, i);

        // Apply the floor to make sure these values are physical.
        prim_pt[PTM] = eos_.GetTemperatureFromP(prim_pt[PRH], prim_pt[PPR], &prim_pt[PYF]);
        bool floor = eos_.ApplyPrimitiveFloor(prim_pt[PRH], &prim_pt[PVX],
                                             prim_pt[PPR], prim_pt[PTM], &prim_pt[PYF]);
        
        ps_.PrimToCon(prim_pt, cons_pt, b, g3d);

        // Check for NaNs
        if (CheckForConservedNaNs(cons_pt)) {
          printf("Error occurred in PrimToCons at (%d, %d, %d, %d)\n", m, k, j, i);
          DumpPrimitiveVars(prim_pt);
        }

        // Save the densitized conserved variables.
        cons(m, IDN, k, j, i) = cons_pt[CDN]*sdetg;
        cons(m, IM1, k, j, i) = cons_pt[CSX]*sdetg;
        cons(m, IM2, k, j, i) = cons_pt[CSY]*sdetg;
        cons(m, IM3, k, j, i) = cons_pt[CSZ]*sdetg;
        cons(m, IEN, k, j, i) = cons_pt[CTA]*sdetg;
        for (int n = 0; n < nscal; n++) {
          cons(m, nhyd + n, k, j, i) = cons_pt[CYD + n]*sdetg;
        }

        // If we floored the primitive variables, we need to adjust those, too.
        if (floor) {
          prim(m, IDN, k, j, i) = prim_pt[PRH]*mb;
          prim(m, IVX, k, j, i) = prim_pt[PVX];
          prim(m, IVY, k, j, i) = prim_pt[PVY];
          prim(m, IVZ, k, j, i) = prim_pt[PVZ];
          //prim(m, IEN, k, j, i) = eos_.GetEnergy(prim_pt[PRH], prim_pt[PTM], &prim_pt[PYF]) 
          //                        - prim(m, IDN, k, j, i);
          prim(m, IPR, k, j, i) = prim_pt[PPR];
          for (int n = 0; n < nscal; n++) {
            prim(m, nhyd + n, k, j, i) = prim_pt[PYF + n];
          }
        }
      });

      return;
    }

    void ConsToPrim(DvceArray5D<Real> &cons, const DvceFaceFld4D<Real> &bfc, 
                    DvceArray5D<Real> &bcc0, DvceArray5D<Real> &prim,
                    const int il, const int iu, const int jl, const int ju,
                    const int kl, const int ku, bool floors_only=false) {
      //auto &indcs = pmy_pack->pmesh->mb_indcs;
      //int &is = indcs.is, &js = indcs.js, &ks = indcs.ks;
      //auto &size = pmy_pack->pmb->mb_size;

      int &nhyd = pmy_pack->pmhd->nmhd;
      int &nscal = pmy_pack->pmhd->nscalars;
      int &nmb = pmy_pack->nmb_thispack;
      auto &fofc_ = pmy_pack->pmhd->fofc;

      // Some problem-specific parameters
      auto &excise = pmy_pack->pcoord->coord_data.bh_excise;
      auto &excision_floor_ = pmy_pack->pcoord->excision_floor;
      //auto &excision_flux_ = pmy_pack->pcoord->excision_flux;
      auto &dexcise_ = pmy_pack->pcoord->coord_data.dexcise;
      auto &pexcise_ = pmy_pack->pcoord->coord_data.pexcise;

      auto &adm  = pmy_pack->padm->adm;
      auto &eos_ = ps.GetEOS();
      auto &ps_  = ps;

      const int ni = (iu - il + 1);
      const int nji = (ju - jl + 1)*ni;
      const int nkji = (ku - kl + 1)*nji;
      const int nmkji = nmb*nkji;

      Real mb = eos_.GetBaryonMass();

      // FIXME: This only works for a flooring policy that has these functions!
      bool prim_failure, cons_failure;
      if (floors_only) {
        prim_failure = ps.GetEOSMutable().IsPrimitiveFlooringFailure();
        cons_failure = ps.GetEOSMutable().IsConservedFlooringFailure();
        ps.GetEOSMutable().SetPrimitiveFloorFailure(true);
        ps.GetEOSMutable().SetConservedFloorFailure(true);
      }

      int nfloord_=0;
      Kokkos::parallel_reduce("pshyd_c2p",Kokkos::RangePolicy<>(DevExeSpace(), 0, nmkji),
      KOKKOS_LAMBDA(const int &idx, int &sumd) {
        int m = (idx)/nkji;
        int k = (idx - m*nkji)/nji;
        int j = (idx - m*nkji - k*nji)/ni;
        int i = (idx - m*nkji - k*nji - j*ni) + il;
        j += jl;
        k += kl;

        // Extract the metric
        Real g3d[NSPMETRIC], g3u[NSPMETRIC], detg, sdetg;
        g3d[S11] = adm.g_dd(m, 0, 0, k, j, i);
        g3d[S12] = adm.g_dd(m, 0, 1, k, j, i);
        g3d[S13] = adm.g_dd(m, 0, 2, k, j, i);
        g3d[S22] = adm.g_dd(m, 1, 1, k, j, i);
        g3d[S23] = adm.g_dd(m, 1, 2, k, j, i);
        g3d[S33] = adm.g_dd(m, 2, 2, k, j, i);
        detg = Primitive::GetDeterminant(g3d);
        sdetg = sqrt(detg);
        Real isdetg = 1.0/sdetg;
        SpatialInv(1.0/detg, g3d[S11], g3d[S12], g3d[S13], g3d[S22], g3d[S23], g3d[S33],
                   &g3u[S11], &g3u[S12], &g3u[S13], &g3u[S22], &g3u[S23], &g3u[S33]);

        // Extract the conserved variables
        Real cons_pt[NCONS], cons_pt_old[NCONS], prim_pt[NPRIM];
        cons_pt[CDN] = cons_pt_old[CDN] = cons(m, IDN, k, j, i)*isdetg;
        cons_pt[CSX] = cons_pt_old[CSX] = cons(m, IM1, k, j, i)*isdetg;
        cons_pt[CSY] = cons_pt_old[CSY] = cons(m, IM2, k, j, i)*isdetg;
        cons_pt[CSZ] = cons_pt_old[CSZ] = cons(m, IM3, k, j, i)*isdetg;
        cons_pt[CTA] = cons_pt_old[CTA] = cons(m, IEN, k, j, i)*isdetg;
        for (int n = 0; n < nscal; n++) {
          cons_pt[CYD + n] = cons(m, nhyd + n, k, j, i)*isdetg;
        }
        // If we're only testing the floors, we can use the CC fields.
        Real b3u[NMAG];
        if (floors_only) {
          b3u[IBX] = bcc0(m, IBX, k, j, i)*isdetg;
          b3u[IBY] = bcc0(m, IBY, k, j, i)*isdetg;
          b3u[IBZ] = bcc0(m, IBZ, k, j, i)*isdetg;
        }
        // Otherwise we don't have the correct CC fields yet, so use
        // the FC fields.
        else {
          bcc0(m, IBX, k, j, i) = 0.5*(bfc.x1f(m,k,j,i) + bfc.x1f(m,k,j,i+1));
          bcc0(m, IBY, k, j, i) = 0.5*(bfc.x2f(m,k,j,i) + bfc.x2f(m,k,j+1,i));
          bcc0(m, IBZ, k, j, i) = 0.5*(bfc.x3f(m,k,j,i) + bfc.x3f(m,k+1,j,i));
          b3u[IBX] = bcc0(m, IBX, k, j, i)*isdetg;
          b3u[IBY] = bcc0(m, IBY, k, j, i)*isdetg;
          b3u[IBZ] = bcc0(m, IBZ, k, j, i)*isdetg;
        }

        // If we're in an excised region, set the primitives to some default value.
        Primitive::SolverResult result;
        if (excise) {
          if (excision_floor_(m,k,j,i)) {
            prim_pt[PRH] = dexcise_/mb;
            prim_pt[PVX] = 0.0;
            prim_pt[PVY] = 0.0;
            prim_pt[PVZ] = 0.0;
            prim_pt[PPR] = pexcise_;
            for (int n = 0; n < nscal; n++) {
              // FIXME: Particle abundances should probably be set to a
              // default inside an excised region.
              prim_pt[PYF + n] = cons_pt[CYD]/cons_pt[CDN];
            }
            prim_pt[PTM] = eos_.GetTemperatureFromP(prim_pt[PRH], prim_pt[PPR], &prim_pt[PYF]);
            result.error = Primitive::Error::SUCCESS;
            result.iterations = 0;
            result.cons_floor = false;
            result.prim_floor = false;
            result.cons_adjusted = true;
            ps_.PrimToCon(prim_pt, cons_pt, b3u, g3d);
          }
          else {
            result = ps_.ConToPrim(prim_pt, cons_pt, b3u, g3d, g3u);
          }
        }
        else {
          result = ps_.ConToPrim(prim_pt, cons_pt, b3u, g3d, g3u);
        }

        if (result.error != Primitive::Error::SUCCESS && floors_only) {
          fofc_(m,k,j,i) = true;
          sumd++;
        }
        else {
          if (result.error != Primitive::Error::SUCCESS) {
            // TODO: put in a proper error response here.
            printf("An error occurred during the primitive solve: %s\n"
                   "  Location: (%d, %d, %d, %d)\n"
                   "  Conserved vars: \n"
                   "    D   = %g\n"
                   "    Sx  = %g\n"
                   "    Sy  = %g\n"
                   "    Sz  = %g\n"
                   "    tau = %g\n"
                   "  Metric vars: \n"
                   "    detg = %g\n"
                   "    g_dd = {%g, %g, %g, %g, %g, %g}\n"
                   "    alp  = %g\n"
                   "    beta = {%g, %g, %g}\n"
                   "    psi4 = %g\n"
                   "    K_dd = {%g, %g, %g, %g, %g, %g}\n",
                   ErrorToString(result.error),
                   m, k, j, i, cons_pt_old[CDN], cons_pt_old[CSX], cons_pt_old[CSY], cons_pt_old[CSZ],
                   cons_pt_old[CTA], detg, 
                   g3d[S11], g3d[S12], g3d[S13], g3d[S22], g3d[S23], g3d[S33],
                   adm.alpha(m, k, j, i), 
                   adm.beta_u(m, 0, k, j, i), adm.beta_u(m, 1, k, j, i), adm.beta_u(m, 2, k, j, i),
                   adm.psi4(m, k, j, i),
                   adm.K_dd(m, 0, 0, k, j, i), adm.K_dd(m, 0, 1, k, j, i), adm.K_dd(m, 0, 2, k, j, i),
                   adm.K_dd(m, 1, 1, k, j, i), adm.K_dd(m, 1, 2, k, j, i), adm.K_dd(m, 2, 2, k, j, i));
          }
          // Regardless of failure, we need to copy the primitives.
          prim(m, IDN, k, j, i) = prim_pt[PRH]*mb;
          prim(m, IVX, k, j, i) = prim_pt[PVX];
          prim(m, IVY, k, j, i) = prim_pt[PVY];
          prim(m, IVZ, k, j, i) = prim_pt[PVZ];
          //prim(m, IEN, k, j, i) = eos_.GetEnergy(prim_pt[PRH], prim_pt[PTM], &prim_pt[PYF]) -
          //                        prim(m, IDN, k, j, i);
          prim(m, IPR, k, j, i) = prim_pt[PPR];
          for (int n = 0; n < nscal; n++) {
            prim(m, nhyd + n, k, j, i);
          }

          // If the conservative variables were floored or adjusted for consistency,
          // we need to copy the conserved variables, too.
          if (result.cons_floor || result.cons_adjusted) {
            cons(m, IDN, k, j, i) = cons_pt[CDN]*sdetg;
            cons(m, IM1, k, j, i) = cons_pt[CSX]*sdetg;
            cons(m, IM2, k, j, i) = cons_pt[CSY]*sdetg;
            cons(m, IM3, k, j, i) = cons_pt[CSZ]*sdetg;
            cons(m, IEN, k, j, i) = cons_pt[CTA]*sdetg;
            for (int n = 0; n < nscal; n++) {
              cons(m, nhyd + n, k, j, i) = cons_pt[CYD + n]*sdetg;
            }
          }
        }
      }, Kokkos::Sum<int>(nfloord_));

      if (floors_only) {
        ps.GetEOSMutable().SetPrimitiveFloorFailure(prim_failure);
        ps.GetEOSMutable().SetConservedFloorFailure(cons_failure);
        pmy_pack->pmesh->ecounter.nfofc += nfloord_;
      }
    }

    // Get the transformed sound speeds at a point in a given direction.
    KOKKOS_INLINE_FUNCTION
    void GetGRSoundSpeeds(Real& lambda_p, Real& lambda_m, Real prim[NPRIM], Real g3d[NSPMETRIC],
                          Real beta_u[3], Real alpha, Real gii, int pvx) const {
      Real uu[3] = {prim[PVX], prim[PVY], prim[PVZ]};
      Real usq = Primitive::SquareVector(uu, g3d);
      int index = pvx - PVX;

      // Get the Lorentz factor and the 3-velocity.
      Real iWsq = 1.0/(1.0 + usq);
      Real iW = sqrt(iWsq);
      Real vsq = usq*iWsq;
      Real vu[3] = {uu[0]*iW, uu[1]*iW, uu[2]*iW};

      Real cs = ps.GetEOS().GetSoundSpeed(prim[PRH], prim[PTM], &prim[PYF]);
      Real csq = cs*cs;

      Real iWsq_ad = 1.0 - vsq*csq;
      Real dis = (csq*iWsq)*(gii*iWsq_ad - vu[index]*vu[index]*(1.0 - csq));
      Real sdis = sqrt(dis);
      if (!isfinite(sdis)) {
        printf("There's a problem with the sound speed!\n"
               "  dis = %g\n"
               "  gii = %g\n"
               "  csq = %g\n"
               "  vsq = %g\n"
               "  usq = %g\n"
               "  rho = %g\n"
               "  T   = %g\n", 
               dis, gii, csq, vsq, usq, prim[PRH], prim[PTM]);
        exit(EXIT_FAILURE);
      }

      lambda_p = alpha*(vu[index]*(1.0 - csq) + sdis)/iWsq_ad - beta_u[index];
      lambda_m = alpha*(vu[index]*(1.0 - csq) - sdis)/iWsq_ad - beta_u[index];
    }

    // Get the transformed magnetosonic speeds at a point in a given direction.
    KOKKOS_INLINE_FUNCTION
    void GetGRFastMagnetosonicSpeeds(Real& lambda_p, Real& lambda_m, Real prim[NPRIM], Real bsq,
                                     Real g3d[NSPMETRIC], Real beta_u[3], Real alpha, Real gii,
                                     int pvx) const {
      Real uu[3] = {prim[PVX], prim[PVY], prim[PVZ]};
      Real usq = Primitive::SquareVector(uu, g3d);
      int index = pvx - PVX;

      // Get the Lorentz factor and the 3-velocity.
      Real iWsq = 1.0/(1.0 + usq);
      Real iW = sqrt(iWsq);
      Real vsq = usq*iWsq;
      Real vu = uu[index]*iW;

      // Calculate the fast magnetosonic speed in the comoving frame.
      Real cs = ps.GetEOS().GetSoundSpeed(prim[PRH], prim[PTM], &prim[PYF]);
      Real csq = cs*cs;
      Real H = ps.GetEOS().GetBaryonMass()*prim[PRH]*
               ps.GetEOS().GetEnthalpy(prim[PRH], prim[PTM], &prim[PYF]);
      Real vasq = bsq/(bsq + H);
      Real cmsq = csq + vasq - csq*vasq;

      Real iWsq_ad = 1.0 - vsq*cmsq;
      Real dis = (cmsq*iWsq)*(gii*iWsq_ad - vu*vu*(1.0 - cmsq));
      Real sdis = sqrt(dis);
      if (!isfinite(sdis)) {
        printf("There's a problem with the magnetosonic speed!\n"
               "  dis = %g\n"
               "  gii = %g\n"
               "  csq = %g\n"
               "  vsq = %g\n"
               "  usq = %g\n"
               "  rho = %g\n"
               "  vu  = %g\n"
               "  T   = %g\n"
               "  bsq = %g\n", 
               dis, gii, csq, vsq, usq, prim[PRH], prim[PTM], vu, bsq);
        //exit(EXIT_FAILURE);
      }

      lambda_p = alpha*(vu*(1.0 - cmsq) + sdis)/iWsq_ad - beta_u[index];
      lambda_m = alpha*(vu*(1.0 - cmsq) - sdis)/iWsq_ad - beta_u[index];
    }

    // A function for converting PrimitiveSolver errors to strings
    KOKKOS_INLINE_FUNCTION
    const char * ErrorToString(Primitive::Error e) {
      switch(e) {
        case Primitive::Error::SUCCESS:
          return "SUCCESS";
          break;
        case Primitive::Error::RHO_TOO_BIG:
          return "RHO_TOO_BIG";
          break;
        case Primitive::Error::RHO_TOO_SMALL:
          return "RHO_TOO_SMALL";
          break;
        case Primitive::Error::NANS_IN_CONS:
          return "NANS_IN_CONS";
          break;
        case Primitive::Error::MAG_TOO_BIG:
          return "MAG_TOO_BIG";
          break;
        case Primitive::Error::BRACKETING_FAILED:
          return "BRACKETING_FAILED";
          break;
        case Primitive::Error::NO_SOLUTION:
          return "NO_SOLUTION";
          break;
        default:
          return "OTHER";
          break;
      }
    }

    // A function for checking for NaNs in the conserved variables.
    KOKKOS_INLINE_FUNCTION
    int CheckForConservedNaNs(const Real cons_pt[NCONS]) const {
      int nans = 0;
      if (!isfinite(cons_pt[CDN])) {
        printf("D is NaN!\n");
        nans = 1;
      }
      if (!isfinite(cons_pt[CSX])) {
        printf("Sx is NaN!\n");
        nans = 1;
      }
      if (!isfinite(cons_pt[CSY])) {
        printf("Sy is NaN!\n");
        nans = 1;
      }
      if (!isfinite(cons_pt[CSZ])) {
        printf("Sz is NaN!\n");
        nans = 1;
      }
      if (!isfinite(cons_pt[CTA])) {
        printf("Tau is NaN!\n");
        nans = 1;
      }

      return nans;
    }

    KOKKOS_INLINE_FUNCTION
    void DumpPrimitiveVars(const Real prim_pt[NPRIM]) const {
      printf("Primitive vars: \n"
             "  rho = %g\n"
             "  ux  = %g\n"
             "  uy  = %g\n"
             "  uz  = %g\n"
             "  P   = %g\n"
             "  T   = %g\n",
             prim_pt[PRH], prim_pt[PVX], prim_pt[PVY], 
             prim_pt[PVZ], prim_pt[PPR], prim_pt[PTM]);
    }
};
#endif
