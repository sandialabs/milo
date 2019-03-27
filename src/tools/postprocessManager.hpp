/***********************************************************************
 Multiscale/Multiphysics Interfaces for Large-scale Optimization (MILO)
 
 Copyright 2018 National Technology & Engineering Solutions of Sandia,
 LLC (NTESS). Under the terms of Contract DE-NA0003525 with NTESS, the
 U.S. Government retains certain rights in this software.”
 
 Questions? Contact Tim Wildey (tmwilde@sandia.gov) and/or
 Bart van Bloemen Waanders (bartv@sandia.gov)
 ************************************************************************/

#ifndef PP_H
#define PP_H

#include "trilinos.hpp"
#include "preferences.hpp"
#include "physicsInterface.hpp"
#include "discretizationInterface.hpp"
#include "assemblyManager.hpp"
#include "parameterManager.hpp"
#include "sensorManager.hpp"
#include "solverInterface.hpp"

using namespace std;
using namespace Intrepid2;

void static postprocessHelp(const string & details) {
  cout << "********** Help and Documentation for the Postprocess Interface **********" << endl;
}

class PostprocessManager {
public:
  
  // ========================================================================================
  /* Constructor to set up the problem */
  // ========================================================================================
  
  PostprocessManager(const Teuchos::RCP<LA_MpiComm> & Comm_, Teuchos::RCP<Teuchos::ParameterList> & settings,
              Teuchos::RCP<panzer_stk::STK_Interface> & mesh_,
              Teuchos::RCP<discretization> & disc_, Teuchos::RCP<physics> & phys_,
              Teuchos::RCP<solver> & solve_, Teuchos::RCP<panzer::DOFManager<int,int> > & DOF_,
              vector<vector<Teuchos::RCP<cell> > > cells_,
              Teuchos::RCP<FunctionInterface> & functionManager,
              Teuchos::RCP<AssemblyManager> & assembler_,
              Teuchos::RCP<ParameterManager> & params_,
              Teuchos::RCP<SensorManager> & sensors_);
  
  // ========================================================================================
  // ========================================================================================
  
  void computeError(const vector_RCP & F_soln);
  
  // ========================================================================================
  // ========================================================================================
  
  AD computeObjective(const vector_RCP & F_soln);
  
  // ========================================================================================
  // ========================================================================================
  
  Kokkos::View<ScalarT***,HostDevice> computeResponse(const vector_RCP & F_soln, const int & b);
  
  // ========================================================================================
  // ========================================================================================
  
  void computeResponse(const vector_RCP & F_soln);
  
  // ========================================================================================
  // ========================================================================================
  
  vector<ScalarT> computeSensitivities(const vector_RCP & F_soln, const vector_RCP & A_soln);
  
  // ========================================================================================
  // ========================================================================================
  
  void writeSolution(const vector_RCP & E_soln, const std::string & filelabel);
  
  // ========================================================================================
  // ========================================================================================
  
  ScalarT makeSomeNoise(ScalarT stdev);
  
protected:
  
  Teuchos::RCP<LA_MpiComm> Comm;
  Teuchos::RCP<panzer_stk::STK_Interface>  mesh;
  Teuchos::RCP<discretization> disc;
  Teuchos::RCP<physics> phys;
  Teuchos::RCP<const panzer::DOFManager<int,int> > DOF;
  Teuchos::RCP<solver> solve;
  Teuchos::RCP<AssemblyManager> assembler;
  Teuchos::RCP<ParameterManager> params;
  Teuchos::RCP<SensorManager> sensors;
  
  int spaceDim;                                                // spatial dimension
  //int numNodes;                                              // total number of nodes in the mesh
  int numNodesPerElem;                                         // nodes on each element
  int numCells;                                                // number of domain cells (normall it is 1)
  size_t numBlocks;                                            // number of element blocks
  
  vector<vector<int> > numBasis;
  vector<vector<int> > useBasis;
  vector<int> maxbasis;
  bool have_sensor_data, save_sensor_data, write_dakota_output, isTD;
  bool plot_response, save_height_file;
  string sname;
  ScalarT stddev;
  
  vector<string> blocknames;
  
  vector<int> numVars;                                    // Number of variables used by the application (may not be used yet)
  int numsteps;
  vector<vector<string> > varlist;
  //vector<FCint > offsets;                   // matrix of offsets (numVars,numBasis)
  
  bool use_sol_mod_mesh, use_sol_mod_height;
  int sol_to_mod_mesh, sol_to_mod_height;
  ScalarT meshmod_TOL, layer_size;
  bool compute_subgrid_error, have_subgrids;
  
  
  
  Teuchos::RCP<const LA_Map> overlapped_map;
  Teuchos::RCP<const LA_Map> param_overlapped_map;
  string response_type, error_type;
  
  vector<vector<Teuchos::RCP<cell> > > cells;
  int verbosity;
};

#endif