// ==== CMAKE =====
// @FUSE_BLAS
// ================
// Keep in private GIT
// @FUSE_MPI
// @FUSE_STARPU


#include "Utils/FGlobal.hpp"

#include "GroupTree/Core/FGroupTree.hpp"

#include "Components/FSimpleLeaf.hpp"
#include "Components/FSymbolicData.hpp"
#include "Containers/FVector.hpp"

#include "Kernels/P2P/FP2PParticleContainer.hpp"

#include "Kernels/Chebyshev/FChebSymKernel.hpp"
#include "Kernels/Chebyshev/FChebCell.hpp"
#include "Kernels/Interpolation/FInterpMatrixKernel.hpp"

#include "Utils/FMath.hpp"
#include "Utils/FMemUtils.hpp"
#include "Utils/FParameters.hpp"

#include "Files/FRandomLoader.hpp"
#include "Files/FFmaGenericLoader.hpp"

#include "GroupTree/Core/FGroupSeqAlgorithm.hpp"
#include "GroupTree/Core/FGroupTaskAlgorithm.hpp"
#include "GroupTree/Core/FGroupTaskStarpuAlgorithm.hpp"
#include "GroupTree/Core/FP2PGroupParticleContainer.hpp"

#include "Utils/FParameterNames.hpp"

#include "Components/FTestParticleContainer.hpp"
#include "Components/FTestCell.hpp"
#include "Components/FTestKernels.hpp"

#include "Core/FFmmAlgorithmThreadProc.hpp"
#include "Files/FMpiTreeBuilder.hpp"
#include "GroupTree/Core/FGroupTaskStarpuMpiAlgorithm.hpp"

#include "Files/FMpiFmaGenericLoader.hpp"
#include "Containers/FCoordinateComputer.hpp"

#include "GroupTree/StarPUUtils/FStarPUKernelCapacities.hpp"

#include <memory>


int main(int argc, char* argv[]){
    const FParameterNames LocalOptionBlocSize { {"-bs"}, "The size of the block of the blocked tree"};
    const FParameterNames LocalOptionNoValidate { {"-no-validation"}, "To avoid comparing with direct computation"};
    FHelpDescribeAndExit(argc, argv, "Test the blocked tree by counting the particles.",
                         FParameterDefinitions::OctreeHeight,FParameterDefinitions::InputFile,
                         FParameterDefinitions::OctreeSubHeight,
                         LocalOptionBlocSize, LocalOptionNoValidate);

    typedef double FReal;
    // Initialize the types
    static const int ORDER = 6;
    typedef FInterpMatrixKernelR<FReal> MatrixKernelClass;

    using GroupCellClass     = FChebCell<FReal, ORDER>;
    using GroupCellUpClass   = typename GroupCellClass::multipole_t;
    using GroupCellDownClass = typename GroupCellClass::local_expansion_t;
    using GroupCellSymbClass = FSymbolicData;


    typedef FP2PGroupParticleContainer<FReal>          GroupContainerClass;
    typedef FGroupTree< FReal, GroupCellSymbClass, GroupCellUpClass, GroupCellDownClass, GroupContainerClass, 1, 4, FReal>  GroupOctreeClass;

    typedef FStarPUAllCpuCapacities<FChebSymKernel<FReal,GroupCellClass,GroupContainerClass,MatrixKernelClass,ORDER>> GroupKernelClass;
    typedef FStarPUCpuWrapper<typename GroupOctreeClass::CellGroupClass, GroupCellClass, GroupKernelClass, typename GroupOctreeClass::ParticleGroupClass, GroupContainerClass> GroupCpuWrapper;
    typedef FGroupTaskStarPUMpiAlgorithm<GroupOctreeClass, typename GroupOctreeClass::CellGroupClass, GroupKernelClass, typename GroupOctreeClass::ParticleGroupClass, GroupCpuWrapper> GroupAlgorithm;

    // Get params
    FTic timer;
    const int groupSize     = FParameters::getValue(argc,argv,LocalOptionBlocSize.options, 250);

    FMpi mpiComm(argc,argv);

    const char* const filename       = FParameters::getStr(argc,argv,FParameterDefinitions::InputFile.options, "../Data/test20k.fma");
    const unsigned int TreeHeight    = FParameters::getValue(argc, argv, FParameterDefinitions::OctreeHeight.options, 5);
    const unsigned int SubTreeHeight = FParameters::getValue(argc, argv, FParameterDefinitions::OctreeSubHeight.options, 2);

    // init particles position and physical value
    struct TestParticle{
        FPoint<FReal> position;
        FReal physicalValue;
        const FPoint<FReal>& getPosition(){
            return position;
        }
    };

    // open particle file
    std::cout << "Opening : " << filename << "\n" << std::endl;
    FMpiFmaGenericLoader<FReal> loader(filename,mpiComm.global());
    FAssertLF(loader.isOpen());

    TestParticle* allParticles = new TestParticle[loader.getMyNumberOfParticles()];
    memset(allParticles,0,(unsigned int) (sizeof(TestParticle)* loader.getMyNumberOfParticles()));
    for(FSize idxPart = 0 ; idxPart < loader.getMyNumberOfParticles() ; ++idxPart){
        loader.fillParticle(&allParticles[idxPart].position,&allParticles[idxPart].physicalValue);
    }

    FVector<TestParticle> myParticles;
    FLeafBalance balancer;
    FMpiTreeBuilder< FReal,TestParticle >::DistributeArrayToContainer(mpiComm.global(),allParticles,
                                                                loader.getMyNumberOfParticles(),
                                                                loader.getCenterOfBox(),
                                                                loader.getBoxWidth(),TreeHeight,
                                                                &myParticles, &balancer);

    std::cout << "Creating & Inserting " << loader.getMyNumberOfParticles() << " particles ..." << std::endl;
    std::cout << "For a total of " << loader.getNumberOfParticles() << " particles ..." << std::endl;
    std::cout << "\tHeight : " << TreeHeight << " \t sub-height : " << SubTreeHeight << std::endl;

    // Each proc need to know the righest morton index
    const FTreeCoordinate host = FCoordinateComputer::GetCoordinateFromPosition<FReal>(
                loader.getCenterOfBox(),
                loader.getBoxWidth(),
                TreeHeight,
                myParticles[myParticles.getSize()-1].position );
    const MortonIndex myLeftLimite = host.getMortonIndex();
    MortonIndex leftLimite = -1;
    if(mpiComm.global().processId() != 0){
        FMpi::Assert(MPI_Recv(&leftLimite, sizeof(leftLimite), MPI_BYTE,
                              mpiComm.global().processId()-1, 0,
                              mpiComm.global().getComm(), MPI_STATUS_IGNORE), __LINE__);
    }
    if(mpiComm.global().processId() != mpiComm.global().processCount()-1){
        FMpi::Assert(MPI_Send(const_cast<MortonIndex*>(&myLeftLimite), sizeof(myLeftLimite), MPI_BYTE,
                              mpiComm.global().processId()+1, 0,
                              mpiComm.global().getComm()), __LINE__);
    }
    FLOG(std::cout << "My last index is " << leftLimite << "\n");
    FLOG(std::cout << "My left limite is " << myLeftLimite << "\n");


    // Put the data into the tree
    FP2PParticleContainer<FReal> myParticlesInContainer;
    for(FSize idxPart = 0 ; idxPart < myParticles.getSize() ; ++idxPart){
        myParticlesInContainer.push(myParticles[idxPart].position,
                                    myParticles[idxPart].physicalValue);
    }
    GroupOctreeClass groupedTree(TreeHeight, loader.getBoxWidth(), loader.getCenterOfBox(), groupSize,
                                 &myParticlesInContainer, true, leftLimite);
    groupedTree.printInfoBlocks();

    timer.tac();
    std::cout << "Done  " << "(@Creating and Inserting Particles = "
              << timer.elapsed() << "s)." << std::endl;

    { // -----------------------------------------------------
        std::cout << "\nChebyshev FMM (ORDER="<< ORDER << ") ... " << std::endl;
        timer.tic();

        MatrixKernelClass MatrixKernel;
        // Create Matrix Kernel
        GroupKernelClass groupkernel(TreeHeight, loader.getBoxWidth(), loader.getCenterOfBox(), &MatrixKernel);
        // Run the algorithm
        GroupAlgorithm groupalgo(mpiComm.global(), &groupedTree,&groupkernel);
        groupalgo.execute();

        timer.tac();
        std::cout << "Done  " << "(@Algorithm = " << timer.elapsed() << "s)." << std::endl;
    } // -----------------------------------------------------


    if(FParameters::existParameter(argc, argv, LocalOptionNoValidate.options) == false){
        typedef FP2PParticleContainer<FReal> ContainerClass;
        typedef FSimpleLeaf<FReal, ContainerClass >  LeafClass;
        typedef FChebCell<FReal,ORDER> CellClass;
        typedef FOctree<FReal, CellClass,ContainerClass,LeafClass> OctreeClass;
        typedef FChebSymKernel<FReal,CellClass,ContainerClass,MatrixKernelClass,ORDER> KernelClass;
        typedef FFmmAlgorithmThreadProc<OctreeClass,CellClass,ContainerClass,KernelClass,LeafClass> FmmClass;

        const FReal epsi = 1E-10;

        OctreeClass treeCheck(TreeHeight, SubTreeHeight,loader.getBoxWidth(),loader.getCenterOfBox());

        for(FSize idxPart = 0 ; idxPart < myParticles.getSize() ; ++idxPart){
            // put in tree
            treeCheck.insert(myParticles[idxPart].position,
                             myParticles[idxPart].physicalValue);
        }

        MatrixKernelClass MatrixKernel;
        KernelClass kernels(TreeHeight, loader.getBoxWidth(), loader.getCenterOfBox(), &MatrixKernel);
        FmmClass algorithm(mpiComm.global(),&treeCheck, &kernels);
        algorithm.execute();
        std::cout << "Algo is over" << std::endl;

        groupedTree.forEachCellWithLevel(
            [&](GroupCellSymbClass* gsymb ,
                GroupCellUpClass*   gmul,
                GroupCellDownClass* gloc,
                const int level)
            {
                const CellClass* cell = treeCheck.getCell(gsymb->getMortonIndex(), level);
                if(cell == nullptr){
                    std::cout << "[Empty] Error cell should exist " << gsymb->getMortonIndex() << "\n";
                }
                else {
                    FMath::FAccurater<FReal> diffUp;
                    diffUp.add(cell->getMultipoleData().get(0), gmul->get(0), gmul->getVectorSize());
                    if(diffUp.getRelativeInfNorm() > epsi || diffUp.getRelativeL2Norm() > epsi){
                        std::cout << "[Up] Up is different at index " << gsymb->getMortonIndex() << " level " << level << " is " << diffUp << "\n";
                    }
                    FMath::FAccurater<FReal> diffDown;
                    diffDown.add(cell->getLocalExpansionData().get(0), gloc->get(0), gloc->getVectorSize());
                    if(diffDown.getRelativeInfNorm() > epsi || diffDown.getRelativeL2Norm() > epsi){
                        std::cout << "[Up] Down is different at index " << gsymb->getMortonIndex() << " level " << level << " is " << diffDown << "\n";
                    }
                }
            });

        groupedTree.forEachCellLeaf<FP2PGroupParticleContainer<FReal> >(
            [&](GroupCellSymbClass* gsymb ,
                GroupCellUpClass*   /* gmul */,
                GroupCellDownClass* /* gloc */,
                FP2PGroupParticleContainer<FReal> * leafTarget)
            {
            const ContainerClass* targets = treeCheck.getLeafSrc(gsymb->getMortonIndex());
            if(targets == nullptr){
                std::cout << "[Empty] Error leaf should exist " << gsymb->getMortonIndex() << "\n";
            }
            else{
                const FReal*const gposX = leafTarget->getPositions()[0];
                const FReal*const gposY = leafTarget->getPositions()[1];
                const FReal*const gposZ = leafTarget->getPositions()[2];
                const FSize gnbPartsInLeafTarget = leafTarget->getNbParticles();
                const FReal*const gforceX = leafTarget->getForcesX();
                const FReal*const gforceY = leafTarget->getForcesY();
                const FReal*const gforceZ = leafTarget->getForcesZ();
                const FReal*const gpotential = leafTarget->getPotentials();

                const FReal*const posX = targets->getPositions()[0];
                const FReal*const posY = targets->getPositions()[1];
                const FReal*const posZ = targets->getPositions()[2];
                const FSize nbPartsInLeafTarget = targets->getNbParticles();
                const FReal*const forceX = targets->getForcesX();
                const FReal*const forceY = targets->getForcesY();
                const FReal*const forceZ = targets->getForcesZ();
                const FReal*const potential = targets->getPotentials();

                if(gnbPartsInLeafTarget != nbPartsInLeafTarget){
                    std::cout << "[Empty] Not the same number of particles at " << gsymb->getMortonIndex()
                              << " gnbPartsInLeafTarget " << gnbPartsInLeafTarget << " nbPartsInLeafTarget " << nbPartsInLeafTarget << "\n";
                }
                else{
                    FMath::FAccurater<FReal> potentialDiff;
                    FMath::FAccurater<FReal> fx, fy, fz;
                    for(FSize idxPart = 0 ; idxPart < nbPartsInLeafTarget ; ++idxPart){
                        if(gposX[idxPart] != posX[idxPart] || gposY[idxPart] != posY[idxPart]
                                || gposZ[idxPart] != posZ[idxPart]){
                            std::cout << "[Empty] Not the same particlea at " << gsymb->getMortonIndex() << " idx " << idxPart
                                      << gposX[idxPart] << " " << posX[idxPart] << " " << gposY[idxPart] << " " << posY[idxPart]
                                      << " " << gposZ[idxPart] << " " << posZ[idxPart] << "\n";
                        }
                        else{
                            potentialDiff.add(potential[idxPart], gpotential[idxPart]);
                            fx.add(forceX[idxPart], gforceX[idxPart]);
                            fy.add(forceY[idxPart], gforceY[idxPart]);
                            fz.add(forceZ[idxPart], gforceZ[idxPart]);
                        }
                    }
                    if(potentialDiff.getRelativeInfNorm() > epsi || potentialDiff.getRelativeL2Norm() > epsi){
                        std::cout << "[Up] potentialDiff is different at index " << gsymb->getMortonIndex() << " is " << potentialDiff << "\n";
                    }
                    if(fx.getRelativeInfNorm() > epsi || fx.getRelativeL2Norm() > epsi){
                        std::cout << "[Up] fx is different at index " << gsymb->getMortonIndex() << " is " << fx << "\n";
                    }
                    if(fy.getRelativeInfNorm() > epsi || fy.getRelativeL2Norm() > epsi){
                        std::cout << "[Up] fy is different at index " << gsymb->getMortonIndex() << " is " << fy << "\n";
                    }
                    if(fz.getRelativeInfNorm() > epsi || fz.getRelativeL2Norm() > epsi){
                        std::cout << "[Up] fz is different at index " << gsymb->getMortonIndex() << " is " << fz << "\n";
                    }
                }
            }
        });

        std::cout << "Comparing is over" << std::endl;
    }

    return 0;
}
