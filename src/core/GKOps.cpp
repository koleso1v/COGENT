#include "GKOps.H"
#include "CONSTANTS.H"
#include "FORT_PROTO.H"
#include "inspect.H"

#include <fstream>
#include <sstream>

#include "MillerPhaseCoordSys.H"
#include "SingleNullPhaseCoordSys.H"
#include "SNCorePhaseCoordSys.H"
#include "SlabPhaseCoordSys.H"
#include "Kernels.H"
#include "newMappedGridIO.H"


#include "GKCollisions.H"

#include "LocalizedF_F.H"

#if 1  // warning, OS dependencies, will not work on all platforms
#include <iostream>
#include <sys/types.h>
#include <sys/stat.h>
#endif

#include "CH_HDF5.H"
#include "HDF5Portable.H"

#undef CH_SPACEDIM
#define CH_SPACEDIM CFG_DIM
#include "BoxIterator.H"
#include "LoadBalance.H"
#include "MagBlockCoordSys.H"
#include "MillerBlockCoordSys.H"
#include "MillerCoordSys.H"
#include "SlabBlockCoordSys.H"
#include "SlabCoordSys.H"
#include "SingleNullBlockCoordSys.H"
#include "SNCoreBlockCoordSys.H"
#include "newMappedGridIO.H"
#include "FourthOrderUtil.H"
#include "DataArray.H"
#include "FluidSpecies.H"
#include "Field.H"
#include "inspect.H"
#undef CH_SPACEDIM
#define CH_SPACEDIM PDIM

#undef CH_SPACEDIM
#define CH_SPACEDIM 3
#include "AMRIO.H"
#include "Slicing.H.transdim"
#undef CH_SPACEDIM
#define CH_SPACEDIM PDIM


#include "FourthOrderUtil.H"
#include "Directions.H"

#include "NamespaceHeader.H"

using namespace CH_MultiDim;

#if 0
#ifdef USE_ARK4
void GKOps::define( const GKState& a_state,
                    const Real a_dt,
                    const Real a_dt_scale )
{
   m_dt_scale = a_dt_scale;
   define( a_state, a_dt );
}
#endif
#endif


void GKOps::define( const GKState& a_state,
                    const Real a_dt )
{
   CH_assert( isDefined()==false );
   CH_assert( a_dt>0.0 );
   m_dt = a_dt;
   m_phase_geometry = a_state.geometry();
   CH_assert( m_phase_geometry != NULL );

   ParmParse ppgksys( "gksystem" );
   parseParameters( ppgksys );

   m_units = new GKUnits( ppgksys );

   ParmParse pp;
   m_boundary_conditions = new GKSystemBC( pp, a_state );
   m_initial_conditions = new GKSystemIC( pp, a_state );
   
   const double larmor( m_units->larmorNumber() );
   ParmParse pp_vlasov(GKVlasov::pp_name);
   m_vlasov      = new GKVlasov( pp_vlasov, larmor );
   m_collisions  = new GKCollisions( m_verbosity );
   m_fieldOp     = new CFG::GKFieldOp( m_verbosity );
   m_fluidOp     = new CFG::GKFluidOp( m_verbosity );
   if (m_transport_model_on) {
      m_transport = new GKTransport( m_verbosity );
   }
   if (m_neutrals_model_on) {
      m_neutrals = new GKNeutrals( m_verbosity );
   }
 
   m_Y.define(a_state);

   CFG::IntVect phi_ghost_vect( 4*CFG::IntVect::Unit );
   m_phi.define( m_phase_geometry->magGeom().gridsFull(), 1, phi_ghost_vect );

   m_is_defined = true;
}


void
GKOps::initializeElectricField( const GKState& a_state, const int a_cur_step )
{
   GKState initial_state( a_state );
   
   const KineticSpeciesPtrVect kinetic_species( a_state.dataKinetic() );
   KineticSpeciesPtrVect& initial_kinetic_species( initial_state.dataKinetic() );
   for (int s(0); s<kinetic_species.size(); s++) {
      initial_kinetic_species[s] = kinetic_species[s]->clone( IntVect::Unit );
   }

   const CFG::FluidSpeciesPtrVect fluid_species( a_state.dataFluid() );
   CFG::FluidSpeciesPtrVect& initial_fluid_species( initial_state.dataFluid() );
   for (int s(0); s<fluid_species.size(); s++) {
      initial_fluid_species[s] = fluid_species[s]->clone( CFG::IntVect::Unit );
   }

   const CFG::FieldPtrVect fields( a_state.dataField() );
   CFG::FieldPtrVect& initial_fields( initial_state.dataField() );
   for (int s(0); s<fields.size(); s++) {
      initial_fields[s] = fields[s]->clone( CFG::IntVect::Unit );
   }

   if ( a_cur_step > 0 ) {
      // If we are restarting, initial_kinetic_species contains the current
      // solution, so overwrite it with the initial condition. In particular,
      // we need this to pass initial ion desnity into some electron models
      initializeState( initial_state, 0. );

      //NB: Although the input parametr a_state is physical satate, initialize state gives
      //mapped_space (a.k.a. comp_state). Thus we need to get rid of J to bring it to physical space.
      for (int s(0); s<initial_kinetic_species.size(); s++) {
         LevelData<FArrayBox>& dfn( initial_kinetic_species[s]->distributionFunction() );
         const PhaseGeom& geometry( initial_kinetic_species[s]->phaseSpaceGeometry() );
         geometry.divideJonValid( dfn );
      }
   }
   else if (m_consistent_potential_bcs) {
      m_Er_lo = 0.;
      m_Er_hi = 0.;
   }

   const CFG::MagGeom& mag_geom( m_phase_geometry->magGeom() );
   const CFG::DisjointBoxLayout& mag_grids = mag_geom.gridsFull();

   if (m_consistent_potential_bcs) {

      m_radial_gkp_divergence_average.define( mag_grids, 1, CFG::IntVect::Zero );
      m_radial_flux_divergence_average.define( mag_grids, 1, CFG::IntVect::Zero );
    
      if (a_cur_step==0) {
         m_Er_average_face.define( mag_grids, 3, CFG::IntVect::Unit );
         m_Er_average_cell.define( mag_grids, 3, CFG::IntVect::Unit );
         m_E_tilde_face.define( mag_grids, 3, CFG::IntVect::Unit );
         m_E_tilde_cell.define( mag_grids, 3, CFG::IntVect::Unit );

         for (CFG::DataIterator dit(mag_grids); dit.ok(); ++dit) {
            m_Er_average_face[dit].setVal(0.);
            m_Er_average_cell[dit].setVal(0.);
            m_E_tilde_face[dit].setVal(0.);
            m_E_tilde_cell[dit].setVal(0.);
         }
      }
   }
   
   CFG::LevelData<CFG::FArrayBox> initial_ion_charge_density( mag_grids, 1, CFG::IntVect::Zero);
   // JAFH: Need to include non-kinetic charges
   computeIonChargeDensity( initial_ion_charge_density, initial_kinetic_species );
   
   //We need this to communicate the initial density to certain adiabatic electron models
   createGKPoisson( initial_ion_charge_density );
   
   if ( m_ampere_law && a_cur_step == 0 ) {
      //Initialize E-field
      m_poisson->fillInternalGhosts( m_phi );
      m_poisson->computeField( m_phi, m_Er_average_cell );
      m_poisson->computeField( m_phi, m_Er_average_face );
   }
   
   CH_assert( isDefined() );
   CH_assert( m_phase_geometry != NULL );
   m_E_field_face.define( mag_grids, 3, CFG::IntVect::Unit );
   m_E_field_cell.define( mag_grids, 3, CFG::IntVect::Unit );
   computeEField( m_E_field_face,
                  m_E_field_cell,
                  kinetic_species,
                  fluid_species,
                  fields,
                  a_cur_step );
   
   //Improve Er field calculation to take into accout the dealigment between the grid and magnetic surfaces
   //Should not be used with poloidal variations: FIX LATER!!!
   const CFG::MagCoordSys& coords = *mag_geom.getCoordSys();
   if ( (typeid(coords) == typeid(CFG::SingleNullCoordSys)) && (m_dealignment_corrections)) {
      mag_geom.interpolateErFromMagFS(m_E_field_face, m_E_field_cell);
   }
}

GKOps::~GKOps()
{
   if (m_boltzmann_electron) delete m_boltzmann_electron;
   delete m_collisions;
   delete m_fieldOp;
   delete m_fluidOp;
   if (m_transport) delete m_transport;
   if (m_neutrals)  delete m_neutrals;
   delete m_vlasov;
   if (m_poisson) delete m_poisson;
   for (int ihist(0); ihist<m_hist_count; ihist++) {
     FieldHist *save_hist = &m_fieldHistLists[ihist];
     delete save_hist->fieldvals;
     delete save_hist->timevals;
     delete save_hist->timestep;
   }
}

Real GKOps::stableDtExpl( const GKState& a_state, const int a_step_number )
{
   CH_assert( isDefined() );
   Real dt_stable( DBL_MAX );
   dt_stable = Min( dt_stable, m_dt_vlasov );
   dt_stable = Min( dt_stable, m_dt_collisions );
   dt_stable = Min( dt_stable, m_dt_fields );
   dt_stable = Min( dt_stable, m_dt_fluids );
   if (m_transport_model_on) {
      dt_stable = Min( dt_stable, m_dt_transport );
   }
   if (m_neutrals_model_on) {
      dt_stable = Min( dt_stable, m_dt_neutrals );
   }
   return dt_stable;
}

Real GKOps::stableDtImEx( const GKState& a_state, const int a_step_number )
{
   CH_assert( isDefined() );
   Real dt_stable( DBL_MAX );
   dt_stable = Min( dt_stable, m_dt_vlasov );
   dt_stable = Min( dt_stable, m_dt_fields );
   dt_stable = Min( dt_stable, m_dt_fluids );
   if (m_neutrals_model_on) {
      dt_stable = Min( dt_stable, m_dt_neutrals );
   }
   return dt_stable;
}

void GKOps::preTimeStep (const int a_step, 
                         const Real a_time, 
                         const GKState& a_state_comp, 
                         const GKState& a_state_phys)
{
  setElectricField( a_time, a_state_comp );
  const KineticSpeciesPtrVect& soln_comp( a_state_comp.dataKinetic() );
  const KineticSpeciesPtrVect& soln_phys( a_state_phys.dataKinetic() );

  m_dt_vlasov = m_vlasov->computeDt( m_E_field, soln_comp );
  m_time_scale_vlasov = m_vlasov->computeTimeScale (m_E_field, soln_comp );

  m_dt_collisions = m_collisions->computeDt( soln_comp );
  m_time_scale_collisions = m_collisions->computeTimeScale( soln_comp );

  const CFG::FluidSpeciesPtrVect& fluids_comp( a_state_comp.dataFluid() );
  const CFG::FieldPtrVect& fields_comp( a_state_comp.dataField() );

  m_dt_fields = m_fieldOp->computeDt( fields_comp, fluids_comp );
  m_time_scale_fields = m_fieldOp->computeTimeScale( fields_comp, fluids_comp );
 
  m_dt_fluids = m_fluidOp->computeDt( fields_comp, fluids_comp );
  m_time_scale_fluids = m_fluidOp->computeTimeScale( fields_comp, fluids_comp );

  if (m_transport_model_on) {
    m_dt_transport = m_transport->computeDt( soln_comp );
    m_time_scale_transport = m_transport->computeTimeScale( soln_comp );
  }
  if (m_neutrals_model_on) {
    m_dt_neutrals = m_neutrals->computeDt( soln_comp );
    m_time_scale_neutrals = m_neutrals->computeTimeScale( soln_comp );
  }
  m_collisions->preTimeStep( soln_comp, a_time, soln_phys );
}

void GKOps::postTimeStep (const int a_step, const Real a_time, const GKState& a_state)
{
  /* nothing to do here for now */
}

void GKOps::postTimeStage(const int a_step, const Real a_time, const GKState& a_state, const int a_stage )
{
  /* Stage number follows C convention: they go from 0, ..., n_stages-1 */
  if (a_stage) {
    setElectricField( a_time, a_state );
  }
  const KineticSpeciesPtrVect& soln( a_state.dataKinetic() );
  m_collisions->postTimeStage( soln, a_time, a_stage );
}

void GKOps::setElectricField( const Real a_time, const GKState& a_state)
{
   const KineticSpeciesPtrVect& species_comp( a_state.dataKinetic() );
   const CFG::FluidSpeciesPtrVect& fluids_comp( a_state.dataFluid() );
   const CFG::FieldPtrVect& fields_comp( a_state.dataField() );
   
   if (m_consistent_potential_bcs) {
      // We're not fourth-order accurate with this option anyway,
      // so only compute the field at the beginning of the step.
      computeElectricField( m_E_field, species_comp, fluids_comp, fields_comp, -1 );
   }
   else {
      computeElectricField( m_E_field, species_comp, fluids_comp, fields_comp, -1 );
   }
   return;
}

void GKOps::explicitOp( GKRHSData& a_rhs,
                        const Real a_time,
                        const GKState& a_state,
                        const int a_stage )
{
   CH_assert( isDefined() );
   a_rhs.zero();
   const KineticSpeciesPtrVect& species_comp( a_state.dataKinetic() );
      
   KineticSpeciesPtrVect species_phys;
   createTemporarySpeciesVector( species_phys, species_comp );
   fillGhostCells( species_phys, m_E_field, a_time );
   applyVlasovOperator( a_rhs.dataKinetic(), species_phys, m_E_field, a_time );

   if (m_transport_model_on) {
      applyTransportOperator( a_rhs.dataKinetic(), species_phys, a_time );
   }
    
   if (m_neutrals_model_on) {
      applyNeutralsOperator( a_rhs.dataKinetic(), species_comp, a_time );
   }

   applyCollisionOperator( a_rhs.dataKinetic(), species_comp, a_time );
   
   const CFG::FluidSpeciesPtrVect& fluids_comp( a_state.dataFluid() );
   const CFG::FieldPtrVect& fields_comp( a_state.dataField() );

   applyFieldOperator( a_rhs.dataField(), fields_comp, fluids_comp, species_phys, m_E_field_face, a_time );
   applyFluidOperator( a_rhs.dataFluid(), fields_comp, fluids_comp, species_phys, m_E_field_face, a_time );

   /* The following is hard-coded for a 4-stage time integrator!! */
   if (a_stage == 0) m_stage0_time = a_time;
   if (m_consistent_potential_bcs && a_stage == 3) {

      double dt = a_time - m_stage0_time;

      m_Er_lo += dt * (-m_lo_radial_flux_divergence_average / m_lo_radial_gkp_divergence_average);
      m_Er_hi += dt * (-m_hi_radial_flux_divergence_average / m_hi_radial_gkp_divergence_average);

      setCoreBC( m_Er_lo, -m_Er_hi, m_boundary_conditions->getPotentialBC() );

      if (procID()==0) cout << "  Er_lo = " << m_Er_lo << ", Er_hi = " << m_Er_hi << endl;
       
      if (m_ampere_law) {

         CFG::LevelData<CFG::FluxBox> E_tilde_mapped_face;
         CFG::LevelData<CFG::FArrayBox> E_tilde_mapped_cell;
         CFG::LevelData<CFG::FArrayBox> phi_tilde_fs_average;
         double phi_tilde_fs_average_lo;
         double phi_tilde_fs_average_hi;

         if (!m_ampere_cold_electrons) {

            const CFG::MagGeom& mag_geom = m_phase_geometry->magGeom();
            const CFG::DisjointBoxLayout& mag_grids = mag_geom.gridsFull();

            // Compute the poloidal variation phi_tilde.  The poloidal variation of the
            // current potential is used as the initial guess for the interative procedure.
            int num_phi_ghosts_filled = m_poisson->numPotentialGhosts();
            CH_assert(num_phi_ghosts_filled >=3 );
            CFG::LevelData<CFG::FArrayBox> phi_tilde(mag_grids, 1, num_phi_ghosts_filled*CFG::IntVect::Unit);

            computePhiTilde( species_phys, *m_boltzmann_electron, phi_tilde );

            // Fill the three ghost cell layers at block boundaries as required by the
            // subsequent field calculations
            m_poisson->fillInternalGhosts( phi_tilde );

            E_tilde_mapped_face.define(mag_grids, 3, CFG::IntVect::Unit);
            m_poisson->computeMappedField( phi_tilde, E_tilde_mapped_face );

            E_tilde_mapped_cell.define(mag_grids, 3, CFG::IntVect::Unit);
            m_poisson->computeMappedField( phi_tilde, E_tilde_mapped_cell );

            CFG::LevelData<CFG::FluxBox> Er_tilde_mapped(mag_grids, 1, CFG::IntVect::Zero);
            m_poisson->extractNormalComponent(E_tilde_mapped_face, RADIAL_DIR, Er_tilde_mapped);

            phi_tilde_fs_average.define(mag_grids, 1, CFG::IntVect::Zero);
            m_poisson->computeRadialFSAverage( Er_tilde_mapped, phi_tilde_fs_average_lo, phi_tilde_fs_average_hi, phi_tilde_fs_average );
         }

         updateAveragedEfield( m_Er_average_face, m_Er_average_cell,
                               m_radial_flux_divergence_average, m_lo_radial_flux_divergence_average, m_hi_radial_flux_divergence_average, dt );
          
         updateEfieldPoloidalVariation( E_tilde_mapped_face, E_tilde_mapped_cell, m_E_tilde_face, m_E_tilde_cell,
                                        phi_tilde_fs_average, phi_tilde_fs_average_lo, phi_tilde_fs_average_hi );
      }
   }
}

void GKOps::explicitOp( GKRHSData& a_rhs,
                        const Real a_time,
                        const GKRHSData& a_state,
                        const int a_stage )
{
  m_Y.copy(a_state);
  explicitOp(a_rhs,a_time,m_Y,a_stage);
}

void GKOps::explicitOpImEx( GKRHSData& a_rhs,
                            const Real a_time,
                            const GKState& a_state,
                            const int a_stage )
{
   CH_assert( isDefined() );
   a_rhs.zero();
   const KineticSpeciesPtrVect& species_comp( a_state.dataKinetic() );
   
   KineticSpeciesPtrVect species_phys;
   createTemporarySpeciesVector( species_phys, species_comp );
   fillGhostCells( species_phys, m_E_field, a_time );
   applyVlasovOperator( a_rhs.dataKinetic(), species_phys, m_E_field, a_time );

   if (m_neutrals_model_on) {
      applyNeutralsOperator( a_rhs.dataKinetic(), species_comp, a_time );
   }

   const CFG::FluidSpeciesPtrVect& fluids_comp( a_state.dataFluid() );
   const CFG::FieldPtrVect& fields_comp( a_state.dataField() );

   applyFieldOperator( a_rhs.dataField(), fields_comp, fluids_comp, species_phys, m_E_field_face, a_time );
   applyFluidOperator( a_rhs.dataFluid(), fields_comp, fluids_comp, species_phys, m_E_field_face, a_time );

   /* The following is hard-coded for a 4-stage time integrator!! */
   if (a_stage == 0) m_stage0_time = a_time;
   if (m_consistent_potential_bcs && a_stage == 3) {

      double dt = a_time - m_stage0_time;

      m_Er_lo += dt * (-m_lo_radial_flux_divergence_average / m_lo_radial_gkp_divergence_average);
      m_Er_hi += dt * (-m_hi_radial_flux_divergence_average / m_hi_radial_gkp_divergence_average);

      setCoreBC( m_Er_lo, -m_Er_hi, m_boundary_conditions->getPotentialBC() );

      if (procID()==0) cout << "  Er_lo = " << m_Er_lo << ", Er_hi = " << m_Er_hi << endl;
       
      if (m_ampere_law) {

         CFG::LevelData<CFG::FluxBox> E_tilde_mapped_face;
         CFG::LevelData<CFG::FArrayBox> E_tilde_mapped_cell;
         CFG::LevelData<CFG::FArrayBox> phi_tilde_fs_average;
         double phi_tilde_fs_average_lo;
         double phi_tilde_fs_average_hi;

         if (!m_ampere_cold_electrons) {

            const CFG::MagGeom& mag_geom = m_phase_geometry->magGeom();
            const CFG::DisjointBoxLayout& mag_grids = mag_geom.gridsFull();

            // Compute the poloidal variation phi_tilde.  The poloidal variation of the
            // current potential is used as the initial guess for the interative procedure.
            int num_phi_ghosts_filled = m_poisson->numPotentialGhosts();
            CH_assert(num_phi_ghosts_filled >=2 );
            CFG::LevelData<CFG::FArrayBox> phi_tilde(mag_grids, 1, num_phi_ghosts_filled*CFG::IntVect::Unit);

            computePhiTilde( species_phys, *m_boltzmann_electron, phi_tilde );

            // Fill the two ghost cell layers at block boundaries as required by the
            // subsequent field calculations
            m_poisson->fillInternalGhosts( phi_tilde );

            E_tilde_mapped_face.define(mag_grids, 3, CFG::IntVect::Zero);
            m_poisson->computeMappedField( phi_tilde, E_tilde_mapped_face );

            E_tilde_mapped_cell.define(mag_grids, 3, CFG::IntVect::Zero);
            m_poisson->computeMappedField( phi_tilde, E_tilde_mapped_cell );

            CFG::LevelData<CFG::FluxBox> Er_tilde_mapped(mag_grids, 1, CFG::IntVect::Zero);
            m_poisson->extractNormalComponent(E_tilde_mapped_face, RADIAL_DIR, Er_tilde_mapped);

            phi_tilde_fs_average.define(mag_grids, 1, CFG::IntVect::Zero);
            m_poisson->computeRadialFSAverage( Er_tilde_mapped, phi_tilde_fs_average_lo, phi_tilde_fs_average_hi, phi_tilde_fs_average );
         }

         updateAveragedEfield( m_Er_average_face, m_Er_average_cell,
                               m_radial_flux_divergence_average, m_lo_radial_flux_divergence_average, m_hi_radial_flux_divergence_average, dt );
          
         updateEfieldPoloidalVariation( E_tilde_mapped_face, E_tilde_mapped_cell, m_E_tilde_face, m_E_tilde_cell,
                                        phi_tilde_fs_average, phi_tilde_fs_average_lo, phi_tilde_fs_average_hi );
      }
   }
}

void GKOps::explicitOpImEx( GKRHSData& a_rhs,
                            const Real a_time,
                            const GKRHSData& a_state,
                            const int a_stage )
{
  m_Y.copy(a_state);
  explicitOpImEx(a_rhs,a_time,m_Y,a_stage);
}

void GKOps::implicitOpImEx( GKRHSData& a_rhs,
                            const Real a_time,
                            const GKState& a_state,
                            const int a_stage,
                            const int a_flag )
{
   CH_assert( isDefined() );
   a_rhs.zero();
   const KineticSpeciesPtrVect& species_comp( a_state.dataKinetic() );

   KineticSpeciesPtrVect species_phys;
   createTemporarySpeciesVector( species_phys, species_comp );
   fillGhostCells( species_phys, m_E_field, a_time );

   if (m_transport_model_on) {
      applyTransportOperator( a_rhs.dataKinetic(), species_phys, a_time );
   }
   applyCollisionOperator( a_rhs.dataKinetic(), species_comp, a_time, a_flag );
}

void GKOps::implicitOpImEx( GKRHSData& a_rhs,
                            const Real a_time,
                            const GKRHSData& a_state,
                            const int a_stage,
                            const int a_flag )
{
  m_Y.copy(a_state);
  implicitOpImEx(a_rhs,a_time,m_Y,a_stage,a_flag);
}

static inline bool setupPrecondMatrix(void              *a_P, 
                                      const int         a_N,
                                      const GlobalDOF*  a_global_dof,
                                      GKCollisions      *a_collisions )
{
  /* find the maximum number of bands */
  int nbands_max = 0;
  nbands_max = a_collisions->precondMatrixBands();

  /* define the matrix */
  if (!procID()) {
    cout << "Setting up banded matrix with " << a_N << " rows ";
    cout << "and " << nbands_max << " bands for the preconditioner.\n";
  }
  BandedMatrix *P = (BandedMatrix*) a_P;
  P->define(a_N,nbands_max,a_global_dof->mpi_offset());
  return(P->isDefined());
}

bool GKOps::setupPCImEx( void *a_P, GKState& a_state )
{
   return(setupPrecondMatrix( a_P, 
                              a_state.getVectorSize(), 
                              a_state.getGlobalDOF(),
                              m_collisions ));
}

bool GKOps::setupPCImEx( void *a_P, GKRHSData& a_state )
{
   return(setupPrecondMatrix( a_P, 
                              a_state.getVectorSize(), 
                              a_state.getGlobalDOF(),
                              m_collisions ));
}

static inline void assemblePrecondMatrix(
                                          void                            *a_P,
                                          const KineticSpeciesPtrVect&    a_kinetic_species,
                                          const CFG::FluidSpeciesPtrVect& a_fluid_species,
                                          const CFG::FieldPtrVect&        a_fields,
                                          const GlobalDOF*                a_global_dof,
                                          GKCollisions                    *a_collisions
                                          /* CFG::GKFieldOp                  *a_fluid_op  */
                                          /* CFG::GKFluidOp                  *a_field_op  */
                                        )
{
  BandedMatrix *Pmat = (BandedMatrix*) a_P;
  Pmat->zeroEntries();
  a_collisions->assemblePrecondMatrix(Pmat,a_kinetic_species,a_global_dof->dataKinetic());
  /* a_fluid_op->assemblePrecondMatrix(Pmat,a_fluid_species,a_global_dof->dataFluid()); */
  /* a_field_op->assemblePrecondMatrix(Pmat,a_fields,a_global_dof->dataFields()); */
  return;
}

void GKOps::assemblePCImEx( void *a_P, const GKState& a_state )
{
  assemblePrecondMatrix(
                        a_P,
                        a_state.dataKinetic(),
                        a_state.dataFluid(),
                        a_state.dataField(),
                        a_state.getGlobalDOF(),
                        m_collisions
                        /* m_fluidOp */
                        /* m_fieldOp */
                       );
  return;
}

void GKOps::assemblePCImEx( void *a_P, const GKRHSData& a_state )
{
  assemblePrecondMatrix(
                        a_P,
                        a_state.dataKinetic(),
                        a_state.dataFluid(),
                        a_state.dataField(),
                        a_state.getGlobalDOF(),
                        m_collisions
                        /* m_fluidOp */
                        /* m_fieldOp */
                       );
  return;
}

inline
void GKOps::computeElectricField( LevelData<FluxBox>&               a_E_field,
                                  const KineticSpeciesPtrVect&      a_kinetic_species,
                                  const CFG::FluidSpeciesPtrVect&   a_fluid_species,
                                  const CFG::FieldPtrVect&          a_fields,
                                  const int                         a_step_number )
{
   CH_assert( isDefined() );
   
   CFG::IntVect ghost_vect_cfg;
   for (int d(0); d<CFG_DIM; d++) {
      ghost_vect_cfg[d] = m_ghost_vect[d];
   }
   
   //Obtain physical solutions
   const int num_kinetic_species( a_kinetic_species.size() );
   KineticSpeciesPtrVect kinetic_result;
   kinetic_result.resize(num_kinetic_species);
   for (int species(0); species<num_kinetic_species; species++) {
      kinetic_result[species] = a_kinetic_species[species]->clone( m_ghost_vect );
   }
   divideJ( a_kinetic_species, kinetic_result );
   
   const int num_fluid_species( a_fluid_species.size() );
   CFG::FluidSpeciesPtrVect fluid_result;
   fluid_result.resize(num_fluid_species);

   for (int species(0); species<num_fluid_species; species++) {
      fluid_result[species] = a_fluid_species[species]->clone( ghost_vect_cfg );
   }
   divideJ( a_fluid_species, fluid_result );
   
   const int num_field_comp( a_fields.size() );
   CFG::FieldPtrVect field_result;
   field_result.resize(num_field_comp);
   for (int comp(0); comp<num_field_comp; comp++) {
      field_result[comp] = a_fields[comp]->clone( ghost_vect_cfg );
   }
   divideJ( a_fields, field_result );

   
   computeEField( m_E_field_face,
                  m_E_field_cell,
                  kinetic_result,
                  fluid_result,
                  field_result,
                  a_step_number );

   CH_assert( m_phase_geometry != NULL );
   m_phase_geometry->injectConfigurationToPhase( m_E_field_face,
                                                 m_E_field_cell,
                                                 a_E_field );
}


inline
void GKOps::createTemporaryFieldVector( CFG::FieldPtrVect& a_out,
                                        const CFG::FieldPtrVect& a_in )
{
   CFG::IntVect ghost_vect;
   for (int d(0); d<CFG_DIM; d++) {
      ghost_vect[d] = m_ghost_vect[d];
   }
   a_out.resize( a_in.size() );
   for (int s(0); s<a_in.size(); s++) {
      a_out[s] = a_in[s]->clone( ghost_vect );
      CFG::LevelData<CFG::FArrayBox>& out_data( a_out[s]->data() );
      const CFG::MagGeom& geometry( a_in[s]->configurationSpaceGeometry() );
      geometry.divideJonValid( out_data );
   }
}


inline
void GKOps::createTemporarySpeciesVector( CFG::FluidSpeciesPtrVect& a_out,
                                          const CFG::FluidSpeciesPtrVect& a_in )
{
   CFG::IntVect ghost_vect;
   for (int d(0); d<CFG_DIM; d++) {
      ghost_vect[d] = m_ghost_vect[d];
   }
   a_out.resize( a_in.size() );
   for (int s(0); s<a_in.size(); s++) {
      a_out[s] = a_in[s]->clone( ghost_vect );
      CFG::LevelData<CFG::FArrayBox>& out_data( a_out[s]->data() );
      const CFG::MagGeom& geometry( a_in[s]->configurationSpaceGeometry() );
      geometry.divideJonValid( out_data );
   }
}


inline
void GKOps::createTemporarySpeciesVector( KineticSpeciesPtrVect& a_out,
                                          const KineticSpeciesPtrVect& a_in )
{
   a_out.resize( a_in.size() );
   for (int s(0); s<a_in.size(); s++) {
      a_out[s] = a_in[s]->clone( m_ghost_vect );
      LevelData<FArrayBox>& out_dfn( a_out[s]->distributionFunction() );
      const PhaseGeom& geometry( a_in[s]->phaseSpaceGeometry() );
      geometry.divideJonValid( out_dfn );
   }
}


inline
void GKOps::createTemporaryState( GKState& a_out, const GKState& a_in )
{
   createTemporarySpeciesVector( a_out.dataKinetic(), a_in.dataKinetic() );
   createTemporarySpeciesVector( a_out.dataFluid(), a_in.dataFluid() );
   createTemporaryFieldVector( a_out.dataField(), a_in.dataField() );
}

inline
void GKOps::fillGhostCells(
   KineticSpeciesPtrVect&       a_species_phys,
   const LevelData<FluxBox>&    a_E_field,
   const Real&                  a_time )
{
   CH_assert( isDefined() );
   m_boundary_conditions->fillGhostCells( a_species_phys,
                                          m_phi,
                                          a_E_field,
                                          a_time );
}

inline
void GKOps::fillGhostCells( GKState&                  a_state_phys,
                            const LevelData<FluxBox>& a_E_field,
                            const Real&               a_time )
{
   CH_assert( isDefined() );
   m_boundary_conditions->fillGhostCells( a_state_phys,
                                          m_phi,
                                          a_E_field,
                                          a_time );
}


inline
void GKOps::applyVlasovOperator(
   KineticSpeciesPtrVect&       a_rhs,
   const KineticSpeciesPtrVect& a_soln,
   const LevelData<FluxBox>&    a_E_field,
   const Real&                  a_time )
{
   CH_assert( isDefined() );
   m_count_vlasov++;

   if (m_consistent_potential_bcs) {
      m_lo_radial_flux_divergence_average = 0.;
      m_hi_radial_flux_divergence_average = 0.;

      for (int s(0); s<a_rhs.size(); s++) {
         const KineticSpecies& soln_species( *(a_soln[s]) );
         KineticSpecies& rhs_species( *(a_rhs[s]) );
         double lo_value, hi_value;
         m_vlasov->evalRHS( rhs_species, lo_value, hi_value, m_radial_flux_divergence_average, soln_species, a_E_field, a_time );

         m_lo_radial_flux_divergence_average += lo_value;
         m_hi_radial_flux_divergence_average += hi_value;
      }
   }
   else {
      for (int s(0); s<a_rhs.size(); s++) {
         const KineticSpecies& soln_species( *(a_soln[s]) );
         KineticSpecies& rhs_species( *(a_rhs[s]) );
         m_vlasov->evalRHS( rhs_species, soln_species, a_E_field, a_time );
      }
   }
}


inline
void GKOps::applyCollisionOperator( KineticSpeciesPtrVect&       a_rhs,
                                    const KineticSpeciesPtrVect& a_soln,
                                    const Real&                  a_time,
                                    const int                    a_flag )
{
   CH_assert( isDefined() );
   m_count_collision++;
   m_collisions->accumulateRHS( a_rhs, a_soln, a_time, a_flag );
}


inline
void GKOps::applyTransportOperator( KineticSpeciesPtrVect&       a_rhs,
                                    const KineticSpeciesPtrVect& a_soln,
                                    const Real&                  a_time )
{
   CH_assert( isDefined() );
   m_count_transport++;
   m_transport->accumulateRHS( a_rhs, a_soln, a_time );

}

inline
void GKOps::applyNeutralsOperator( KineticSpeciesPtrVect&       a_rhs,
                                   const KineticSpeciesPtrVect& a_soln,
                                   const Real&                  a_time )
{
    CH_assert( isDefined() );
    m_count_neutrals++;
    m_neutrals->accumulateRHS( a_rhs, a_soln, a_time );
    
}

inline
void GKOps::applyFieldOperator( CFG::FieldPtrVect&                         a_rhs,
                                const CFG::FieldPtrVect&                   a_fields,
                                const CFG::FluidSpeciesPtrVect&            a_fluids,
                                const KineticSpeciesPtrVect&               a_soln,
                                const CFG::LevelData<CFG::FluxBox>&        a_E_field,
                                const Real&                                a_time)
{
   CH_assert( isDefined() );
   m_count_fields++;
   m_fieldOp->accumulateRHS( a_rhs, a_fields, a_fluids, a_soln, a_E_field, a_time );
}

inline
void GKOps::applyFluidOperator( CFG::FluidSpeciesPtrVect&                  a_rhs,
                                const CFG::FieldPtrVect&                   a_fields,
                                const CFG::FluidSpeciesPtrVect&            a_fluids,
                                const KineticSpeciesPtrVect&               a_soln,
                                const CFG::LevelData<CFG::FluxBox>&        a_E_field,
                                const Real&                                a_time)
{
   CH_assert( isDefined() );
   m_count_fluids++;
   m_fluidOp->accumulateRHS( a_rhs, a_fields, a_fluids, a_soln, a_E_field, a_time );
}

inline
void setZero( CFG::LevelData<CFG::FArrayBox>& a_data )
{
   for (CFG::DataIterator dit(a_data.dataIterator()); dit.ok(); ++dit) {
      a_data[dit].setVal(0.);
   }
}


void
GKOps::computeTotalChargeDensity( CFG::LevelData<CFG::FArrayBox>& a_charge_density,
                                  const KineticSpeciesPtrVect&    a_species ) const
{
   // Container for individual species charge density
   CH_assert( m_phase_geometry != NULL );
   const CFG::MagGeom& mag_geom( m_phase_geometry->magGeom() );
   CFG::LevelData<CFG::FArrayBox> species_charge_density( mag_geom.gridsFull(),
                                                          1,
                                                          CFG::IntVect::Zero );

   setZero( a_charge_density );

   for (int species(0); species<a_species.size(); species++) {
      const KineticSpecies& this_species( *(a_species[species]) );

      // Compute the charge density for this species
      this_species.chargeDensity( species_charge_density );
      
      CFG::DataIterator dit( a_charge_density.dataIterator() );
      for (dit.begin(); dit.ok(); ++dit) {
         a_charge_density[dit].plus( species_charge_density[dit] );
      }
   }
}


void
GKOps::computeIonChargeDensity( CFG::LevelData<CFG::FArrayBox>& a_ion_charge_density,
                                const KineticSpeciesPtrVect&    a_species ) const
{
   // Container for individual species charge density
   CH_assert( m_phase_geometry != NULL );
   const CFG::MagGeom& mag_geom( m_phase_geometry->magGeom() );
   CFG::LevelData<CFG::FArrayBox> species_charge_density( mag_geom.gridsFull(),
                                                          1,
                                                          CFG::IntVect::Zero );

   setZero( a_ion_charge_density );

   for (int species(0); species<a_species.size(); species++) {

      const KineticSpecies& this_species( *(a_species[species]) );
      if ( this_species.charge() < 0.0 ) continue;
      
      // Compute the charge density for this species
      this_species.chargeDensity( species_charge_density );
      
      CFG::DataIterator dit( a_ion_charge_density.dataIterator() );
      for (dit.begin(); dit.ok(); ++dit) {
         a_ion_charge_density[dit].plus( species_charge_density[dit] );
      }
   }
}

void
GKOps::computeIonParallelCurrentDensity( CFG::LevelData<CFG::FArrayBox>& a_ion_current_density,
                                         const KineticSpeciesPtrVect&    a_species ) const
{
   // Container for individual species charge density
   CH_assert( m_phase_geometry != NULL );
   const CFG::MagGeom& mag_geom( m_phase_geometry->magGeom() );
   CFG::LevelData<CFG::FArrayBox> species_current_density( mag_geom.gridsFull(),
                                                          1,
                                                          CFG::IntVect::Zero );

   setZero( a_ion_current_density );

   for (int species(0); species<a_species.size(); species++) {

      const KineticSpecies& this_species( *(a_species[species]) );
      if ( this_species.charge() < 0.0 ) continue;
      
      // Compute the charge density for this species
      this_species.ParallelMomentum( species_current_density );
      
      CFG::DataIterator dit( a_ion_current_density.dataIterator() );
      for (dit.begin(); dit.ok(); ++dit) {
         species_current_density[dit].mult( this_species.charge() );
         a_ion_current_density[dit].plus( species_current_density[dit] );
      }
   }
}


void
GKOps::computeSignedChargeDensities( CFG::LevelData<CFG::FArrayBox>& a_pos_charge_density,
                                     CFG::LevelData<CFG::FArrayBox>& a_neg_charge_density,
                                     const KineticSpeciesPtrVect& a_species ) const
{
   // Container for individual species charge density
   CH_assert( m_phase_geometry != NULL );
   const CFG::MagGeom& mag_geom( m_phase_geometry->magGeom() );
   CFG::LevelData<CFG::FArrayBox> species_charge_density( mag_geom.gridsFull(),
                                                          1,
                                                          CFG::IntVect::Zero );

   setZero( a_pos_charge_density );
   setZero( a_neg_charge_density );

   for (int species(0); species<a_species.size(); species++) {
      const KineticSpecies& this_species( *(a_species[species]) );

      // Compute the charge density for this species
      this_species.chargeDensity( species_charge_density );

      if ( this_species.charge() > 0.0 ) {
         CFG::DataIterator dit( a_pos_charge_density.dataIterator() );
         for (dit.begin(); dit.ok(); ++dit) {
            a_pos_charge_density[dit].plus( species_charge_density[dit] );
         }
      }
      else {
         CFG::DataIterator dit( a_neg_charge_density.dataIterator() );
         for (dit.begin(); dit.ok(); ++dit) {
            a_neg_charge_density[dit].plus( species_charge_density[dit] );
         }
      }
   }
}


void
GKOps::computeIonMassDensity( CFG::LevelData<CFG::FArrayBox>& a_mass_density,
                              const KineticSpeciesPtrVect&    a_species ) const
{
   // Container for individual species charge density
   CH_assert( m_phase_geometry != NULL );
   const CFG::MagGeom& mag_geom( m_phase_geometry->magGeom() );
   CFG::LevelData<CFG::FArrayBox> species_mass_density( mag_geom.gridsFull(),
                                                        1,
                                                        CFG::IntVect::Zero );

   setZero( a_mass_density );

   for (int species(0); species<a_species.size(); species++) {
         
      const KineticSpecies& this_species( *(a_species[species]) );
      if ( this_species.charge() < 0.0 ) continue;

      // Compute the charge density for this species
      this_species.massDensity( species_mass_density );
      
      CFG::DataIterator dit( a_mass_density.dataIterator() );
      for (dit.begin(); dit.ok(); ++dit) {
         a_mass_density[dit].plus( species_mass_density[dit] );
      }
   }
}


void GKOps::computeEField( CFG::LevelData<CFG::FluxBox>&       a_E_field_face,
                           CFG::LevelData<CFG::FArrayBox>&     a_E_field_cell,
                           const KineticSpeciesPtrVect&        a_soln_kinetic,
                           const CFG::FluidSpeciesPtrVect&     a_soln_fluid,
                           const CFG::FieldPtrVect&            a_soln_field,
                           const int                           a_step_number )
{
   CH_assert(a_E_field_face.ghostVect() == CFG::IntVect::Unit);
   CH_assert(a_E_field_cell.ghostVect() == CFG::IntVect::Unit);
   CH_assert( isDefined() );

   if (m_poisson) {
      CFG::PotentialBC& bc = m_boundary_conditions->getPotentialBC();

      const CFG::MagGeom& mag_geom( m_phase_geometry->magGeom() );
      const CFG::DisjointBoxLayout& grids( mag_geom.gridsFull() );

      if (!m_fixed_efield){
         CH_assert( m_phase_geometry != NULL );
         // Update the potential and field, if not fixed_efield

         CFG::LevelData<CFG::FArrayBox> ion_mass_density( grids, 1, CFG::IntVect::Zero );
         computeIonMassDensity( ion_mass_density, a_soln_kinetic );

         if (m_boltzmann_electron == NULL) {
            m_poisson->setOperatorCoefficients( ion_mass_density, bc );

            CFG::LevelData<CFG::FArrayBox> gkPoissonRHS( grids, 1, CFG::IntVect::Zero );

            if (m_vorticity_model) {
               CH_assert(a_soln_field.size()>0);
               CFG::Field& field( *(a_soln_field[0]) );
               CFG::LevelData<CFG::FArrayBox>& vorticity( field.data() );

               for (CFG::DataIterator dit(gkPoissonRHS.dataIterator()); dit.ok(); ++dit) {
                  gkPoissonRHS[dit].copy(vorticity[dit]);
               }
            }
            
            else {
               computeTotalChargeDensity( gkPoissonRHS, a_soln_kinetic );
            }
            
            m_poisson->computePotential( m_phi, gkPoissonRHS );
            
         }
         else {

            // Boltzmann electron model

            if (m_consistent_potential_bcs) {
               setCoreBC( m_Er_lo, -m_Er_hi, bc );
            }

            CFG::LevelData<CFG::FArrayBox> ion_charge_density( grids, 1, CFG::IntVect::Zero );
            computeIonChargeDensity( ion_charge_density, a_soln_kinetic );
            
            bool single_null = typeid(*(mag_geom.getCoordSys())) == typeid(CFG::SingleNullCoordSys);

            if (single_null) {

               CFG::LevelData<CFG::FArrayBox> ion_parallel_current_density( grids, 1, CFG::IntVect::Zero );
               computeIonParallelCurrentDensity( ion_parallel_current_density, a_soln_kinetic );

              ((CFG::NewGKPoissonBoltzmann*)m_poisson)
                 ->setDivertorBVs( ion_charge_density, ion_parallel_current_density, bc );
            }

            if (m_consistent_potential_bcs) {

               double lo_value, hi_value;

               if (single_null) {

                  double core_outer_bv = -m_Er_hi;

                  ((CFG::NewGKPoissonBoltzmann*)m_poisson)
                     ->setOperatorCoefficients( ion_mass_density, bc, core_outer_bv, lo_value, hi_value, m_radial_gkp_divergence_average );
               }
               else {
                  ((CFG::GKPoissonBoltzmann*)m_poisson)
                     ->setOperatorCoefficients( ion_mass_density, bc, lo_value, hi_value, m_radial_gkp_divergence_average );
               }

               m_lo_radial_gkp_divergence_average = lo_value;
               m_hi_radial_gkp_divergence_average = hi_value;
            }
            else {
               m_poisson->setOperatorCoefficients( ion_mass_density, bc );
            }

            if ( !m_ampere_law ) {
             
               if (single_null) {
                  ((CFG::NewGKPoissonBoltzmann*)m_poisson)
                     ->computePotentialAndElectronDensity( m_phi,
                                                           *m_boltzmann_electron,
                                                           ion_charge_density,
                                                           bc,
                                                           a_step_number==0 );
               }
               else {
                  ((CFG::GKPoissonBoltzmann*)m_poisson)
                     ->computePotentialAndElectronDensity( m_phi,
                                                           *m_boltzmann_electron,
                                                           ion_charge_density,
                                                           bc,
                                                           a_step_number==0 );
               }
            }
         }
      }

      if (!m_fixed_efield || (m_initializedE==false)){
         // only calculate E if not fixed efield or if this is the first step in the simulation.
         //  Need to take care that this is done right for a restart
         m_poisson->fillInternalGhosts( m_phi );

         if (m_ampere_law) {
            for (CFG::DataIterator dit(a_E_field_cell.dataIterator()); dit.ok(); ++dit) {
               a_E_field_face[dit].copy(m_Er_average_face[dit]);
               a_E_field_face[dit] += m_E_tilde_face[dit];
               a_E_field_cell[dit].copy(m_Er_average_cell[dit]);
               a_E_field_cell[dit] += m_E_tilde_cell[dit];
            }
         }
         else {
            m_poisson->computeField( m_phi, a_E_field_cell );
            m_poisson->computeField( m_phi, a_E_field_face );
         }
         m_initializedE = true;
      }
   }
   else if (m_initializedE==false) {
      // Only calculate E if not fixed efield or if this is the first step in the simulation.
      // Need to take care that this is done right for a restart
         
      for (CFG::DataIterator dit( a_E_field_face.dataIterator() ); dit.ok(); ++dit) {
         a_E_field_cell[dit].setVal(0.);
         for (int dir(0); dir<CFG_DIM; ++dir) {
            a_E_field_face[dit].setVal(0.,dir);
         }
      }
      m_initializedE = true;
   }
}


void
GKOps::setCoreBC( const double      a_core_inner_bv,
                  const double      a_core_outer_bv,
                  CFG::PotentialBC& a_bc ) const 
{
   const CFG::MagGeom& mag_geom = m_phase_geometry->magGeom();

   if ( typeid(*(mag_geom.getCoordSys())) == typeid(CFG::MillerCoordSys) ) {
      a_bc.setBCValue(0,RADIAL_DIR,0,a_core_inner_bv);
      a_bc.setBCValue(0,RADIAL_DIR,1,a_core_outer_bv);
   }
   else if ( typeid(*(mag_geom.getCoordSys())) == typeid(CFG::SlabCoordSys) ) {
      a_bc.setBCValue(0,RADIAL_DIR,0,a_core_inner_bv);
      a_bc.setBCValue(0,RADIAL_DIR,1,a_core_outer_bv);
   }
   else if ( typeid(*(mag_geom.getCoordSys())) == typeid(CFG::SNCoreCoordSys) ) {
      a_bc.setBCValue(L_CORE,RADIAL_DIR,0,a_core_inner_bv);
      a_bc.setBCValue(L_CORE,RADIAL_DIR,1,a_core_outer_bv);
   }
   else if ( typeid(*(mag_geom.getCoordSys())) == typeid(CFG::SingleNullCoordSys) ) {
      a_bc.setBCValue(LCORE,RADIAL_DIR,0,a_core_inner_bv);
   }
   else {
      MayDay::Error("GKOps::computeEfield(): unknown geometry with consistent bcs");
   }
}


void
GKOps::computePhiTilde( const KineticSpeciesPtrVect&          a_kinetic_species,
                        const CFG::BoltzmannElectron&         a_ne,
                        CFG::LevelData<CFG::FArrayBox>&       a_phi_tilde ) const
{
   CFG::LevelData<CFG::FArrayBox> Zni(m_phase_geometry->magGeom().gridsFull(), 1, CFG::IntVect::Zero);
   computeIonChargeDensity( Zni, a_kinetic_species );

   if ( CFG::GKPoissonBoltzmann* gkpb = dynamic_cast<CFG::GKPoissonBoltzmann*>(m_poisson) ) {
      gkpb->getPhiTilde( Zni, a_ne, a_phi_tilde );
   }
   else if ( CFG::NewGKPoissonBoltzmann* new_gkpb = dynamic_cast<CFG::NewGKPoissonBoltzmann*>(m_poisson) ) {
      new_gkpb->getPhiTilde( Zni, a_ne, a_phi_tilde );
   }
   else {
      MayDay::Error("GKOps::computeGKPPhiTildeDivergence() can only be called with Boltzmann electrons");
   }
}


void
GKOps::updateAveragedEfield( CFG::LevelData<CFG::FluxBox>&   a_Er_average_face,
                            CFG::LevelData<CFG::FArrayBox>& a_Er_average_cell,
                            CFG::LevelData<CFG::FArrayBox>& a_flux_divergence,
                            double&                         a_flux_divergence_lo,
                            double&                         a_flux_divergence_hi,
                            const double                    dt ) const
{
   CH_assert(a_Er_average_face.ghostVect() == CFG::IntVect::Unit);
   CH_assert(a_Er_average_cell.ghostVect() == CFG::IntVect::Unit);
   CH_assert(a_flux_divergence.ghostVect() == CFG::IntVect::Zero);

   //Geometry parameters
    const CFG::MagGeom& mag_geom( m_phase_geometry->magGeom() );
    const CFG::MagCoordSys& coords = *mag_geom.getCoordSys();
    
    //Create face-centered increment for the mapped E-field (- grad Phi )
    CFG::LevelData<CFG::FluxBox> Er_mapped_face(mag_geom.gridsFull(), 3, CFG::IntVect::Unit );
    
    //Creating tmp arrays with extra layers of ghost cells
    CFG::LevelData<CFG::FArrayBox> gkp_divergence_tmp( mag_geom.gridsFull(), 1, 2*CFG::IntVect::Unit );
    CFG::LevelData<CFG::FArrayBox> flux_divergence_tmp( mag_geom.gridsFull(), 1, 2*CFG::IntVect::Unit );
    
    setZero(gkp_divergence_tmp);
    setZero(flux_divergence_tmp);
    
    for (CFG::DataIterator dit(gkp_divergence_tmp.dataIterator()); dit.ok(); ++dit) {
        gkp_divergence_tmp[dit].copy(m_radial_gkp_divergence_average[dit]);
        flux_divergence_tmp[dit].copy(a_flux_divergence[dit]);
    }
    
    gkp_divergence_tmp.exchange();
    flux_divergence_tmp.exchange();
    
    for (CFG::DataIterator dit(a_Er_average_face.dataIterator()); dit.ok(); ++dit) {
        
        Er_mapped_face[dit].setVal(0.0);
        
        CFG::FArrayBox& this_gkp_divergence = gkp_divergence_tmp[dit];
        CFG::FArrayBox& this_flux_divergence = flux_divergence_tmp[dit];
        
        int block_number( coords.whichBlock( mag_geom.gridsFull()[dit] ) );
        const CFG::MagBlockCoordSys& block_coord_sys = (const CFG::MagBlockCoordSys&)(*(coords.getCoordSys(block_number)));
        int lo_radial_index = block_coord_sys.domain().domainBox().smallEnd(RADIAL_DIR);
        int hi_radial_index = block_coord_sys.domain().domainBox().bigEnd(RADIAL_DIR);
        
        //This is the main calculation of Er on closed flux surfaces (within the core).
        if ( block_number <= RCORE || block_number == MCORE ) {
            
            for (int dir = 0; dir < 2; dir++) {
                
                CFG::FArrayBox& this_Er_mapped_dir = Er_mapped_face[dit][dir];
                
                CFG::Box box( this_Er_mapped_dir.box() );
                CFG::BoxIterator bit(box);
                for (bit.begin(); bit.ok(); ++bit) {
                    
                    CFG::IntVect iv = bit();
                    
                    if (iv[0]<lo_radial_index) {
                        this_Er_mapped_dir(iv,0) =   dt * (-a_flux_divergence_lo / m_lo_radial_gkp_divergence_average);
                    }
                    else if (iv[0]>hi_radial_index) {
                        this_Er_mapped_dir(iv,0) =  dt * (-a_flux_divergence_hi / m_hi_radial_gkp_divergence_average);
                    }
                    else {
                        
                        CFG::IntVect iv_tmp = bit();
                        iv_tmp[1] = mag_geom.gridsFull()[dit].smallEnd(POLOIDAL_DIR);
                        
                        if ( dir == RADIAL_DIR ) {
                            this_Er_mapped_dir(iv,0) =   dt * ( - this_flux_divergence(iv_tmp,0) / this_gkp_divergence(iv_tmp,0) );
                        }
                        
                        if ( dir == POLOIDAL_DIR ) {
                            CFG::IntVect iv_incr = iv_tmp;
                            iv_incr[0] = iv_tmp[0] + 1;
                            if (iv[0]<hi_radial_index) {
                                this_Er_mapped_dir(iv,0) =   0.5 * dt * ( - this_flux_divergence(iv_tmp,0) / this_gkp_divergence(iv_tmp,0)
                                                                         - this_flux_divergence(iv_incr,0) / this_gkp_divergence(iv_incr,0) );
                            }
                            else  {
                                this_Er_mapped_dir(iv,0) =   0.5 * dt * ( - this_flux_divergence(iv_tmp,0) / this_gkp_divergence(iv_tmp,0)
                                                                         -a_flux_divergence_hi / m_hi_radial_gkp_divergence_average );
                            }
                            
                        }
                        
                    }
                }
            }
        }
        
        //Linear extrapolation of Er into the SOL blocks (here, assumed that the cut is vertical, i.e., dPsi/dz = dPsi/dr)
        //To provide continuity, the mapped field is scaled by |grad_Psi|, i.e., Emapped_sol = Emapped_core * gradPsi_core / gradPsi_sol
        //Fix later for 10 block and DIII-D
        else if (m_Esol_extrapolation && ((block_number < LPF) || (block_number == MCORE) || (block_number == MCSOL)))
        {
            
            const CFG::MagBlockCoordSys& lcore_coord_sys = (const CFG::MagBlockCoordSys&)(*(coords.getCoordSys(LCORE)));
            const CFG::MagBlockCoordSys& lsol_coord_sys = (const CFG::MagBlockCoordSys&)(*(coords.getCoordSys(LCSOL)));
            double lo_rad_end = block_coord_sys.lowerMappedCoordinate(0);
            double hi_rad_end = block_coord_sys.upperMappedCoordinate(0);
            
            CFG::RealVect topSep_core;
            topSep_core[0] = lcore_coord_sys.upperMappedCoordinate(0);
            topSep_core[1] = lcore_coord_sys.lowerMappedCoordinate(1);
            
            CFG::RealVect topSep_sol;
            topSep_sol[0] = lsol_coord_sys.lowerMappedCoordinate(0);
            topSep_sol[1] = lsol_coord_sys.lowerMappedCoordinate(1);
            
            Real dZdR_core = lcore_coord_sys.dXdXi(topSep_core,1,1)/lcore_coord_sys.dXdXi(topSep_core,0,1);
            Real dPsidZ_core = 1.0/(lcore_coord_sys.dXdXi(topSep_core,1,0) - lcore_coord_sys.dXdXi(topSep_core,0,0)*dZdR_core);
            
            Real dZdR_sol = lsol_coord_sys.dXdXi(topSep_sol,1,1)/lsol_coord_sys.dXdXi(topSep_sol,0,1);
            Real dPsidZ_sol = 1.0/(lsol_coord_sys.dXdXi(topSep_sol,1,0) - lsol_coord_sys.dXdXi(topSep_sol,0,0)*dZdR_sol);
            
            for (int dir = 0; dir < 2; dir++) {
                CFG::FArrayBox& this_Er_mapped_dir = Er_mapped_face[dit][dir];
                
                CFG::Box box( this_Er_mapped_dir.box() );
                CFG::FArrayBox xi(box,2);
                block_coord_sys.getFaceCenteredMappedCoords(dir, xi);
                
                CFG::BoxIterator bit(box);
                for (bit.begin(); bit.ok(); ++bit) {
                    CFG::IntVect iv = bit();
                    double amplitude = -dt * m_hi_radial_flux_divergence_average / m_hi_radial_gkp_divergence_average;
                    amplitude *= dPsidZ_core / dPsidZ_sol;
                    this_Er_mapped_dir(iv,0) =  amplitude  * (1.0 - (xi(iv,0) - lo_rad_end)/(hi_rad_end - lo_rad_end) );
                    
                }
            }
        }
        
        else {
            double E_open = 0.0;
            Er_mapped_face[dit].setVal(E_open,RADIAL_DIR,0,1);
            Er_mapped_face[dit].setVal(E_open,POLOIDAL_DIR,0,1);
        }
    }

    //Compute cell-centered increment for the mapped E-field
    CFG::LevelData<CFG::FArrayBox> Er_mapped_cell(mag_geom.gridsFull(), 3, CFG::IntVect::Unit );
    CFG::DataIterator dit( Er_mapped_cell.dataIterator() );
    for (dit.begin(); dit.ok(); ++dit) {
        CFG::Box box( Er_mapped_cell[dit].box() );
        CFG::BoxIterator bit(box);
        for (bit.begin(); bit.ok(); ++bit) {
            CFG::IntVect iv = bit();
            CFG::IntVect iv_r = bit();
            iv_r[0] = iv[0] + 1;
            Er_mapped_cell[dit](iv,0) = 0.5*(Er_mapped_face[dit][0](iv,0)+Er_mapped_face[dit][0](iv_r,0));
            Er_mapped_cell[dit](iv,1) = 0.5*(Er_mapped_face[dit][0](iv,1)+Er_mapped_face[dit][0](iv_r,1));
            Er_mapped_cell[dit](iv,2) = 0.5*(Er_mapped_face[dit][0](iv,2)+Er_mapped_face[dit][0](iv_r,2));
        }
    }
    
    //Multiply by NJtranspose (E_fs_average = - NJInverse * grad Phi)
    CFG::LevelData<CFG::FluxBox> Er_face_incr( mag_geom.gridsFull(), 3, CFG::IntVect::Unit );
    mag_geom.unmapGradient(Er_mapped_face, Er_face_incr);
    
    CFG::LevelData<CFG::FArrayBox> Er_cell_incr( mag_geom.gridsFull(), 3, CFG::IntVect::Unit );
    mag_geom.unmapGradient(Er_mapped_cell, Er_cell_incr);
    
    //Update E-field
    for (CFG::DataIterator dit(a_Er_average_face.dataIterator()); dit.ok(); ++dit) {
        if (dt>0) {
            a_Er_average_face[dit] += Er_face_incr[dit];
            a_Er_average_cell[dit] += Er_cell_incr[dit];
        }
        
        else {
            Real inv_dt = 1.0 / dt;
            a_Er_average_face[dit].copy(Er_face_incr[dit]);
            a_Er_average_face[dit] *= inv_dt;
            a_Er_average_cell[dit].copy(Er_cell_incr[dit]);
            a_Er_average_cell[dit].mult(inv_dt);
        }
    }
    
    //Improve Er field calculation to take into accout the dealigment between the grid and magnetic surfaces
    if ( (typeid(coords) == typeid(CFG::SingleNullCoordSys)) && (m_dealignment_corrections)) {
        
        mag_geom.interpolateErFromMagFS(a_Er_average_face, a_Er_average_cell);
        
        //If not doing extrapolation, zero out efield in the PF region,
        //which appeares in the present version of interpolateFromMagFS
        //as symmetric reflection of E in the core region
        if (!m_Esol_extrapolation)
        {
            for (CFG::DataIterator dit(a_Er_average_face.dataIterator()); dit.ok(); ++dit) {
                int block_number( coords.whichBlock( mag_geom.gridsFull()[dit] ) );
                if ((block_number == RPF) || (block_number == LPF)) {
                  a_Er_average_face[dit].setVal(0.0);
                  a_Er_average_cell[dit].setVal(0.0);
                }
            }
        }
    }
}

void
GKOps::updateEfieldPoloidalVariation( const CFG::LevelData<CFG::FluxBox>&   a_E_tilde_mapped_face,
                                      const CFG::LevelData<CFG::FArrayBox>& a_E_tilde_mapped_cell,
                                      CFG::LevelData<CFG::FluxBox>&         a_E_tilde_phys_face,
                                      CFG::LevelData<CFG::FArrayBox>&       a_E_tilde_phys_cell,
                                      CFG::LevelData<CFG::FArrayBox>&       a_phi_tilde_fs_average,
                                      double&                               a_phi_tilde_fs_average_lo,
                                      double&                               a_phi_tilde_fs_average_hi ) const
{
   CH_assert(a_E_tilde_phys_face.ghostVect() == CFG::IntVect::Unit);
   CH_assert(a_E_tilde_phys_cell.ghostVect() == CFG::IntVect::Unit);

   for (CFG::DataIterator dit(a_E_tilde_phys_face.dataIterator()); dit.ok(); ++dit) {
      a_E_tilde_phys_face[dit].setVal(0.0);
   }

   for (CFG::DataIterator dit(a_E_tilde_phys_cell.dataIterator()); dit.ok(); ++dit) {
      a_E_tilde_phys_cell[dit].setVal(0.0);
   }

   if ( !m_ampere_cold_electrons ) {
      CH_assert(a_E_tilde_mapped_face.ghostVect() == CFG::IntVect::Unit);
      CH_assert(a_E_tilde_mapped_cell.ghostVect() == CFG::IntVect::Unit);
      CH_assert(a_phi_tilde_fs_average.ghostVect() == CFG::IntVect::Zero);

      const CFG::MagGeom& mag_geom( m_phase_geometry->magGeom() );

      mag_geom.unmapGradient(a_E_tilde_mapped_face, a_E_tilde_phys_face);
      mag_geom.unmapGradient(a_E_tilde_mapped_cell, a_E_tilde_phys_cell);
      
      CFG::LevelData<CFG::FluxBox> Er_average_face_pol_contrib(mag_geom.gridsFull(), 3, CFG::IntVect::Unit);
      CFG::LevelData<CFG::FArrayBox> Er_average_cell_pol_contrib(mag_geom.gridsFull(), 3, CFG::IntVect::Unit);
 
      updateAveragedEfield( Er_average_face_pol_contrib, Er_average_cell_pol_contrib,
                            a_phi_tilde_fs_average, a_phi_tilde_fs_average_lo, a_phi_tilde_fs_average_hi, -1.0 );
       
      for (CFG::DataIterator dit(a_E_tilde_phys_face.dataIterator()); dit.ok(); ++dit) {
         a_E_tilde_phys_face[dit] -= Er_average_face_pol_contrib[dit];
         a_E_tilde_phys_cell[dit] -= Er_average_cell_pol_contrib[dit];
      }
   }
}

void
GKOps::setErAverage( const LevelData<FluxBox>& Er_face_injected )
{
   m_Er_average_face.define(m_phase_geometry->magGeom().gridsFull(), 3, CFG::IntVect::Unit);
   for (CFG::DataIterator dit(m_Er_average_face.disjointBoxLayout()); dit.ok(); ++dit) {
      m_Er_average_face[dit].setVal(0.);
   }
   m_phase_geometry->projectPhaseToConfiguration(Er_face_injected, m_Er_average_face);
}

void
GKOps::setErAverage( const LevelData<FArrayBox>& Er_cell_injected)
{
   m_Er_average_cell.define(m_phase_geometry->magGeom().gridsFull(), 3, CFG::IntVect::Unit);
   m_phase_geometry->projectPhaseToConfiguration(Er_cell_injected, m_Er_average_cell);
}

void
GKOps::setETilde( const LevelData<FluxBox>& E_tilde_face_injected )
{
   m_E_tilde_face.define(m_phase_geometry->magGeom().gridsFull(), 3, CFG::IntVect::Unit);
   for (CFG::DataIterator dit(m_E_tilde_face.disjointBoxLayout()); dit.ok(); ++dit) {
      m_E_tilde_face[dit].setVal(0.);
   }
   m_phase_geometry->projectPhaseToConfiguration(E_tilde_face_injected, m_E_tilde_face);
}

void
GKOps::setETilde( const LevelData<FArrayBox>& E_tilde_cell_injected)
{
   m_E_tilde_cell.define(m_phase_geometry->magGeom().gridsFull(), 3, CFG::IntVect::Unit);
   m_phase_geometry->projectPhaseToConfiguration(E_tilde_cell_injected, m_E_tilde_cell);
}

void GKOps::divideJ( const KineticSpeciesPtrVect& a_soln_mapped,
                     KineticSpeciesPtrVect&       a_soln_physical )
{
   CH_assert( isDefined() );
   for (int species(0); species<a_soln_physical.size(); species++) {
      const KineticSpecies& soln_species_mapped( *(a_soln_mapped[species]) );
      KineticSpecies& soln_species_physical( *(a_soln_physical[species]) );

      const LevelData<FArrayBox> & dfn_mapped = soln_species_mapped.distributionFunction();
      LevelData<FArrayBox> & dfn_physical = soln_species_physical.distributionFunction();

      DataIterator dit = dfn_physical.dataIterator();
      for (dit.begin(); dit.ok(); ++dit) {
         dfn_physical[dit].copy( dfn_mapped[dit] );
      }

      CH_assert( m_phase_geometry != NULL );
      m_phase_geometry->divideJonValid( dfn_physical );
   }
}


void GKOps::divideJ( const CFG::FluidSpeciesPtrVect& a_soln_mapped,
                     CFG::FluidSpeciesPtrVect&       a_soln_physical )
{
   CH_assert( isDefined() );
   for (int species(0); species<a_soln_physical.size(); species++) {
      const  CFG::FluidSpecies& soln_species_mapped( *(a_soln_mapped[species]) );
      CFG::FluidSpecies& soln_species_physical( *(a_soln_physical[species]) );
      
      const CFG::LevelData<CFG::FArrayBox>& dfn_mapped( soln_species_mapped.data() );
      CFG::LevelData<CFG::FArrayBox>& dfn_physical( soln_species_physical.data() );

      for (CFG::DataIterator dit( dfn_physical.dataIterator() ); dit.ok(); ++dit) {
         dfn_physical[dit].copy( dfn_mapped[dit] );
      }

      CH_assert( m_phase_geometry != NULL );
      const CFG::MagGeom& mag_geometry( m_phase_geometry->magGeom() );
      mag_geometry.divideJonValid( dfn_physical );
   }
}


void GKOps::divideJ( const CFG::FieldPtrVect& a_soln_mapped,
                     CFG::FieldPtrVect&       a_soln_physical )
{
   CH_assert( isDefined() );
   for (int species(0); species<a_soln_physical.size(); species++) {
      const  CFG::Field& soln_species_mapped( *(a_soln_mapped[species]) );
      CFG::Field& soln_species_physical( *(a_soln_physical[species]) );
      
      const CFG::LevelData<CFG::FArrayBox>& dfn_mapped( soln_species_mapped.data() );
      CFG::LevelData<CFG::FArrayBox>& dfn_physical( soln_species_physical.data() );

      for (CFG::DataIterator dit( dfn_physical.dataIterator() ); dit.ok(); ++dit) {
         dfn_physical[dit].copy( dfn_mapped[dit] );
      }

      CH_assert( m_phase_geometry != NULL );
      const CFG::MagGeom& mag_geometry( m_phase_geometry->magGeom() );
      mag_geometry.divideJonValid( dfn_physical );
   }
}


void GKOps::divideJ( const GKState& a_soln_mapped, GKState& a_soln_physical )
{
   CH_assert( isDefined() );
   divideJ( a_soln_mapped.dataKinetic(), a_soln_physical.dataKinetic() );
   divideJ( a_soln_mapped.dataFluid(), a_soln_physical.dataFluid() );
   divideJ( a_soln_mapped.dataField(), a_soln_physical.dataField() );
}


void GKOps::parseParameters( ParmParse& a_ppgksys )
{
   if (a_ppgksys.contains("fixed_efield")) {
      a_ppgksys.query( "fixed_efield", m_fixed_efield );
   }
   else {
      m_fixed_efield = false;
   }

   if (a_ppgksys.contains("vorticity_model")) {
      a_ppgksys.get("vorticity_model", m_vorticity_model);
   }
   else {
      m_vorticity_model = false;
   }
   
   if (a_ppgksys.contains("consistent_potential_bcs")) {
      a_ppgksys.query( "consistent_potential_bcs", m_consistent_potential_bcs );
   }
   else {
      m_consistent_potential_bcs = false;
   }

   if (a_ppgksys.contains("extrapolated_sol_efield")) {
     a_ppgksys.query( "extrapolated_sol_efield", m_Esol_extrapolation );
   }
   else {
     m_Esol_extrapolation = true;
   }

   if (a_ppgksys.contains("efield_dealignment_corrections")) {
     a_ppgksys.query( "efield_dealignment_corrections", m_dealignment_corrections );
   }
   else {
     m_dealignment_corrections = false;
   }

   if (a_ppgksys.contains("ampere_law")) {
      a_ppgksys.query( "ampere_law", m_ampere_law );

      if (m_ampere_law) {
         m_consistent_potential_bcs = true;

         if (a_ppgksys.contains("ampere_cold_electrons")) {
            a_ppgksys.query( "ampere_cold_electrons", m_ampere_cold_electrons );
         }
         else {
            m_ampere_cold_electrons = false;
         }
      }
      else {
         m_ampere_cold_electrons = false;
      }
   }
   else {
      m_ampere_law = false;
      m_ampere_cold_electrons = false;
   }

   if ( m_fixed_efield && m_ampere_law ) {
      MayDay::Error("GKOps::parseParameters(): Specify either fixed field or ampere law, but not both"); 
   }

   if (a_ppgksys.contains("transport_model_on")) {
      a_ppgksys.get("transport_model_on", m_transport_model_on);
   }
   else {
      m_transport_model_on = false;
   }

   if (a_ppgksys.contains("neutrals_model_on")) {
      a_ppgksys.get("neutrals_model_on", m_neutrals_model_on);
   }
   else {
      m_neutrals_model_on = false;
   }

   bool using_boltzmann_electrons(true);
   m_boltzmann_electron = NULL;
   ParmParse pp_be( "boltzmann_electron" );
   
   string name;
   if (using_boltzmann_electrons && pp_be.contains("name")) {
      pp_be.get( "name", name );
      using_boltzmann_electrons = (name=="electron");
   }
   else using_boltzmann_electrons = false;
   
   double mass;
   if (using_boltzmann_electrons && pp_be.contains("mass")) {
      pp_be.get( "mass", mass );
   }
   else using_boltzmann_electrons = false;
   
   double charge;
   if (using_boltzmann_electrons && pp_be.contains("charge")) {
      pp_be.get( "charge", charge );
   }
   else using_boltzmann_electrons = false;
   
   double temperature;
   if (using_boltzmann_electrons && pp_be.contains("temperature")) {
      pp_be.get( "temperature", temperature );
   }
   else using_boltzmann_electrons = false;
   
   if (using_boltzmann_electrons) {
      CH_assert( m_phase_geometry != NULL );
      const CFG::MagGeom& mag_geom( m_phase_geometry->magGeom() );
      CFG::LevelData<CFG::FArrayBox> temp( mag_geom.gridsFull(),
                                           1,
                                           CFG::IntVect::Unit );
      for (CFG::DataIterator dit( temp.dataIterator() ); dit.ok(); ++dit) {
         temp[dit].setVal( temperature );
      }
      
      m_boltzmann_electron = new CFG::BoltzmannElectron( mass,
                                                         charge,
                                                         mag_geom,
                                                         temp );
   }

   a_ppgksys.query( "gksystem.enforce_quasineutrality", m_enforce_quasineutrality );
}


void GKOps::createGKPoisson( const CFG::LevelData<CFG::FArrayBox>& a_initial_ion_charge_density )
{
   if (m_poisson) {
      MayDay::Error( "GKPoisson has already been created" );
   }

   const double larmor( m_units->larmorNumber() );
   const double debye( m_units->debyeNumber() );
   CH_assert( m_phase_geometry != NULL );
   const CFG::MagGeom& mag_geom( m_phase_geometry->magGeom() );
   if ( m_boltzmann_electron ) {
      ParmParse pp( CFG::GKPoissonBoltzmann::pp_name );
      if ( typeid(*(mag_geom.getCoordSys())) == typeid(CFG::SingleNullCoordSys) ) {
         m_poisson = new CFG::NewGKPoissonBoltzmann( pp, mag_geom, larmor, debye, a_initial_ion_charge_density );
      }
      else {
         m_poisson = new CFG::GKPoissonBoltzmann( pp, mag_geom, larmor, debye, a_initial_ion_charge_density );
      }
   }
   else {
      ParmParse pp( CFG::GKPoisson::pp_name );
      m_poisson = new CFG::GKPoisson( pp, mag_geom, larmor, debye );
   }
}


void GKOps::plotPotential( const std::string& a_filename,
                           const double&      a_time ) const
{
   CH_assert( isDefined() );
   CH_assert( m_phase_geometry != NULL );
   m_phase_geometry->plotConfigurationData( a_filename.c_str(), m_phi, a_time );
}

void GKOps::plotEField( const std::string& a_filename,
                        const double&      a_time ) const
{
   CH_assert( isDefined() );
   CH_assert( m_phase_geometry != NULL );
   const PhaseGeom& phase_geometry( *m_phase_geometry );

   if (m_poisson) { // Plot the psi and theta projections of the physical field

      const CFG::MagGeom& mag_geom( phase_geometry.magGeom() );
      CFG::LevelData<CFG::FluxBox> Efield( mag_geom.gridsFull(), 2, CFG::IntVect::Unit);

#if 1
      m_poisson->computePoloidalField( m_phi, Efield );
#else
      ((CFG::NewGKPoissonBoltzmann*)m_poisson)->getFaceCenteredFieldOnCore( m_phi, Efield );
#endif
      
      const CFG::DisjointBoxLayout& grids = mag_geom.gridsFull();
      for (CFG::DataIterator dit(Efield.dataIterator()); dit.ok(); ++dit) {
        CFG::FluxBox& this_Efield = Efield[dit];
        mag_geom.getBlockCoordSys(grids[dit]).computePsiThetaProjections(this_Efield);
      }

      if ( m_ampere_law ) {
         phase_geometry.plotConfigurationData( a_filename.c_str(), m_E_field_cell, a_time );
      }
      else {
         phase_geometry.plotConfigurationData( a_filename.c_str(), Efield, a_time );
      }
   }
   else { // Plot the unmapped field
      phase_geometry.plotConfigurationData( a_filename.c_str(), m_E_field_cell, a_time );
   }
}


void GKOps::plotDistributionFunction( const std::string&    a_filename,
                                      const KineticSpecies& a_soln_species,
                                      const double&         a_time ) const
{
   const PhaseGeom& species_geometry = a_soln_species.phaseSpaceGeometry();

   const LevelData<FArrayBox>& soln_dfn = a_soln_species.distributionFunction();
   LevelData<FArrayBox> dfn( species_geometry.gridsFull(), 1, IntVect::Zero );
   for (DataIterator dit(dfn.dataIterator()); dit.ok(); ++dit) {
      dfn[dit].copy(soln_dfn[dit]);
   }

   species_geometry.divideBStarParallel( dfn );
   species_geometry.plotData( a_filename.c_str(), dfn, a_time );
}

void GKOps::plotFluids( const std::string&       a_filename,
                        const CFG::FluidSpecies& a_fluid_species,
                        const double&            a_time ) const
{
   m_phase_geometry->plotConfigurationData( a_filename.c_str(), a_fluid_species.data(), a_time );
}

void GKOps::plotFields(const std::string&       a_filename,
                       const CFG::Field&        a_field_comp,
                       const double&            a_time ) const
{
   m_phase_geometry->plotConfigurationData( a_filename.c_str(), a_field_comp.data(), a_time );
}


void GKOps::plotBStarParallel( const std::string&    a_filename,
                               const KineticSpecies& a_soln_species,
                               const double&         a_time ) const
{
   const PhaseGeom& species_geometry = a_soln_species.phaseSpaceGeometry();

   LevelData<FArrayBox> BStarParallel( species_geometry.gridsFull(), 1, IntVect::Zero );
   species_geometry.getBStarParallel(BStarParallel);

   species_geometry.plotData( a_filename.c_str(), BStarParallel, a_time );
}

void GKOps::plotDeltaF( const std::string&    a_filename,
                        const KineticSpecies& a_soln_species,
                        const double&         a_time ) const
{

   CH_assert( isDefined() );
   CH_assert( m_phase_geometry != NULL );
   const PhaseGeom& phase_geometry( *m_phase_geometry );
   const CFG::MagGeom& mag_geom( phase_geometry.magGeom() );

   CFG::LevelData<CFG::FArrayBox> density( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.numberDensity( density );

   CFG::LevelData<CFG::FArrayBox> ParallelMom( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.ParallelMomentum( ParallelMom );

   for (CFG::DataIterator dit(density.dataIterator()); dit.ok(); ++dit) {
      ParallelMom[dit].divide(density[dit]);
   }
   CFG::LevelData<CFG::FArrayBox> pressure( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.pressureMoment(pressure, ParallelMom);

   for (CFG::DataIterator dit(density.dataIterator()); dit.ok(); ++dit) {
      pressure[dit].divide(density[dit]);
   }

   const PhaseGeom& species_geometry = a_soln_species.phaseSpaceGeometry();
   LevelData<FArrayBox> deltaF( species_geometry.gridsFull(), 1, IntVect::Zero );
   DeltaFKernel DeltaF_Kernel(density, pressure, ParallelMom);
   DeltaF_Kernel.eval(deltaF, a_soln_species);

   species_geometry.plotData( a_filename.c_str(), deltaF, a_time );

}


void GKOps::plotDistributionFunctionAtMu( const std::string&    a_filename,
                                          const KineticSpecies& a_soln_species,
                                          const int             a_mu,
                                          const double&         a_time ) const
{
   const PhaseGeom& species_geometry = a_soln_species.phaseSpaceGeometry();

   const LevelData<FArrayBox>& soln_dfn = a_soln_species.distributionFunction();
   LevelData<FArrayBox> dfn( species_geometry.gridsFull(), 1, IntVect::Zero );
   for (DataIterator dit(dfn.dataIterator()); dit.ok(); ++dit) {
      dfn[dit].copy(soln_dfn[dit]);
   }

   species_geometry.divideBStarParallel( dfn );
   species_geometry.plotAtMuIndex( a_filename.c_str(), a_mu, soln_dfn, a_time );
}


void GKOps::plotVParallelTheta( const std::string&    a_filename,
                                const KineticSpecies& a_soln_species,
                                const int             a_radial_index,
                                const int             a_toroidal_index,
                                const int             a_mu_index,
                                const double&         a_time ) const
{
   const PhaseGeom& species_geometry = a_soln_species.phaseSpaceGeometry();

   const LevelData<FArrayBox>& soln_dfn = a_soln_species.distributionFunction();
   LevelData<FArrayBox> dfn( species_geometry.gridsFull(), 1, IntVect::Zero );
   for (DataIterator dit(dfn.dataIterator()); dit.ok(); ++dit) {
      dfn[dit].copy(soln_dfn[dit]);
   }

   species_geometry.divideBStarParallel( dfn );
   species_geometry.plotVParPoloidalData( a_filename.c_str(), a_radial_index,
                                          a_toroidal_index, a_mu_index, dfn, a_time );
}


void GKOps::plotRTheta( const std::string&    a_filename,
                        const KineticSpecies& a_soln_species,
                        const int             a_vpar_index,
                        const int             a_mu_index,
                        const double&         a_time ) const
{
   const PhaseGeom& species_geometry = a_soln_species.phaseSpaceGeometry();

   const LevelData<FArrayBox>& soln_dfn = a_soln_species.distributionFunction();
   LevelData<FArrayBox> dfn( species_geometry.gridsFull(), 1, IntVect::Zero );
   for (DataIterator dit(dfn.dataIterator()); dit.ok(); ++dit) {
      dfn[dit].copy(soln_dfn[dit]);
   }

   VEL::IntVect vspace_index(a_vpar_index, a_mu_index);

   species_geometry.divideBStarParallel( dfn );
   species_geometry.plotAtVelocityIndex( a_filename.c_str(), vspace_index, dfn, a_time );
}



void GKOps::plotVParallelMu( const std::string&    a_filename,
                             const KineticSpecies& a_soln_species,
                             const int             a_radial_index,
                             const int             a_poloidal_index,
                             const double&         a_time ) const
{
   const PhaseGeom& species_geometry = a_soln_species.phaseSpaceGeometry();

   const LevelData<FArrayBox>& soln_dfn = a_soln_species.distributionFunction();
   LevelData<FArrayBox> dfn( species_geometry.gridsFull(), 1, IntVect::Zero );
   for (DataIterator dit(dfn.dataIterator()); dit.ok(); ++dit) {
      dfn[dit].copy(soln_dfn[dit]);
   }

   CFG::IntVect cspace_index(a_radial_index, a_poloidal_index);
   species_geometry.divideBStarParallel( dfn );
   species_geometry.plotAtConfigurationIndex( a_filename.c_str(),
                                              cspace_index, dfn, a_time );
}


void GKOps::plotChargeDensity( const std::string&    a_filename,
                              const KineticSpecies& a_soln_species,
                              const double&         a_time ) const
{
   CH_assert( isDefined() );
   CH_assert( m_phase_geometry != NULL );
   const PhaseGeom& phase_geometry( *m_phase_geometry );
   
   const CFG::MagGeom& mag_geom( phase_geometry.magGeom() );
   CFG::LevelData<CFG::FArrayBox> charge_density( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.chargeDensity( charge_density );
   
   phase_geometry.plotConfigurationData( a_filename.c_str(), charge_density, a_time );
}


void GKOps::plotChargeDensity( const std::string&     a_filename,
                               const KineticSpeciesPtrVect& a_species,
                               const double&          a_time ) const
{
   CH_assert( isDefined() );
   CH_assert( m_phase_geometry != NULL );
   const PhaseGeom& phase_geometry( *m_phase_geometry );
   const CFG::MagGeom& mag_geom( phase_geometry.magGeom() );
   CFG::LevelData<CFG::FArrayBox> charge_density( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   CFG::LevelData<CFG::FArrayBox> species_charge_density( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );

   for (CFG::DataIterator dit(charge_density.dataIterator()); dit.ok(); ++dit) {
      charge_density[dit].setVal(0.);
   }

   for (int species(0); species<a_species.size(); species++) {
      KineticSpecies& soln_species( *(a_species[species]) );
      soln_species.chargeDensity(species_charge_density);

      for (CFG::DataIterator dit(charge_density.dataIterator()); dit.ok(); ++dit) {
         charge_density[dit] += species_charge_density[dit];
      }
   }

   if (m_boltzmann_electron) {
      const CFG::LevelData<CFG::FArrayBox>& ne = m_boltzmann_electron->numberDensity();

      for (CFG::DataIterator dit(charge_density.dataIterator()); dit.ok(); ++dit) {
         charge_density[dit] -= ne[dit];
      }
   }

   phase_geometry.plotConfigurationData( a_filename.c_str(), charge_density, a_time );
}


void GKOps::plotParallelMomentum( const std::string&    a_filename,
                                  const KineticSpecies& a_soln_species,
                                  const double&         a_time ) const
{
   CH_assert( isDefined() );
   CH_assert( m_phase_geometry != NULL );
   const PhaseGeom& phase_geometry( *m_phase_geometry );
   const CFG::MagGeom& mag_geom( phase_geometry.magGeom() );
   CFG::LevelData<CFG::FArrayBox> parallel_mom( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.ParallelMomentum(parallel_mom);
   phase_geometry.plotConfigurationData( a_filename.c_str(), parallel_mom, a_time );
}


void GKOps::plotPoloidalMomentum( const std::string&    a_filename,
                                  const KineticSpecies& a_soln_species,
                                  const double&         a_time ) const
{
   CH_assert( isDefined() );
   CH_assert( m_phase_geometry != NULL );
   const PhaseGeom& phase_geometry( *m_phase_geometry );
   const double larmor_number( m_units->larmorNumber() );
   LevelData<FluxBox> E_field_tmp;
   phase_geometry.injectConfigurationToPhase( m_E_field_face,
                                              m_E_field_cell,
                                              E_field_tmp );
   const CFG::MagGeom& mag_geom( phase_geometry.magGeom() );

   CFG::LevelData<CFG::FArrayBox> poloidal_mom( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.PoloidalMomentum( poloidal_mom, E_field_tmp, larmor_number );

   phase_geometry.plotConfigurationData( a_filename.c_str(), poloidal_mom, a_time );
}


void GKOps::plotPressure( const std::string&    a_filename,
                          const KineticSpecies& a_soln_species,
                          const double&         a_time ) const
{
   CH_assert( isDefined() );
   CH_assert( m_phase_geometry != NULL );
   const PhaseGeom& phase_geometry( *m_phase_geometry );
   const CFG::MagGeom& mag_geom( phase_geometry.magGeom() );

   CFG::LevelData<CFG::FArrayBox> density( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   CFG::LevelData<CFG::FArrayBox> ParallelMom( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.numberDensity( density );
   a_soln_species.ParallelMomentum( ParallelMom );

   for (CFG::DataIterator dit(density.dataIterator()); dit.ok(); ++dit) {
      ParallelMom[dit].divide(density[dit]);
   }
   CFG::LevelData<CFG::FArrayBox> pressure( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.pressureMoment(pressure, ParallelMom);

   phase_geometry.plotConfigurationData( a_filename.c_str(), pressure, a_time );
}

void GKOps::plotParallelHeatFlux( const std::string&    a_filename,
                                  const KineticSpecies& a_soln_species,
                                  const double&         a_time ) const
{
   CH_assert( isDefined() );
   CH_assert( m_phase_geometry != NULL );
   const PhaseGeom& phase_geometry( *m_phase_geometry );
   const CFG::MagGeom& mag_geom( phase_geometry.magGeom() );
   
   CFG::LevelData<CFG::FArrayBox> density( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   CFG::LevelData<CFG::FArrayBox> ParallelMom( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.numberDensity( density );
   a_soln_species.ParallelMomentum( ParallelMom );
   
   for (CFG::DataIterator dit(density.dataIterator()); dit.ok(); ++dit) {
      ParallelMom[dit].divide(density[dit]);
   }
   CFG::LevelData<CFG::FArrayBox> parallelHeatFlux( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.parallelHeatFluxMoment( parallelHeatFlux, ParallelMom );
   
   phase_geometry.plotConfigurationData( a_filename.c_str(), parallelHeatFlux, a_time );
}


void GKOps::plotTemperature( const std::string&    a_filename,
                             const KineticSpecies& a_soln_species,
                             const double&         a_time ) const
{
   CH_assert( isDefined() );
   CH_assert( m_phase_geometry != NULL );
   const PhaseGeom& phase_geometry( *m_phase_geometry );
   const CFG::MagGeom& mag_geom( phase_geometry.magGeom() );
   CFG::LevelData<CFG::FArrayBox> density( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.numberDensity( density );

   CFG::LevelData<CFG::FArrayBox> ParallelMom( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.ParallelMomentum( ParallelMom );
   for (CFG::DataIterator dit(density.dataIterator()); dit.ok(); ++dit) {
      ParallelMom[dit].divide(density[dit]);
   }

   CFG::LevelData<CFG::FArrayBox> pressure( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.pressureMoment(pressure, ParallelMom);

   CFG::LevelData<CFG::FArrayBox> temperature;
   temperature.define( pressure );

   for (CFG::DataIterator dit(temperature.dataIterator()); dit.ok(); ++dit) {
      temperature[dit].divide(density[dit]);
   }

   phase_geometry.plotConfigurationData( a_filename.c_str(), temperature, a_time );
}


void GKOps::plotFourthMoment( const std::string&    a_filename,
                              const KineticSpecies& a_soln_species,
                              const double&         a_time ) const
{
   CH_assert( isDefined() );
   CH_assert( m_phase_geometry != NULL );
   const PhaseGeom& phase_geometry( *m_phase_geometry );
   const CFG::MagGeom& mag_geom( phase_geometry.magGeom() );
   CFG::LevelData<CFG::FArrayBox> density( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.numberDensity( density );

   CFG::LevelData<CFG::FArrayBox> ParallelMom( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.ParallelMomentum( ParallelMom );
   for (CFG::DataIterator dit(density.dataIterator()); dit.ok(); ++dit) {
      ParallelMom[dit].divide(density[dit]);
   }

   CFG::LevelData<CFG::FArrayBox> pressure( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.pressureMoment(pressure, ParallelMom);

   CFG::LevelData<CFG::FArrayBox> fourthMoment( mag_geom.gridsFull(), 1, CFG::IntVect::Zero);
   a_soln_species.fourthMoment( fourthMoment );

   CFG::LevelData<CFG::FArrayBox> temp;
   temp.define(pressure);

   for (CFG::DataIterator dit(fourthMoment.dataIterator()); dit.ok(); ++dit) {
      fourthMoment[dit].divide(temp[dit]);       // fourthMom/Pressure
      temp[dit].divide(density[dit]);            // Pressure/Density
      fourthMoment[dit].divide(temp[dit]);       // fourthMom/(N*T^2)
      fourthMoment[dit].mult(4.0/15.0);          // should be unity for Maxwellian!!!
   }

   phase_geometry.plotConfigurationData(a_filename.c_str(), fourthMoment, a_time);
}


void GKOps::plotParticleFlux( const std::string&    a_filename,
                              const KineticSpecies& a_soln_species,
                              const double&         a_time ) const
{
   CH_assert( isDefined() );
   CH_assert( m_phase_geometry != NULL );
   const PhaseGeom& phase_geometry( *m_phase_geometry );
   LevelData<FluxBox> E_field_tmp;
   phase_geometry.injectConfigurationToPhase( m_E_field_face,
                                              m_E_field_cell,
                                              E_field_tmp );
   const CFG::MagGeom& mag_geom( phase_geometry.magGeom() );
   
   CFG::LevelData<CFG::FArrayBox> particle_flux( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.ParticleFlux( particle_flux, E_field_tmp );

   phase_geometry.plotConfigurationData( a_filename.c_str(), particle_flux, a_time );
}


void GKOps::plotHeatFlux( const std::string&    a_filename,
                          const KineticSpecies& a_soln_species,
                          const double&         a_time ) const
{
   CH_assert( isDefined() );
   CH_assert( m_phase_geometry != NULL );
   const PhaseGeom& phase_geometry( *m_phase_geometry );
   LevelData<FluxBox> E_field_tmp;
   LevelData<FArrayBox> phi_injected_tmp;
   phase_geometry.injectConfigurationToPhase( m_E_field_face,
                                              m_E_field_cell,
                                              E_field_tmp );
   phase_geometry.injectConfigurationToPhase( m_phi, phi_injected_tmp );
   const CFG::MagGeom& mag_geom( phase_geometry.magGeom() );

   CFG::LevelData<CFG::FArrayBox> heat_flux( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );
   a_soln_species.HeatFlux( heat_flux, E_field_tmp, phi_injected_tmp );

   phase_geometry.plotConfigurationData( a_filename.c_str(), heat_flux, a_time );
}


void GKOps::plotAmpereErIncrement( const std::string&    a_filename,
				   const KineticSpecies& a_soln_species,
				   const double&         a_time ) const
{
  CH_assert( isDefined() );
  CH_assert( m_phase_geometry != NULL );
  const PhaseGeom& phase_geometry( *m_phase_geometry );
  const CFG::MagGeom& mag_geom( phase_geometry.magGeom() );
   
  CFG::LevelData<CFG::FArrayBox> AmpereErIncr( mag_geom.gridsFull(), 1, CFG::IntVect::Zero );

  CFG::LevelData<CFG::FArrayBox> flux_div_grown( mag_geom.gridsFull(), 1, CFG::IntVect::Unit );
  CFG::LevelData<CFG::FArrayBox> gkp_div_grown( mag_geom.gridsFull(), 1, CFG::IntVect::Unit );
  for (CFG::DataIterator dit(AmpereErIncr.dataIterator()); dit.ok(); ++dit) {
    flux_div_grown[dit].copy(m_radial_flux_divergence_average[dit]);
    gkp_div_grown[dit].copy(m_radial_gkp_divergence_average[dit]);
  }
  flux_div_grown.exchange();
  gkp_div_grown.exchange();
  
  const CFG::MagCoordSys& coords = *mag_geom.getCoordSys();
  const CFG::MagBlockCoordSys& block0_coord_sys = (const CFG::MagBlockCoordSys&)(*(coords.getCoordSys(0)));
  int hi_radial_index = block0_coord_sys.domain().domainBox().bigEnd(RADIAL_DIR);

  for (CFG::DataIterator dit(AmpereErIncr.dataIterator()); dit.ok(); ++dit) {
    int block_number( coords.whichBlock( mag_geom.gridsFull()[dit] ) );
        
    if ( block_number < 2 ) {
            
      CFG::FArrayBox& this_AmpereErIncr = AmpereErIncr[dit];
      CFG::FArrayBox& this_flux_div_grown = flux_div_grown[dit];
      CFG::FArrayBox& this_gkp_div_grown = gkp_div_grown[dit];
                
      CFG::Box box( this_AmpereErIncr.box() );
      CFG::BoxIterator bit(box);
      for (bit.begin(); bit.ok(); ++bit) {
                    
	CFG::IntVect iv = bit();
	CFG::IntVect iv_shift = bit();
	iv_shift[0] = iv[0]+1;
	this_AmpereErIncr(iv,0) = this_flux_div_grown(iv_shift,0)/this_gkp_div_grown(iv_shift,0);
	if (iv[0]==hi_radial_index) {
	  this_AmpereErIncr(iv,0) =   m_hi_radial_flux_divergence_average/m_hi_radial_gkp_divergence_average;
	}
      }
    }
    else {
      AmpereErIncr[dit].setVal(0.0);
    }
  }

  phase_geometry.plotConfigurationData( a_filename.c_str(), AmpereErIncr, a_time );
  //if (procID()==0) cout<<"flux_hi="<<m_hi_radial_flux_divergence_average<<endl;
}


void GKOps::setupFieldHistories( ParmParse& a_ppsim )
{
   CH_assert( isDefined() );
   //query whether we should write history files
   a_ppsim.query( "histories", m_history );

   if (m_history) {
#ifdef CH_USE_HDF5
      //query how frequently to save histories
      a_ppsim.query( "history_frequency", m_hist_freq );
      //query for indices to generate the history.   If out of bounds, will result in no history.
      std::vector<int> read_hist_indices( CFG_DIM );
      // Set up default to be in middle of array
      bool finding_indices(true);
      // look for index/field pairs numbered sequentially 1, 2, 3, ...
      // e.g., "simulation.1.history_indices"
      // with any break in the enumeration terminating the search
      while (finding_indices) {
         int count( m_hist_count + 1 );
         stringstream sind;
         sind << count << ".history_indices" << ends; // grid indices
         
         stringstream sfield;
         sfield << count << ".history_field" << ends; // e.g., "potential"

         if ( a_ppsim.contains( sind.str().c_str() )   &&
              a_ppsim.contains( sfield.str().c_str() ) ){
            a_ppsim.getarr( sind.str().c_str(), read_hist_indices, 0, CFG_DIM );
            for (int d(0); d<CFG_DIM; d++) {
               m_hist_indices[d] = read_hist_indices[d];
            }

            // query to see what field's history to accumulate
            a_ppsim.get( sfield.str().c_str(), m_hist_fieldname );
            if (m_hist_fieldname == "potential") {
               FieldHist *save_hist = new FieldHist; //create structure
               save_hist->hist_index = m_hist_count;
               save_hist->grid_indices = m_hist_indices; // grid indices
               save_hist->fieldvals = new Vector<Real>( m_expand_incr ); // allocate memory for vectors
               save_hist->timevals = new Vector<Real>( m_expand_incr );
               save_hist->timestep = new Vector<int>( m_expand_incr );
               save_hist->valsize = m_expand_incr; // length of vectors above
               save_hist->cur_entry = 0; // incremented for each entry
               save_hist->fieldname = m_hist_fieldname;
               m_fieldHistLists.push_back(*save_hist); //save structure in array
               m_hist_count++; // count of watchpoint histories in deck
            }
            else if (m_hist_fieldname == "Efield") {
               FieldHist *save_hist = new FieldHist; //create structure
               save_hist->hist_index = m_hist_count;
               save_hist->grid_indices = m_hist_indices; // grid indices
               save_hist->fieldvals = new Vector<Real>( m_expand_incr ); // allocate memory for vectors
               save_hist->timevals = new Vector<Real>( m_expand_incr );
               save_hist->timestep = new Vector<int>( m_expand_incr );
               save_hist->valsize = m_expand_incr; // length of vectors above
               save_hist->cur_entry = 0; // incremented for each entry
               save_hist->fieldname = m_hist_fieldname;
               m_fieldHistLists.push_back(*save_hist); //save structure in array
               m_hist_count++; // count of watchpoint histories in deck
            } else {
               MayDay::Error("Unimplemented field name");
            }
         }
         else {
            if (m_hist_count == 0) {
               MayDay::Error( "If histories are requested, history_field and history_indices must also be specified" );
            } else {
               finding_indices = false;
            }
         }
      }
#else
   MayDay::Error( "histories only defined with hdf5" );
#endif
   }
}


void GKOps::writeFieldHistory( const int a_cur_step,
                               const Real a_cur_time,
                               const bool a_startup_flag )
{
   CH_assert( isDefined() );
   if (a_cur_step % m_hist_freq != 0 && !a_startup_flag) {
      return;
   }
   
   for (int ihist(0); ihist<m_hist_count; ihist++) {
      FieldHist *field_hist_ptr = &m_fieldHistLists[ihist];
      std::string hist_field_name = field_hist_ptr->fieldname;
      CFG:: IntVect inode_hist = field_hist_ptr->grid_indices;
      
      std::string fname;
      std::string fname_suf;
      std::string fname_step;
      CFG::LevelData<CFG::FArrayBox>* field = NULL;

      if (hist_field_name == "potential") {
         field = &m_phi;
         fname = "potential_hist";
         stringstream fname_temp;
         fname_temp << "potential_hist_" << ihist + 1 << ".curve" << ends;
         fname_suf = fname_temp.str();
         stringstream fname_step_temp;
         fname_step_temp << "potential_hist_" << ihist + 1 << ".txt" << ends;
         fname_step = fname_step_temp.str();
      }
      else if (hist_field_name == "Efield") {
         field = &m_E_field_cell;
         fname = "Efield_hist";
         stringstream fname_temp;
         fname_temp << "Efield_hist_" << ihist + 1 << ".curve" << ends;
         fname_suf = fname_temp.str();
         stringstream fname_step_temp;
         fname_step_temp << "Efield_hist_" << ihist + 1 << ".txt" << ends;
         fname_step = fname_step_temp.str();
      }
      else{
         MayDay::Error( "Unimplemented field name" );
      }
      
      // Writes value of a spatial field at a specified point inode_hist to a text file
      // with name fname, adding to what is there
      Real field_val(0.0);
      for (CFG::DataIterator dit( field->dataIterator() ); dit.ok(); ++dit) {
         
        // Extract local fortran array for the field on this patch
        // This differs from syntax in many other parts of code in that a_field is now
        // a pointer, so we need to de-reference it.
         const CFG::FArrayBox& field_on_patch( (*field)[dit] );

         // now loop over nodes of the box and if the node's global indices match
         //   the specified global indices, print.  This will only happen on one
         //   patch, unless it happens in a ghost cell -- accept that duplication
         //   for now.
         CFG::Box patchBox( (field->disjointBoxLayout())[dit] );

         bool found(true);
         for (int i(0); i<CFG_DIM; i++) {
            if ((inode_hist[i]<patchBox.smallEnd(i)) ||
                (inode_hist[i]>patchBox.bigEnd(i))   ){
               found = false;
            }
         }
         if (found) {
            field_val += field_on_patch( inode_hist, 0 );
         }
      }
      
      Real field_val_sum(0.0);
#ifdef CH_MPI
      MPI_Allreduce( &field_val, &field_val_sum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD );
#else
      field_val_sum = field_val;
#endif

      const int cur_index( field_hist_ptr->cur_entry );
      if (cur_index >= field_hist_ptr->valsize) {
         const int length( cur_index + m_expand_incr );
         (*field_hist_ptr->fieldvals).resize( length );
         (*field_hist_ptr->timevals).resize( length );
         (*field_hist_ptr->timestep).resize( length );
         field_hist_ptr->valsize = length;
      }
      (*field_hist_ptr->fieldvals)[cur_index] = field_val_sum;
      (*field_hist_ptr->timevals)[cur_index]  = a_cur_time;
      (*field_hist_ptr->timestep)[cur_index]  = a_cur_step;
      field_hist_ptr->cur_entry += 1;
      
#ifdef CH_MPI
      if (procID()==0) { // write out a file for each watchpoint
#endif
         //overwrite any existing file; this one is for Visit (with no column for the step)
         ofstream fieldout( fname_suf.c_str(), ios::out ); 
         fieldout << "# " << inode_hist[0] << ", " << inode_hist[1] << endl;

         //overwrite any existing file; this one is for human viewing (1st column for the step)
         ofstream fieldout_step( fname_step.c_str(), ios::out ); 
         fieldout_step << "# step  time  potential at ix,iy = " << inode_hist[0] << ", " << inode_hist[1] <<  endl;

         for (int j(0); j<field_hist_ptr->cur_entry; j++) {
            fieldout << (*field_hist_ptr->timevals)[j] << " "
                     << (*field_hist_ptr->fieldvals)[j] << endl;
            fieldout_step << (*field_hist_ptr->timestep)[j] << " "
                          << (*field_hist_ptr->timevals)[j] << " "
                          << (*field_hist_ptr->fieldvals)[j] << endl;
         }
         fieldout.close();
         fieldout_step.close();
#ifdef CH_MPI
      }
#endif
   }
}


void GKOps::plotVlasovDivergence( const std::string&    a_filename,
                                  const KineticSpecies& a_soln_species,
                                  const double&         a_time)
{
   CH_assert( isDefined() );
#if 0
   // This is quite broken
   
   // Make a temporary species vector for the output
   KineticSpeciesPtrVect tmp_species_vect(1);
   tmp_species_vect[0] = a_soln_species.clone( IntVect::Zero, false );

   LevelData<FluxBox> E_field;
   computeElectricField( E_field, tmp_species_vect, 0, 4 );

  // Make another temporary species with ghost cells, divide by J,
  // and apply Vlasov operator
  KineticSpeciesPtrVect species_phys;
  createTemporarySpeciesVector( species_phys, tmp_species_vect );
  fillGhostCells( species_phys, E_field, 0. );
  applyVlasovOperator( tmp_species_vect, species_phys, E_field, 0. );

    KineticSpecies& this_species( *(tmp_species_vect[0]) );

    // Get the velocity index at (v_parallel_max, 0)
    const PhaseGeom& geom = this_species.phaseSpaceGeometry();
    const Box& domainBox = geom.domain().domainBox();
    VEL::IntVect vpt(domainBox.bigEnd(VPARALLEL_DIR),0);

#if 1
    // Construct filename
    string filename("vlasov_divergence." + this_species.name());

    geom.plotAtVelocityIndex( filename.c_str(), vpt, this_species.distributionFunction() a_time );

#else

    // Slice in the mu direction at the low mu coordinate
    SliceSpec slice_mu(MU_DIR,domainBox.smallEnd(MU_DIR));

    CP1::Box sliced_box;
    sliceBox(sliced_box, domainBox, slice_mu);

    CP1::LevelData<D3::FArrayBox> sliced_divergence;
    sliceLevelData(sliced_divergence, this_species.distributionFunction(), slice_mu);

    Vector<string> names;
    names.push_back("velocity_divergence");
    Vector<CP1::DisjointBoxLayout> layouts;
    layouts.push_back(sliced_divergence.disjointBoxLayout());

    Vector<CP1::LevelData<CP1::FArrayBox>* > data;
    data.push_back(&sliced_divergence);

    double dx = 1.;
    double dt = 1.;
    Vector<int> ratio;
    for (int dir=0; dir<CFG_DIM+1; ++dir) {
      ratio.push_back(1);
    }
    int num_levels = 1;

    // Construct filename
    string filename("vlasov_divergence." + this_species.name() + ".hdf5");

    WriteAMRHierarchyHDF5(filename.c_str(), layouts, data, names, sliced_box,
                          dx, dt, a_time, ratio, num_levels);
#endif
#endif
}

void GKOps::writeCheckpointFile( HDF5Handle& a_handle ) const
{
   CH_assert( isDefined() );
   char buff[100];
   hsize_t flatdims[1], count[1], sizebuff[1];

   for (int ihist(0); ihist<m_hist_count; ihist++) {

      const FieldHist *field_hist_ptr = &m_fieldHistLists[ihist];

      sprintf(buff,"field_hist_%d", ihist+1);
      a_handle.setGroup(buff);

      flatdims[0] = 1;
      count[0] = 1;
      sizebuff[0] = field_hist_ptr->valsize;

      hid_t sizedataspace = H5Screate_simple(1, flatdims, NULL);
#ifdef H516
      hid_t sizedataset   = H5Dcreate(a_handle.groupID(), "size",
                        H5T_NATIVE_INT, sizedataspace, H5P_DEFAULT);
#else
      hid_t sizedataset   = H5Dcreate(a_handle.groupID(), "size",
                        H5T_NATIVE_INT, sizedataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
#endif
      hid_t szmemdataspace = H5Screate_simple(1, count, NULL);
      H5Dwrite(sizedataset, H5T_NATIVE_INT, szmemdataspace, sizedataspace,
             H5P_DEFAULT, sizebuff);
      H5Dclose(sizedataset);

      int indices[CFG_DIM];

      for (int i = 0; i < CFG_DIM; i++) {
          indices[i] = (field_hist_ptr->grid_indices)[i];
      }

      flatdims[0] =  CFG_DIM;
      count[0] =  CFG_DIM;

      hid_t indexdataspace = H5Screate_simple(1, flatdims, NULL);
#ifdef H516
      hid_t indexdataset   = H5Dcreate(a_handle.groupID(), "indices",
                        H5T_NATIVE_INT, indexdataspace, H5P_DEFAULT);
#else
      hid_t indexdataset   = H5Dcreate(a_handle.groupID(), "indices",
                        H5T_NATIVE_INT, indexdataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
#endif
      hid_t imemdataspace = H5Screate_simple(1, count, NULL);
      H5Dwrite(indexdataset, H5T_NATIVE_INT, imemdataspace, indexdataspace,
             H5P_DEFAULT, indices);
      H5Dclose(indexdataset);
      flatdims[0] =  field_hist_ptr->valsize;
      count[0] =  field_hist_ptr->valsize;

      //cout << "valsize = " << field_hist_ptr->valsize << endl;

      Real *rbuff = new Real[field_hist_ptr->valsize];

      for (int i = 0; i < field_hist_ptr->valsize; i++) {
        rbuff[i] = (*field_hist_ptr->fieldvals)[i];
      }
      hid_t fielddataspace = H5Screate_simple(1, flatdims, NULL);
#ifdef H516
      hid_t fielddataset   = H5Dcreate(a_handle.groupID(), "field",
                        H5T_NATIVE_REAL, fielddataspace, H5P_DEFAULT);
#else
      hid_t fielddataset   = H5Dcreate(a_handle.groupID(), "field",
                        H5T_NATIVE_REAL, fielddataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
#endif
      hid_t fmemdataspace = H5Screate_simple(1, count, NULL);
      H5Dwrite(fielddataset, H5T_NATIVE_REAL, fmemdataspace, fielddataspace,
             H5P_DEFAULT, rbuff);
      H5Dclose(fielddataset);

      for (int i = 0; i < field_hist_ptr->valsize; i++) {
        rbuff[i] = (*field_hist_ptr->timevals)[i];
      }
      hid_t timedataspace = H5Screate_simple(1, flatdims, NULL);
#ifdef H516
      hid_t timedataset   = H5Dcreate(a_handle.groupID(), "times",
                                      H5T_NATIVE_REAL, timedataspace, 
                                      H5P_DEFAULT);
      
#else
      hid_t timedataset   = H5Dcreate(a_handle.groupID(), "times",
                        H5T_NATIVE_REAL, timedataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
#endif
      hid_t tmemdataspace = H5Screate_simple(1, count, NULL);
      H5Dwrite(timedataset, H5T_NATIVE_REAL, tmemdataspace, timedataspace,
             H5P_DEFAULT, rbuff);
      H5Dclose(timedataset);

      delete [] rbuff;

      int *ibuff = new int[field_hist_ptr->valsize];
      for (int i = 0; i < field_hist_ptr->valsize; i++) {
        ibuff[i] = (*field_hist_ptr->timestep)[i];
      }
      hid_t stepdataspace = H5Screate_simple(1, flatdims, NULL);
#ifdef H516
      hid_t stepdataset   = H5Dcreate(a_handle.groupID(), "steps",
                        H5T_NATIVE_INT, stepdataspace, H5P_DEFAULT);
#else
      hid_t stepdataset   = H5Dcreate(a_handle.groupID(), "steps",
                        H5T_NATIVE_INT, stepdataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
#endif
      hid_t smemdataspace = H5Screate_simple(1, count, NULL);
      H5Dwrite(stepdataset, H5T_NATIVE_INT, smemdataspace, stepdataspace,
             H5P_DEFAULT, ibuff);
      H5Dclose(stepdataset);

      delete [] ibuff;

   }
}
   
void GKOps::readCheckpointFile( HDF5Handle& a_handle, const int& a_cur_step )
{
   CH_assert( isDefined() );
   char buff[100];
   hsize_t flatdims[1], count[1], sizebuff[1];

   for (int ihist(0); ihist<m_hist_count; ihist++) {
      
      FieldHist *field_hist_ptr = &m_fieldHistLists[ihist];
      
      sprintf(buff,"field_hist_%d", ihist+1);
      a_handle.setGroup(buff);
      
      flatdims[0] =  1;
      count[0] = 1;
      hsize_t dims[1], maxdims[1];

#ifdef H516
      hid_t sizedataset   = H5Dopen(a_handle.groupID(), "size");
#else
      hid_t sizedataset   = H5Dopen(a_handle.groupID(), "size", H5P_DEFAULT);
#endif
      hid_t sizedataspace = H5Dget_space(sizedataset);
      H5Sget_simple_extent_dims(sizedataspace, dims, maxdims);
      hid_t szmemdataspace = H5Screate_simple(1, dims, NULL);
      H5Dread(sizedataset, H5T_NATIVE_INT, szmemdataspace, sizedataspace,
             H5P_DEFAULT, sizebuff);
      H5Dclose(sizedataset);

      if (sizebuff[0] >= field_hist_ptr->valsize) {
         (*field_hist_ptr->fieldvals).resize(sizebuff[0]);
         (*field_hist_ptr->timevals).resize(sizebuff[0]);
         (*field_hist_ptr->timestep).resize(sizebuff[0]);
         field_hist_ptr->valsize = sizebuff[0];
      }

      int indices[CFG_DIM];
      int readin_indices[CFG_DIM];

      for (int i = 0; i < CFG_DIM; i++) {
          indices[i] = (field_hist_ptr->grid_indices)[i];
      }
      flatdims[0] =  CFG_DIM;
      count[0] =  CFG_DIM;

#ifdef H516
      hid_t indexdataset   = H5Dopen(a_handle.groupID(), "indices");
#else
      hid_t indexdataset   = H5Dopen(a_handle.groupID(), "indices", H5P_DEFAULT);
#endif
      hid_t indexdataspace = H5Dget_space(indexdataset);
      H5Sget_simple_extent_dims(indexdataspace, dims, maxdims);
      hid_t imemdataspace = H5Screate_simple(1, dims, NULL);
      H5Dread(indexdataset, H5T_NATIVE_INT, imemdataspace, indexdataspace,
             H5P_DEFAULT, readin_indices);
      H5Dclose(indexdataset);
      for (int i = 0; i < CFG_DIM; i++) {
        if (indices[i] != readin_indices[i]) {
          MayDay::Error("Grid indices for field history don't match previous run");
        }
      }

      flatdims[0] =  field_hist_ptr->valsize;
      count[0] =  field_hist_ptr->valsize;

      // cout << "valsize = " << field_hist_ptr->valsize << endl;

      Real *rbuff = new Real[field_hist_ptr->valsize];
#ifdef H516
      hid_t fielddataset   = H5Dopen(a_handle.groupID(), "field");
#else
      hid_t fielddataset   = H5Dopen(a_handle.groupID(), "field", H5P_DEFAULT);
#endif
      hid_t fielddataspace = H5Dget_space(fielddataset);
      H5Sget_simple_extent_dims(fielddataspace, dims, maxdims);
      hid_t fmemdataspace = H5Screate_simple(1, dims, NULL);
      H5Dread(fielddataset, H5T_NATIVE_REAL, fmemdataspace, fielddataspace,
             H5P_DEFAULT, rbuff);
      H5Dclose(fielddataset);
      for (int i = 0; i < field_hist_ptr->valsize; i++) {
        (*field_hist_ptr->fieldvals)[i] = rbuff[i];
      }

#ifdef H516
      hid_t timedataset   = H5Dopen(a_handle.groupID(), "times");
#else
      hid_t timedataset   = H5Dopen(a_handle.groupID(), "times", H5P_DEFAULT);
#endif
      hid_t timedataspace = H5Dget_space(timedataset);
      H5Sget_simple_extent_dims(timedataspace, dims, maxdims);
      hid_t tmemdataspace = H5Screate_simple(1, dims, NULL);
      H5Dread(timedataset, H5T_NATIVE_REAL, tmemdataspace, timedataspace,
             H5P_DEFAULT, rbuff);
      H5Dclose(timedataset);
      for (int i = 0; i < field_hist_ptr->valsize; i++) {
        (*field_hist_ptr->timevals)[i] = rbuff[i];
      }

      delete [] rbuff;

      int *ibuff = new int[field_hist_ptr->valsize];
#ifdef H516
      hid_t stepdataset   = H5Dopen(a_handle.groupID(), "steps");
#else
      hid_t stepdataset   = H5Dopen(a_handle.groupID(), "steps", H5P_DEFAULT);
#endif
      hid_t stepdataspace = H5Dget_space(stepdataset);
      H5Sget_simple_extent_dims(stepdataspace, dims, maxdims);
      hid_t smemdataspace = H5Screate_simple(1, dims, NULL);
      H5Dread(stepdataset, H5T_NATIVE_INT, smemdataspace, stepdataspace,
             H5P_DEFAULT, ibuff);
      H5Dclose(stepdataset);
      for (int i = 0; i < field_hist_ptr->valsize; i++) {
        (*field_hist_ptr->timestep)[i] = ibuff[i];
      }

      field_hist_ptr->cur_entry = field_hist_ptr->valsize; //where to add new entries
      for (int i = 0; i < field_hist_ptr->valsize; i++) {
        if (ibuff[i] > a_cur_step || (i>0 && ibuff[i] == 0)) {
          field_hist_ptr->cur_entry = i; // first location in table for next entry
          break;
        }
      }

      delete [] ibuff;

   }
}


inline
KineticSpecies& findElectronSpecies( KineticSpeciesPtrVect& a_species )
{
   int electron_index(-1);
   for (int species(0); species<a_species.size(); species++) {
      if (a_species[species]->isSpecies( "electron" )) {
         electron_index = species;
      }
   }
   CH_assert( electron_index>-1 );
   return ( *(a_species[electron_index]) );
}


void GKOps::enforceQuasiNeutrality(
   KineticSpeciesPtrVect&          a_species,
   CFG::LevelData<CFG::FArrayBox>& a_potential ) const
{
   if (m_enforce_quasineutrality) {
      
      CH_assert( m_phase_geometry != NULL );
      const CFG::MagGeom& mag_geom( m_phase_geometry->magGeom() );
      const CFG::DisjointBoxLayout& grids( mag_geom.gridsFull() );
      CFG::LevelData<CFG::FArrayBox> ion_density( grids, 1, CFG::IntVect::Zero );
      CFG::LevelData<CFG::FArrayBox> electron_density( grids, 1, CFG::IntVect::Zero );
      
      computeSignedChargeDensities( ion_density, electron_density, a_species );
 
      CFG::LevelData<CFG::FArrayBox> quasineutral_density( grids, 1, m_cfg_nghosts );
      computeQuasiNeutralElectronDensity( quasineutral_density,
                                          a_potential,
                                          m_boundary_conditions->getPotentialBC(),
                                          ion_density );

      KineticSpecies& electron_species( findElectronSpecies( a_species ) );
      renormalizeElectronDistributionFunction( electron_species,
                                               quasineutral_density,
                                               electron_density );
   }
}


inline
void GKOps::computeQuasiNeutralElectronDensity(
   CFG::LevelData<CFG::FArrayBox>&       a_quasineutral_density,
   CFG::LevelData<CFG::FArrayBox>&       a_potential,
   const CFG::PotentialBC&               a_bc, 
   const CFG::LevelData<CFG::FArrayBox>& a_ion_density) const
{
   a_ion_density.copyTo( a_quasineutral_density );

   m_poisson->setOperatorCoefficients( a_ion_density, a_bc );
   const CFG::DisjointBoxLayout& grids( a_ion_density.disjointBoxLayout() );
   CFG::LevelData<CFG::FArrayBox> polarization_density( grids, 1, m_cfg_nghosts );

   m_poisson->computeFluxDivergence( a_potential, polarization_density );
   for (CFG::DataIterator cdit( grids.dataIterator() ); cdit.ok(); ++cdit) {
      a_quasineutral_density[cdit] -= polarization_density[cdit];
   }
}


inline
void GKOps::renormalizeElectronDistributionFunction(
   KineticSpecies&                 a_electron_species,
   CFG::LevelData<CFG::FArrayBox>& a_quasineutral_density,
   CFG::LevelData<CFG::FArrayBox>& a_initial_density ) const
{
   const int CELL_AVERAGE_TO_POINT_VALUE(-1);

   CFG::fourthOrderAverage( a_quasineutral_density, CELL_AVERAGE_TO_POINT_VALUE );
   LevelData<FArrayBox> injected_quasineutral_density;
   m_phase_geometry->injectConfigurationToPhase( a_quasineutral_density,
                                                 injected_quasineutral_density );

   CFG::fourthOrderAverage( a_initial_density, CELL_AVERAGE_TO_POINT_VALUE );
   LevelData<FArrayBox> injected_initial_density;
   m_phase_geometry->injectConfigurationToPhase( a_initial_density, injected_initial_density );

   LevelData<FArrayBox>& electron_dfn( a_electron_species.distributionFunction() );
   fourthOrderAverage( electron_dfn, CELL_AVERAGE_TO_POINT_VALUE );
   for (DataIterator dit( electron_dfn.dataIterator() ); dit.ok(); ++dit) {
      electron_dfn[dit].divide( injected_initial_density[dit] );
      electron_dfn[dit].mult( injected_quasineutral_density[dit] );
   }
   fourthOrderAverage( electron_dfn );
}



#include "NamespaceFooter.H"

