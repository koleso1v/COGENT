#include "Poisson.H"
#include "PoissonF_F.H"
#include "FourthOrderUtil.H"

#include "NamespaceHeader.H"

const std::string Poisson::pp_name = "poisson";



Poisson::Poisson( const ParmParse& a_pp,
                  const MagGeom&   a_geom )
   : FieldSolver(a_pp, a_geom)
{
   const DisjointBoxLayout& grids = a_geom.grids();

#ifdef with_petsc
   m_preconditioner = new MBPETScSolver(a_geom, 2);
#else
   m_preconditioner = new MBHypreSolver(a_geom, 2);
#endif

   // We give the mapped coefficients one ghost cell layer so that the
   // usual second-order centered difference formula can be used to compute
   // the transverse gradients needed for the fourth-order formulas even
   // at box boundaries.
   m_mapped_coefficients.define(grids, SpaceDim*SpaceDim, IntVect::Unit);
}



Poisson::~Poisson()
{
   if (m_preconditioner) delete m_preconditioner;
}



void
Poisson::setOperatorCoefficients( const PotentialBC& a_bc )
{
   setBc(a_bc);

   computeCoefficients(m_mapped_coefficients);

   computeBcDivergence( a_bc, m_bc_divergence );

   m_preconditioner->constructMatrix(m_volume_reciprocal, m_mapped_coefficients, a_bc);
}



void
Poisson::computeCoefficients(LevelData<FluxBox>& a_coefficients)
{
   const DisjointBoxLayout & grids = m_geometry.grids();

   const IntVect grown_ghosts = a_coefficients.ghostVect() + IntVect::Unit;
   CH_assert(grown_ghosts == 2*IntVect::Unit);

   // Compute the coefficients at the face centers of a grown (by one ghost cell)
   // LevelData so that they can then be face averaged
   LevelData<FluxBox> grown_coefficients(grids, SpaceDim*SpaceDim, grown_ghosts);

   LevelData<FluxBox> N(grids, SpaceDim*SpaceDim, grown_ghosts);
   m_geometry.getPointwiseN(N);

   LevelData<FluxBox> NJinverse(grids, SpaceDim*SpaceDim, grown_ghosts);
   m_geometry.getPointwiseNJinverse(NJinverse);

   for (DataIterator dit(grids.dataIterator()); dit.ok(); ++dit) {

     FluxBox& this_coef = grown_coefficients[dit];

     for (int dir=0; dir<SpaceDim; ++dir) {
       FArrayBox& this_coef_dir = this_coef[dir];
       const Box& box_dir = this_coef_dir.box();

       FORT_COMPUTE_LAPLACIAN_COEFFICIENTS(CHF_BOX(box_dir),
                                           CHF_CONST_FRA(N[dit][dir]),
                                           CHF_CONST_FRA(NJinverse[dit][dir]),
                                           CHF_FRA(this_coef_dir));
     }

     // Convert from face-centered to face-averaged
     fourthOrderAverageFace(this_coef);

     m_mapped_coefficients[dit].copy(this_coef);
   }
}



void
Poisson::setPreconditionerConvergenceParams( const double a_tol,
                                             const int    a_max_iter,
                                             const double a_precond_tol,
                                             const int    a_precond_max_iter )
{
   m_preconditioner->setParams(m_precond_method, a_tol, a_max_iter, m_precond_verbose,
                               m_precond_precond_method, a_precond_tol, a_precond_max_iter,
                               m_precond_precond_verbose);
}



void
Poisson::solvePreconditioner( const LevelData<FArrayBox>& a_in,
                              LevelData<FArrayBox>&       a_out )
{
   m_preconditioner->solve(a_in, a_out, true);
}



#include "NamespaceFooter.H"
