/*
  Copyright 2013--2018 James E. McClure, Virginia Polytechnic & State University

  This file is part of the Open Porous Media project (OPM).
  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/
/*
 * Multi-relaxation time LBM Model
 */
#include "models/MRTModel.h"

double voxelSize = 0.0;
int fqInterval = 1e10;
bool fqFlag = false;
bool restartFq = false;
bool bgkFlag = false;
bool thermalFlag = false;
//bool FDThermalFlag = false;
double tol=1e6;
double permTolerance = 1e-5;
bool visTolerance = true;
double DiffCoeff = 0.1;
//int toleranceInterval = 10000;
int analysis_interval = 1000;
ScaLBL_MRTModel::ScaLBL_MRTModel(int RANK, int NP, MPI_Comm COMM):
rank(RANK), nprocs(NP), Restart(0),timestep(0),timestepMax(0),tau(0),
Fx(0),Fy(0),Fz(0),flux(0),din(0),dout(0),mu(0),
Nx(0),Ny(0),Nz(0),N(0),Np(0),nprocx(0),nprocy(0),nprocz(0),BoundaryCondition(0),Lx(0),Ly(0),Lz(0),comm(COMM)
{

}
ScaLBL_MRTModel::~ScaLBL_MRTModel(){

}

void ScaLBL_MRTModel::ReadParams(string filename){
	// read the input database 
	db = std::make_shared<Database>( filename );
	domain_db = db->getDatabase( "Domain" );
	mrt_db = db->getDatabase( "MRT" );

	// Model parameters
	if (mrt_db->keyExists( "bgkFlag" )){
		bgkFlag = mrt_db->getScalar<bool>( "bgkFlag" );
	}
	if (rank==0) {
        if (bgkFlag) printf("LBM Implementation is SRT\n");
        else printf("LBM Implementation is MRT (TRT)\n");
    }
	if (mrt_db->keyExists( "thermalFlag" )){
		thermalFlag = mrt_db->getScalar<bool>( "thermalFlag" );
	}
    if (thermalFlag){
        thermal_db = db->getDatabase( "Thermal" );
	    if (thermal_db->keyExists( "DiffCoeff" )){
		    DiffCoeff = thermal_db->getScalar<double>( "DiffCoeff" );
	    }
	}
	if (rank==0) {
        if (thermalFlag) printf("Thermal LBM is active, Diffusion Coefficient: %f\n",DiffCoeff);
        else printf("Thermal LBM inactive, running single phase NVE\n");
    }
	timestepMax = mrt_db->getScalar<int>( "timestepMax" );
	tau = mrt_db->getScalar<double>( "tau" );
	Fx = mrt_db->getVector<double>( "F" )[0];
	Fy = mrt_db->getVector<double>( "F" )[1];
	Fz = mrt_db->getVector<double>( "F" )[2];
	Restart = mrt_db->getScalar<bool>( "Restart" );
	din = mrt_db->getScalar<double>( "din" );
	dout = mrt_db->getScalar<double>( "dout" );
	flux = mrt_db->getScalar<double>( "flux" );

	if (mrt_db->keyExists( "fqInterval" )){
		fqInterval = mrt_db->getScalar<int>( "fqInterval" );
	}
	if (mrt_db->keyExists( "fqFlag" )){
		fqFlag = mrt_db->getScalar<bool>( "fqFlag" );
	}
	if (mrt_db->keyExists( "restartFq" )){
		restartFq = mrt_db->getScalar<bool>( "restartFq" );
	}
//	if (mrt_db->keyExists( "toleranceInterval" )){
//		toleranceInterval = mrt_db->getScalar<int>( "toleranceInterval" );
//	}
	if (mrt_db->keyExists( "permTolerance" )){
		permTolerance = mrt_db->getScalar<double>( "permTolerance" );
	}
	if (mrt_db->keyExists( "visTolerance" )){
		visTolerance = mrt_db->getScalar<bool>( "visTolerance" );
	}
	if (mrt_db->keyExists( "analysis_interval" )){
		analysis_interval = mrt_db->getScalar<int>( "analysis_interval" );
	}
	// Read domain parameters
	auto L = domain_db->getVector<double>( "L" );
	auto size = domain_db->getVector<int>( "n" );
	auto nproc = domain_db->getVector<int>( "nproc" );
	BoundaryCondition = domain_db->getScalar<int>( "BC" );
	Nx = size[0];
	Ny = size[1];
	Nz = size[2];
	Lx = L[0];
	Ly = L[1];
	Lz = L[2];
	nprocx = nproc[0];
	nprocy = nproc[1];
	nprocz = nproc[2];
	mu=(tau-0.5)/3.0;
	if (domain_db->keyExists( "voxel_length" )){
		voxelSize = domain_db->getScalar<double>( "voxel_length" );
		voxelSize=voxelSize/1000000.0;
	}
	else{
	    voxelSize=Lz/double(Nz*nprocz);
	}
	if (rank==0)    printf("Voxel Size = %f microns\n",voxelSize*1000000);

}
void ScaLBL_MRTModel::SetDomain(){
	//Dm  = std::shared_ptr<Domain>(new Domain(domain_db,comm));      // full domain for analysis
	Mask  = std::shared_ptr<Domain>(new Domain(domain_db,comm));    // mask domain removes immobile phases
	Nx+=2; Ny+=2; Nz += 2;
	N = Nx*Ny*Nz;
	Geom.resize(Nx,Ny,Nz);
	Velocity_x.resize(Nx,Ny,Nz);
	Velocity_y.resize(Nx,Ny,Nz);
	Velocity_z.resize(Nx,Ny,Nz);
	if (thermalFlag) ConcentrationCart.resize(Nx,Ny,Nz);
    if (rank == 0) cout << "Domain set." << endl;
	//for (int i=0; i<Nx*Ny*Nz; i++) Dm->id[i] = 1;               // initialize this way
	//Averages = std::shared_ptr<TwoPhase> ( new TwoPhase(Dm) ); // TwoPhase analysis object
//	MPI_Barrier(comm);
//	Mask->CommInit();
//	MPI_Barrier(comm);
}

void ScaLBL_MRTModel::ReadInput(){
    int rank=Mask->rank();
    //size_t readID;
    //.......................................................................
    //.......................................................................
    Mask->ReadIDs();
    
    sprintf(LocalRankString,"%05d",rank);
    sprintf(LocalRankFilename,"%s%s","ID.",LocalRankString);
    sprintf(LocalRestartFile,"%s%s","Restart.",LocalRankString);

	// Generate the signed Geom map
	// Initialize the domain and communication
	//Array<char> id_solid(Nx,Ny,Nz);
	int count = 0;
	// Solve for the position of the solid phase
	for (int k=0;k<Nz;k++){
		for (int j=0;j<Ny;j++){
			for (int i=0;i<Nx;i++){
				int n = k*Nx*Ny+j*Nx+i;
				// Initialize the solid phase
				if (Mask->id[n] > 0)	Geom(i,j,k) = 1;
				else	     	    Geom(i,j,k) = 0;
			}
		}
	}
    if (rank == 0) cout << "Geometry successfully loaded" << endl;
}

void ScaLBL_MRTModel::Create(){
	/*
	 *  This function creates the variables needed to run a LBM 
	 */
	int rank=Mask->rank();
	//.........................................................
	// Initialize communication structures in averaging domain
	//for (int i=0; i<Nx*Ny*Nz; i++) Dm->id[i] = Mask->id[i];
	Mask->CommInit();
	//...........................................................................
	if (rank==0)    printf ("Create ScaLBL_Communicator \n");
	// Create a communicator for the device (will use optimized layout)
	// ScaLBL_Communicator ScaLBL_Comm(Mask); // original
	ScaLBL_Comm  = std::shared_ptr<ScaLBL_Communicator>(new ScaLBL_Communicator(Mask));
	Np=Mask->PoreCount();
	int Npad=(Np/16 + 2)*16; // the fuck is this
	if (rank==0)    printf ("Set up memory efficient layout \n");
	Map.resize(Nx,Ny,Nz);       
	//Map.fill(-2);
	auto neighborList= new int[18*Npad];
	Np = ScaLBL_Comm->MemoryOptimizedLayoutAA(Map,neighborList,Mask->id,Np);
	MPI_Barrier(comm);
	//...........................................................................
	//                MAIN  VARIABLES ALLOCATED HERE
	//...........................................................................
	// LBM variables
	if (rank==0)    printf ("Allocating distributions \n");
	//......................device distributions.................................
	int dist_mem_size = Np*sizeof(double);
	int neighborSize=18*(Np*sizeof(int));
	//...........................................................................
	ScaLBL_AllocateDeviceMemory((void **) &NeighborList, neighborSize);
	ScaLBL_AllocateDeviceMemory((void **) &fq, 19*dist_mem_size);  
	ScaLBL_AllocateDeviceMemory((void **) &Pressure, sizeof(double)*Np);
	ScaLBL_AllocateDeviceMemory((void **) &Velocity, 3*sizeof(double)*Np);
	if (thermalFlag) {
    	ScaLBL_AllocateDeviceMemory((void **) &cq, 19*dist_mem_size);  
		ScaLBL_AllocateDeviceMemory((void **) &Concentration, sizeof(double)*Np);  
	}
//	if (FDThermalFlag) {
//    	ScaLBL_AllocateDeviceMemory((void **) &cq, dist_mem_size);  
//	}
	//...........................................................................
	// Update GPU data structures
	if (rank==0)    printf ("Setting up device map and neighbor list \n");
	// copy the neighbor list 
	ScaLBL_CopyToDevice(NeighborList, neighborList, neighborSize);
	MPI_Barrier(comm);
	
}        

void ScaLBL_MRTModel::Initialize(){
	/*
	 * This function initializes model
	 */
    if (rank==0)    printf ("Initializing fq distribution \n");
    ScaLBL_D3Q19_Init(fq, Np);
    if (restartFq) {
        if (rank==0)    printf ("Reading fq distributions from checkpoint \n");
        // read in standard layout and save to Np layout
	    MPI_Barrier(comm);
	    //load the file
	    FILE *OUTFILE;
	    char LocalRankFilename[100];
	    sprintf(LocalRankFilename,"restartFq/Part_%d_%d_%d_%d_%d_%d_%d.txt",rank,Nx,Ny,Nz,nprocx,nprocy,nprocz); //change this file name to include the size
	    OUTFILE = fopen(LocalRankFilename,"r");
	    // initialise the fq vector
        double *mrtDist;
        mrtDist = new double[19*Np];
        // scan the file in regular
        for (int d=0; d<19; d++) {
            float locFq = 0.0;
            int idx=0;
	        for (int k=0; k<Nz; k++){
		        for (int j=0; j<Ny; j++){
			        for (int i=0; i<Nx; i++){
                        fscanf(OUTFILE,"%f\n",&locFq); //scan the value
                        idx = Map(i,j,k);
                        if (idx >= 0) {
                            // if in Np, save the value into dist
                            mrtDist[d*Np+idx]=locFq;
                        }
			        }
		        }
	        }
	    }
	    fclose(OUTFILE);
	    ScaLBL_CopyToDevice(fq,mrtDist,19*Np*sizeof(double));
	    ScaLBL_DeviceBarrier();
	    MPI_Barrier(comm);
    }
    if (thermalFlag) {
        if (rank==0)    printf ("Initializing Lattice Boltzmann cq distribution and initial velocity field \n");
        ScaLBL_D3Q19_Init(cq, Np);
		ScaLBL_D3Q19_Momentum(fq,Velocity,Np); //get velocity (does velocity exist in odd time?)
		// add option to read velocity fields in directly instead of from fq format
		// add option to read in cq file or concentration file
    }
//    if (FDThermalFlag) {
//        if (rank==0)    printf ("Initializing Finite Difference cq distribution and initial velocity field \n");
//        ScaLBL_FDM_Init(cq, Np);
//		ScaLBL_D3Q19_Momentum(fq,Velocity,Np); //get velocity (does velocity exist in odd time?)
//		// add option to read velocity fields in directly instead of from fq format
//		// add option to read in cq file or concentration file
//    }
}

void ScaLBL_MRTModel::Run(){
	double rlx_setA=1.0/tau;
	double rlx_setB = 8.f*(2.f-rlx_setA)/(8.f-rlx_setA);
	double Kold = 0.0;
	// thermal temp params
	double omega=1/(3*DiffCoeff+0.5);

	if (rank==0){
		FILE * log_file = fopen("Permeability.csv","a");
		fprintf(log_file,"timestep Fx Fy Fz din dout mu vax vay vaz absperm\n");
		fclose(log_file);
	}

	//.......create and start timer............
	double starttime,stoptime,cputime;
	ScaLBL_DeviceBarrier(); MPI_Barrier(comm);
	starttime = MPI_Wtime();
	
	if (rank==0) printf("No. of timesteps: %i , Boundary Condition: %i \n", timestepMax, BoundaryCondition);
	
	if (rank==0) printf("********************************************************\n");
	timestep=0;
	while (timestep < timestepMax) {
		//************************************************************************/
		//ODD TIMESTEP************************************************************
		timestep++;// odd timesteps need to be solved interior then exterior
		if (thermalFlag) { //run thermal flag update vel fields every 2 steps
			ScaLBL_Comm->SendD3Q19AA(cq); //read overlapping boundary info from neighbouring blocks
	        ScaLBL_D3Q19_AAodd_ThermalBGK(NeighborList, Velocity, cq,  ScaLBL_Comm->FirstInterior(), ScaLBL_Comm->LastInterior(), Np, omega);		
		    ScaLBL_Comm->RecvD3Q19AA(cq); //write boundary info to neighbouring blocks
		    ScaLBL_DeviceBarrier();
		    // Set BCs
		    ScaLBL_Comm->D3Q19_Pressure_BC_z(NeighborList, cq, 1.1, timestep);
		    ScaLBL_Comm->D3Q19_Pressure_BC_Z(NeighborList, cq, 1.0, timestep);
	        ScaLBL_D3Q19_AAodd_ThermalBGK(NeighborList, Velocity, cq, 0, ScaLBL_Comm->LastExterior(), Np, omega); //exteriors after BCs enforced
		    ScaLBL_DeviceBarrier(); MPI_Barrier(comm);
		}
		ScaLBL_Comm->SendD3Q19AA(fq); //send exteriors to other ranks, acts as a streaming step
		if (bgkFlag) { //  collide neighbours, stream to opposite neighbour in the interior
		    ScaLBL_D3Q19_AAodd_BGK(NeighborList, fq,  ScaLBL_Comm->FirstInterior(), ScaLBL_Comm->LastInterior(), Np, rlx_setA, Fx, Fy, Fz);		
	    } else {
		    ScaLBL_D3Q19_AAodd_MRT(NeighborList, fq,  ScaLBL_Comm->FirstInterior(), ScaLBL_Comm->LastInterior(), Np, rlx_setA, rlx_setB, Fx, Fy, Fz);
		}
		ScaLBL_Comm->RecvD3Q19AA(fq); //receive exterior info, pre-streamed
		ScaLBL_DeviceBarrier();
		// Set BCs at exteriors
		if (BoundaryCondition == 3){
			ScaLBL_Comm->D3Q19_Pressure_BC_z(NeighborList, fq, din, timestep);
			ScaLBL_Comm->D3Q19_Pressure_BC_Z(NeighborList, fq, dout, timestep);
		}
		if (BoundaryCondition == 4){
			din = ScaLBL_Comm->D3Q19_Flux_BC_z(NeighborList, fq, flux, timestep);
			ScaLBL_Comm->D3Q19_Pressure_BC_Z(NeighborList, fq, dout, timestep);
		}
		if (bgkFlag) { //stream and collide the exteriors, since scaLBL handled offrank streaming, neighbours to offrank cells are walled off
		    ScaLBL_D3Q19_AAodd_BGK(NeighborList, fq, 0, ScaLBL_Comm->LastExterior(), Np, rlx_setA, Fx, Fy, Fz); //exteriors after BCs enforced
		} else {
		    ScaLBL_D3Q19_AAodd_MRT(NeighborList, fq, 0, ScaLBL_Comm->LastExterior(), Np, rlx_setA, rlx_setB, Fx, Fy, Fz); 
		}
		ScaLBL_DeviceBarrier(); MPI_Barrier(comm);
		
		//EVEN TIMESTEP************************************************************
		timestep++;
		// even timesteps are collision only, and can be solved in a single pass
		ScaLBL_Comm->SendD3Q19AA(fq); //stream out exteriors
		ScaLBL_Comm->RecvD3Q19AA(fq); //stream in in offrank exteriors
		ScaLBL_DeviceBarrier();
		// Set BCs
		if (BoundaryCondition == 3){
			ScaLBL_Comm->D3Q19_Pressure_BC_z(NeighborList, fq, din, timestep);
			ScaLBL_Comm->D3Q19_Pressure_BC_Z(NeighborList, fq, dout, timestep);
		}
		if (BoundaryCondition == 4){
			din = ScaLBL_Comm->D3Q19_Flux_BC_z(NeighborList, fq, flux, timestep);
			ScaLBL_Comm->D3Q19_Pressure_BC_Z(NeighborList, fq, dout, timestep);
		}
		if (bgkFlag) {
		    ScaLBL_D3Q19_AAeven_BGK(fq, 0, ScaLBL_Comm->LastInterior(), Np, rlx_setA, Fx, Fy, Fz);
		} else {
		    ScaLBL_D3Q19_AAeven_MRT(fq, 0, ScaLBL_Comm->LastInterior(), Np, rlx_setA, rlx_setB, Fx, Fy, Fz);
		}
		ScaLBL_DeviceBarrier(); MPI_Barrier(comm);
		if (thermalFlag) { //whether thermal should be solved ABAB or ABBA or BAAB is uncertain yet....
			ScaLBL_D3Q19_Momentum(fq,Velocity,Np); //get velocity
			ScaLBL_Comm->SendD3Q19AA(cq); //read overlapping boundary info from neighbouring blocks
		    ScaLBL_Comm->RecvD3Q19AA(cq); //write boundary info to neighbouring blocks
		    ScaLBL_DeviceBarrier();
		    // Set BCs
		    ScaLBL_Comm->D3Q19_Pressure_BC_z(NeighborList, cq, 1.1, timestep);
		    ScaLBL_Comm->D3Q19_Pressure_BC_Z(NeighborList, cq, 1.0, timestep);
	        ScaLBL_D3Q19_AAeven_ThermalBGK(Velocity, cq, 0, ScaLBL_Comm->LastInterior(), Np, omega); 
		    ScaLBL_DeviceBarrier(); MPI_Barrier(comm);
		}
//		if (FDThermalFlag) {
//			ScaLBL_D3Q19_Momentum(fq,Velocity,Np); //get velocity
//		    ScaLBL_Comm->FDM_Concentration_BC_z(NeighborList, cq, 2.0);
//			ScaLBL_Comm->SendHalo(cq); //read overlapping boundary info from neighbouring blocks
//		    ScaLBL_Comm->RecvHalo(cq); //write boundary info to neighbouring blocks
//		    ScaLBL_DeviceBarrier();
//            //explicit FDM using ghost cells doesnt need BC updating
//            ScaLBL_FDM_ConvectionDiffusion(NeighborList, Velocity, cq, 0, ScaLBL_Comm->LastInterior(), Np, DiffCoeff, 2.0);
//		    ScaLBL_DeviceBarrier(); MPI_Barrier(comm);
//		}
		//************************************************************************/
		if (timestep%analysis_interval==0){
			ScaLBL_D3Q19_Momentum(fq,Velocity,Np);
			ScaLBL_D3Q19_Pressure(fq,Pressure,Np);
		    
			ScaLBL_DeviceBarrier(); MPI_Barrier(comm);
			ScaLBL_Comm->RegularLayout(Map,&Velocity[0],Velocity_x);
			ScaLBL_Comm->RegularLayout(Map,&Velocity[Np],Velocity_y);
			ScaLBL_Comm->RegularLayout(Map,&Velocity[2*Np],Velocity_z);
			double count_loc=0;
			double count;
			double vax,vay,vaz;
			double vax_loc,vay_loc,vaz_loc;
			vax_loc = vay_loc = vaz_loc = 0.f;
			for (int k=1; k<Nz-1; k++){
				for (int j=1; j<Ny-1; j++){
					for (int i=1; i<Nx-1; i++){
						count_loc+=1.0;
						if (Geom(i,j,k) > 0){
							vax_loc += Velocity_x(i,j,k);
							vay_loc += Velocity_y(i,j,k);
							vaz_loc += Velocity_z(i,j,k);

						}
					}
				}
			}
			MPI_Allreduce(&vax_loc,&vax,1,MPI_DOUBLE,MPI_SUM,Mask->Comm);
			MPI_Allreduce(&vay_loc,&vay,1,MPI_DOUBLE,MPI_SUM,Mask->Comm);
			MPI_Allreduce(&vaz_loc,&vaz,1,MPI_DOUBLE,MPI_SUM,Mask->Comm);
			MPI_Allreduce(&count_loc,&count,1,MPI_DOUBLE,MPI_SUM,Mask->Comm);
			
			vax /= count;
			vay /= count;
			vaz /= count;
			
		    if (thermalFlag) {// get concentration
    			ScaLBL_D3Q19_Pressure(cq,Concentration,Np);
				ScaLBL_DeviceBarrier(); MPI_Barrier(comm);
			    ScaLBL_Comm->RegularLayout(Map,&Concentration[0],ConcentrationCart);

                //some stupid bs is preventing me from functionalising this, probably the bool init
		        if (timestep%fqInterval==0) {
	                char LocalRankFoldername[100];
	                if (rank==0) {
		                sprintf(LocalRankFoldername,"./rawVisConcentration%d",timestep); 
	                    mkdir(LocalRankFoldername, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
                    }
	                MPI_Barrier(comm);
	                //create the file
	                FILE *OUTFILE;
	                char LocalRankFilename[100];
	                sprintf(LocalRankFilename,"rawVisConcentration%d/Part_%d_%d_%d_%d_%d_%d_%d.txt",timestep,rank,Nx,Ny,Nz,nprocx,nprocy,nprocz); //change this file name to include the size
	                OUTFILE = fopen(LocalRankFilename,"w");
                    int idx;
                    for (int k=0; k<Nz; k++){
	                    for (int j=0; j<Ny; j++){
		                    for (int i=0; i<Nx; i++){
				                idx=Map(i,j,k);
		                        //if (idx<0) ConcentrationCart(i, j, k)=69420;
                                //fqTensor(i,j,k,d)=fqField(i,j,k);
			                    //fprintf(OUTFILE,"%f\n",ConcentrationCart(i, j, k));
			                    double temp = ConcentrationCart(i,j,k);
	                            fwrite(&temp,sizeof(double),1,OUTFILE);
		                    }
	                    }
                    }
	                fclose(OUTFILE);
	                MPI_Barrier(comm);
                }


			    double meanCLoc=0.0;
			    double meanCGlob=0.0;
    			double count_loc=0;
			    double count;
			    for (int k=1; k<Nz-1; k++){
				    for (int j=1; j<Ny-1; j++){
					    for (int i=1; i<Nx-1; i++){
						    count_loc+=1.0;
						    if (Geom(i,j,k) > 0){
							    meanCLoc += ConcentrationCart(i,j,k);
						    }
					    }
				    }
			    }
			    MPI_Allreduce(&meanCLoc,&meanCGlob,1,MPI_DOUBLE,MPI_SUM,Mask->Comm);
			    MPI_Allreduce(&count_loc,&count,1,MPI_DOUBLE,MPI_SUM,Mask->Comm);
			    meanCGlob /= count;
			    double NPe = sqrt(vax*vax+vay*vay+vaz*vaz)/DiffCoeff;
		        if (rank==0) printf("Mean Peclet Number: %0.4e, Mean concentration in domain: %0.4f\n",NPe,meanCGlob);
		    }
			
			
			//if (rank==0) printf("Computing Minkowski functionals \n");
			//Morphology.ComputeScalar(Geom,0.f);
			//Morphology.PrintAll();
			double mu = (tau-0.5)/3.f; //this is the kimematic viscosity, so use momentum in v to cancel out
			double gradP=sqrt(Fx*Fx+Fy*Fy+Fz*Fz)+(din-dout)/((Nz-2)*nprocz)/3;
			double absperm = voxelSize*voxelSize*mu*sqrt(vax*vax+vay*vay+vaz*vaz)/gradP;
			double abspermZ = voxelSize*voxelSize*mu*vaz/gradP;
			double convRate = fabs((absperm-Kold)/Kold)*100;
            double MLUPSGlob;
            double MLUPS;
        	stoptime = MPI_Wtime();
    		cputime = (stoptime - starttime);
    		if (thermalFlag) {
			    MLUPS =  double(Np)*2*timestep/cputime/1000000;
			} else {
			    MLUPS =  double(Np)*timestep/cputime/1000000;
			}
			MPI_Allreduce(&MLUPS,&MLUPSGlob,1,MPI_DOUBLE,MPI_SUM,Mask->Comm);
			if (rank==0) {
				printf("Timestep: %d, MLUPS: %f, K = %f Darcies (RMS), %f Darcies (Z-Dir), Time %0.2fs, dK/dt (%) = %0.4f, deltaP: %0.4e, fluxBar: %0.4e\n",timestep, MLUPSGlob,absperm*9.87e11,  abspermZ*9.87e11, cputime, convRate, gradP, vaz*count/(Nz*nprocz));
				FILE * log_file = fopen("Permeability.csv","a");
				fprintf(log_file,"%i %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g\n",timestep, Fx, Fy, Fz, din, dout, mu, vax,vay,vaz, absperm);
				fclose(log_file);
			}
			Kold=absperm;
			tol = convRate/100;
			if (tol<permTolerance) {
			    if (rank==0) printf("Convergence criteria reached, stopping early\n");
			    if (visTolerance) {
			        if (fqFlag) fqField();//
			        else velPField();//
			    }
			    break;
			}
		}

		if (timestep%fqInterval==0) {
	        if (fqFlag) fqField();//
	        else velPField();//
		}
	}
	//************************************************************************/
}

void ScaLBL_MRTModel::fqField(){
    	//create the folder
	char LocalRankFoldername[100];
	if (rank==0) {
		sprintf(LocalRankFoldername,"./rawVisFq%d",timestep); 
	    mkdir(LocalRankFoldername, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    }
	MPI_Barrier(comm);
	//create the file
	FILE *OUTFILE;
	char LocalRankFilename[100];
	sprintf(LocalRankFilename,"rawVisFq%d/Part_%d_%d_%d_%d_%d_%d_%d.txt",timestep,rank,Nx,Ny,Nz,nprocx,nprocy,nprocz); //change this file name to include the size
	OUTFILE = fopen(LocalRankFilename,"w");
	
    for (int d=0; d<19; d++) {
	    // copy to regular layout
        DoubleArray fqField(Nx, Ny, Nz);
		ScaLBL_Comm->RegularLayout(Map,&fq[d*Np],fqField);   
	    for (int k=0; k<Nz; k++){
		    for (int j=0; j<Ny; j++){
			    for (int i=0; i<Nx; i++){
    			    //fprintf(OUTFILE,"%f\n",fqField(i, j, k));
			        double temp = fqField(i,j,k);
	                fwrite(&temp,sizeof(double),1,OUTFILE);
			    }
		    }
	    }
	}
	fclose(OUTFILE);
	MPI_Barrier(comm);
}

void ScaLBL_MRTModel::velPField(){
    	//create the folder
	char LocalRankFoldername[100];
	if (rank==0) {
		sprintf(LocalRankFoldername,"./rawVisVelP%d",timestep); 
	    mkdir(LocalRankFoldername, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    }
	MPI_Barrier(comm);
	//create the file
	FILE *OUTFILE;
	char LocalRankFilename[100];
	sprintf(LocalRankFilename,"rawVisVelP%d/Part_%d_%d_%d_%d_%d_%d_%d.txt",timestep,rank,Nx,Ny,Nz,nprocx,nprocy,nprocz); //change this file name to include the size
	OUTFILE = fopen(LocalRankFilename,"wb");
    DoubleArray vx(Nx, Ny, Nz);
    DoubleArray vy(Nx, Ny, Nz);
    DoubleArray vz(Nx, Ny, Nz);
    DoubleArray P(Nx, Ny, Nz);
	ScaLBL_Comm->RegularLayout(Map,&Velocity[0],vx);
	ScaLBL_Comm->RegularLayout(Map,&Velocity[Np],vy);
	ScaLBL_Comm->RegularLayout(Map,&Velocity[2*Np],vz);
    ScaLBL_Comm->RegularLayout(Map,&Pressure[0],P);
//	fwrite(vx.data(),sizeof(double),N,OUTFILE);
//	fwrite(vy.data(),sizeof(double),N,OUTFILE);
//	fwrite(vz.data(),sizeof(double),N,OUTFILE);
//	fwrite(P.data(),sizeof(double),N,OUTFILE);
    for (int k=0; k<Nz; k++){
	    for (int j=0; j<Ny; j++){
		    for (int i=0; i<Nx; i++){
			    //fprintf(OUTFILE,"%f\n",vx(i, j, k));
			    double temp = vx(i,j,k);
	            fwrite(&temp,sizeof(double),1,OUTFILE);
		    }
	    }
    }
    for (int k=0; k<Nz; k++){
	    for (int j=0; j<Ny; j++){
		    for (int i=0; i<Nx; i++){
			    double temp = vy(i,j,k);
			    //fprintf(OUTFILE,"%f\n",vy(i, j, k));
	            fwrite(&temp,sizeof(double),1,OUTFILE);
		    }
	    }
    }
    for (int k=0; k<Nz; k++){
	    for (int j=0; j<Ny; j++){
		    for (int i=0; i<Nx; i++){
			    double temp = vz(i,j,k);
			    //fprintf(OUTFILE,"%f\n",vz(i, j, k));
	            fwrite(&temp,sizeof(double),1,OUTFILE);
		    }
	    }
    }
    for (int k=0; k<Nz; k++){
	    for (int j=0; j<Ny; j++){
		    for (int i=0; i<Nx; i++){
			    double temp = P(i,j,k);
			    //fprintf(OUTFILE,"%f\n",P(i, j, k));
	            fwrite(&temp,sizeof(double),1,OUTFILE);
		    }
	    }
    }
	fclose(OUTFILE);
	MPI_Barrier(comm);
}

