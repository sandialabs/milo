/***********************************************************************
 Multiscale/Multiphysics Interfaces for Large-scale Optimization (MILO)
 
 Copyright 2018 National Technology & Engineering Solutions of Sandia,
 LLC (NTESS). Under the terms of Contract DE-NA0003525 with NTESS, the
 U.S. Government retains certain rights in this software.”
 
 Questions? Contact Tim Wildey (tmwilde@sandia.gov) and/or
 Bart van Bloemen Waanders (bartv@sandia.gov)
 ************************************************************************/

#include "multiscaleInterface.hpp"
#include "solverInterface.hpp"
#include "discretizationTools.hpp"
#include "workset.hpp"
#include <boost/algorithm/string.hpp>


// ========================================================================================
/* Constructor to set up the problem */
// ========================================================================================

solver::solver(const Teuchos::RCP<LA_MpiComm> & Comm_, Teuchos::RCP<Teuchos::ParameterList> & settings,
               Teuchos::RCP<panzer_stk::STK_Interface> & mesh_, Teuchos::RCP<discretization> & disc_,
               Teuchos::RCP<physics> & phys_, Teuchos::RCP<panzer::DOFManager<int,int> > & DOF_,
               Teuchos::RCP<AssemblyManager> & assembler_,
               Teuchos::RCP<ParameterManager> & params_) :
               //vector<vector<Teuchos::RCP<cell> > > & cells_) :
Comm(Comm_), mesh(mesh_), disc(disc_), phys(phys_), DOF(DOF_), assembler(assembler_), params(params_) { //cells(assembler->cells_) {
  
  // Get the required information from the settings
  spaceDim = settings->sublist("Mesh").get<int>("dim",2);
  numsteps = settings->sublist("Solver").get("numSteps",1);
  verbosity = settings->get<int>("verbosity",0);
  usestrongDBCs = settings->sublist("Solver").get<bool>("use strong DBCs",true);
  use_meas_as_dbcs = settings->sublist("Mesh").get<bool>("Use Measurements as DBCs", false);
  solver_type = settings->sublist("Solver").get<string>("solver","none"); // or "transient"
  allow_remesh = settings->sublist("Solver").get<bool>("Remesh",false);
  finaltime = settings->sublist("Solver").get<ScalarT>("finaltime",1.0);
  time_order = settings->sublist("Solver").get<int>("time order",1);
  NLtol = settings->sublist("Solver").get<ScalarT>("NLtol",1.0E-6);
  MaxNLiter = settings->sublist("Solver").get<int>("MaxNLiter",10);
  NLsolver = settings->sublist("Solver").get<string>("Nonlinear Solver","Newton");
  line_search = false;//settings->sublist("Solver").get<bool>("Use Line Search","false");
  store_adjPrev = false;
  
  isTransient = false;
  if (solver_type == "transient") {
    isTransient = true;
  }
  else {
    numsteps = 1;
  }
  
  isInitial = false;
  current_time = settings->sublist("Solver").get<ScalarT>("Initial Time",0.0);
  solvetimes.push_back(current_time);
  
  if (isTransient) {
    ScalarT deltat = finaltime / numsteps;
    ScalarT ctime = current_time; // local current time
    for (int timeiter = 0; timeiter < numsteps; timeiter++) {
      ctime += deltat;
      solvetimes.push_back(ctime);
    }
  }
  
  response_type = settings->sublist("Postprocess").get("response type", "pointwise"); // or "global"
  compute_objective = settings->sublist("Postprocess").get("compute objective",false);
  compute_sensitivity = settings->sublist("Postprocess").get("compute sensitivities",false);
  
  meshmod_xvar = settings->sublist("Solver").get<int>("Solution For x-Mesh Mod",-1);
  meshmod_yvar = settings->sublist("Solver").get<int>("Solution For y-Mesh Mod",-1);
  meshmod_zvar = settings->sublist("Solver").get<int>("Solution For z-Mesh Mod",-1);
  meshmod_TOL = settings->sublist("Solver").get<ScalarT>("Solution Based Mesh Mod TOL",1.0);
  meshmod_usesmoother = settings->sublist("Solver").get<bool>("Solution Based Mesh Mod Smoother",false);
  meshmod_center = settings->sublist("Solver").get<ScalarT>("Solution Based Mesh Mod Param",0.1);
  meshmod_layer_size = settings->sublist("Solver").get<ScalarT>("Solution Based Mesh Mod Layer Thickness",0.1);
  
  initial_type = settings->sublist("Solver").get<string>("Initial type","L2-projection");
  multigrid_type = settings->sublist("Solver").get<string>("Multigrid type","sa");
  smoother_type = settings->sublist("Solver").get<string>("Smoother type","CHEBYSHEV"); // or RELAXATION
  lintol = settings->sublist("Solver").get<ScalarT>("lintol",1.0E-7);
  liniter = settings->sublist("Solver").get<int>("liniter",100);
  kspace = settings->sublist("Solver").get<int>("krylov vectors",100);
  useDomDecomp = settings->sublist("Solver").get<bool>("use dom decomp",false);
  useDirect = settings->sublist("Solver").get<bool>("use direct solver",false);
  usePrec = settings->sublist("Solver").get<bool>("use preconditioner",true);
  dropTol = settings->sublist("Solver").get<ScalarT>("ILU drop tol",0.0); //defaults to AztecOO default
  fillParam = settings->sublist("Solver").get<ScalarT>("ILU fill param",3.0); //defaults to AztecOO default
  
  // needed information from the mesh
  mesh->getElementBlockNames(blocknames);
  
  // needed information from the physics interface
  numVars = phys->numVars; //
  vector<vector<string> > phys_varlist = phys->varlist;
  //offsets = phys->offsets;
  
  // Set up the time integrator
  string timeinttype = settings->sublist("Solver").get<string>("Time integrator","RK");
  string timeintmethod = settings->sublist("Solver").get<string>("Time method","Implicit");
  int timeintorder = settings->sublist("Solver").get<int>("Time order",1);
  bool timeintstagger = settings->sublist("Solver").get<bool>("Stagger solutions",true);

  //if (timeinttype == "RK") {
  //  timeInt = Teuchos::rcp(new RungeKutta(timeintmethod,timeintorder,timeintstagger));
  //}
  
  // needed information from the DOF manager
  DOF->getOwnedIndices(LA_owned);
  numUnknowns = (int)LA_owned.size();
  DOF->getOwnedAndGhostedIndices(LA_ownedAndShared);
  numUnknownsOS = (int)LA_ownedAndShared.size();
  int localNumUnknowns = numUnknowns;
  
  DOF->getOwnedIndices(owned);
  DOF->getOwnedAndGhostedIndices(ownedAndShared);
  
  int nstages = 1;//timeInt->num_stages;
  bool sol_staggered = true;//timeInt->sol_staggered;
  /*
  LA_owned = vector(numUnknowns);
  LA_ownedAndShared = vector(numUnknownsOS);
  for (size_t i=0; i<numUnknowns; i++) {
    LA_owned[i] = owned[i];
  }
  for (size_t i=0; i<numUnknownsOS; i++) {
    LA_ownedAndShared[i] = ownedAndShared[i];
  }
   */
  //for (size_t i=0; i<numUnknowns; i++) {
    //for (int s=0; s<nstages; s++) {
    //  LA_owned[i*(nstages)+s] = owned[i]*nstages+s;
    //}
  //}
  //for (size_t i=0; i<numUnknownsOS; i++) {
  //  for (int s=0; s<nstages; s++) {
  //    LA_ownedAndShared[i*(nstages)+s] = ownedAndShared[i]*nstages+s;
  //  }
  //}
  
  globalNumUnknowns = 0;
  Teuchos::reduceAll(*Comm,Teuchos::REDUCE_SUM,1,&localNumUnknowns,&globalNumUnknowns);
  //Comm->SumAll(&localNumUnknowns, &globalNumUnknowns, 1);
  
  // needed information from the disc interface
  vector<vector<int> > cards = disc->cards;
  
  for (size_t b=0; b<blocknames.size(); b++) {
    
    vector<int> curruseBasis(numVars[b]);
    vector<int> currnumBasis(numVars[b]);
    vector<string> currvarlist(numVars[b]);
    
    int currmaxbasis = 0;
    for (int j=0; j<numVars[b]; j++) {
      string var = phys_varlist[b][j];
      int vnum = DOF->getFieldNum(var);
      int vub = phys->getUniqueIndex(b,var);
      currvarlist[j] = var;
      curruseBasis[j] = vub;
      currnumBasis[j] = cards[b][vub];
      //currvarlist[vnum] = var;
      //curruseBasis[vnum] = vub;
      //currnumBasis[vnum] = cards[b][vub];
      currmaxbasis = std::max(currmaxbasis,cards[b][vub]);
    }
    
    phys->setVars(b,currvarlist);
    
    varlist.push_back(currvarlist);
    useBasis.push_back(curruseBasis);
    numBasis.push_back(currnumBasis);
    maxbasis.push_back(currmaxbasis);
    
    vector<size_t> localIds;
    DRV blocknodes;
    panzer_stk::workset_utils::getIdsAndVertices(*mesh, blocknames[b], localIds, blocknodes);
    elemnodes.push_back(blocknodes);
    
  }
  
  /////////////////////////////////////////////////////////////////////////////
  // Epetra maps
  /////////////////////////////////////////////////////////////////////////////
  
  this->setupLinearAlgebra();
  
  /////////////////////////////////////////////////////////////////////////////
  // Worksets
  /////////////////////////////////////////////////////////////////////////////
  
  
  if (settings->sublist("Mesh").get<bool>("Have Element Data", false) ||
      settings->sublist("Mesh").get<bool>("Have Nodal Data", false)) {
    this->readMeshData(settings);
  }
  
  /////////////////////////////////////////////////////////////////////////////
  
}

void solver::finalizeWorkset() {
  
  int nstages = 1;//timeInt->num_stages;
  
  for (size_t b=0; b<assembler->cells.size(); b++) {
    /*
    wkset.push_back(Teuchos::rcp( new workset(assembler->cells[b][0]->getInfo(), disc->ref_ip[b],
                                              disc->ref_wts[b], disc->ref_side_ip[b],
                                              disc->ref_side_wts[b], disc->basis_types[b],
                                              disc->basis_pointers[b],
                                              discretized_param_basis,
                                              mesh->getCellTopology(blocknames[b])) ) );
    
    wkset[b]->isInitialized = true;
    wkset[b]->block = b;
     */
    //wkset[b]->num_stages = nstages;
    vector<vector<int> > voffsets = phys->offsets[b];
    size_t maxoff = 0;
    for (size_t i=0; i<voffsets.size(); i++) {
      if (voffsets[i].size() > maxoff) {
        maxoff = voffsets[i].size();
      }
    }
    Kokkos::View<int**,HostDevice> offsets_host("offsets on host device",voffsets.size(),maxoff);
    for (size_t i=0; i<voffsets.size(); i++) {
      for (size_t j=0; j<voffsets[i].size(); j++) {
        offsets_host(i,j) = voffsets[i][j];
      }
    }
    Kokkos::View<int**,AssemblyDevice>::HostMirror offsets_device = Kokkos::create_mirror_view(offsets_host);
    Kokkos::deep_copy(offsets_host, offsets_device);
    assembler->wkset[b]->offsets = offsets_device;//phys->voffsets[b];
    
    size_t maxpoff = 0;
    for (size_t i=0; i<params->paramoffsets.size(); i++) {
      if (params->paramoffsets[i].size() > maxpoff) {
        maxpoff = params->paramoffsets[i].size();
      }
      //maxpoff = max(maxpoff,paramoffsets[i].size());
    }
    Kokkos::View<int**,HostDevice> poffsets_host("param offsets on host device",params->paramoffsets.size(),maxpoff);
    for (size_t i=0; i<params->paramoffsets.size(); i++) {
      for (size_t j=0; j<params->paramoffsets[i].size(); j++) {
        poffsets_host(i,j) = params->paramoffsets[i][j];
      }
    }
    Kokkos::View<int**,AssemblyDevice>::HostMirror poffsets_device = Kokkos::create_mirror_view(poffsets_host);
    Kokkos::deep_copy(poffsets_host, poffsets_device);
    
    assembler->wkset[b]->usebasis = useBasis[b];
    assembler->wkset[b]->paramusebasis = params->discretized_param_usebasis;
    assembler->wkset[b]->paramoffsets = poffsets_device;//paramoffsets;
    assembler->wkset[b]->varlist = varlist[b];
    int numDOF = assembler->cells[b][0]->GIDs[0].size();
    for (size_t e=0; e<assembler->cells[b].size(); e++) {
      assembler->cells[b][e]->wkset = assembler->wkset[b];
      assembler->cells[b][e]->setUseBasis(useBasis[b],nstages);
      assembler->cells[b][e]->setUpAdjointPrev(numDOF);
      assembler->cells[b][e]->setUpSubGradient(params->num_active_params);
    }
    
    assembler->wkset[b]->params = params->paramvals_AD;
    assembler->wkset[b]->params_AD = params->paramvals_KVAD;
    assembler->wkset[b]->paramnames = params->paramnames;
    //assembler->wkset[b]->setupParamBasis(discretized_param_basis);
  }
  //phys->setWorkset(wkset);
  
}

// ========================================================================================
// Set up the Tpetra objects (maps, importers, exporters and graphs)
// These do need to be recomputed whenever the mesh changes */
// ========================================================================================

void solver::setupLinearAlgebra() {
  
  // Need to construct two types of vectors
  // One for storing the end-stage solutions of length N
  // Another for the linear algebra objects which are length N*s or (N*s)x(N*s)
  
  //sol_overlapped_map = Teuchos::rcp(new LA_Map(-1, (int)ownedAndShared.size(), &ownedAndShared[0], 0, *Comm));
  //sol_overlapped_graph = Teuchos::rcp(new LA_CrsGraph(Copy, *sol_overlapped_map, 0));
  
  //int nstages = timeInt->num_stages;
  //bool sol_staggered = timeInt->sol_staggered;
  
  /*
  Epetra_MpiComm EP_Comm(*(Comm->getRawMpiComm()));
  LA_owned_map = Teuchos::rcp(new LA_Map(-1, (int)LA_owned.size(), &LA_owned[0], 0, EP_Comm));
  LA_overlapped_map = Teuchos::rcp(new LA_Map(-1, (int)LA_ownedAndShared.size(), &LA_ownedAndShared[0], 0, EP_Comm));
  
  LA_owned_graph = Teuchos::rcp(new LA_CrsGraph(Copy, *LA_owned_map, 0));
  LA_overlapped_graph = Teuchos::rcp(new LA_CrsGraph(Copy, *LA_overlapped_map, 0));
  */
  
  const Tpetra::global_size_t INVALID = Teuchos::OrdinalTraits<Tpetra::global_size_t>::invalid ();
  
  LA_owned_map = Teuchos::rcp(new LA_Map(INVALID, LA_owned, 0, Comm));
  LA_overlapped_map = Teuchos::rcp(new LA_Map(INVALID, LA_ownedAndShared, 0, Comm));
  LA_owned_graph = createCrsGraph(LA_owned_map);//Teuchos::rcp(new LA_CrsGraph(Copy, *LA_owned_map, 0));
  LA_overlapped_graph = createCrsGraph(LA_overlapped_map);//Teuchos::rcp(new LA_CrsGraph(Copy, *LA_overlapped_map, 0));
  
  exporter = Teuchos::rcp(new LA_Export(LA_overlapped_map, LA_owned_map));
  importer = Teuchos::rcp(new LA_Import(LA_owned_map, LA_overlapped_map));
  //importer = Teuchos::rcp(new LA_Import(LA_overlapped_map, LA_owned_map));
  
  
  vector<vector<int> > gids;
  
  for (size_t b=0; b<assembler->cells.size(); b++) {
    vector<vector<int> > curroffsets = phys->offsets[b];
    for(size_t e=0; e<assembler->cells[b].size(); e++) {
      gids = assembler->cells[b][e]->GIDs;
      
      int numElem = assembler->cells[b][e]->numElem;
      
      // this should fail on the first iteration through if maxDerivs is not large enough
      TEUCHOS_TEST_FOR_EXCEPTION(gids[0].size() > maxDerivs,std::runtime_error,"Error: maxDerivs is not large enough to support the number of degrees of freedom per element times the number of time stages.");
      vector<vector<vector<int> > > cellindices;
      for (int p=0; p<numElem; p++) {
        vector<vector<int> > indices;
        for (int n=0; n<numVars[b]; n++) {
          vector<int> cindex;
          for( int i=0; i<numBasis[b][n]; i++ ) {
            int cgid = gids[p][curroffsets[n][i]]; // now an index into a block of DOFs
            cindex.push_back(LA_overlapped_map->getLocalElement(cgid));
          }
          indices.push_back(cindex);
        }
        
        for (size_t i=0; i<gids[p].size(); i++) {
          //for (size_t j=0; j<gids[p].size(); j++) {
            //int err = LA_owned_graph->InsertGlobalIndices(gids[i],1,&gids[j]);
            //int err = LA_overlapped_graph->InsertGlobalIndices(gids[p][i],1,&gids[p][j]);
            int ind1 = gids[p][i];
            //int ind2 = gids[p][j];
            LA_overlapped_graph->insertGlobalIndices(ind1,gids[p]);
            //LA_owned_graph->insertGlobalIndices(ind1,gids[p]);
            /*for (int si=0; si<nstages; si++) {
              int ind1 = gids[p][i]*nstages+si;
              for (int sj=0; sj<nstages; sj++) {
                int ind2 = gids[p][j]*nstages+sj;
                int err = LA_overlapped_graph->InsertGlobalIndices(ind1,1,&ind2);
              }
            }*/
          //}
        }
        cellindices.push_back(indices);
        
      }
      assembler->cells[b][e]->setIndex(cellindices);
    }
  }
  
  //LA_owned_graph->fillComplete();
  //sol_overlapped_graph->FillComplete();
  LA_overlapped_graph->fillComplete();
  
  /*
  if (num_discretized_params > 0) {
    param_owned_map = Teuchos::rcp(new LA_Map(INVALID, paramOwned, 0, Comm));
    param_overlapped_map = Teuchos::rcp(new LA_Map(INVALID, paramOwnedAndShared, 0, Comm));
    
    param_exporter = Teuchos::rcp(new LA_Export(param_overlapped_map, param_owned_map));
    param_importer = Teuchos::rcp(new LA_Import(param_overlapped_map, param_owned_map));
    
    vector<vector<int> > gids;
    vector< vector<int> > param_nodesOS(numParamUnknownsOS); // should be overlapped
    vector< vector<int> > param_nodes(numParamUnknowns); // not overlapped -- for bounds
    vector< vector< vector<ScalarT> > > param_initial_vals; // custom initial guess set by assembler->cells
    DRV nodes;
    vector_RCP paramVec = this->setInitialParams(); // TMW: this will be deprecated soon
    
    for (size_t b=0; b<assembler->cells.size(); b++) {
      vector<vector<int> > curroffsets = phys->offsets[b];
      for(size_t e=0; e<assembler->cells[b].size(); e++) {
        gids = assembler->cells[b][e]->paramGIDs;
        // this should fail on the first iteration through if maxDerivs is not large enough
        TEUCHOS_TEST_FOR_EXCEPTION(gids[0].size() > maxDerivs,std::runtime_error,"Error: maxDerivs is not large enough to support the number of parameter degrees of freedom per element.");
        
        vector<vector<vector<int> > > cellindices;
        int numElem = assembler->cells[b][e]->numElem;
        for (int p=0; p<numElem; p++) {
          
          vector<vector<int> > indices;
          
          for (int n=0; n<num_discretized_params; n++) {
            vector<int> cindex;
            for( int i=0; i<paramNumBasis[n]; i++ ) {
              int globalIndexOS = param_overlapped_map->getLocalElement(gids[p][paramoffsets[n][i]]);
              cindex.push_back(globalIndexOS);
              param_nodesOS[n].push_back(globalIndexOS);
              int globalIndex_owned = param_owned_map->getLocalElement(gids[p][paramoffsets[n][i]]);
              param_nodes[n].push_back(globalIndex_owned);
            }
            indices.push_back(cindex);
          }
          cellindices.push_back(indices);
        }
        assembler->cells[b][e]->setParamIndex(cellindices);
        
      }
    }
    for (int n=0; n<num_discretized_params; n++) {
      std::sort(param_nodesOS[n].begin(), param_nodesOS[n].end());
      param_nodesOS[n].erase( std::unique(param_nodesOS[n].begin(),
                                          param_nodesOS[n].end()), param_nodesOS[n].end());
      
      std::sort(param_nodes[n].begin(), param_nodes[n].end());
      param_nodes[n].erase( std::unique(param_nodes[n].begin(),
                                        param_nodes[n].end()), param_nodes[n].end());
    }
    for (int n = 0; n < num_discretized_params; n++) {
      if (!use_custom_initial_param_guess) {
        for (size_t i = 0; i < param_nodesOS[n].size(); i++) {
          paramVec->replaceGlobalValue(paramOwnedAndShared[param_nodesOS[n][i]]
                                       ,0,initialParamValues[n]);
        }
      }
      paramNodesOS.push_back(param_nodesOS[n]); // store for later use
      paramNodes.push_back(param_nodes[n]); // store for later use
    }
    Psol.push_back(paramVec);
  }
  else {
    // set up a dummy parameter vector
    paramOwnedAndShared.push_back(0);
    param_overlapped_map = Teuchos::rcp(new LA_Map(INVALID, paramOwnedAndShared, 0, Comm));
    
    vector_RCP paramVec = this->setInitialParams(); // TMW: this will be deprecated soon
    Psol.push_back(paramVec);
  }*/
  
}

// ========================================================================================
// Set up the Epetra overlapped CrsGraph (for bwds compat.)
// ========================================================================================

Teuchos::RCP<Epetra_CrsGraph> solver::buildEpetraOverlappedGraph(Epetra_MpiComm & EP_Comm) {
  
  //Epetra_MpiComm EP_Comm(*(Comm->getRawMpiComm()));
  Teuchos::RCP<Epetra_Map> Ep_map = Teuchos::rcp(new Epetra_Map(-1, (int)LA_ownedAndShared.size(), &LA_ownedAndShared[0], 0, EP_Comm));
   
  Teuchos::RCP<Epetra_CrsGraph> Ep_graph = Teuchos::rcp(new Epetra_CrsGraph(Copy, *Ep_map, 0));
  
  vector<vector<int> > gids;
  
  for (size_t b=0; b<assembler->cells.size(); b++) {
    vector<vector<int> > curroffsets = phys->offsets[b];
    for(size_t e=0; e<assembler->cells[b].size(); e++) {
      gids = assembler->cells[b][e]->GIDs;
      for (int p=0; p<assembler->cells[b][e]->numElem; p++) {
        for (size_t i=0; i<gids[p].size(); i++) {
          int ind1 = gids[p][i];
          for (size_t j=0; j<gids[p].size(); j++) {
            int ind2 = gids[p][j];
            int err = Ep_graph->InsertGlobalIndices(ind1,1,&ind2);
          }
        }
      }
    }
  }
  Ep_graph->FillComplete();
  return Ep_graph;
}

// ========================================================================================
// Set up the Epetra owned CrsGraph (for bwds compat.)
// ========================================================================================

Teuchos::RCP<Epetra_CrsGraph> solver::buildEpetraOwnedGraph(Epetra_MpiComm & EP_Comm) {
  
  //Epetra_MpiComm EP_Comm(*(Comm->getRawMpiComm()));
  Teuchos::RCP<Epetra_Map> Ep_map = Teuchos::rcp(new Epetra_Map(-1, (int)LA_owned.size(), &LA_owned[0], 0, EP_Comm));
  
  Teuchos::RCP<Epetra_CrsGraph> Ep_graph = Teuchos::rcp(new Epetra_CrsGraph(Copy, *Ep_map, 0));
  
  vector<vector<int> > gids;
  
  for (size_t b=0; b<assembler->cells.size(); b++) {
    vector<vector<int> > curroffsets = phys->offsets[b];
    for(size_t e=0; e<assembler->cells[b].size(); e++) {
      gids = assembler->cells[b][e]->GIDs;
      for (int p=0; p<assembler->cells[b][e]->numElem; p++) {
        for (size_t i=0; i<gids[p].size(); i++) {
          int ind1 = gids[p][i];
          for (size_t j=0; j<gids[p].size(); j++) {
            int ind2 = gids[p][j];
            int err = Ep_graph->InsertGlobalIndices(ind1,1,&ind2);
          }
        }
      }
    }
  }
  Ep_graph->FillComplete();
  return Ep_graph;
}

// ========================================================================================
// Set up the parameters (inactive, active, stochastic, discrete)
// Communicate these parameters back to the physics interface and the enabled modules
// ========================================================================================
/*
void solver::setupParameters(Teuchos::RCP<Teuchos::ParameterList> & settings) {
  
  Teuchos::ParameterList parameters;
  
  if (settings->isSublist("Parameters")) {
    parameters = settings->sublist("Parameters");
    Teuchos::ParameterList::ConstIterator pl_itr = parameters.begin();
    while (pl_itr != parameters.end()) {
      Teuchos::ParameterList newparam = parameters.sublist(pl_itr->first);
      vector<ScalarT> newparamvals;
      int numnewparams = 0;
      if (!newparam.isParameter("type") || !newparam.isParameter("usage")) {
        // print out error message
      }
      
      if (newparam.get<string>("type") == "scalar") {
        newparamvals.push_back(newparam.get<ScalarT>("value"));
        numnewparams = 1;
      }
      else if (newparam.get<string>("type") == "vector") {
        std::string filename = newparam.get<string>("source");
        std::ifstream fin(filename.c_str());
        std::istream_iterator<ScalarT> start(fin), end;
        vector<ScalarT> importedparamvals(start, end);
        for (size_t i=0; i<importedparamvals.size(); i++) {
          newparamvals.push_back(importedparamvals[i]);
        }
      }
      
      paramnames.push_back(pl_itr->first);
      paramvals.push_back(newparamvals);
      
      Teuchos::RCP<vector<AD> > newparam_AD = Teuchos::rcp(new vector<AD>(newparamvals.size()));
      paramvals_AD.push_back(newparam_AD);
      
      //blank bounds
      vector<ScalarT> lo(newparamvals.size(),0.0);
      vector<ScalarT> up(newparamvals.size(),0.0);
      
      if (newparam.get<string>("usage") == "inactive") {
        paramtypes.push_back(0);
        num_inactive_params += newparamvals.size();
      }
      else if (newparam.get<string>("usage") == "active") {
        paramtypes.push_back(1);
        num_active_params += newparamvals.size();
        
        //if active, look for actual bounds
        if(newparam.isParameter("bounds")){
          std::string filename = newparam.get<string>("bounds");
          FILE* BoundsFile = fopen(filename.c_str(),"r");
          float a,b;
          int i = 0;
          while( !feof(BoundsFile) ) {
            char line[100] = "";
            fgets(line,100,BoundsFile);
            if( strcmp(line,"") ) {
              sscanf(line, "%f %f", &a, &b);
              lo[i] = a;
              up[i] = b;
            }
            i++;
          }
        }
      }
      else if (newparam.get<string>("usage") == "stochastic") {
        paramtypes.push_back(2);
        num_stochastic_params += newparamvals.size();
        for (size_t i=0; i<newparamvals.size(); i++) {
          stochastic_distribution.push_back(newparam.get<string>("distribution","uniform"));
          stochastic_mean.push_back(newparam.get<ScalarT>("mean",0.0));
          stochastic_variance.push_back(newparam.get<ScalarT>("variance",1.0));
          stochastic_min.push_back(newparam.get<ScalarT>("min",0.0));
          stochastic_max.push_back(newparam.get<ScalarT>("max",0.0));
        }
      }
      else if (newparam.get<string>("usage") == "discrete") {
        paramtypes.push_back(3);
        num_discrete_params += newparamvals.size();
      }
      else if (newparam.get<string>("usage") == "discretized") {
        paramtypes.push_back(4);
        num_discretized_params += 1;
        if (!discretized_stochastic) { // once this is turned on, it stays on
          discretized_stochastic = newparam.get<bool>("stochastic",false);
        }
        discretized_param_basis_types.push_back(newparam.get<string>("type","HGRAD"));
        discretized_param_basis_orders.push_back(newparam.get<int>("order",1));
        discretized_param_names.push_back(pl_itr->first);
        initialParamValues.push_back(newparam.get<ScalarT>("initial_value",1.0));
        lowerParamBounds.push_back(newparam.get<ScalarT>("lower_bound",-1.0));
        upperParamBounds.push_back(newparam.get<ScalarT>("upper_bound",1.0));
        discparam_distribution.push_back(newparam.get<string>("distribution","uniform"));
        discparamVariance.push_back(newparam.get<ScalarT>("variance",1.0));
        if (newparam.get<bool>("isDomainParam",true)) {
          domainRegTypes.push_back(newparam.get<int>("reg_type",0));
          domainRegConstants.push_back(newparam.get<ScalarT>("reg_constant",0.0));
          domainRegIndices.push_back(num_discretized_params - 1);
        }
        else {
          boundaryRegTypes.push_back(newparam.get<int>("reg_type",0));
          boundaryRegConstants.push_back(newparam.get<ScalarT>("reg_constant",0.0));
          boundaryRegSides.push_back(newparam.get<string>("sides"," "));
          boundaryRegIndices.push_back(num_discretized_params - 1);
        }
      }
      
      paramLowerBounds.push_back(lo);
      paramUpperBounds.push_back(up);
      
      pl_itr++;
    }
    
    TEUCHOS_TEST_FOR_EXCEPTION(num_active_params > maxDerivs,std::runtime_error,"Error: maxDerivs is not large enough to support the number of parameters.");
    
    if (num_discretized_params > 0) {
      // determine the unique list of basis'
      vector<int> disc_orders;
      vector<string> disc_types;
      vector<int> disc_usebasis;
      
      for (size_t j=0; j<discretized_param_basis_orders.size(); j++) {
        bool is_unique = true;
        for (size_t k=0; k<disc_orders.size(); k++) {
          if (disc_orders[k] == discretized_param_basis_orders[j] &&
              disc_types[k] == discretized_param_basis_types[j]) {
            is_unique = false;
            disc_usebasis.push_back(k);
          }
        }
        if (is_unique) {
          disc_orders.push_back(discretized_param_basis_orders[j]);
          disc_types.push_back(discretized_param_basis_types[j]);
          disc_usebasis.push_back(disc_orders.size()-1);
        }
      }
      
      discretized_param_basis_types = disc_types;
      discretized_param_basis_orders = disc_orders;
      discretized_param_usebasis = disc_usebasis;
      
      for (size_t n=0; n<disc_orders.size(); n++) {
        topo_RCP cellTopo = mesh->getCellTopology(blocknames[0]);
        basis_RCP basis = DiscTools::getBasis(spaceDim, cellTopo, disc_types[n],
                                              disc_orders[n]);
        discretized_param_basis.push_back(basis);
        
      }
      
      paramDOF = Teuchos::rcp(new panzer::DOFManager<int,int>());
      Teuchos::RCP<panzer::ConnManager<int,int> > conn = Teuchos::rcp(new panzer_stk::STKConnManager<int>(mesh));
      paramDOF->setConnManager(conn,*(Comm->getRawMpiComm()));
      
      Teuchos::RCP<const panzer::Intrepid2FieldPattern> Pattern;
      
      for (size_t b=0; b<blocknames.size(); b++) {
        for (size_t j=0; j<discretized_param_names.size(); j++) {
          
          Pattern = Teuchos::rcp(new panzer::Intrepid2FieldPattern(discretized_param_basis[disc_usebasis[j]]));
          paramDOF->addField(blocknames[b], discretized_param_names[j], Pattern);
        }
      }
      
      paramDOF->buildGlobalUnknowns();
      paramDOF->getOwnedIndices(paramOwned);
      numParamUnknowns = (int)paramOwned.size();
      paramDOF->getOwnedAndGhostedIndices(paramOwnedAndShared);
      numParamUnknownsOS = (int)paramOwnedAndShared.size();
      int localParamUnknowns = numParamUnknowns;
      
      Teuchos::reduceAll(*Comm,Teuchos::REDUCE_SUM,1,&localParamUnknowns,&globalParamUnknowns);
      //Comm->SumAll(&localParamUnknowns, &globalParamUnknowns, 1);
      
      for (size_t j=0; j<discretized_param_names.size(); j++) {
        int num = paramDOF->getFieldNum(discretized_param_names[j]);
        vector<int> poffsets = paramDOF->getGIDFieldOffsets(blocknames[0],num);
        paramoffsets.push_back(poffsets);
        paramNumBasis.push_back(discretized_param_basis[discretized_param_usebasis[j]]->getCardinality());
      }
      
      for (size_t b=0; b<assembler->cells.size(); b++) {
        int eprog = 0;
        for (size_t e=0; e<assembler->cells[b].size(); e++) {
          vector<vector<int> > GIDs;
          int numElem = assembler->cells[b][e]->numElem;
          for (int p=0; p<numElem; p++) {
            size_t elemID = disc->myElements[b][eprog+p];
            vector<int> localGIDs;
            paramDOF->getElementGIDs(elemID, localGIDs, blocknames[b]);
            GIDs.push_back(localGIDs);
          }
          eprog += numElem;
          assembler->cells[b][e]->paramGIDs = GIDs;
          assembler->cells[b][e]->setParamUseBasis(disc_usebasis, paramNumBasis);
        }
      }
      
      if (discretized_stochastic) { // add the param DOFs as indep. rv's
        for (size_t j=0; j<numParamUnknownsOS; j++) {
          // hard coding for one disc param just to get something working
          stochastic_distribution.push_back(discparam_distribution[0]);
          stochastic_mean.push_back(initialParamValues[0]);
          stochastic_variance.push_back(discparamVariance[0]);
          stochastic_min.push_back(lowerParamBounds[0]);
          stochastic_max.push_back(upperParamBounds[0]);
          
        }
      }
    }
  }
  
  assembler->paramDOF = paramDOF;
  assembler->globalParamUnknowns = globalParamUnknowns;
  size_t maxcomp = 0;
  for (size_t k=0; k<paramvals.size(); k++) {
    if (paramvals[k].size() > maxcomp) {
      maxcomp = paramvals[k].size();
    }
  }
  
  paramvals_KVAD = Kokkos::View<AD**,AssemblyDevice>("parameter values (AD)", paramvals.size(), maxcomp);
  
  // Go through the physics interface to setup the parameters in the physics modules
  // phys->setParameters(paramnames);
  
}
*/
/////////////////////////////////////////////////////////////////////////////
// Read in discretized data from an exodus mesh
/////////////////////////////////////////////////////////////////////////////

void solver::readMeshData(Teuchos::RCP<Teuchos::ParameterList> & settings) {
  string exofile;
  string fname;
  
  
  exofile = settings->sublist("Mesh").get<std::string>("Mesh_File","mesh.exo");
  
  if (Comm->getSize() > 1) {
    stringstream ssProc, ssPID;
    ssProc << Comm->getSize();
    ssPID << Comm->getRank();
    string strProc = ssProc.str();
    string strPID = ssPID.str();
    // this section may need tweaking if the input exodus mesh is
    // spread across 10's, 100's, or 1000's (etc) of processors
    //if (Comm->MyPID() < 10)
    if (false)
    fname = exofile + "." + strProc + ".0" + strPID;
    else
    fname = exofile + "." + strProc + "." + strPID;
  }
  else {
    fname = exofile;
  }
  
  // open exodus file
  int CPU_word_size, IO_word_size, exoid, exo_error;
  int num_dim, num_nods, num_el, num_el_blk, num_ns, num_ss;
  char title[MAX_STR_LENGTH+1];
  float exo_version;
  CPU_word_size = sizeof(ScalarT);
  IO_word_size = 0;
  exoid = ex_open(fname.c_str(), EX_READ, &CPU_word_size,&IO_word_size,
                  &exo_version);
  exo_error = ex_get_init(exoid, title, &num_dim, &num_nods, &num_el,
                          &num_el_blk, &num_ns, &num_ss);
  
  int id = 1; // only one blkid
  int step = 1; // only one time step (for now)
  char elem_type[MAX_STR_LENGTH+1];
  ex_block eblock;
  eblock.id = id;
  eblock.type = EX_ELEM_BLOCK;
  
  exo_error = ex_get_block_param(exoid, &eblock);
  
  int num_el_in_blk = eblock.num_entry;
  int num_node_per_el = eblock.num_nodes_per_entry;
  
  
  // get elem vars
  if (settings->sublist("Mesh").get<bool>("Have Element Data", false)) {
    int num_elem_vars;
    int var_ind;
    numResponses = 1;
    exo_error = ex_get_var_param(exoid, "e", &num_elem_vars);
    for (int i=0; i<num_elem_vars; i++) {
      char varname[MAX_STR_LENGTH+1];
      ScalarT *var_vals = new ScalarT[num_el_in_blk];
      var_ind = i+1;
      exo_error = ex_get_variable_name(exoid, EX_ELEM_BLOCK, var_ind, varname);
      string vname(varname);
      efield_names.push_back(vname);
      size_t found = vname.find("Val");
      if (found != std::string::npos) {
        vector<string> results;
        stringstream sns, snr;
        int ns, nr;
        boost::split(results, vname, [](char u){return u == '_';});
        snr << results[3];
        snr >> nr;
        numResponses = std::max(numResponses,nr);
      }
      efield_vals.push_back(vector<ScalarT>(num_el_in_blk));
      exo_error = ex_get_var(exoid,step,EX_ELEM_BLOCK,var_ind,id,num_el_in_blk,var_vals);
      for (int j=0; j<num_el_in_blk; j++) {
        efield_vals[i][j] = var_vals[j];
      }
      delete [] var_vals;
    }
  }
  
  // assign nodal vars to meas multivector
  if (settings->sublist("Mesh").get<bool>("Have Nodal Data", false)) {
    int *connect = new int[num_el_in_blk*num_node_per_el];
    int edgeconn, faceconn;
    //exo_error = ex_get_elem_conn(exoid, id, connect);
    exo_error = ex_get_conn(exoid, EX_ELEM_BLOCK, id, connect, &edgeconn, &faceconn);
    
    // get nodal vars
    int num_node_vars;
    int var_ind;
    exo_error = ex_get_variable_param(exoid, EX_NODAL, &num_node_vars);
    for (int i=0; i<num_node_vars; i++) {
      char varname[MAX_STR_LENGTH+1];
      ScalarT *var_vals = new ScalarT[num_nods];
      var_ind = i+1;
      exo_error = ex_get_variable_name(exoid, EX_NODAL, var_ind, varname);
      string vname(varname);
      nfield_names.push_back(vname);
      nfield_vals.push_back(vector<ScalarT>(num_nods));
      exo_error = ex_get_var(exoid,step,EX_NODAL,var_ind,0,num_nods,var_vals);
      for (int j=0; j<num_nods; j++) {
        nfield_vals[i][j] = var_vals[j];
      }
      delete [] var_vals;
    }
    
    meas = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // empty solution
    size_t b = 0;
    //meas->sync<HostDevice>();
    auto meas_kv = meas->getLocalView<HostDevice>();
    
    //meas.modify_host();
    int index, dindex;
    vector<vector<int> > curroffsets = phys->offsets[b];
    for( size_t e=0; e<assembler->cells[b].size(); e++ ) {
      for (int n=0; n<numVars[b]; n++) {
        vector<vector<int> > GIDs = assembler->cells[b][e]->GIDs;
        for (int p=0; p<assembler->cells[b][e]->numElem; p++) {
          for( int i=0; i<numBasis[b][n]; i++ ) {
            index = LA_overlapped_map->getLocalElement(GIDs[p][curroffsets[n][i]]);
            dindex = connect[e*num_node_per_el + i] - 1;
            meas_kv(index,0) = nfield_vals[n][dindex];
            //(*meas)[0][index] = nfield_vals[n][dindex];
          }
        }
      }
    }
    //meas.sync<>();
    delete [] connect;
    
  }
  exo_error = ex_close(exoid);
  
}

/////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////

void solver::setupSensors(Teuchos::RCP<Teuchos::ParameterList> & settings) {
  
  
  have_sensor_data = false;
  have_sensor_points = false;
  numSensors = 0;
  
  if (settings->sublist("Mesh").get<bool>("Have Element Data", false)) {
    
    for (size_t i=0; i<assembler->cells[0].size(); i++) {
      vector<Kokkos::View<ScalarT**,HostDevice> > sensorLocations;
      vector<Kokkos::View<ScalarT**,HostDevice> > sensorData;
      int numSensorsInCell = efield_vals[0][i];
      if (numSensorsInCell > 0) {
        assembler->cells[0][i]->mySensorIDs.push_back(numSensors); // hack for dakota
        for (size_t j=0; j<numSensorsInCell; j++) {
          // sensorLocation
          Kokkos::View<ScalarT**,HostDevice> sensor_loc("sensor location",1,spaceDim);
          stringstream ssSensorNum;
          ssSensorNum << j+1;
          string sensorNum = ssSensorNum.str();
          string fieldLocx = "sensor_" + sensorNum + "_Loc_x";
          ptrdiff_t ind_Locx = std::distance(efield_names.begin(), std::find(efield_names.begin(), efield_names.end(), fieldLocx));
          string fieldLocy = "sensor_" + sensorNum + "_Loc_y";
          ptrdiff_t ind_Locy = std::distance(efield_names.begin(), std::find(efield_names.begin(), efield_names.end(), fieldLocy));
          sensor_loc(0,0) = efield_vals[ind_Locx][i];
          sensor_loc(0,1) = efield_vals[ind_Locy][i];
          if (spaceDim > 2) {
            string fieldLocz = "sensor_" + sensorNum + "_Loc_z";
            ptrdiff_t ind_Locz = std::distance(efield_names.begin(), std::find(efield_names.begin(), efield_names.end(), fieldLocz));
            sensor_loc(0,2) = efield_vals[ind_Locz][i];
          }
          // sensorData
          Kokkos::View<ScalarT**,HostDevice> sensor_data("sensor data",1,numResponses+1);
          sensor_data(0,0) = 0.0; // time index
          for (size_t k=1; k<numResponses+1; k++) {
            stringstream ssRespNum;
            ssRespNum << k;
            string respNum = ssRespNum.str();
            string fieldResp = "sensor_" + sensorNum + "_Val_" + respNum;
            ptrdiff_t ind_Resp = std::distance(efield_names.begin(), std::find(efield_names.begin(), efield_names.end(), fieldResp));
            sensor_data(0,k) = efield_vals[ind_Resp][i];
          }
          sensorLocations.push_back(sensor_loc);
          sensorData.push_back(sensor_data);
          numSensors += 1; // solver variable (total number of sensors)
        }
      }
      assembler->cells[0][i]->exodus_sensors = true;
      assembler->cells[0][i]->numSensors = numSensorsInCell;
      assembler->cells[0][i]->sensorLocations = sensorLocations;
      assembler->cells[0][i]->sensorData = sensorData;
    }
    
    Kokkos::View<ScalarT**,HostDevice> tmp_sensor_points;
    vector<Kokkos::View<ScalarT**,HostDevice> > tmp_sensor_data;
    bool have_sensor_data = true;
    ScalarT sensor_loc_tol = 1.0;
    // only needed for passing of basis pointers
    for (size_t j=0; j<assembler->cells[0].size(); j++) {
      assembler->cells[0][j]->addSensors(sensor_points, sensor_loc_tol, sensor_data, have_sensor_data, disc->basis_pointers[0], params->discretized_param_basis);
    }
  }
  else {
    if (settings->sublist("Analysis").get("Have Sensor Data",false)) {
      data sdata("Sensor Measurements", spaceDim, settings->sublist("Analysis").get("Sensor Location File","sensor_points.dat"), settings->sublist("Analysis").get("Sensor Prefix","sensor"));
      sensor_data = sdata.getdata();
      sensor_points = sdata.getpoints();
      numSensors = sensor_points.dimension(0);
      have_sensor_data = true;
      have_sensor_points = true;
    }
    else if (settings->sublist("Analysis").get("Have Sensor Points",false)) {
      data sdata("Sensor Points", spaceDim, settings->sublist("Analysis").get("Sensor Location File","sensor_points.dat"));
      sensor_points = sdata.getpoints();
      numSensors = sensor_points.dimension(0);
      have_sensor_data = false;
      have_sensor_points = true;
    }
    
    if (settings->sublist("Analysis").get("Have Sensor Points",false)) {
      //sensor_locations = FCint(sensor_points.dimension(0),2);
      ScalarT sensor_loc_tol = settings->sublist("Analysis").get("Sensor location tol",1.0E-6);
      for (size_t b=0; b<assembler->cells.size(); b++) {
        for (size_t j=0; j<assembler->cells[b].size(); j++) {
          assembler->cells[b][j]->addSensors(sensor_points, sensor_loc_tol, sensor_data, have_sensor_data, disc->basis_pointers[b], params->discretized_param_basis);
        }
      }
    }
  }  
}

/////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////
/*
int solver::getNumParams(const int & type) {
  int np = 0;
  if (type == 0)
  np = num_inactive_params;
  else if (type == 1)
  np = num_active_params;
  else if (type == 2)
  np = num_stochastic_params;
  else if (type == 3)
  np = num_discrete_params;
  else if (type == 4)
  np = globalParamUnknowns;
  
  return np;
}
*/
/////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////
/*
int solver::getNumParams(const std::string & type) {
  int np = 0;
  if (type == "inactive")
  np = num_inactive_params;
  else if (type == "active")
  np = num_active_params;
  else if (type == "stochastic")
  np = num_stochastic_params;
  else if (type == "discrete")
  np = num_discrete_params;
  else if (type == "discretized")
  np = num_discretized_params;
  
  return np;
}
*/
// ========================================================================================
// return the discretized parameters as vector for use with ROL
// ========================================================================================
/*
vector<ScalarT> solver::getDiscretizedParamsVector() {
  int numParams = this->getNumParams(4);
  vector<ScalarT> discLocalParams(numParams);
  vector<ScalarT> discParams(numParams);
  auto Psol_2d = Psol[0]->getLocalView<HostDevice>();
  
  for (size_t i = 0; i < paramOwned.size(); i++) {
    int gid = paramOwned[i];
    discLocalParams[gid] = Psol_2d(i,0);
  }
  for (size_t i = 0; i < numParams; i++) {
    ScalarT globalval = 0.0;
    ScalarT localval = discLocalParams[i];
    Teuchos::reduceAll(*Comm,Teuchos::REDUCE_SUM,1,&localval,&globalval);
    //Comm->SumAll(&localval, &globalval, 1);
    discParams[i] = globalval;
  }
  return discParams;
}
*/
// ========================================================================================
/* given the parameters, solve the forward  problem */
// ========================================================================================

vector_RCP solver::forwardModel(DFAD & obj) {
  useadjoint = false;
  
  params->sacadoizeParams(false);
  
  // Set the initial condition
  //isInitial = true;
  
  vector_RCP initial = this->setInitial(); // TMW: this will be deprecated soon
  
  vector_RCP I_soln = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // empty solution
  int numsols = 1;
  if (solver_type == "transient") {
    numsols = numsteps+1;
  }
  
  vector_RCP F_soln = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,numsols)); // empty solution
  vector_RCP zero_soln = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // empty solution
  auto initial_2d = initial->getLocalView<HostDevice>();
  auto f_2d = F_soln->getLocalView<HostDevice>();
  
  if (solver_type == "transient") {
    for( size_t i=0; i<LA_ownedAndShared.size(); i++ ) {
      f_2d(i,0) = initial_2d(i,0);
    }
  }
  if (solver_type == "steady-state") {
    
    this->nonlinearSolver(F_soln, zero_soln, zero_soln, zero_soln, 0.0, 1.0);
    if (compute_objective) {
      obj = this->computeObjective(F_soln, 0.0, 0);
    }
    
  }
  else if (solver_type == "transient") {
    vector<ScalarT> gradient; // not really used here
    this->transientSolver(initial, I_soln, F_soln, obj, gradient);
  }
  else {
    // print out an error message
  }
  
  return F_soln;
}

// ========================================================================================
/* given the parameters, solve the fractional forward  problem */
// ========================================================================================

vector_RCP solver::forwardModel_fr(DFAD & obj, ScalarT yt, ScalarT st) {
  useadjoint = false;
  assembler->wkset[0]->y = yt;
  assembler->wkset[0]->s = st;
  params->sacadoizeParams(false);
  
  // Set the initial condition
  //isInitial = true;
  
  vector_RCP initial = this->setInitial(); // TMW: this will be deprecated soon
  
  vector_RCP I_soln = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // empty solution
  int numsols = 1;
  if (solver_type == "transient") {
    numsols = numsteps+1;
  }
  
  vector_RCP F_soln = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,numsols)); // empty solution
  vector_RCP zero_soln = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // empty solution
  
  auto initial_2d = initial->getLocalView<HostDevice>();
  auto f_2d = F_soln->getLocalView<HostDevice>();
  
  if (solver_type == "transient") {
    for( size_t i=0; i<LA_ownedAndShared.size(); i++ ) {
      f_2d(i,0) = initial_2d(i,0);
    }
  }
  if (solver_type == "steady-state") {
    
    vector_RCP SS_soln = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // empty solution
    auto SS_2d = SS_soln->getLocalView<HostDevice>();
    
    for( size_t i=0; i<LA_ownedAndShared.size(); i++ ) {
      SS_2d(i,0) = initial_2d(i,0);
    }
    
    this->nonlinearSolver(SS_soln, zero_soln, zero_soln, zero_soln, 0.0, 1.0);
    for( size_t i=0; i<LA_ownedAndShared.size(); i++ ) {
      f_2d(i,0) = SS_2d(i,0);
    }
    
    if (compute_objective) {
      obj = this->computeObjective(F_soln, 0.0, 0);
    }
    
  }
  else if (solver_type == "transient") {
    vector<ScalarT> gradient; // not really used here
    this->transientSolver(initial, I_soln, F_soln, obj, gradient);
  }
  else {
    // print out an error message
  }
  
  return F_soln;
}

// ========================================================================================
// ========================================================================================

vector_RCP solver::adjointModel(vector_RCP & F_soln, vector<ScalarT> & gradient) {
  useadjoint = true;
  
  params->sacadoizeParams(false);
  
  //isInitial = true;
  vector_RCP initial = setInitial(); // does this need
  // to be updated for adjoint model?
  
  // Solve the forward problem
  int numsols = 1;
  if (solver_type == "transient") {
    numsols = numsteps+1;
  }
  
  vector_RCP zero_soln = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // empty solution
  vector_RCP A_soln = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,numsols)); // empty solution
  
  auto initial_2d = initial->getLocalView<HostDevice>();
  auto asol_2d = A_soln->getLocalView<HostDevice>();
  auto fsol_2d = F_soln->getLocalView<HostDevice>();
  
  for( size_t i=0; i<ownedAndShared.size(); i++ ) {
    asol_2d(i,0) = initial_2d(i,0);
  }
  
  if (solver_type == "steady-state") {
    vector_RCP L_soln = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // empty solution
    vector_RCP SS_soln = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // empty solution
    auto lsol_2d = L_soln->getLocalView<HostDevice>();
    auto SS_2d = SS_soln->getLocalView<HostDevice>();
    
    
    for( size_t i=0; i<ownedAndShared.size(); i++ ) {
      lsol_2d(i,0) = fsol_2d(i,0);
    }
    this->nonlinearSolver(L_soln, zero_soln, SS_soln, zero_soln, 0.0, 1.0);
    for( size_t i=0; i<ownedAndShared.size(); i++ ) {
      asol_2d(i,0) = SS_2d(i,0);
    }
    this->computeSensitivities(F_soln, zero_soln, A_soln, gradient, 0.0, 1.0);
    
  }
  else if (solver_type == "transient") {
    DFAD obj = 0.0;
    this->transientSolver(initial, F_soln, A_soln, obj, gradient);
  }
  else {
    // print out an error message
  }
  
  useadjoint = false;
  return A_soln;
}


// ========================================================================================
/* solve the problem */
// ========================================================================================

void solver::transientSolver(vector_RCP & initial, vector_RCP & L_soln,
                     vector_RCP & SolMat, DFAD & obj, vector<ScalarT> & gradient) {
  vector_RCP u = initial;
  vector_RCP u_dot = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1));
  vector_RCP phi = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1));
  vector_RCP phi_dot = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1));
  
  auto u_kv = u->getLocalView<HostDevice>();
  auto u_dot_kv = u_dot->getLocalView<HostDevice>();
  auto phi_kv = phi->getLocalView<HostDevice>();
  auto phi_dot_kv = phi_dot->getLocalView<HostDevice>();
  
  auto solmat_kv = SolMat->getLocalView<HostDevice>();
  auto lsol_kv = L_soln->getLocalView<HostDevice>();
  
  //int numSteps = 1;
  //ScalarT finaltime = 0.0;
  ScalarT deltat = 0.0;
  
  ScalarT alpha = 0.0;
  ScalarT beta = 1.0;
  
  deltat = finaltime / numsteps;
  if (time_order == 1){
    alpha = 1./deltat;
  }
  else if (time_order == 2) {
    alpha = 3.0/2.0/deltat;
  }
  else {
    alpha = 0.0; // would be better to print out an error message
  }
  
  int numivec = L_soln->getNumVectors();
  
  //ScalarT current_time = 0.0;
  if (useadjoint) {
    current_time = finaltime;
    is_final_time = true;
  }
  else {
    current_time = solvetimes[0];
    is_final_time = false;
  }
  
  // ******************* ITERATE ON THE TIME STEPS **********************
  
  obj = 0.0;
  for (int timeiter = 0; timeiter<numsteps; timeiter++) {
    
    {
      Teuchos::TimeMonitor localtimer(*msprojtimer);
      msprojtimer->start();
      ScalarT my_cost = multiscale_manager->update();
      ScalarT gmin = 0.0;
      Teuchos::reduceAll(*Comm,Teuchos::REDUCE_MIN,1,&my_cost,&gmin);
      //Comm->MinAll(&my_cost, &gmin, 1);
      ScalarT gmax = 0.0;
      Teuchos::reduceAll(*Comm,Teuchos::REDUCE_MAX,1,&my_cost,&gmin);
      //Comm->MaxAll(&my_cost, &gmax, 1);
      
      if(Comm->getRank() == 0 && verbosity>0) {
        cout << "***** Load Balancing Factor " << gmax/gmin <<  endl;
      }
    }
    
    if (!useadjoint) {
      current_time += deltat;
    }
    
    if(Comm->getRank() == 0 && verbosity > 0) {
      cout << endl << endl << "*******************************************************" << endl;
      cout << endl << "**** Beginning Time Step " << timeiter << endl;
      cout << "**** Current time is " << current_time << endl << endl;
      cout << "*******************************************************" << endl << endl << endl;
    }
    
    if (useadjoint) {
      // phi is updated automatically
      // need to update phi_dot, u, u_dot
      for( size_t i=0; i<LA_ownedAndShared.size(); i++ ) {
        u_kv(i,0) = lsol_kv(i,numivec-timeiter-1);
      }
      if (time_order == 1) {
        for( size_t i=0; i<LA_ownedAndShared.size(); i++ ) {
          u_dot_kv(i,0) = alpha*lsol_kv(i,numivec-timeiter-1) - alpha*lsol_kv(i,numivec-timeiter-2);
          //phi_dot[0][i] = alpha*phi[0][i] - alpha*SolMat[timeiter][i];
        }
        phi_dot->putScalar(0.0);
        
      }
      //else if (time_order == 2) { // TMW: not re-implemented yet
      //  for( size_t i=0; i<ownedAndShared.size(); i++ ) {
      //    u_dot[0][i] = alpha*L_soln[numivec-timeiter-1][i] - alpha*L_soln[numivec-timeiter-2][i];
      //    phi_dot[0][i] = alpha*phi[0][i] - alpha*SolMat[timeiter][i];
      //  }
    }
    else {
      // u is updated automatically
      // need to update u_dot (no need to update phi or phi_dot)
      if (time_order == 1 || timeiter == 0) {
        for( size_t i=0; i<LA_ownedAndShared.size(); i++ ) {
          u_dot_kv(i,0) = alpha*u_kv(i,0) - alpha*solmat_kv(i,timeiter);
        }
      }
      else if (time_order == 2) {
        for( size_t i=0; i<LA_ownedAndShared.size(); i++ ) {
          u_dot_kv(i,0) = alpha*u_kv(i,0) - alpha*4.0/3.0*solmat_kv(i,timeiter) + alpha*1.0/3.0*solmat_kv(i,timeiter-1);
        }
      }
    }
    
    this->nonlinearSolver(u, u_dot, phi, phi_dot, alpha, beta);
    
    if (!useadjoint) {
      for( int i=0; i<LA_ownedAndShared.size(); i++ ) {
        solmat_kv(i,timeiter+1) = u_kv(i,0);
      }
    }
    else {
      for( int i=0; i<LA_ownedAndShared.size(); i++ ) {
        solmat_kv(i,timeiter+1) = phi_kv(i,0);
      }
    }
    
    
    //solvetimes.push_back(current_time); - This was causing a bug
    
    if (allow_remesh && !useadjoint) {
      this->remesh(u);
    }
    
    if (useadjoint) { // fill in the gradient
      this->computeSensitivities(u,u_dot,phi,gradient,alpha,beta);
      params->sacadoizeParams(false);
    }
    else if (compute_objective) { // fill in the objective function
      DFAD cobj = this->computeObjective(u, current_time, timeiter);
      obj += cobj;
      params->sacadoizeParams(false);
    }
    
    if (useadjoint) {
      current_time -= deltat;
      is_final_time = false;
    }
    
    //if (subgridModels.size() > 0) { // meaning we have multiscale turned on
    //  // give the assembler->cells the opportunity to change subgrid models for the next time step
    //  for (size_t b=0; b<assembler->cells.size(); b++) {
    //    for (size_t e=0; e<assembler->cells[b].size(); e++) {
    //      assembler->cells[b][e]->updateSubgridModel(subgridModels, phys->udfunc, *(wkset[b]));
    //    }
    //  }
    //}
    //isInitial = false; // only true on first time step
  }
}

// ========================================================================================
// ========================================================================================


void solver::nonlinearSolver(vector_RCP & u, vector_RCP & u_dot,
                     vector_RCP & phi, vector_RCP & phi_dot,
                     const ScalarT & alpha, const ScalarT & beta) {
  
  int NLiter = 0;
  Teuchos::Array<typename Teuchos::ScalarTraits<ScalarT>::magnitudeType> NLerr_first(1);
  Teuchos::Array<typename Teuchos::ScalarTraits<ScalarT>::magnitudeType> NLerr_scaled(1);
  Teuchos::Array<typename Teuchos::ScalarTraits<ScalarT>::magnitudeType> NLerr(1);
  NLerr_first[0] = 10*NLtol;
  NLerr_scaled[0] = NLerr_first[0];
  NLerr[0] = NLerr_first[0];
  
  if (usestrongDBCs) {
    this->setDirichlet(u);
  }
  
  //this->setConstantPin(u); //pinning attempt
  int maxiter = MaxNLiter;
  if (useadjoint) {
    maxiter = 2;
  }
  
  while( NLerr_scaled[0]>NLtol && NLiter<maxiter ) { // while not converged
    
    gNLiter = NLiter;
    
    vector_RCP res = Teuchos::rcp(new LA_MultiVector(LA_owned_map,1));
    matrix_RCP J = Tpetra::createCrsMatrix<ScalarT>(LA_owned_map);
    //matrix_RCP J = Teuchos::rcp(new Tpetra::CrsMatrix<ScalarT,LO,GO,HostNode>(LA_owned_graph));
    vector_RCP res_over = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1));
    //matrix_RCP J_over = Tpetra::createCrsMatrix<ScalarT>(LA_overlapped_map);
    matrix_RCP J_over = Teuchos::rcp(new Tpetra::CrsMatrix<ScalarT,LO,GO,HostNode>(LA_overlapped_graph));
    vector_RCP du = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1));
    vector_RCP du_over = Teuchos::rcp(new LA_MultiVector(LA_owned_map,1));
    
    // *********************** COMPUTE THE JACOBIAN AND THE RESIDUAL **************************
    
    bool build_jacobian = true;
    if (NLsolver == "AA")
    build_jacobian = false;
    
    res_over->putScalar(0.0);
    J_over->setAllToScalar(0.0);
    if ( useadjoint && (NLiter == 1))
      store_adjPrev = true;
    else
      store_adjPrev = false;
    
    //this->computeJacRes(u, u_dot, phi, phi_dot, alpha, beta, build_jacobian, false, false, res_over, J_over);
    assembler->assembleJacRes(u, u_dot, phi, phi_dot, alpha, beta, build_jacobian, false, false,
                              res_over, J_over, isTransient, current_time, useadjoint, store_adjPrev,
                              params->num_active_params, params->Psol[0], is_final_time);
    J_over->fillComplete();
    
    J->setAllToScalar(0.0);
    J->doExport(*J_over, *exporter, Tpetra::ADD);
    J->fillComplete();
    
    //J_over->exportAndFillComplete(J, *exporter);
    
    res->putScalar(0.0);
    res->doExport(*res_over, *exporter, Tpetra::ADD);
    //KokkosTools::print(Comm,res);
    
    //auto out = Teuchos::getFancyOStream (Teuchos::rcpFromRef (std::cout));
    //J->describe(*out, Teuchos::VERB_EXTREME);
    
    // *********************** CHECK THE NORM OF THE RESIDUAL **************************
    if (NLiter == 0) {
      res->normInf(NLerr_first);
      if (NLerr_first[0] > 1.0e-14)
        NLerr_scaled[0] = 1.0;
      else
        NLerr_scaled[0] = 0.0;
    }
    else {
      res->normInf(NLerr);
      NLerr_scaled[0] = NLerr[0]/NLerr_first[0];
    }
    
    if(Comm->getRank() == 0 && verbosity > 1) {
      cout << endl << "*********************************************************" << endl;
      cout << "***** Iteration: " << NLiter << endl;
      cout << "***** Norm of nonlinear residual: " << NLerr[0] << endl;
      cout << "***** Scaled Norm of nonlinear residual: " << NLerr_scaled[0] << endl;
      cout << "*********************************************************" << endl;
    }
    
    // *********************** SOLVE THE LINEAR SYSTEM **************************
    
    if (NLerr_scaled[0] > NLtol) {
      
      this->linearSolver(J, res, du_over);
      
      //du->doImport(*du_over, *importer, Tpetra::ADD);
      du->doImport(*du_over, *importer, Tpetra::ADD);
      
      if (useadjoint) {
        phi->update(1.0, *du, 1.0);
        phi_dot->update(alpha, *du, 1.0);
      }
      else {
        u->update(1.0, *du, 1.0);
        u_dot->update(alpha, *du, 1.0);
      }
      //Teuchos::Array<typename Teuchos::ScalarTraits<ScalarT>::magnitudeType> ncheck(1);
      //u->normInf(ncheck);
      //cout << "Norm of solution: " << ncheck[0] << endl;
      //KokkosTools::print(Comm,u);
      
      /*
       if (line_search) {
       ScalarT err0 = NLerr;
       res_over.PutScalar(0.0);
       this->computeJacRes(u, u_dot, phi, phi_dot, alpha, beta, false, false, false, res_over, J_over);
       res.PutScalar(0.0);
       res.Export(res_over, *exporter, Add);
       
       ScalarT err1;
       res.NormInf(&err1);
       
       if (useadjoint) {
       phi.Update(-0.5, du, 1.0);
       phi_dot.Update(-0.5*alpha, du, 1.0);
       }
       else {
       u.Update(-0.5, du, 1.0);
       u_dot.Update(-0.5*alpha, du, 1.0);
       }
       res_over.PutScalar(0.0);
       this->computeJacRes(u, u_dot, phi, phi_dot, alpha, beta, false, false, false, res_over, J_over);
       res.PutScalar(0.0);
       res.Export(res_over, *exporter, Add);
       
       ScalarT errhalf;
       res.NormInf(&errhalf);
       
       ScalarT opt_alpha = -(-3.0*err0+4.0*errhalf-err1) / (2.0*(2.0*err0-4.0*errhalf+2.9*err1));
       if (opt_alpha > 1.0)
       opt_alpha = 1.0;
       else if (opt_alpha < 0.0)
       opt_alpha = 0.1;
       
       if(Comm->MyPID() == 0 && verbosity > 10) {
       cout << "Optimal step size: " << opt_alpha << endl;
       cout << "err0 " << err0 << endl;
       cout << "errhalf " << errhalf << endl;
       cout << "err1 " << err1 << endl;
       }
       
       if (useadjoint) {
       phi.Update(opt_alpha-0.5, du, 1.0);
       phi_dot.Update(alpha*(opt_alpha-0.5), du, 1.0);
       }
       else {
       u.Update(opt_alpha-0.5, du, 1.0);
       u_dot.Update(alpha*(opt_alpha-0.5), du, 1.0);
       }
       
       }*/
      
    }
    
    NLiter++; // increment number of iterations
  } // while loop
  
  if(Comm->getRank() == 0) {
    if (!useadjoint) {
      if( (NLiter>MaxNLiter || NLerr_scaled[0]>NLtol) && verbosity > 1) {
        cout << endl << endl << "********************" << endl;
        cout << endl << "SOLVER FAILED TO CONVERGE CONVERGED in " << NLiter
        << " iterations with residual norm " << NLerr[0] << endl;
        cout << "********************" << endl;
      }
    }
  }
  
}


// ========================================================================================
// ========================================================================================

void solver::remesh(const vector_RCP & u) {
  
  
  auto u_kv = u->getLocalView<HostDevice>();
  
  for (size_t b=0; b<assembler->cells.size(); b++) {
    for( size_t e=0; e<assembler->cells[b].size(); e++ ) {
      vector<vector<int> > GIDs = assembler->cells[b][e]->GIDs;
      DRV nodes = assembler->cells[b][e]->nodes;
      vector<vector<int> > offsets = phys->offsets[b];
      bool changed = false;
      for (int p=0; p<assembler->cells[b][e]->numElem; p++) {
        
        for( int i=0; i<nodes.dimension(1); i++ ) {
          if (meshmod_xvar >= 0) {
            int pindex = LA_overlapped_map->getLocalElement(GIDs[p][offsets[meshmod_xvar][i]]);
            ScalarT xval = u_kv(pindex,0);
            ScalarT xpert = xval;
            if (meshmod_usesmoother)
            xpert = meshmod_layer_size*(1.0/3.14159*atan(100.0*(xval-meshmod_center)+0.5));
            
            if (xpert > meshmod_TOL) {
              nodes(p,i,0) += xpert;
              changed = true;
            }
          }
          if (meshmod_yvar >= 0) {
            int pindex = LA_overlapped_map->getLocalElement(GIDs[p][offsets[meshmod_yvar][i]]);
            ScalarT yval = u_kv(pindex,0);
            ScalarT ypert = yval;
            if (meshmod_usesmoother)
            ypert = meshmod_layer_size*(1.0/3.14159*atan(100.0*(yval-meshmod_center)+0.5));
            
            if (ypert > meshmod_TOL) {
              nodes(p,i,1) += ypert;
              changed = true;
            }
          }
          if (meshmod_zvar >= 0) {
            int pindex = LA_overlapped_map->getLocalElement(GIDs[p][offsets[meshmod_zvar][i]]);
            ScalarT zval = u_kv(pindex,0);
            ScalarT zpert = zval;
            if (meshmod_usesmoother)
            zpert = meshmod_layer_size*(1.0/3.14159*atan(100.0*(zval-meshmod_center)+0.5));
            
            if (zpert > meshmod_TOL) {
              nodes(p,i,2) += zpert;
              changed = true;
            }
          }
          if (changed) {
            assembler->cells[b][e]->nodes = nodes;
          }
        }
        
      }
    }
  }
}

// ========================================================================================
// ========================================================================================

DFAD solver::computeObjective(const vector_RCP & F_soln, const ScalarT & time, const size_t & tindex) {
  
  DFAD totaldiff = 0.0;
  AD regDomain = 0.0;
  AD regBoundary = 0.0;
  int numDomainParams = params->domainRegIndices.size();
  int numBoundaryParams = params->boundaryRegIndices.size();
  
  params->sacadoizeParams(true);
  
  int numParams = params->num_active_params + params->globalParamUnknowns;
  vector<ScalarT> regGradient(numParams);
  vector<ScalarT> dmGradient(numParams);
  
  for (size_t b=0; b<assembler->cells.size(); b++) {
    
    assembler->performGather(b, F_soln, 0, 0);
    assembler->performGather(b, params->Psol[0], 4, 0);
    
    for (size_t e=0; e<assembler->cells[b].size(); e++) {
      
      Kokkos::View<AD**,AssemblyDevice> obj = assembler->cells[b][e]->computeObjective(time, tindex, 0);
      vector<vector<int> > paramGIDs = assembler->cells[b][e]->paramGIDs;
      int numElem = assembler->cells[b][e]->numElem;
      
      if (obj.dimension(1) > 0) {
        for (int c=0; c<numElem; c++) {
          for (size_t i=0; i<obj.dimension(1); i++) {
            totaldiff += obj(c,i);
            if (params->num_active_params > 0) {
              if (obj(c,i).size() > 0) {
                ScalarT val;
                val = obj(c,i).fastAccessDx(0);
                dmGradient[0] += val;
              }
            }
            
            if (params->globalParamUnknowns > 0) {
              for (int row=0; row<params->paramoffsets[0].size(); row++) {
                int rowIndex = paramGIDs[c][params->paramoffsets[0][row]];
                int poffset = params->paramoffsets[0][row];
                ScalarT val;
                if (obj(c,i).size() > params->num_active_params) {
                  val = obj(c,i).fastAccessDx(poffset+params->num_active_params);
                  dmGradient[rowIndex+params->num_active_params] += val;
                }
              }
            }
          }
        }
      }
      
      if ((numDomainParams > 0) || (numBoundaryParams > 0)) {
        
        vector<vector<int> > paramGIDs = assembler->cells[b][e]->paramGIDs;
        
        if (numDomainParams > 0) {
          int paramIndex, rowIndex, poffset;
          ScalarT val;
          regDomain = assembler->cells[b][e]->computeDomainRegularization(params->domainRegConstants,
                                                                          params->domainRegTypes,
                                                                          params->domainRegIndices);
          
          for (int c=0; c<numElem; c++) {
            for (size_t p = 0; p < numDomainParams; p++) {
              paramIndex = params->domainRegIndices[p];
              for( size_t row=0; row<params->paramoffsets[paramIndex].size(); row++ ) {
                if (regDomain.size() > 0) {
                  rowIndex = paramGIDs[c][params->paramoffsets[paramIndex][row]];
                  poffset = params->paramoffsets[paramIndex][row];
                  val = regDomain.fastAccessDx(poffset);
                  regGradient[rowIndex+params->num_active_params] += val;
                }
              }
            }
          }
        }
        
      
        if (numBoundaryParams > 0) {
          int paramIndex, rowIndex, poffset;
          ScalarT val;
          
          regBoundary = assembler->cells[b][e]->computeBoundaryRegularization(params->boundaryRegConstants,
                                                                              params->boundaryRegTypes,
                                                                              params->boundaryRegIndices,
                                                                              params->boundaryRegSides);
          for (int c=0; c<numElem; c++) {
            for (size_t p = 0; p < numBoundaryParams; p++) {
              paramIndex = params->boundaryRegIndices[p];
              for( size_t row=0; row<params->paramoffsets[paramIndex].size(); row++ ) {
                if (regBoundary.size() > 0) {
                  rowIndex = paramGIDs[c][params->paramoffsets[paramIndex][row]];
                  poffset = params->paramoffsets[paramIndex][row];
                  val = regBoundary.fastAccessDx(poffset);
                  regGradient[rowIndex+params->num_active_params] += val;
                }
              }
            }
          }
        }
        
        
        totaldiff += (regDomain + regBoundary);
        
      }
      
    }
    //totaldiff += phys->computeTopoResp(b);
  }
  
  //to gather contributions across processors
  ScalarT meep = 0.0;
  Teuchos::reduceAll(*Comm,Teuchos::REDUCE_SUM,1,&totaldiff.val(),&meep);
  //Comm->SumAll(&totaldiff.val(), &meep, 1);
  totaldiff.val() = meep;
  
  DFAD fullobj(numParams,meep);
  
  for (size_t j=0; j< numParams; j++) {
    ScalarT dval;
    ScalarT ldval = dmGradient[j] + regGradient[j];
    Teuchos::reduceAll(*Comm,Teuchos::REDUCE_SUM,1,&ldval,&dval);
    //Comm->SumAll(&ldval,&dval,1);
    fullobj.fastAccessDx(j) = dval;
  }
  
  return fullobj;
  
}

// ========================================================================================
// ========================================================================================

vector<ScalarT> solver::computeSensitivities(const vector_RCP & GF_soln,
                                    const vector_RCP & GA_soln) {
  if(Comm->getRank() == 0 && verbosity>0) {
    cout << endl << "*********************************************************" << endl;
    cout << "***** Computing Sensitivities ******" << endl << endl;
  }
  
  vector_RCP u = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // forward solution
  vector_RCP a2 = Teuchos::rcp(new LA_MultiVector(LA_owned_map,1)); // adjoint solution
  vector_RCP u_dot = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // previous solution (can be either fwd or adj)
  
  auto u_kv = u->getLocalView<HostDevice>();
  auto a2_kv = a2->getLocalView<HostDevice>();
  auto u_dot_kv = u_dot->getLocalView<HostDevice>();
  auto GF_kv = GF_soln->getLocalView<HostDevice>();
  auto GA_kv = GA_soln->getLocalView<HostDevice>();
  
  ScalarT alpha = 0.0;
  ScalarT beta = 1.0;
  
  vector<ScalarT> gradient(params->num_active_params);
  
  params->sacadoizeParams(true);
  
  vector<ScalarT> localsens(params->num_active_params);
  ScalarT globalsens = 0.0;
  int nsteps = 1;
  if (isTransient)
  nsteps = solvetimes.size()-1;
  
  for (int timeiter = 0; timeiter<nsteps; timeiter++) {
    
    if (isTransient) {
      current_time = solvetimes[timeiter+1];
      for( size_t i=0; i<LA_ownedAndShared.size(); i++ ) {
        u_dot_kv(i,0) = alpha*(GF_kv(i,timeiter+1) - GF_kv(i,timeiter));
        u_kv(i,0) = GF_kv(i,timeiter+1);
      }
      for( size_t i=0; i<LA_owned.size(); i++ ) {
        a2_kv(i,0) = GA_kv(i,nsteps-timeiter);
      }
    }
    else {
      current_time = solvetimes[timeiter];
      for( size_t i=0; i<LA_ownedAndShared.size(); i++ ) {
        u_kv(i,0) = GF_kv(i,timeiter);
      }
      for( size_t i=0; i<LA_owned.size(); i++ ) {
        a2_kv(i,0) = GA_kv(i,nsteps-timeiter-1);
      }
    }
    
    
    vector_RCP res = Teuchos::rcp(new LA_MultiVector(LA_owned_map,params->num_active_params)); // reset residual
    matrix_RCP J = Tpetra::createCrsMatrix<ScalarT>(LA_owned_map); // reset Jacobian
    vector_RCP res_over = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,params->num_active_params)); // reset residual
    matrix_RCP J_over = Tpetra::createCrsMatrix<ScalarT>(LA_overlapped_map); // reset Jacobian
    res_over->putScalar(0.0);
    
    //this->computeJacRes(u, u_dot, u, u_dot, alpha, beta, false, true, false, res_over, J_over);
    assembler->assembleJacRes(u, u_dot, u, u_dot, alpha, beta, false, true, false,
                              res_over, J_over, isTransient, current_time, useadjoint, store_adjPrev,
                              params->num_active_params, params->Psol[0], is_final_time);
    
    res->putScalar(0.0);
    res->doExport(*res_over, *exporter, Tpetra::ADD);
    
    auto res_kv = res->getLocalView<HostDevice>();
    
    for (size_t paramiter=0; paramiter < params->num_active_params; paramiter++) {
      ScalarT currsens = 0.0;
      for( size_t i=0; i<LA_owned.size(); i++ ) {
        currsens += a2_kv(i,0) * res_kv(i,paramiter);
      }
      localsens[paramiter] -= currsens;
    }
  }
  
  ScalarT localval = 0.0;
  ScalarT globalval = 0.0;
  for (size_t paramiter=0; paramiter < params->num_active_params; paramiter++) {
    localval = localsens[paramiter];
    Teuchos::reduceAll(*Comm,Teuchos::REDUCE_SUM,1,&localval,&globalval);
    //Comm->SumAll(&localval, &globalval, 1);
    gradient[paramiter] = globalval;
  }
  
  if(Comm->getRank() == 0 && batchID == 0) {
    stringstream ss;
    std::string sname2 = "sens.dat";
    ofstream sensOUT(sname2.c_str());
    sensOUT.precision(16);
    for (size_t paramiter=0; paramiter < params->num_active_params; paramiter++) {
      sensOUT << gradient[paramiter] << "  ";
    }
    sensOUT << endl;
    sensOUT.close();
  }
  
  return gradient;
}


// ========================================================================================
// Compute the sensitivity of the objective with respect to discretized parameters
// ========================================================================================

vector<ScalarT> solver::computeDiscretizedSensitivities(const vector_RCP & F_soln,
                                               const vector_RCP & A_soln) {
  
  if(Comm->getRank() == 0 && verbosity>0) {
    cout << endl << "*********************************************************" << endl;
    cout << "***** Computing Discretized Sensitivities ******" << endl << endl;
  }
  auto F_kv = F_soln->getLocalView<HostDevice>();
  auto A_kv = A_soln->getLocalView<HostDevice>();
  
  vector_RCP u = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // forward solution
  vector_RCP a2 = Teuchos::rcp(new LA_MultiVector(LA_owned_map,1)); // adjoint solution
  vector_RCP u_dot = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // previous solution (can be either fwd or adj)
  
  auto u_kv = u->getLocalView<HostDevice>();
  auto a2_kv = a2->getLocalView<HostDevice>();
  auto u_dot_kv = u_dot->getLocalView<HostDevice>();
  
  ScalarT alpha = 0.0;
  ScalarT beta = 1.0;
  
  params->sacadoizeParams(false);
  
  int nsteps = 1;
  if (isTransient) {
    nsteps = solvetimes.size()-1;
  }
  
  vector_RCP totalsens = Teuchos::rcp(new LA_MultiVector(params->param_owned_map,1));
  auto tsens_kv = totalsens->getLocalView<HostDevice>();
  
  
  for (int timeiter = 0; timeiter<nsteps; timeiter++) {
    
    if (isTransient) {
      current_time = solvetimes[timeiter+1];
      for( size_t i=0; i<LA_ownedAndShared.size(); i++ ) {
        u_dot_kv(i,0) = alpha*(F_kv(i,timeiter+1) - F_kv(i,timeiter));
        u_kv(i,0) = F_kv(i,timeiter+1);
      }
      for( size_t i=0; i<LA_owned.size(); i++ ) {
        a2_kv(i,0) = A_kv(i,nsteps-timeiter);
      }
    }
    else {
      current_time = solvetimes[timeiter];
      for( size_t i=0; i<LA_ownedAndShared.size(); i++ ) {
        u_kv(i,0) = F_kv(i,timeiter);
      }
      for( size_t i=0; i<LA_owned.size(); i++ ) {
        a2_kv(i,0) = A_kv(i,nsteps-timeiter-1);
      }
    }
    /*
     current_time = solvetimes[timeiter+1];
     for( size_t i=0; i<ownedAndShared.size(); i++ ) {
     u[0][i] = F_soln[timeiter+1][i];
     u_dot[0][i] = alpha*(F_soln[timeiter+1][i] - F_soln[timeiter][i]);
     }
     for( size_t i=0; i<owned.size(); i++ ) {
     a2[0][i] = A_soln[nsteps-timeiter][i];
     }
     */
    
    vector_RCP res_over = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // reset residual
    matrix_RCP J_over = Tpetra::createCrsMatrix<ScalarT>(params->param_overlapped_map); // reset Jacobian
    matrix_RCP J = Tpetra::createCrsMatrix<ScalarT>(params->param_owned_map); // reset Jacobian
    //this->computeJacRes(u, u_dot, u, u_dot, alpha, beta, true, false, true, res_over, J_over);
    assembler->assembleJacRes(u, u_dot, u, u_dot, alpha, beta, true, false, true,
                              res_over, J_over, isTransient, current_time, useadjoint, store_adjPrev,
                              params->num_active_params, params->Psol[0], is_final_time);
    
    J_over->fillComplete(LA_owned_map, params->param_owned_map);
    vector_RCP sens_over = Teuchos::rcp(new LA_MultiVector(params->param_overlapped_map,1)); // reset residual
    vector_RCP sens = Teuchos::rcp(new LA_MultiVector(params->param_owned_map,1)); // reset residual
    
    J->setAllToScalar(0.0);
    J->doExport(*J_over, *(params->param_exporter), Tpetra::ADD);
    J->fillComplete(LA_owned_map, params->param_owned_map);
    
    J->apply(*a2,*sens);
    
    totalsens->update(1.0, *sens, 1.0);
  }
  
  params->dRdP.push_back(totalsens);
  params->have_dRdP = true;
  
  int numParams = params->getNumParams(4);
  vector<ScalarT> discLocalGradient(numParams);
  vector<ScalarT> discGradient(numParams);
  for (size_t i = 0; i < params->paramOwned.size(); i++) {
    int gid = params->paramOwned[i];
    discLocalGradient[gid] = tsens_kv(i,0);
  }
  for (size_t i = 0; i < numParams; i++) {
    ScalarT globalval = 0.0;
    ScalarT localval = discLocalGradient[i];
    Teuchos::reduceAll(*Comm,Teuchos::REDUCE_SUM,1,&localval,&globalval);
    //Comm->SumAll(&localval, &globalval, 1);
    discGradient[i] = globalval;
  }
  return discGradient;
}


// ========================================================================================
// ========================================================================================

void solver::computeSensitivities(vector_RCP & u, vector_RCP & u_dot,
                          vector_RCP & a2, vector<ScalarT> & gradient,
                          const ScalarT & alpha, const ScalarT & beta) {
  
  DFAD obj_sens = this->computeObjective(u, current_time, 0);
  
  auto u_kv = u->getLocalView<HostDevice>();
  auto u_dot_kv = u_dot->getLocalView<HostDevice>();
  auto a2_kv = a2->getLocalView<HostDevice>();
  
  if (params->num_active_params > 0) {
  
    params->sacadoizeParams(true);
    
    vector<ScalarT> localsens(params->num_active_params);
    ScalarT globalsens = 0.0;
    
    vector_RCP res = Teuchos::rcp(new LA_MultiVector(LA_owned_map,params->num_active_params)); // reset residual
    matrix_RCP J = Tpetra::createCrsMatrix<ScalarT>(LA_owned_map); // reset Jacobian
    vector_RCP res_over = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,params->num_active_params)); // reset residual
    matrix_RCP J_over = Tpetra::createCrsMatrix<ScalarT>(LA_overlapped_map); // reset Jacobian
    
    auto res_kv = res->getLocalView<HostDevice>();
    
    res_over->putScalar(0.0);
    
    bool curradjstatus = useadjoint;
    useadjoint = false;
    
    //this->computeJacRes(u, u_dot, u, u_dot, alpha, beta, false, true, false, res_over, J_over);
    assembler->assembleJacRes(u, u_dot, u, u_dot, alpha, beta, false, true, false,
                              res_over, J_over, isTransient, current_time, useadjoint, store_adjPrev,
                              params->num_active_params, params->Psol[0], is_final_time);
    useadjoint = curradjstatus;
    
    res->putScalar(0.0);
    res->doExport(*res_over, *exporter, Tpetra::ADD);
  
    for (size_t paramiter=0; paramiter < params->num_active_params; paramiter++) {
      // fine-scale
      if (assembler->cells[0][0]->multiscale) {
        ScalarT subsens = 0.0;
        for (size_t b=0; b<assembler->cells.size(); b++) {
          for (size_t e=0; e<assembler->cells[b].size(); e++) {
            subsens = -assembler->cells[b][e]->subgradient(0,paramiter);
            localsens[paramiter] += subsens;
          }
        }
      }
      else { // coarse-scale
      
        ScalarT currsens = 0.0;
        for( size_t i=0; i<LA_owned.size(); i++ ) {
          currsens += a2_kv(i,0) * res_kv(i,paramiter);
        }
        localsens[paramiter] = -currsens;
      }
      
    }
    
    
    ScalarT localval = 0.0;
    ScalarT globalval = 0.0;
    for (size_t paramiter=0; paramiter < params->num_active_params; paramiter++) {
      localval = localsens[paramiter];
      Teuchos::reduceAll(*Comm,Teuchos::REDUCE_SUM,1,&localval,&globalval);
      //Comm->SumAll(&localval, &globalval, 1);
      ScalarT cobj = 0.0;
      if (paramiter<obj_sens.size()) {
        cobj = obj_sens.fastAccessDx(paramiter);
      }
      globalval += cobj;
      if (gradient.size()<=paramiter) {
        gradient.push_back(globalval);
      }
      else {
        gradient[paramiter] += globalval;
      }
    }
  }
  
  int numDiscParams = params->getNumParams(4);
  
  if (numDiscParams > 0) {
    params->sacadoizeParams(false);
    
    
    vector_RCP a_owned = Teuchos::rcp(new LA_MultiVector(LA_owned_map,1)); // adjoint solution
    auto ao_kv = a_owned->getLocalView<HostDevice>();
    
    for( size_t i=0; i<LA_owned.size(); i++ ) {
      ao_kv(i,0) = a2_kv(i,0);
    }
    
    vector_RCP res_over = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // reset residual
    matrix_RCP J_over = Tpetra::createCrsMatrix<ScalarT>(params->param_overlapped_map); // reset Jacobian
    matrix_RCP J = Tpetra::createCrsMatrix<ScalarT>(params->param_owned_map); // reset Jacobian
    
    res_over->putScalar(0.0);
    J->setAllToScalar(0.0);
    J_over->setAllToScalar(0.0);
    
    //this->computeJacRes(u, u_dot, u, u_dot, alpha, beta, true, false, true, res_over, J_over);
    assembler->assembleJacRes(u, u_dot, u, u_dot, alpha, beta, true, false, true,
                              res_over, J_over, isTransient, current_time, useadjoint, store_adjPrev,
                              params->num_active_params, params->Psol[0], is_final_time);
    J_over->fillComplete(LA_owned_map, params->param_owned_map);
    
    vector_RCP sens_over = Teuchos::rcp(new LA_MultiVector(params->param_overlapped_map,1)); // reset residual
    vector_RCP sens = Teuchos::rcp(new LA_MultiVector(params->param_owned_map,1)); // reset residual
    auto sens_kv = sens->getLocalView<HostDevice>();
    
    J->setAllToScalar(0.0);
    J->doExport(*J_over, *(params->param_exporter), Tpetra::ADD);
    J->fillComplete(LA_owned_map, params->param_owned_map);
    
    J->apply(*a_owned,*sens);
    
    vector<ScalarT> discLocalGradient(numDiscParams);
    vector<ScalarT> discGradient(numDiscParams);
    for (size_t i = 0; i < params->paramOwned.size(); i++) {
      int gid = params->paramOwned[i];
      discLocalGradient[gid] = sens_kv(i,0);
    }
    for (size_t i = 0; i < numDiscParams; i++) {
      ScalarT globalval = 0.0;
      ScalarT localval = discLocalGradient[i];
      Teuchos::reduceAll(*Comm,Teuchos::REDUCE_SUM,1,&localval,&globalval);
      //Comm->SumAll(&localval, &globalval, 1);
      ScalarT cobj = 0.0;
      if ((i+params->num_active_params)<obj_sens.size()) {
        cobj = obj_sens.fastAccessDx(i+params->num_active_params);
      }
      globalval += cobj;
      if (gradient.size()<=params->num_active_params+i) {
        gradient.push_back(globalval);
      }
      else {
        gradient[params->num_active_params+i] += globalval;
      }
    }
  }
}

// ========================================================================================
// The following function is the adjoint-based error estimate
// Not to be confused with the postprocess::computeError function which uses a true
//   solution to perform verification studies
// ========================================================================================

ScalarT solver::computeError(const vector_RCP & GF_soln, const vector_RCP & GA_soln) {
  if(Comm->getRank() == 0 && verbosity>0) {
    cout << endl << "*********************************************************" << endl;
    cout << "***** Computing Error Estimate ******" << endl << endl;
  }
  
  auto GF_kv = GF_soln->getLocalView<HostDevice>();
  auto GA_kv = GA_soln->getLocalView<HostDevice>();
  
  vector_RCP u = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // forward solution
  //LA_MultiVector A_soln(*LA_overlapped_map,1); // adjoint solution
  vector_RCP a = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // adjoint solution
  vector_RCP a2 = Teuchos::rcp(new LA_MultiVector(LA_owned_map,1)); // adjoint solution
  vector_RCP u_dot = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // previous solution (can be either fwd or adj)
  
  auto u_kv = u->getLocalView<HostDevice>();
  auto a_kv = a->getLocalView<HostDevice>();
  auto a2_kv = a2->getLocalView<HostDevice>();
  auto u_dot_kv = u_dot->getLocalView<HostDevice>();
  
  
  ScalarT deltat = 0.0;
  ScalarT alpha = 0.0;
  ScalarT beta = 1.0;
  if (isTransient) {
    deltat = finaltime / numsteps;
    alpha = 1./deltat;
  }
  
  ScalarT errorest = 0.0;
  params->sacadoizeParams(false);
  
  // ******************* ITERATE ON THE Parameters **********************
  
  current_time = 0.0;
  ScalarT localerror = 0.0;
  for (int timeiter = 0; timeiter<numsteps; timeiter++) {
    
    current_time += deltat;
    
    for( size_t i=0; i<LA_ownedAndShared.size(); i++ ) {
      u_kv(i,0) = GF_kv(i,timeiter+1);
      u_dot_kv(i,0) = alpha*(GF_kv(i,timeiter+1) - GF_kv(i,timeiter));
    }
    for( size_t i=0; i<LA_owned.size(); i++ ) {
      a2_kv(i,0) = GA_kv(i,numsteps-timeiter);
    }
    
    vector_RCP res = Teuchos::rcp(new LA_MultiVector(LA_owned_map,params->num_active_params)); // reset residual
    vector_RCP res_over = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,params->num_active_params)); // reset residual
    matrix_RCP J_over = Tpetra::createCrsMatrix<ScalarT>(LA_overlapped_map); // reset Jacobian
    res_over->putScalar(0.0);
    //this->computeJacRes(u, u_dot, u, u_dot, alpha, beta, false, false, false, res_over, J_over);
    assembler->assembleJacRes(u, u_dot, u, u_dot, alpha, beta, false, false, false,
                              res_over, J_over, isTransient, current_time, useadjoint, store_adjPrev,
                              params->num_active_params, params->Psol[0], is_final_time);
    res->putScalar(0.0);
    res->doExport(*res_over, *exporter, Tpetra::ADD);
    auto res_kv = res->getLocalView<HostDevice>();
    
    ScalarT currerror = 0.0;
    for( size_t i=0; i<LA_owned.size(); i++ ) {
      currerror += a2_kv(i,0) * res_kv(i,0);
    }
    localerror += currerror;
  }
  Teuchos::reduceAll(*Comm,Teuchos::REDUCE_SUM,1,&localerror,&errorest);
  //Comm->SumAll(&localerror, &errorest, 1);
  
  if(Comm->getRank() == 0 && verbosity>0) {
    cout << "Error estimate = " << errorest << endl;
  }
  return errorest;
}

// ========================================================================================
// ========================================================================================

void solver::setDirichlet(vector_RCP & initial) {
  
  auto init_kv = initial->getLocalView<HostDevice>();
  //auto meas_kv = meas->getLocalView<HostDevice>();
  
  // TMW: this function needs to be fixed
  vector<vector<int> > fixedDOFs = phys->dbc_dofs;
  
  for (size_t b=0; b<blocknames.size(); b++) {
    string blockID = blocknames[b];
    Kokkos::View<int**,HostDevice> side_info;
    
    for (int n=0; n<numVars[b]; n++) {
      
      vector<size_t> localDirichletSideIDs = phys->localDirichletSideIDs[b][n];
      vector<size_t> boundDirichletElemIDs = phys->boundDirichletElemIDs[b][n];
      int fnum = DOF->getFieldNum(varlist[b][n]);
      for( size_t e=0; e<disc->myElements[b].size(); e++ ) { // loop through all the elements
        side_info = phys->getSideInfo(b,n,e);
        int numSides = side_info.dimension(0);
        DRV I_elemNodes = this->getElemNodes(b,e);//assembler->cells[b][e]->nodes;
        // enforce the boundary conditions if the element is on the given boundary
        
        for( int i=0; i<numSides; i++ ) {
          if( side_info(i,0)==1 ) {
            vector<int> elemGIDs;
            int gside_index = side_info(i,1);
            string gside = phys->sideSets[gside_index];
            size_t elemID = disc->myElements[b][e];
            DOF->getElementGIDs(elemID, elemGIDs, blockID); // global index of each node
            // get the side index and the node->global mapping for the side that is on the boundary
            const pair<vector<int>,vector<int> > SideIndex = DOF->getGIDFieldOffsets_closure(blockID, fnum, spaceDim-1, i);
            const vector<int> elmtOffset = SideIndex.first;
            const vector<int> basisIdMap = SideIndex.second;
            // for each node that is on the boundary side
            for( size_t j=0; j<elmtOffset.size(); j++ ) {
              // get the global row and coordinate
              int row =  LA_overlapped_map->getLocalElement(elemGIDs[elmtOffset[j]]);
              ScalarT x = I_elemNodes(0,basisIdMap[j],0);
              ScalarT y = 0.0;
              if (spaceDim > 1) {
                y = I_elemNodes(0,basisIdMap[j],1);
              }
              ScalarT z = 0.0;
              if (spaceDim > 2) {
                z = I_elemNodes(0,basisIdMap[j],2);
              }
              
              if (use_meas_as_dbcs) {
                //init_kv(row,0) = meas_kv(row,0);
              }
              else {
                // put the value into the soln vector
                AD diri_FAD_tmp;
                diri_FAD_tmp = phys->getDirichletValue(b, x, y, z, current_time, varlist[b][n], gside, useadjoint, assembler->wkset[b]);
                
                init_kv(row,0) = diri_FAD_tmp.val();
              }
            }
          }
        }
      }
    }
    // set point dbcs
    vector<int> dbc_dofs = fixedDOFs[b];
    
    for (int i = 0; i < dbc_dofs.size(); i++) {
      int row = LA_overlapped_map->getLocalElement(dbc_dofs[i]);
      init_kv(row,0) = 0.0; // fix to zero for now
    }
    
  }
  
}

// ========================================================================================
// ========================================================================================

vector_RCP solver::setInitialParams() {
  vector_RCP initial = Teuchos::rcp(new LA_MultiVector(params->param_overlapped_map,1));
  ScalarT value = 2.0;
  initial->putScalar(value);
  return initial;
}

// ========================================================================================
// ========================================================================================

vector_RCP solver::setInitial() {
  
  vector_RCP initial = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1));
  vector_RCP glinitial = Teuchos::rcp(new LA_MultiVector(LA_owned_map,1));
  initial->putScalar(0.0);
  
  if (initial_type == "L2-projection") {
    
    // Compute the L2 projection of the initial data into the discrete space
    vector_RCP rhs = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,1)); // reset residual
    matrix_RCP mass = Tpetra::createCrsMatrix<ScalarT>(LA_overlapped_map); // reset Jacobian
    vector_RCP glrhs = Teuchos::rcp(new LA_MultiVector(LA_owned_map,1)); // reset residual
    matrix_RCP glmass = Tpetra::createCrsMatrix<ScalarT>(LA_owned_map); // reset Jacobian
    
    assembler->setInitial(rhs, mass, useadjoint);
    /*
    for (size_t b=0; b<assembler->cells.size(); b++) {
      for (size_t e=0; e<assembler->cells[b].size(); e++) {
        
        int numElem = assembler->cells[b][e]->numElem;
        vector<vector<int> > GIDs = assembler->cells[b][e]->GIDs;
        
        Kokkos::View<ScalarT**,AssemblyDevice> localrhs = assembler->cells[b][e]->getInitial(true, useadjoint);
        Kokkos::View<ScalarT***,AssemblyDevice> localmass = assembler->cells[b][e]->getMass();
        
        // assemble into global matrix
        for (int c=0; c<numElem; c++) {
          for( size_t row=0; row<GIDs[c].size(); row++ ) {
            int rowIndex = GIDs[c][row];
            ScalarT val = localrhs(c,row);
            rhs->sumIntoGlobalValue(rowIndex,0, val);
            for( size_t col=0; col<GIDs[c].size(); col++ ) {
              int colIndex = GIDs[c][col];
              ScalarT val = localmass(c,row,col);
              mass->insertGlobalValues(rowIndex, 1, &val, &colIndex);
            }
          }
        }
      }
    }
    
    mass->fillComplete();
    */
     
    glmass->setAllToScalar(0.0);
    glmass->doExport(*mass, *exporter, Tpetra::ADD);
    
    glrhs->putScalar(0.0);
    glrhs->doExport(*rhs, *exporter, Tpetra::ADD);
    
    glmass->fillComplete();
    
    this->linearSolver(glmass, glrhs, glinitial);
    
    initial->doImport(*glinitial, *importer, Tpetra::ADD);
    
  }
  else if (initial_type == "interpolation") {
    
    assembler->setInitial(initial, useadjoint);
    /*
    for (size_t b=0; b<assembler->cells.size(); b++) {
      for (size_t e=0; e<assembler->cells[b].size(); e++) {
        vector<vector<int> > GIDs = assembler->cells[b][e]->GIDs;
        Kokkos::View<ScalarT**,AssemblyDevice> localinit = assembler->cells[b][e]->getInitial(false, useadjoint);
        int numElem = assembler->cells[b][e]->numElem;
        for (int c=0; c<numElem; c++) {
          
          for( size_t row=0; row<GIDs[c].size(); row++ ) {
            int rowIndex = GIDs[c][row];
            ScalarT val = localinit(c,row);
            initial->replaceGlobalValue(rowIndex,0, val);
          }
        }
      }
    }*/
    
  }
  
  return initial;
}

// ========================================================================================
// Linear Solver for Tpetra stack
// ========================================================================================

void solver::linearSolver(matrix_RCP & J, vector_RCP & r, vector_RCP & soln)  {
  Teuchos::TimeMonitor localtimer(*linearsolvertimer);
  //KokkosTools::print(r);
  //LA_LinearProblem LinSys(J.get(), soln.get(), r.get());
  Teuchos::RCP<LA_LinearProblem> Problem = Teuchos::rcp(new LA_LinearProblem(J, soln, r));
  Teuchos::RCP<MueLu::TpetraOperator<ScalarT, LO, GO, HostNode> > M = buildPreconditioner(J);
  
  Problem->setLeftPrec(M);
  Problem->setProblem();
  
  Teuchos::RCP<Teuchos::ParameterList> belosList = Teuchos::rcp(new Teuchos::ParameterList());
  belosList->set("Maximum Iterations",    kspace); // Maximum number of iterations allowed
  belosList->set("Convergence Tolerance", lintol);    // Relative convergence tolerance requested
  if (verbosity > 9) {
    belosList->set("Verbosity", Belos::Errors + Belos::Warnings + Belos::StatusTestDetails);
  }
  else {
    belosList->set("Verbosity", Belos::Errors);
  }
  if (verbosity > 8) {
    belosList->set("Output Frequency",10);
  }
  else {
    belosList->set("Output Frequency",0);
  }
  int numEqns = 1;
  if (assembler->cells.size() == 1) {
    numEqns = numVars[0];
  }
  belosList->set("number of equations",numEqns);
  
  belosList->set("Output Style",          Belos::Brief);
  belosList->set("Implicit Residual Scaling", "None");
  
  Teuchos::RCP<Belos::SolverManager<ScalarT, LA_MultiVector, LA_Operator> > solver = Teuchos::rcp(new Belos::BlockGmresSolMgr<ScalarT, LA_MultiVector, LA_Operator>(Problem, belosList));
  
  solver->solve();
}

// ========================================================================================
// Linear solver for Epetra stack (mostly deprecated)
// ========================================================================================

void solver::linearSolver(Teuchos::RCP<Epetra_CrsMatrix> & J,
                          Teuchos::RCP<Epetra_MultiVector> & r,
                          Teuchos::RCP<Epetra_MultiVector> & soln)  {
  
  Teuchos::TimeMonitor localtimer(*linearsolvertimer);
    
  Epetra_LinearProblem LinSys(J.get(), soln.get(), r.get());

  
  // SOLVE ....
  if (useDirect) {
    Amesos AmFactory;
    char* SolverType = "Amesos_Klu";
    Amesos_BaseSolver * AmSolver = AmFactory.Create(SolverType, LinSys);
    AmSolver->SymbolicFactorization();
    AmSolver->NumericFactorization();
    AmSolver->Solve();
    delete AmSolver;
  }
  else {
    AztecOO linsolver(LinSys);
    
    // Set up the preconditioner
    ML_Epetra::MultiLevelPreconditioner* MLPrec;
    
    linsolver.SetAztecOption(AZ_solver,AZ_gmres);
    if(useDomDecomp){ //domain decomposition preconditioner, specific to Helmholtz at high frequencies
      linsolver.SetAztecOption(AZ_precond,AZ_dom_decomp);
      linsolver.SetAztecOption(AZ_subdomain_solve,AZ_ilut);
      linsolver.SetAztecParam(AZ_drop,dropTol);
      linsolver.SetAztecParam(AZ_ilut_fill,fillParam);
      
      if(verbosity == 0)
      linsolver.SetAztecOption(AZ_diagnostics,AZ_none);
      
      ScalarT condest = 0.0;
      linsolver.ConstructPreconditioner(condest);
      if(condest > 1.e13 || condest < 1.0){
        linsolver.DestroyPreconditioner();
        linsolver.SetAztecParam(AZ_athresh,1.e-5);
        linsolver.SetAztecParam(AZ_rthresh,0.0);
        linsolver.ConstructPreconditioner(condest);
        if(condest > 1.e13 || condest < 1.0){
          linsolver.DestroyPreconditioner();
          linsolver.SetAztecParam(AZ_athresh,1.e-5);
          linsolver.SetAztecParam(AZ_rthresh,0.01);
          linsolver.ConstructPreconditioner(condest);
          if(condest > 1.e13 || condest < 1.0){
            linsolver.DestroyPreconditioner();
            linsolver.SetAztecParam(AZ_athresh,1.e-2);
            linsolver.SetAztecParam(AZ_rthresh,0.0);
            linsolver.ConstructPreconditioner(condest);
            if(condest > 1.e13 || condest < 1.0){
              linsolver.DestroyPreconditioner();
              linsolver.SetAztecParam(AZ_athresh,1.e-2);
              linsolver.SetAztecParam(AZ_rthresh,0.01);
              linsolver.ConstructPreconditioner(condest);
              if(condest > 1.e13){
                cout << "SAD PRECONDITIONER: condition number " << condest << endl;
              }
            }
          }
        }
      }
    }
    else if (usePrec) { //multi-level preconditioner
      MLPrec = buildPreconditioner(J);
      linsolver.SetPrecOperator(MLPrec);
    }
    else {
      linsolver.SetAztecOption(AZ_precond, AZ_none);
    }
    linsolver.SetAztecOption(AZ_kspace,kspace);
   
    if (verbosity > 8)
    linsolver.SetAztecOption(AZ_output,10);
    else
    linsolver.SetAztecOption(AZ_output,0);
    
    linsolver.Iterate(liniter,lintol);
    
    if(!useDomDecomp && usePrec)
    delete MLPrec;
   
  }
  
  //return soln;
}


// ========================================================================================
// Preconditioner for Tpetra stack
// ========================================================================================

Teuchos::RCP<MueLu::TpetraOperator<ScalarT, LO, GO, HostNode> > solver::buildPreconditioner(const matrix_RCP & J) {
  Teuchos::ParameterList mueluParams;
  
  mueluParams.setName("MueLu");
  
  // Main settings
  if (verbosity >= 10){
    mueluParams.set("verbosity","high");
  }
  else {
    mueluParams.set("verbosity","none");
  }
  int numEqns = 1;
  if (assembler->cells.size() == 1) {
    numEqns = numVars[0];
  }
  //mueluParams.set("number of equations",numEqns);
  
  mueluParams.set("coarse: max size",500);
  mueluParams.set("multigrid algorithm", multigrid_type);
  
  // Aggregation
  mueluParams.set("aggregation: type","uncoupled");
  mueluParams.set("aggregation: drop scheme","classical");
  
  //Smoothing
  Teuchos::ParameterList smootherParams = mueluParams.sublist("smoother: params");
  mueluParams.set("smoother: type",smoother_type);
  if (smoother_type == "CHEBYSHEV") {
    mueluParams.sublist("smoother: params").set("chebyshev: degree",2);
    mueluParams.sublist("smoother: params").set("chebyshev: ratio eigenvalue",7.0);
    mueluParams.sublist("smoother: params").set("chebyshev: min eigenvalue",1.0);
    mueluParams.sublist("smoother: params").set("chebyshev: zero starting solution",true);
  }
  else if (smoother_type == "RELAXATION") {
    mueluParams.sublist("smoother: params").set("relaxation: type","Jacobi");
  }
  
  // Repartitioning
  
  mueluParams.set("repartition: enable",false);
  mueluParams.set("repartition: partitioner","zoltan");
  mueluParams.set("repartition: start level",2);
  mueluParams.set("repartition: min rows per proc",800);
  mueluParams.set("repartition: max imbalance", 1.1);
  mueluParams.set("repartition: remap parts",false);
  
  Teuchos::RCP<MueLu::TpetraOperator<ScalarT, LO, GO, HostNode> > M = MueLu::CreateTpetraPreconditioner((Teuchos::RCP<LA_Operator>)J, mueluParams);

  return M;
}

// ========================================================================================
// Preconditioner for Epetra stack
// ========================================================================================

ML_Epetra::MultiLevelPreconditioner* solver::buildPreconditioner(const Teuchos::RCP<Epetra_CrsMatrix> & J) {
  Teuchos::ParameterList MLList;
  ML_Epetra::SetDefaults("SA",MLList);
  MLList.set("ML output", 0);
  MLList.set("max levels",5);
  MLList.set("increasing or decreasing","increasing");
  int numEqns;
  if (assembler->cells.size() == 1)
  numEqns = numVars[0];
  else
  numEqns = 1;
  
  MLList.set("PDE equations",numEqns);
  MLList.set("aggregation: type", "Uncoupled");
  MLList.set("smoother: type","IFPACK");
  MLList.set("smoother: sweeps",1);
  MLList.set("smoother: ifpack type","ILU");
  MLList.set("smoother: ifpack overlap",1);
  MLList.set("smoother: pre or post", "both");
  MLList.set("coarse: type","Amesos-KLU");
  ML_Epetra::MultiLevelPreconditioner* MLPrec =
  new ML_Epetra::MultiLevelPreconditioner(*J, MLList);
  
  return MLPrec;
}


// ========================================================================================
// ========================================================================================
/*
void solver::sacadoizeParams(const bool & seed_active) {
  
  //vector<vector<AD> > paramvals_AD;
  if (seed_active) {
    size_t pprog = 0;
    for (size_t i=0; i<paramvals.size(); i++) {
      vector<AD> currparams;
      if (paramtypes[i] == 1) { // active parameters
        for (size_t j=0; j<paramvals[i].size(); j++) {
          //currparams.push_back(Sacado::Fad::DFad<ScalarT>(num_active_params,pprog,paramvals[i][j]));
          paramvals_KVAD(i,j) = AD(maxDerivs,pprog,paramvals[i][j]);
          currparams.push_back(AD(maxDerivs,pprog,paramvals[i][j]));
          pprog++;
        }
      }
      else { // inactive, stochastic, or discrete parameters
        for (size_t j=0; j<paramvals[i].size(); j++) {
          //currparams.push_back(Sacado::Fad::DFad<ScalarT>(paramvals[i][j]));
          paramvals_KVAD(i,j) = AD(paramvals[i][j]);
          currparams.push_back(AD(paramvals[i][j]));
        }
      }
      *(paramvals_AD[i]) = currparams;
    }
  }
  else {
    size_t pprog = 0;
    for (size_t i=0; i<paramvals.size(); i++) {
      vector<AD> currparams;
      for (size_t j=0; j<paramvals[i].size(); j++) {
        //currparams.push_back(Sacado::Fad::DFad<ScalarT>(paramvals[i][j]));
        currparams.push_back(AD(paramvals[i][j]));
        paramvals_KVAD(i,j) = AD(paramvals[i][j]);
      }
      *(paramvals_AD[i]) = currparams;
    }
  }
  
  phys->updateParameters(paramvals_AD, paramnames);
  multiscale_manager->updateParameters(paramvals_AD, paramnames);
  
}
*/
// ========================================================================================
// ========================================================================================
/*
void solver::updateParams(const vector<ScalarT> & newparams, const int & type) {
  size_t pprog = 0;
  // perhaps add a check that the size of newparams equals the number of parameters of the
  // requested type
  
  for (size_t i=0; i<paramvals.size(); i++) {
    if (paramtypes[i] == type) {
      for (size_t j=0; j<paramvals[i].size(); j++) {
        if (Comm->getRank() == 0 && verbosity > 0) {
          cout << "Updated Params: " << paramvals[i][j] << " (old value)   " << newparams[pprog] << " (new value)" << endl;
        }
        paramvals[i][j] = newparams[pprog];
        pprog++;
      }
    }
  }
  if ((type == 4) && (globalParamUnknowns > 0)) {
    int numClassicParams = this->getNumParams(1); // offset for ROL param vector
    for (size_t i = 0; i < paramOwnedAndShared.size(); i++) {
      int gid = paramOwnedAndShared[i];
      Psol[0]->replaceGlobalValue(gid,0,newparams[gid+numClassicParams]);
    }
  }
  if ((type == 2) && (globalParamUnknowns > 0)) {
    int numClassicParams = this->getNumParams(2); // offset for ROL param vector
    for (size_t i=0; i<paramOwnedAndShared.size(); i++) {
      int gid = paramOwnedAndShared[i];
      Psol[0]->replaceGlobalValue(gid,0,newparams[i+numClassicParams]);
    }
  }
}
*/
// ========================================================================================
// ========================================================================================
/*
void solver::updateParams(const vector<ScalarT> & newparams, const std::string & stype) {
  size_t pprog = 0;
  int type;
  // perhaps add a check that the size of newparams equals the number of parameters of the
  // requested type
  if (stype == "inactive") { type = 0;}
  else if (stype == "active") { type = 1;}
  else if (stype == "stochastic") { type = 2;}
  else if (stype == "discrete") { type = 3;}
  else {
    //complain
  }
  for (size_t i=0; i<paramvals.size(); i++) {
    if (paramtypes[i] == type) {
      for (size_t j=0; j<paramvals[i].size(); j++) {
        paramvals[i][j] = newparams[pprog];
        pprog++;
      }
    }
  }
}
*/
// ========================================================================================
// ========================================================================================

void solver::updateMeshData(const int & newrandseed) {
  
  // Determine how many seeds there are
  int localnumSeeds = 0;
  int numSeeds = 0;
  for (int b=0; b<assembler->cells.size(); b++) {
    for (int e=0; e<assembler->cells[b].size(); e++) {
      for (int k=0; k<assembler->cells[b][e]->numElem; k++) {
        if (assembler->cells[b][e]->cell_data_seed[k] > localnumSeeds) {
          localnumSeeds = assembler->cells[b][e]->cell_data_seed[k];
        }
      }
    }
  }
  //Comm->MaxAll(&localnumSeeds, &numSeeds, 1);
  Teuchos::reduceAll(*Comm,Teuchos::REDUCE_MAX,1,&localnumSeeds,&numSeeds);
  numSeeds += 1; //To properly allocate and iterate
  
  // Create a random number generator
  std::default_random_engine generator(newrandseed);
  
  ////////////////////////////////////////////////////////////////////////////////
  // Set seed data
  ////////////////////////////////////////////////////////////////////////////////
  
  int numdata = 9;
  
  //cout << "solver numSeeds = " << numSeeds << endl;
  
  std::normal_distribution<ScalarT> ndistribution(0.0,1.0);
  Kokkos::View<ScalarT**,HostDevice> rotation_data("cell_data",numSeeds,numdata);
  for (int k=0; k<numSeeds; k++) {
    ScalarT x = ndistribution(generator);
    ScalarT y = ndistribution(generator);
    ScalarT z = ndistribution(generator);
    ScalarT w = ndistribution(generator);
    
    ScalarT r = sqrt(x*x + y*y + z*z + w*w);
    x *= 1.0/r;
    y *= 1.0/r;
    z *= 1.0/r;
    w *= 1.0/r;
    
    rotation_data(k,0) = w*w + x*x - y*y - z*z;
    rotation_data(k,1) = 2.0*(x*y - w*z);
    rotation_data(k,2) = 2.0*(x*z + w*y);
    
    rotation_data(k,3) = 2.0*(x*y + w*z);
    rotation_data(k,4) = w*w - x*x + y*y - z*z;
    rotation_data(k,5) = 2.0*(y*z - w*x);
    
    rotation_data(k,6) = 2.0*(x*z - w*y);
    rotation_data(k,7) = 2.0*(y*z + w*x);
    rotation_data(k,8) = w*w - x*x - y*y + z*z;
    
  }
  
  ////////////////////////////////////////////////////////////////////////////////
  // Set cell data
  ////////////////////////////////////////////////////////////////////////////////
  
  for (size_t b=0; b<assembler->cells.size(); b++) {
    for (size_t e=0; e<assembler->cells[b].size(); e++) {
      int numElem = assembler->cells[b][e]->numElem;
      for (int c=0; c<numElem; c++) {
        int cnode = assembler->cells[b][e]->cell_data_seed[c];
        for (int i=0; i<9; i++) {
          assembler->cells[b][e]->cell_data(c,i) = rotation_data(cnode,i);
        }
      }
    }
  }
  
  ////////////////////////////////////////////////////////////////////////////////
  // Update subgrid elements
  ////////////////////////////////////////////////////////////////////////////////
  
  //multiscale_manager->updateMeshData(rotation_data);
  
}

// ========================================================================================
// ========================================================================================
/*
vector<ScalarT> solver::getParams(const int & type) {
  vector<ScalarT> reqparams;
  for (size_t i=0; i<paramvals.size(); i++) {
    if (paramtypes[i] == type) {
      for (size_t j=0; j<paramvals[i].size(); j++) {
        reqparams.push_back(paramvals[i][j]);
      }
    }
  }
  return reqparams;
}
*/
// ========================================================================================
// ========================================================================================
/*
vector<string> solver::getParamsNames(const int & type) {
  vector<string> reqparams;
  for (size_t i=0; i<paramvals.size(); i++) {
    if (paramtypes[i] == type) {
      reqparams.push_back(paramnames[i]);
    }
  }
  return reqparams;
}
*/
// ========================================================================================
// ========================================================================================
/*
vector<size_t> solver::getParamsLengths(const int & type) {
  vector<size_t> reqparams;
  for (size_t i=0; i<paramvals.size(); i++) {
    if (paramtypes[i] == type) {
      reqparams.push_back(paramvals[i].size());
    }
  }
  return reqparams;
}
*/
// ========================================================================================
// ========================================================================================
/*
vector<ScalarT> solver::getParams(const std::string & stype) {
  vector<ScalarT> reqparams;
  int type;
  if (stype == "inactive")
  type = 0;
  else if (stype == "active")
  type = 1;
  else if (stype == "stochastic")
  type = 2;
  else if (stype == "discrete")
  type = 3;
  else
  //complain
  
  for (size_t i=0; i<paramvals.size(); i++) {
    if (paramtypes[i] == type) {
      for (size_t j=0; j<paramvals[i].size(); j++) {
        reqparams.push_back(paramvals[i][j]);
      }
    }
  }
  return reqparams;
}
*/
// ========================================================================================
// ========================================================================================
/*
vector<vector<ScalarT> > solver::getParamBounds(const std::string & stype) {
  vector<vector<ScalarT> > reqbnds;
  vector<ScalarT> reqlo;
  vector<ScalarT> requp;
  int type;
  if (stype == "inactive") {type = 0;}
  else if (stype == "active") {type = 1;}
  else if (stype == "stochastic") {type = 2;}
  else if (stype == "discrete") {type = 3;}
  else if (stype == "discretized") {type = 4;}
  
  if (type == 0) {
    std::cout << "Bounds for inactive parameters are currently at default of (0,0)" << std::endl;
  }
  
  for (size_t i=0; i<paramvals.size(); i++) {
    if (paramtypes[i] == type) {
      for (size_t j=0; j<paramvals[i].size(); j++) {
        reqlo.push_back(paramLowerBounds[i][j]);
        requp.push_back(paramUpperBounds[i][j]);
      }
    }
  }
  
  if (type == 4 && globalParamUnknowns > 0) {
    int numDiscParams = this->getNumParams(4);
    vector<ScalarT> rLocalLo(numDiscParams);
    vector<ScalarT> rLocalUp(numDiscParams);
    vector<ScalarT> rlo(numDiscParams);
    vector<ScalarT> rup(numDiscParams);
    int pindex;
    for (int n = 0; n < num_discretized_params; n++) {
      for (size_t i = 0; i < paramNodes[n].size(); i++) {
        int pnode = paramNodes[n][i];
        if (pnode >= 0) {
          int pindex = paramOwned[pnode];
          rLocalLo[pindex] = lowerParamBounds[n];
          rLocalUp[pindex] = upperParamBounds[n];
        }
      }
    }
    
    for (size_t i = 0; i < numDiscParams; i++) {
      
      ScalarT globalval = 0.0;
      ScalarT localval = rLocalLo[i];
      Teuchos::reduceAll(*Comm,Teuchos::REDUCE_SUM,1,&localval,&globalval);
      //Comm->SumAll(&localval, &globalval, 1);
      rlo[i] = globalval;
      
      globalval = 0.0;
      localval = rLocalUp[i];
      Teuchos::reduceAll(*Comm,Teuchos::REDUCE_SUM,1,&localval,&globalval);
      //Comm->SumAll(&localval, &globalval, 1);
      rup[i] = globalval;
    }
    
    reqlo = rlo;
    requp = rup;
    
  }
  
  reqbnds.push_back(reqlo);
  reqbnds.push_back(requp);
  return reqbnds;
}
*/
// ========================================================================================
// ========================================================================================

void solver::setBatchID(const int & bID){
  batchID = bID;
  params->batchID = bID;
}

// ========================================================================================
// ========================================================================================
/*
void solver::stashParams(){
  if (batchID == 0 && Comm->getRank() == 0){
    string outname = "param_stash.dat";
    ofstream respOUT(outname);
    respOUT.precision(16);
    for (size_t i=0; i<paramvals.size(); i++) {
      if (paramtypes[i] == 1) {
        for (size_t j=0; j<paramvals[i].size(); j++) {
          respOUT << paramvals[i][j] << endl;
        }
      }
    }
    respOUT.close();
  }
}
*/
// ========================================================================================
// ========================================================================================
/*
vector<ScalarT> solver::getStochasticParams(const std::string & whichparam) {
  if (whichparam == "mean")
    return stochastic_mean;
  else if (whichparam == "variance")
    return stochastic_variance;
  else if (whichparam == "min")
    return stochastic_min;
  else if (whichparam == "max")
    return stochastic_max;
  else {
    vector<ScalarT> emptyvec;
    return emptyvec;
  }
}
*/
// ========================================================================================
// ========================================================================================
/*
vector<ScalarT> solver::getFractionalParams(const std::string & whichparam) {
  if (whichparam == "s-exponent")
    return s_exp;
  else if (whichparam == "mesh-resolution")
    return h_mesh;
  else {
    vector<ScalarT> emptyvec;
    return emptyvec;
  }
}
*/
// ========================================================================================
// ========================================================================================

vector_RCP solver::blankState(){
  vector_RCP F_soln = Teuchos::rcp(new LA_MultiVector(LA_overlapped_map,numsteps+1)); // empty solution
  return F_soln;
}

// ========================================================================================
// ========================================================================================

DRV solver::getElemNodes(const int & block, const int & elemID) {
  int nnodes = elemnodes[block].dimension(1);
  
  DRV cnodes("element nodes",1,nnodes,spaceDim);
  for (int i=0; i<nnodes; i++) {
    for (int j=0; j<spaceDim; j++) {
      cnodes(0,i,j) = elemnodes[block](elemID,i,j);
    }
  }
  return cnodes;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

void solver::finalizeMultiscale() {
  if (multiscale_manager->subgridModels.size() > 0 ) {
    for (size_t k=0; k<multiscale_manager->subgridModels.size(); k++) {
      multiscale_manager->subgridModels[k]->paramvals_KVAD = params->paramvals_KVAD;
    //  multiscale_manager->subgridModels[k]->wkset[0]->paramnames = paramnames;
    }
    
    multiscale_manager->setMacroInfo(disc->basis_pointers, disc->basis_types,
                                     phys->varlist, useBasis, phys->offsets,
                                     params->paramnames, params->discretized_param_names);
    
    multiscale_manager->macro_wkset = assembler->wkset;
    ScalarT my_cost = multiscale_manager->initialize();
    ScalarT gmin = 0.0;
    Teuchos::reduceAll(*Comm,Teuchos::REDUCE_MIN,1,&my_cost,&gmin);
    //Comm->MinAll(&my_cost, &gmin, 1);
    ScalarT gmax = 0.0;
    Teuchos::reduceAll(*Comm,Teuchos::REDUCE_MAX,1,&my_cost,&gmin);
    //Comm->MaxAll(&my_cost, &gmax, 1);
    
    if(Comm->getRank() == 0 && verbosity>0) {
      cout << "***** Load Balancing Factor " << gmax/gmin <<  endl;
    }
    
    
  }

}
