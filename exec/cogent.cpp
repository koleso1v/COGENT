#include "ParmParse.H"

#define CH_SPACEDIM 4

#include "GKSystem.H"
#include "Simulation.H"

#include "parstream.H"
#ifdef CH_MPI
#include "CH_Attach.H"
#endif

#include "UsingNamespace.H"

inline int checkCommandLineArgs( int a_argc, char* a_argv[] )
{
   // Check for an input file
   if (a_argc<=1) {
      pout() << "Usage:  cogent...ex <inputfile>" << endl;
      pout() << "No input file specified" << endl;
      return -1;
   }
   return 0;
}

int main( int a_argc, char* a_argv[] )
{
#ifdef CH_MPI
   // Start MPI
   MPI_Init( &a_argc, &a_argv );
   setChomboMPIErrorHandler();
#endif

   int status = checkCommandLineArgs( a_argc, a_argv );

   if (status==0) {
      ParmParse pp( a_argc-2, a_argv+2, NULL, a_argv[1] );
      Simulation<GKSystem> simulation( pp );

      while ( simulation.notDone() ) {
         simulation.advance();
      }

      simulation.finalize();
   }

#ifdef CH_MPI
   MPI_Finalize();
#endif

   return status;
}

#include "Simulation.cpp"

#include "NamespaceHeader.H"
template class Simulation<GKSystem>;
#include "NamespaceFooter.H"