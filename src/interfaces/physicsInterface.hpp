/***********************************************************************
 Multiscale/Multiphysics Interfaces for Large-scale Optimization (MILO)
 
 Copyright 2018 National Technology & Engineering Solutions of Sandia,
 LLC (NTESS). Under the terms of Contract DE-NA0003525 with NTESS, the
 U.S. Government retains certain rights in this software.”
 
 Questions? Contact Tim Wildey (tmwilde@sandia.gov) and/or
 Bart van Bloemen Waanders (bartv@sandia.gov)
 ************************************************************************/

#ifndef PHYSICS_H
#define PHYSICS_H

#include "physics_base.hpp"
#include "workset.hpp"
#include "trilinos.hpp"
#include "preferences.hpp"


static void physicsHelp(const string & details) {
  
  if (details == "none") {
    cout << "********** Help and Documentation for the Physics Interface **********" << endl;
  }
  /*
  else if (details == "porousHDIV") {
    porousHDIVHelp();
  }
  else if (details == "thermal") {
    thermalHelp();
  }
  else if (details == "thermal_enthalpy") {
    thermal_enthalpyHelp();
  }
  else if (details == "msphasefield") {
    msphasefieldHelp();
  }
  else if (details == "navierstokes") {
    navierstokesHelp();
  }
  else if (details == "linearelasticity") {
    linearelasticityHelp();
  }
  else if (details == "helmholtz") {
    helmholtzHelp();
  }
  else if (details == "maxwells_fp") {
    maxwells_fpHelp();
  }
  else if (details == "shallowwater") {
    shallowwaterHelp();
  }
  else {
    cout << "Physics module help: unrecognized details: " << details << endl;
  }
   */
}

class physics {
public:
  
  // ========================================================================================
  /* Constructor to set up the problem */
  // ========================================================================================
  
  physics() {} ;
  
  physics(Teuchos::RCP<Teuchos::ParameterList> & settings, Teuchos::RCP<LA_MpiComm> & Comm_,
          vector<topo_RCP> & cellTopo, vector<topo_RCP> & sideTopo,
          Teuchos::RCP<FunctionInterface> & functionManager_,
          Teuchos::RCP<panzer_stk::STK_Interface> & mesh);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  // Add the requested physics modules, variables, discretization types 
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  void importPhysics(Teuchos::RCP<Teuchos::ParameterList> & settings, Teuchos::ParameterList & currsettings,
                     Teuchos::ParameterList & discsettings,
                     vector<int> & currorders, vector<string> & currtypes,
                     vector<string> & currvarlist, vector<int> & currvarowned,
                     vector<bool> & useScalarFunc, const size_t & numip, const size_t & numip_side,
                     const size_t & blocknum);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  // After the mesh and the discretizations have been defined, we can create and add the physics 
  // to the DOF manager
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  Teuchos::RCP<panzer::DOFManager<int,int> > buildDOF(Teuchos::RCP<panzer_stk::STK_Interface> & mesh);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  int getvarOwner(const int & block, const string & var);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  AD getDirichletValue(const int & block, const double & x, const double & y, const double & z,
                       const double & t, const string & var, const string & gside,
                       const bool & useadjoint, Teuchos::RCP<workset> & wkset);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  double getInitialValue(const int & block, const double & x, const double & y, const double & z,
                         const string & var, const bool & useadjoint);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  int getNumResponses(const int & block, const string & var);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  /*
  double trueSolution(const int & block, const string & var, const double & x, const double & y,
                      const double & z, const double & time);
  */
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  void trueSolution(const int & block, const double & time,
                        Kokkos::View<double***,AssemblyDevice> truesol);

  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  void trueSolutionGrad(const int & block, const double & time,
                    Kokkos::View<double****,AssemblyDevice> truesol);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  int getNumResponses(const int & block);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  Kokkos::View<AD***,AssemblyDevice> getResponse(const int & block,
                                                 Kokkos::View<AD****,AssemblyDevice> u_ip,
                                                 Kokkos::View<AD****,AssemblyDevice> ugrad_ip,
                                                 Kokkos::View<AD****,AssemblyDevice> p_ip,
                                                 Kokkos::View<AD****,AssemblyDevice> pgrad_ip,
                                                 const DRV & ip, const double & time,
                                                 Teuchos::RCP<workset> & wkset);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  bool useScalarRespFunc(const int & block);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  /*
  Kokkos::View<AD*,AssemblyDevice> applyScalarRespFunc(const vector<AD> & integralvals, const int & block,
                           const bool & justDeriv);
   */
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  AD computeTopoResp(const int & block);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  Kokkos::View<AD***,AssemblyDevice> target(const int & block, const DRV & ip,
                                            const double & current_time,
                                            Teuchos::RCP<workset> & wkset);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  Kokkos::View<AD***,AssemblyDevice> weight(const int & block, const DRV & ip,
                                            const double & current_time,
                                            Teuchos::RCP<workset> & wkset);

  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////

  Kokkos::View<double**,AssemblyDevice> getInitial(const DRV & ip, const string var,
                                                   const double & current_time,
                                                   const bool & isAdjoint,
                                                   Teuchos::RCP<workset> & wkset);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  void setVars(size_t & block, vector<string> & vars);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  void updateParameters(vector<Teuchos::RCP<vector<AD> > > & params, const vector<string> & paramnames);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  std::vector<string> getResponseFieldNames(const int & block);

  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  std::vector<string> getExtraFieldNames(const int & block);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  vector<string> getExtraCellFieldNames(const int & block);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  vector<Kokkos::View<double***,AssemblyDevice> > getExtraFields(const int & block);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  Kokkos::View<double***,AssemblyDevice> getExtraFields(const int & block, const DRV & ip,
                                                        const double & time,
                                                        Teuchos::RCP<workset> & wkset);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  Kokkos::View<double***,AssemblyDevice> getExtraCellFields(const int & block,
                                                            const size_t & numElem);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  int getUniqueIndex(const int & block, const std::string & var);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  void setBCData(Teuchos::RCP<Teuchos::ParameterList> & settings, Teuchos::RCP<panzer_stk::STK_Interface> & mesh, 
                 Teuchos::RCP<panzer::DOFManager<int,int> > & DOF, std::vector<std::vector<int> > cards);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  Kokkos::View<int****,HostDevice> getSideInfo(const size_t & block, Kokkos::View<int*> elem);

  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  vector<vector<int> > getOffsets(const int & block, Teuchos::RCP<panzer::DOFManager<int,int> > & DOF);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  Kokkos::View<int**,HostDevice> getSideInfo(const int & block, int & num, size_t & e);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  void setPeriBCs(Teuchos::RCP<Teuchos::ParameterList> & settings, Teuchos::RCP<panzer_stk::STK_Interface> & mesh);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  void volumeResidual(const size_t block);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  void boundaryResidual(const size_t block);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  void computeFlux(const size_t block);
   
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////
  
  void setWorkset(vector<Teuchos::RCP<workset> > & wkset);
  
  /////////////////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////////////////

  //void printTimers() {
  //  if (activeModules[0][4]) {
  //    thermal_RCP->printTimers();
  //  }
  //
  //}

  /////////////////////////////////////////////////////////////////////////////////////////////
  // Public data members
  /////////////////////////////////////////////////////////////////////////////////////////////

  vector<vector<Teuchos::RCP<physicsbase> > > modules;
  vector<vector<bool> > module_useScalarRespFunc;
  Teuchos::RCP<FunctionInterface> functionManager;
  Teuchos::RCP<LA_MpiComm> Commptr;
  
  vector<string> blocknames;
  int spaceDim, numElemPerCell;
  size_t numBlocks;
  
  vector<int> numVars;
  
  vector<vector<string> > varlist;
  vector<vector<int> > varowned;
  vector<vector<int> > orders;
  vector<vector<string> > types;
  vector<vector<int> > unique_orders;
  vector<vector<string> > unique_types;
  vector<vector<int> > unique_index;
  
  vector<Kokkos::View<int****,HostDevice> > side_info;
  vector<vector<vector<size_t> > > localDirichletSideIDs, globalDirichletSideIDs;
  vector<vector<vector<size_t> > > boundDirichletElemIDs;
  vector<vector<int> > dbc_dofs;
  
  //vector<FCint> offsets;
  vector<vector<vector<int> > > offsets;
  vector<string> sideSets;
  vector<string> nodeSets;
  int numSidesPerElem, numNodesPerElem;
  vector<size_t> numElem;
  string initial_type, cellfield_reduction;
  
  vector<vector<string> > extrafields_list, extracellfields_list, response_list, target_list, weight_list;
  
};


#endif
