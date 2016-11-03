#if USE_PETSC

/*
Copyright (c) 2016, Los Alamos National Security, LLC
All rights reserved.
Copyright 2016. Los Alamos National Security, LLC. This software was produced 
under U.S. Government contract DE-AC52-06NA25396 for Los Alamos National 
Laboratory (LANL), which is operated by Los Alamos National Security, LLC for 
the U.S. Department of Energy. The U.S. Government has rights to use, 
reproduce, and distribute this software.  NEITHER THE GOVERNMENT NOR LOS 
ALAMOS NATIONAL SECURITY, LLC MAKES ANY WARRANTY, EXPRESS OR IMPLIED, OR 
ASSUMES ANY LIABILITY FOR THE USE OF THIS SOFTWARE.  If software is modified 
to produce derivative works, such modified software should be clearly marked, 
so as not to confuse it with the version available from LANL.
Additionally, redistribution and use in source and binary forms, with or 
without modification, are permitted provided that the following conditions 
are met:
1.      Redistributions of source code must retain the above copyright notice, 
        this list of conditions and the following disclaimer.
2.      Redistributions in binary form must reproduce the above copyright 
        notice, this list of conditions and the following disclaimer in the 
        documentation and/or other materials provided with the distribution.
3.      Neither the name of Los Alamos National Security, LLC, Los Alamos 
        National Laboratory, LANL, the U.S. Government, nor the names of its 
        contributors may be used to endorse or promote products derived from 
        this software without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY LOS ALAMOS NATIONAL SECURITY, LLC AND 
CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT 
NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A 
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL LOS ALAMOS NATIONAL 
SECURITY, LLC OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, 
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; 
OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR 
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED 
OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include "SweeperSchur.hh"
#include "SourceIteration.hh"
#include "Global.hh"
#include "TraverseGraph.hh"
#include "Priorities.hh"
#include "Transport.hh"
#include "PsiData.hh"
#include "Comm.hh"
#include "SweepData.hh"
#include "CommSides.hh"
#include <omp.h>
#include <vector>
#include <math.h>
#include <limits>
#include <string.h>
#include <petscmat.h>
#include <petscvec.h>
#include <petscksp.h>


// Constants
static const bool s_doComm = false;
static const UINT s_maxComputePerStep = std::numeric_limits<uint64_t>::max();


// Data needed for Schur and SchurOuter functions
struct SchurData
{
    CommSides *commSides;
    PsiData *psi;
    PsiBoundData *psiBound;
    PsiData *source;
    Mat2<UINT> *priorities;

    // Only needed for SchurOuter
    std::vector<UINT> *sourceIts;
    SweeperSchurOuter *sweeperSchurOuter;
};


/*
    psiBoundToVec 
*/
static
void psiBoundToVec(double *x, const PsiBoundData &psiBound)
{
    int xArrayIndex = 0;
    

    // Set array
    for (UINT angle = 0; angle < g_nAngles; ++angle) {
    for (UINT cell = 0; cell < g_nCells; ++cell) {
    for (UINT group = 0; group < g_nGroups; group++) {
    for (UINT face = 0; face < g_nFacePerCell; face++) {
        if (g_tychoMesh->isIncoming(angle, cell, face)) {
            UINT adjCell = g_tychoMesh->getAdjCell(cell, face);
            UINT adjRank = g_tychoMesh->getAdjRank(cell, face);
            
            // On internal boundary
            if (adjCell == TychoMesh::BOUNDARY_FACE && 
                adjRank != TychoMesh::BAD_RANK)
            {
                UINT side = g_tychoMesh->getSide(cell, face);
                for (UINT fvrtx = 0; fvrtx < g_nVrtxPerFace; fvrtx++) {
                    x[xArrayIndex] = psiBound(group, fvrtx, angle, side);
                    xArrayIndex++;
                }
            }
        }
    }}}}
}


/*
    vecToPsiBound
*/
static
void vecToPsiBound(const double *x, PsiBoundData &psiBound)
{
    int xArrayIndex = 0;


    // Set psiBound
    for (UINT angle = 0; angle < g_nAngles; ++angle) {
    for (UINT cell = 0; cell < g_nCells; ++cell) {
    for (UINT group = 0; group < g_nGroups; group++) {
    for (UINT face = 0; face < g_nFacePerCell; face++) {
        if (g_tychoMesh->isIncoming(angle, cell, face)) {
            UINT adjCell = g_tychoMesh->getAdjCell(cell, face);
            UINT adjRank = g_tychoMesh->getAdjRank(cell, face);
            
            // On internal boundary
            if (adjCell == TychoMesh::BOUNDARY_FACE && 
                adjRank != TychoMesh::BAD_RANK)
            {
                UINT side = g_tychoMesh->getSide(cell, face);
                for (UINT fvrtx = 0; fvrtx < g_nVrtxPerFace; fvrtx++) {
                    psiBound(group, fvrtx, angle, side) = x[xArrayIndex];
                    xArrayIndex++;
                }
            }
        }
    }}}}
}


/*
    getVecSize
*/
static
UINT getVecSize()
{
    // Start an array index
    UINT size = 0;
    

    // Incoming flux
    for (UINT angle = 0; angle < g_nAngles; ++angle) {
    for (UINT cell = 0; cell < g_nCells; ++cell) {
    for (UINT group = 0; group < g_nGroups; group++) {
    for (UINT face = 0; face < g_nFacePerCell; face++) {
        
        if (g_tychoMesh->isIncoming(angle, cell, face)) {
            
            UINT neighborCell = g_tychoMesh->getAdjCell(cell, face);
            UINT adjRank = g_tychoMesh->getAdjRank(cell, face);
            
            // In local mesh
            if (neighborCell == TychoMesh::BOUNDARY_FACE &&  
                adjRank != TychoMesh::BAD_RANK)
            {
                for (UINT fvrtx = 0; fvrtx < g_nVrtxPerFace; fvrtx++) {
                    size++;
                }
            }
        }
    }}}}

    return size;
}


/*
    Schur 

    This performs the sweep and returns the boundary
*/
static
void Schur(const double *x, double *b, void *voidData)
{
    // Get data for the solve
    SchurData *data = (SchurData*) voidData;


    // Vector -> array
    vecToPsiBound(x, *data->psiBound);


    // Traverse the graph to get the values on the outward facing boundary
    // call commSides to transfer boundary data
    SweepData sweepData(*data->psi, *data->source, *data->psiBound, g_sigmaTotal, 
                        *data->priorities);
    traverseGraph(s_maxComputePerStep, sweepData, s_doComm, MPI_COMM_WORLD, 
                  Direction_Forward);
    data->commSides->commSides(*data->psi, *data->psiBound);

    
    // Take the values of s_psi from the sweep and put them in b
    psiBoundToVec(b, *data->psiBound);


    // b = x - b (aka Out = In - Out)
    UINT vecSize = getVecSize();
    for (UINT i = 0; i < vecSize; i++) {
        b[i] = x[i] - b[i];
    }
}


/*
    SchurOuter 

    This performs the sweep and returns the boundary
*/
static
void SchurOuter(const double *x, double *b, void *voidData)
{
    // Get data for the solve
    SchurData *data = (SchurData*) voidData;


    // Vector -> array
    vecToPsiBound(x, *data->psiBound);


    // Traverse the graph to get the values on the outward facing boundary
    // call commSides to transfer boundary data
    UINT its;
    data->source->setToValue(0.0);
    if (g_useSourceIteration)
        its = SourceIteration::fixedPoint(data->sweeperSchurOuter, *data->psi,
                                          *data->source, true);
    else
        its = SourceIteration::krylov(data->sweeperSchurOuter, *data->psi,
                                      *data->source, true);
    data->sourceIts->push_back(its);
    data->commSides->commSides(*data->psi, *data->psiBound);

    
    // Take the values of s_psi from the sweep and put them in b
    psiBoundToVec(b, *data->psiBound);


    // b = x - b (aka Out = In - Out)
    for (UINT i = 0; i < getVecSize(); i++) {
        b[i] = x[i] - b[i];
    }
}


/*
    petscInit
*/
/*void petscInit(Mat &A, Vec &x, Vec &b, KSP &ksp, void (*func)(void))
{
    PetscInt vecSize;
    PetscInt totalVecSize;


    // Local and global vector sizes
    vecSize = getVecSize();
    UINT totalSize = vecSize;
    Comm::gsum(totalSize);
    totalVecSize = totalSize;


    // Create vectors
    VecCreate(MPI_COMM_WORLD, &x);
    VecSetSizes(x, vecSize, totalVecSize);
    VecSetType(x, VECMPI);
    VecDuplicate(x, &b);
    

    // Create matrix shell and define it as the operator
    MatCreateShell(MPI_COMM_WORLD, vecSize, vecSize, totalVecSize, totalVecSize,
                   (void*)(NULL), &A);
    MatShellSetOperation(A, MATOP_MULT, func);
    

    // Set solver context
    KSPCreate(MPI_COMM_WORLD, &ksp);
    

    // Set operator to KSP context. No preconditioning will actually
    // be used due to the PCNONE option.
    KSPSetOperators(ksp, A, A);
    KSPSetTolerances(ksp, g_ddErrMax, PETSC_DEFAULT, PETSC_DEFAULT, 
                     g_ddIterMax);
}*/


/*
    petscEnd
*/
/*static
void petscEnd(Mat &A, Vec &x, Vec &b, KSP &ksp)
{
    // Destroy vectors, ksp, and matrices to free work space
    VecDestroy(&x);
    VecDestroy(&b);
    MatDestroy(&A);
    KSPDestroy(&ksp);
}*/


/*
    solve
*/
void SweeperSchur::solve()
{
    // Init petsc
    c_krylovSolver = 
        new KrylovSolver(getVecSize(), g_ddErrMax, g_ddIterMax, Schur);

    
    // Solve
    c_iters = 0;
    SourceIteration::getProblemSource(c_source);
    c_psi.setToValue(0.0);

    if (g_useSourceIteration)
        SourceIteration::fixedPoint(this, c_psi, c_source);
    else
        SourceIteration::krylov(this, c_psi, c_source);

    
    // Print data
    if (Comm::rank() == 0) {
        printf("Num source iters: %" PRIu64 "\n", c_iters);
    }

    delete c_krylovSolver;
}


/*
    sweep
    
    Run the Krylov solver
*/
void SweeperSchur::sweep(PsiData &psi, const PsiData &source)
{
    // Initialize variables
    PsiData zeroSource;
    zeroSource.setToValue(0.0);
    
    Mat2<UINT> priorities(g_nCells, g_nAngles);
    PsiBoundData psiBound;
    SweepData sweepData(psi, source, psiBound, g_sigmaTotal, priorities);
    
    double rnorm;
    UINT its;
    double *x;
    double *b;

    
    // Set static variables
    SchurData data;
    data.commSides = &c_commSides;
    data.psi = &psi;
    data.psiBound = &psiBound;
    data.source = &zeroSource;
    data.priorities = &priorities;
    c_krylovSolver->setData(&data);
    
    
    // Initial guess
    x = c_krylovSolver->getX();
    psiBoundToVec(x, c_psiBoundPrev);
    c_krylovSolver->releaseX();
    c_krylovSolver->setInitialGuessNonzero();
    
    
    // Do a sweep on the source 
    if (Comm::rank() == 0) {
        printf("    Sweeping Source\n");
    }
    psiBound.setToValue(0.0);
    traverseGraph(s_maxComputePerStep, sweepData, s_doComm, 
                  MPI_COMM_WORLD, Direction_Forward);
    if (Comm::rank() == 0) {
        printf("    Source Swept\n");
    }


    // Input source into b
    c_commSides.commSides(psi, psiBound);
    b = c_krylovSolver->getB();
    psiBoundToVec(b, psiBound);
    c_krylovSolver->releaseB();


    // Solve the system (x is the solution, b is the RHS)
    if (Comm::rank() == 0) {
        printf("    Starting Krylov Solve on Boundary\n");
    }
    c_krylovSolver->solve();
    

    // Print some stats
    its = c_krylovSolver->getNumIterations();
    rnorm = c_krylovSolver->getResidualNorm();
    if (Comm::rank() == 0) {
        printf("    Krylov iterations: %" PRIu64 " with Rnorm: %e\n", its, rnorm);
    }


    // Put x in XOut and output the answer from XOut to psi
    x = c_krylovSolver->getX();
    vecToPsiBound(x, psiBound);
    vecToPsiBound(x, c_psiBoundPrev);
    c_krylovSolver->releaseX();


    // Sweep to solve for the non-boundary values
    if (Comm::rank() == 0) {
        printf("    Sweeping to solve non-boundary values\n");
    }
    traverseGraph(s_maxComputePerStep, sweepData, s_doComm, MPI_COMM_WORLD, 
                  Direction_Forward);
    if (Comm::rank() == 0) {
        printf("    Non-boundary values swept\n");
    }


    c_iters += 2 + its;
}


/*
    solve
*/
void SweeperSchurOuter::solve()
{
    double *x;
    double *b;


    // Init petsc
    c_krylovSolver = 
        new KrylovSolver(getVecSize(), g_ddErrMax, g_ddIterMax, SchurOuter);

    
    // Variables
    double rnorm;
    UINT its;
    UINT sourceIts1, sourceIts3;
    SweepData sweepData(c_psi, c_source, c_psiBound, g_sigmaTotal, c_priorities);
    std::vector<UINT> sourceItsVec;


    // Set data for Krylov solver
    SchurData data;
    data.commSides = &c_commSides;
    data.psi = &c_psi;
    data.psiBound = &c_psiBound;
    data.source = &c_source;
    data.priorities = &c_priorities;
    data.sourceIts = &sourceItsVec;
    data.sweeperSchurOuter = this;
    c_krylovSolver->setData(&data);


    // Initialize class variables
    SourceIteration::getProblemSource(c_source);
    c_psi.setToValue(0.0);
    c_psiBound.setToValue(0.0);
    c_useZeroPsiBound = false;
    
    
    // Do a sweep on the source 
    if (Comm::rank() == 0) {
        printf("    Sweeping Source\n");
    }
    
    if (g_useSourceIteration)
        sourceIts1 = SourceIteration::fixedPoint(this, c_psi, c_source);
    else
        sourceIts1 = SourceIteration::krylov(this, c_psi, c_source);
    
    if (Comm::rank() == 0) {
        printf("    Source Swept\n");
    }


    // Input source into b
    //PsiBoundData psiBound;
    c_commSides.commSides(c_psi, c_psiBound);
    b = c_krylovSolver->getB();
    psiBoundToVec(b, c_psiBound);
    c_krylovSolver->releaseB();


    // Initial guess
    c_psiBound.setToValue(0.0);
    c_psi.setToValue(0.0);
    c_source.setToValue(0.0);
    
    
    // Solve the system (x is the solution, b is the RHS)
    if (Comm::rank() == 0) {
        printf("    Starting Krylov Solve on Boundary\n");
    }
    c_krylovSolver->solve();
    

    // Print some stats
    its = c_krylovSolver->getNumIterations();
    rnorm = c_krylovSolver->getResidualNorm();


    // Put x in XOut and output the answer from XOut to psi
    x = c_krylovSolver->getX();
    vecToPsiBound(x, c_psiBound);
    c_krylovSolver->releaseX();


    // Sweep to solve for the non-boundary values
    if (Comm::rank() == 0) {
        printf("    Sweeping to solve non-boundary values\n");
    }
    
    SourceIteration::getProblemSource(c_source);
    if (g_useSourceIteration)
        sourceIts3 = SourceIteration::fixedPoint(this, c_psi, c_source);
    else
        sourceIts3 = SourceIteration::krylov(this, c_psi, c_source);
    
    if (Comm::rank() == 0) {
        printf("Non-boundary values swept\n");
        printf("Krylov iterations: %" PRIu64 " with Rnorm: %e\n", its, rnorm);
        printf("Num sweeps Q: %" PRIu64 "\n", sourceIts1);
        printf("Num sweeps KSP:");
        for (UINT i = 0; i < sourceItsVec.size(); i++)
            printf(" %" PRIu64, sourceItsVec[i]);
        printf("\n");
        printf("Num sweeps END: %" PRIu64 "\n", sourceIts3);
    }
}


/*
    sweep
    
    Run the Krylov solver
*/
void SweeperSchurOuter::sweep(PsiData &psi, const PsiData &source)
{
    PsiBoundData zeroPsiBound;
    
    if (c_useZeroPsiBound) {
        SweepData sweepData(psi, source, zeroPsiBound, g_sigmaTotal, c_priorities);
        traverseGraph(s_maxComputePerStep, sweepData, s_doComm, MPI_COMM_WORLD,
                      Direction_Forward);
    }
    else {
        SweepData sweepData(psi, source, c_psiBound, g_sigmaTotal, c_priorities);
        traverseGraph(s_maxComputePerStep, sweepData, s_doComm, MPI_COMM_WORLD,
                      Direction_Forward);
    }

}


#endif
